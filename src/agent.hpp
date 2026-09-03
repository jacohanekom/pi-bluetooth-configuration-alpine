#pragma once
/**
 * agent.hpp -- registers a minimal "NoInputNoOutput" BlueZ pairing agent.
 *
 * Without ANY agent registered, BlueZ has no way to complete pairing when
 * something demands it -- and something does, regardless of what this
 * daemon itself requires: BlueZ's own built-in GATT profiles (Battery
 * Service, Device Information, Current Time Service -- see main.cpp's
 * GATT service comment) declare characteristics that need an
 * authenticated link, and iOS's system Bluetooth daemon reads Battery
 * Level on every connected peripheral automatically, for its own "device
 * battery %" indicator, independent of anything this app's own client
 * does. That read fails with Insufficient Authentication, BlueZ sends an
 * SMP Security Request to try to fix that, and with no agent registered
 * at all, pairing fails outright with "Pairing not supported" -- which
 * BlueZ then treats as fatal enough to disconnect the device entirely.
 * Confirmed via a live HCI trace (btmon) against a real device: this
 * exact sequence repeats every single connection, a few seconds apart,
 * forever -- indistinguishable from constant BLE flakiness until traced
 * at this level.
 *
 * This registers a "NoInputNoOutput" agent so that sequence can instead
 * complete via Just Works pairing -- the simplest, fully headless form
 * of BLE bonding: no PIN entry, no numeric comparison, no confirmation
 * dialog anywhere, nothing for a person to interact with. This is
 * distinct from the interactive pairing this project's README documents
 * as unreliable on the Pi 3's hardware (see "Security model") -- that
 * was about pairing requiring a person to do something during setup;
 * this is BlueZ silently satisfying its own internal profiles'
 * authentication requirement in the background, unrelated to anything
 * this app's own WiFi-configuration protocol does (which still has no
 * pairing/encryption of its own at all, unchanged).
 */
#include <dbus/dbus.h>

#include <functional>
#include <string>

namespace agentctl {

namespace detail {

inline bool call_and_check(DBusConnection* conn, const std::string& dest, const std::string& path,
                            const std::string& iface, const std::string& method,
                            const std::function<void(DBusMessageIter&)>& build_args,
                            std::string& err_out) {
    DBusMessage* msg = dbus_message_new_method_call(dest.c_str(), path.c_str(), iface.c_str(), method.c_str());
    if (build_args) {
        DBusMessageIter it;
        dbus_message_iter_init_append(msg, &it);
        build_args(it);
    }
    DBusError err;
    dbus_error_init(&err);
    // Unlike GattServer::start()'s RegisterApplication/RegisterAdvertisement
    // calls, BlueZ doesn't need to call back into anything we export as
    // part of handling these two -- it just records our object path and
    // capability for later, so a plain blocking call here can't deadlock
    // the way those needed to avoid.
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(conn, msg, 5000, &err);
    dbus_message_unref(msg);
    if (!reply) {
        err_out = method + " failed: " + (err.message ? err.message : "unknown");
        dbus_error_free(&err);
        return false;
    }
    dbus_message_unref(reply);
    return true;
}

inline void reply_empty(DBusConnection* conn, DBusMessage* msg) {
    DBusMessage* reply = dbus_message_new_method_return(msg);
    dbus_connection_send(conn, reply, nullptr);
    dbus_message_unref(reply);
}

inline DBusHandlerResult on_message(DBusConnection* conn, DBusMessage* msg, void*) {
    const char* ifc_c = dbus_message_get_interface(msg);
    const char* mem_c = dbus_message_get_member(msg);
    std::string ifc = ifc_c ? ifc_c : "";
    std::string mem = mem_c ? mem_c : "";

    if (ifc == "org.freedesktop.DBus.Introspectable" && mem == "Introspect") {
        DBusMessage* reply = dbus_message_new_method_return(msg);
        const char* xml =
            "<node><interface name=\"org.bluez.Agent1\">"
            "<method name=\"Release\"/>"
            "<method name=\"RequestConfirmation\"><arg type=\"o\"/><arg type=\"u\"/></method>"
            "<method name=\"RequestAuthorization\"><arg type=\"o\"/></method>"
            "<method name=\"AuthorizeService\"><arg type=\"o\"/><arg type=\"s\"/></method>"
            "<method name=\"Cancel\"/>"
            "</interface></node>";
        dbus_message_append_args(reply, DBUS_TYPE_STRING, &xml, DBUS_TYPE_INVALID);
        dbus_connection_send(conn, reply, nullptr);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (ifc == "org.bluez.Agent1") {
        // NoInputNoOutput means BlueZ never calls RequestPinCode/
        // DisplayPinCode/RequestPasskey/DisplayPasskey on us at all --
        // Just Works pairing skips all of that. What it does call is one
        // of these three to confirm it's fine to proceed without any
        // user interaction; accepting unconditionally is the entire
        // point of this agent existing (see this file's header comment).
        if (mem == "RequestConfirmation" || mem == "RequestAuthorization" || mem == "AuthorizeService" ||
            mem == "Release" || mem == "Cancel") {
            reply_empty(conn, msg);
            return DBUS_HANDLER_RESULT_HANDLED;
        }
    }

    DBusMessage* reply = dbus_message_new_error(msg, "org.bluez.Error.Rejected", "not supported by this agent");
    dbus_connection_send(conn, reply, nullptr);
    dbus_message_unref(reply);
    return DBUS_HANDLER_RESULT_HANDLED;
}

} // namespace detail

constexpr const char* AGENT_PATH = "/org/bluez/aipicam_agent";

// Registers the agent and requests it as BlueZ's default -- call once,
// early in startup, on the same DBusConnection the rest of the daemon
// uses. Failure isn't treated as fatal to starting the daemon (the GATT
// service itself doesn't depend on it), but leaves this daemon exposed
// to exactly the disconnect-loop this file exists to prevent.
inline bool register_pairing_agent(DBusConnection* conn, std::string& err_out) {
    static DBusObjectPathVTable vtable = {nullptr, &detail::on_message, nullptr, nullptr, nullptr, nullptr};
    DBusError err;
    dbus_error_init(&err);
    if (!dbus_connection_try_register_object_path(conn, AGENT_PATH, &vtable, nullptr, &err)) {
        err_out = std::string("failed to register agent object path: ") + (err.message ? err.message : "unknown");
        dbus_error_free(&err);
        return false;
    }

    if (!detail::call_and_check(
            conn, "org.bluez", "/org/bluez", "org.bluez.AgentManager1", "RegisterAgent",
            [](DBusMessageIter& it) {
                const char* path = AGENT_PATH;
                const char* cap = "NoInputNoOutput";
                dbus_message_iter_append_basic(&it, DBUS_TYPE_OBJECT_PATH, &path);
                dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &cap);
            },
            err_out)) {
        return false;
    }

    return detail::call_and_check(
        conn, "org.bluez", "/org/bluez", "org.bluez.AgentManager1", "RequestDefaultAgent",
        [](DBusMessageIter& it) {
            const char* path = AGENT_PATH;
            dbus_message_iter_append_basic(&it, DBUS_TYPE_OBJECT_PATH, &path);
        },
        err_out);
}

} // namespace agentctl
