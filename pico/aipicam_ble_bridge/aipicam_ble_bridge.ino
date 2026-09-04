/**
 * aipicam_ble_bridge -- hosts the aipicam BLE GATT peripheral entirely on
 * this Pico 2 W's own BTstack build, relaying characteristic reads/
 * writes/notifications to pi-bluetooth-configuration-alpine (running on
 * the Pi this board is plugged into over USB) as a plain line-based
 * protocol over the native USB serial (CDC-ACM) connection. See the
 * Pi-side src/pico_transport.hpp for the full story of why this exists
 * (BlueZ's own built-in GATT profiles forcing a disconnect loop this
 * daemon couldn't fix from userspace) and the exact wire protocol this
 * firmware implements the other half of.
 *
 * Board: Raspberry Pi Pico 2 W, via the earlephilhower arduino-pico core
 * (https://github.com/earlephilhower/arduino-pico) -- install via
 * Arduino IDE's Boards Manager ("Raspberry Pi Pico/RP2040/RP2350" by
 * Earle F. Philhower, III), then select Tools > Board > "Raspberry Pi
 * Pico 2 W". Bluetooth support comes from BTstackLib, which ships
 * bundled with that core -- no separate library install needed for it.
 *
 * NOT YET VERIFIED AGAINST A REAL COMPILE: this was written from
 * documented BTstackLib usage patterns (Arduino-Pico's own bundled
 * GATTServer example and related community references), not compiled or
 * flashed by me -- I have no Pico 2 W or Arduino toolchain available.
 * Expect to iterate on the first build or two; share the exact compiler
 * error and I'll correct it precisely rather than guessing further.
 * Likely trouble spots if something doesn't match: the exact
 * BTstackLib callback signatures/method names (should match this core's
 * bundled example sketch -- compare against that if in doubt), and
 * which Tools > USB Stack setting makes Serial behave as a plain
 * USB-CDC serial port (the default Pico SDK USB stack should already do
 * this; only try Adafruit TinyUSB if plain Serial doesn't enumerate).
 */
#include <BTstackLib.h>
#include <SPI.h>
#include <pico/unique_id.h>

// Same nine UUIDs pi-bluetooth-configuration-alpine has always used --
// see that repo's main.cpp *_UUID constants and README. Nothing here is
// negotiated at runtime; both sides just hardcode the same table.
static const char* SERVICE_UUID = "7b1e0000-6a45-4d1f-9b0a-3c2f8e4d5a10";

struct CharDef {
    const char* name; // short wire-protocol name -- see pico_transport.hpp
    const char* uuid;
    uint8_t properties;
};

static const CharDef CHAR_TABLE[] = {
    {"SSID",    "7b1e0001-6a45-4d1f-9b0a-3c2f8e4d5a10", ATT_PROPERTY_WRITE},
    {"PSK",     "7b1e0002-6a45-4d1f-9b0a-3c2f8e4d5a10", ATT_PROPERTY_WRITE},
    {"COMMAND", "7b1e0003-6a45-4d1f-9b0a-3c2f8e4d5a10", ATT_PROPERTY_WRITE},
    {"STATUS",  "7b1e0004-6a45-4d1f-9b0a-3c2f8e4d5a10", (uint8_t)(ATT_PROPERTY_READ | ATT_PROPERTY_NOTIFY)},
    {"SCAN",    "7b1e0005-6a45-4d1f-9b0a-3c2f8e4d5a10", (uint8_t)(ATT_PROPERTY_READ | ATT_PROPERTY_NOTIFY)},
    {"ETH",     "7b1e0006-6a45-4d1f-9b0a-3c2f8e4d5a10", (uint8_t)(ATT_PROPERTY_READ | ATT_PROPERTY_WRITE | ATT_PROPERTY_NOTIFY)},
    {"LEASES",  "7b1e0007-6a45-4d1f-9b0a-3c2f8e4d5a10", (uint8_t)(ATT_PROPERTY_READ | ATT_PROPERTY_NOTIFY)},
    {"RELAYS",  "7b1e0008-6a45-4d1f-9b0a-3c2f8e4d5a10", (uint8_t)(ATT_PROPERTY_READ | ATT_PROPERTY_NOTIFY)},
    {"VICTRON", "7b1e0009-6a45-4d1f-9b0a-3c2f8e4d5a10", (uint8_t)(ATT_PROPERTY_READ | ATT_PROPERTY_NOTIFY)},
};
static const int NUM_CHARS = sizeof(CHAR_TABLE) / sizeof(CHAR_TABLE[0]);
static const size_t MAX_VALUE_LEN = 512; // comfortably above the largest JSON payload this project ever sends

static uint16_t char_handles[NUM_CHARS];
static uint8_t cache_buf[NUM_CHARS][MAX_VALUE_LEN];
static uint16_t cache_len[NUM_CHARS];

static volatile bool connected = false;
static uint16_t connection_handle = 0;

static int index_for_handle(uint16_t value_handle) {
    for (int i = 0; i < NUM_CHARS; i++) {
        if (char_handles[i] == value_handle) return i;
    }
    return -1;
}

static int index_for_name(const String& name) {
    for (int i = 0; i < NUM_CHARS; i++) {
        if (name == CHAR_TABLE[i].name) return i;
    }
    return -1;
}

// --- base64, mirroring pico_transport.hpp's implementation on the Pi side ---

static const char* B64_TABLE = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static String base64_encode(const uint8_t* data, size_t len) {
    String out;
    out.reserve(((len + 2) / 3) * 4);
    size_t i = 0;
    while (i + 2 < len) {
        uint32_t n = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8) | data[i + 2];
        out += B64_TABLE[(n >> 18) & 0x3f];
        out += B64_TABLE[(n >> 12) & 0x3f];
        out += B64_TABLE[(n >> 6) & 0x3f];
        out += B64_TABLE[n & 0x3f];
        i += 3;
    }
    size_t rem = len - i;
    if (rem == 1) {
        uint32_t n = (uint32_t)data[i] << 16;
        out += B64_TABLE[(n >> 18) & 0x3f];
        out += B64_TABLE[(n >> 12) & 0x3f];
        out += "==";
    } else if (rem == 2) {
        uint32_t n = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8);
        out += B64_TABLE[(n >> 18) & 0x3f];
        out += B64_TABLE[(n >> 12) & 0x3f];
        out += B64_TABLE[(n >> 6) & 0x3f];
        out += "=";
    }
    return out;
}

static int b64_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

// Decodes into out[], capped at out_cap bytes, returning the number of
// bytes actually written -- a payload larger than out_cap is silently
// truncated rather than overflowing (shouldn't happen at MAX_VALUE_LEN,
// but truncating beats corrupting adjacent memory if it ever does).
static uint16_t base64_decode(const String& in, uint8_t* out, size_t out_cap) {
    int vals[4];
    int vi = 0;
    uint16_t written = 0;
    for (size_t i = 0; i < in.length(); i++) {
        char c = in[i];
        if (c == '=' || c == '\r' || c == '\n') continue;
        int v = b64_val(c);
        if (v < 0) continue;
        vals[vi++] = v;
        if (vi == 4) {
            uint32_t n = ((uint32_t)vals[0] << 18) | ((uint32_t)vals[1] << 12) | ((uint32_t)vals[2] << 6) | (uint32_t)vals[3];
            if (written < out_cap) out[written++] = (uint8_t)((n >> 16) & 0xff);
            if (written < out_cap) out[written++] = (uint8_t)((n >> 8) & 0xff);
            if (written < out_cap) out[written++] = (uint8_t)(n & 0xff);
            vi = 0;
        }
    }
    if (vi == 2) {
        uint32_t n = ((uint32_t)vals[0] << 18) | ((uint32_t)vals[1] << 12);
        if (written < out_cap) out[written++] = (uint8_t)((n >> 16) & 0xff);
    } else if (vi == 3) {
        uint32_t n = ((uint32_t)vals[0] << 18) | ((uint32_t)vals[1] << 12) | ((uint32_t)vals[2] << 6);
        if (written < out_cap) out[written++] = (uint8_t)((n >> 16) & 0xff);
        if (written < out_cap) out[written++] = (uint8_t)((n >> 8) & 0xff);
    }
    return written;
}

// --- BTstack callbacks ---

void deviceConnectedCallback(BLEStatus status, BLEDevice* device) {
    if (status == BLE_STATUS_OK) {
        connected = true;
        connection_handle = device->getHandle();
    }
}

void deviceDisconnectedCallback(BLEDevice* device) {
    (void)device;
    connected = false;
}

uint16_t gattReadCallback(uint16_t value_handle, uint8_t* buffer, uint16_t buffer_size) {
    int idx = index_for_handle(value_handle);
    if (idx < 0) return 0;
    uint16_t len = cache_len[idx];
    if (len > buffer_size) len = buffer_size;
    memcpy(buffer, cache_buf[idx], len);
    return len;
}

int gattWriteCallback(uint16_t value_handle, uint8_t* buffer, uint16_t size) {
    int idx = index_for_handle(value_handle);
    if (idx < 0) return 0;
    // Forwarded as-is to the Pi -- see pico_transport.hpp's WRITE line.
    // Fire-and-forget: this project's protocol has no reply value for a
    // characteristic write, matching how it always worked over BlueZ.
    Serial.print("WRITE ");
    Serial.print(CHAR_TABLE[idx].name);
    Serial.print(' ');
    Serial.println(base64_encode(buffer, size));
    return 0;
}

// --- Pi -> Pico serial line handling ---

static String line_buf;

static void process_line(const String& line) {
    int sp1 = line.indexOf(' ');
    if (sp1 < 0) return;
    if (line.substring(0, sp1) != "VALUE") return;
    int sp2 = line.indexOf(' ', sp1 + 1);
    if (sp2 < 0) return;

    String name = line.substring(sp1 + 1, sp2);
    String b64 = line.substring(sp2 + 1);

    int idx = index_for_name(name);
    if (idx < 0) return;

    cache_len[idx] = base64_decode(b64, cache_buf[idx], MAX_VALUE_LEN);

    // No separate subscription tracking here -- att_server_notify()
    // itself is a no-op against a central that hasn't enabled
    // notifications for this handle, so it's safe to just always call it
    // rather than pull in extra bookkeeping for something BTstack
    // already handles.
    if ((CHAR_TABLE[idx].properties & ATT_PROPERTY_NOTIFY) && connected) {
        att_server_notify(connection_handle, char_handles[idx], cache_buf[idx], cache_len[idx]);
    }
}

static void handle_serial_input() {
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\n') {
            process_line(line_buf);
            line_buf = "";
        } else if (c != '\r') {
            line_buf += c;
        }
    }
}

void setup() {
    Serial.begin(115200);

    // A stable per-board name derived from the Pico's own unique flash
    // ID, not something the Pi hands over at boot -- keeps this firmware
    // fully self-contained and BLE-advertising before the Pi's daemon
    // has even opened the serial port. Multiple aipicam units are still
    // distinguishable in a client's device list, same as before -- just
    // by this board's own serial now, not the Pi's CPU serial.
    pico_unique_board_id_t board_id;
    pico_get_unique_board_id(&board_id);
    char name[24];
    snprintf(name, sizeof(name), "aipicam-%02x%02x%02x%02x",
             board_id.id[4], board_id.id[5], board_id.id[6], board_id.id[7]);

    BTstack.setBLEDeviceConnectedCallback(deviceConnectedCallback);
    BTstack.setBLEDeviceDisconnectedCallback(deviceDisconnectedCallback);
    BTstack.setGATTCharacteristicRead(gattReadCallback);
    BTstack.setGATTCharacteristicWrite(gattWriteCallback);

    BTstack.addGATTService(new UUID(SERVICE_UUID));
    for (int i = 0; i < NUM_CHARS; i++) {
        char_handles[i] = BTstack.addGATTCharacteristicDynamic(new UUID(CHAR_TABLE[i].uuid), CHAR_TABLE[i].properties, 0);
        cache_len[i] = 0;
    }

    BTstack.setup(name);
    BTstack.startAdvertising();

    // See pico_transport.hpp's header comment -- the Pi waits for this
    // before seeding initial characteristic values.
    Serial.println("READY");
}

void loop() {
    BTstack.loop();
    handle_serial_input();
}
