#pragma once
/**
 * gatt_server.hpp -- a minimal BlueZ LE GATT peripheral, built directly on
 * libdbus (no GLib/sd-bus). Exports one GATT service with a handful of
 * characteristics, an LE advertisement, and a pairing agent, using BlueZ's
 * standard D-Bus API:
 *
 *   org.freedesktop.DBus.ObjectManager   (GetManagedObjects, on the app root)
 *   org.bluez.GattService1 / GattCharacteristic1
 *   org.bluez.LEAdvertisement1 / LEAdvertisingManager1
 *   org.bluez.Agent1 / AgentManager1
 *
 * This mirrors the object model BlueZ's own example-gatt-server.py uses --
 * BlueZ drives everything through method calls on objects *we* export, so
 * ours is a single message handler dispatched by a small "what kind of
 * object is this" tag rather than one class per D-Bus interface.
 *
 * Only what this daemon needs is implemented: read/write/notify on
 * characteristics (with BlueZ's long-read "offset" option honoured), a
 * peripheral advertisement, and an agent that auto-accepts pairing
 * (suitable for a headless "NoInputNoOutput" device -- see the README for
 * what that does and doesn't protect against).
 */
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <dbus/dbus.h>

namespace gattsrv {

constexpr const char* BLUEZ_BUS         = "org.bluez";
constexpr const char* IFACE_OM          = "org.freedesktop.DBus.ObjectManager";
constexpr const char* IFACE_PROPS       = "org.freedesktop.DBus.Properties";
constexpr const char* IFACE_GATT_SVC    = "org.bluez.GattService1";
constexpr const char* IFACE_GATT_CHAR   = "org.bluez.GattCharacteristic1";
constexpr const char* IFACE_LE_ADV      = "org.bluez.LEAdvertisement1";
constexpr const char* IFACE_LE_ADV_MGR  = "org.bluez.LEAdvertisingManager1";
constexpr const char* IFACE_GATT_MGR    = "org.bluez.GattManager1";
constexpr const char* IFACE_AGENT       = "org.bluez.Agent1";
constexpr const char* IFACE_AGENT_MGR   = "org.bluez.AgentManager1";
constexpr const char* IFACE_ADAPTER     = "org.bluez.Adapter1";

struct PropVal {
    enum Kind { STR, BOOL, OBJPATH, STR_ARRAY } kind;
    std::string name;
    std::string s;
    bool b = false;
    std::vector<std::string> arr;

    static PropVal str(std::string n, std::string v)      { PropVal p; p.kind=STR; p.name=std::move(n); p.s=std::move(v); return p; }
    static PropVal boolean(std::string n, bool v)          { PropVal p; p.kind=BOOL; p.name=std::move(n); p.b=v; return p; }
    static PropVal objpath(std::string n, std::string v)   { PropVal p; p.kind=OBJPATH; p.name=std::move(n); p.s=std::move(v); return p; }
    static PropVal str_array(std::string n, std::vector<std::string> v) { PropVal p; p.kind=STR_ARRAY; p.name=std::move(n); p.arr=std::move(v); return p; }
};

struct Characteristic {
    std::string path;
    std::string uuid;
    std::string service_path;
    std::vector<std::string> flags;
    std::function<std::vector<uint8_t>()> on_read;
    std::function<void(const std::vector<uint8_t>&)> on_write;
    bool notifying = false;
};

// ── D-Bus container helpers ─────────────────────────────────────────────
namespace detail {

inline void append_variant_props(DBusMessageIter* dict_iter, const std::vector<PropVal>& props) {
    for (auto& p : props) {
        DBusMessageIter entry, variant;
        dbus_message_iter_open_container(dict_iter, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
        const char* name_c = p.name.c_str();
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &name_c);
        switch (p.kind) {
            case PropVal::STR: {
                dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &variant);
                const char* v = p.s.c_str();
                dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &v);
                dbus_message_iter_close_container(&entry, &variant);
                break;
            }
            case PropVal::BOOL: {
                dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "b", &variant);
                dbus_bool_t v = p.b ? 1 : 0;
                dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &v);
                dbus_message_iter_close_container(&entry, &variant);
                break;
            }
            case PropVal::OBJPATH: {
                dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "o", &variant);
                const char* v = p.s.c_str();
                dbus_message_iter_append_basic(&variant, DBUS_TYPE_OBJECT_PATH, &v);
                dbus_message_iter_close_container(&entry, &variant);
                break;
            }
            case PropVal::STR_ARRAY: {
                dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "as", &variant);
                DBusMessageIter arr;
                dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY, "s", &arr);
                for (auto& s : p.arr) {
                    const char* v = s.c_str();
                    dbus_message_iter_append_basic(&arr, DBUS_TYPE_STRING, &v);
                }
                dbus_message_iter_close_container(&variant, &arr);
                dbus_message_iter_close_container(&entry, &variant);
                break;
            }
        }
        dbus_message_iter_close_container(dict_iter, &entry);
    }
}

inline void reply_empty(DBusConnection* conn, DBusMessage* msg) {
    DBusMessage* reply = dbus_message_new_method_return(msg);
    dbus_connection_send(conn, reply, nullptr);
    dbus_message_unref(reply);
}

inline void reply_error(DBusConnection* conn, DBusMessage* msg, const char* name, const std::string& text) {
    DBusMessage* reply = dbus_message_new_error(msg, name, text.c_str());
    dbus_connection_send(conn, reply, nullptr);
    dbus_message_unref(reply);
}

inline void reply_bytes(DBusConnection* conn, DBusMessage* msg, const std::vector<uint8_t>& bytes) {
    DBusMessage* reply = dbus_message_new_method_return(msg);
    DBusMessageIter it, arr;
    dbus_message_iter_init_append(reply, &it);
    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "y", &arr);
    for (uint8_t b : bytes) dbus_message_iter_append_basic(&arr, DBUS_TYPE_BYTE, &b);
    dbus_message_iter_close_container(&it, &arr);
    dbus_connection_send(conn, reply, nullptr);
    dbus_message_unref(reply);
}

inline void reply_props_dict(DBusConnection* conn, DBusMessage* msg, const std::vector<PropVal>& props) {
    DBusMessage* reply = dbus_message_new_method_return(msg);
    DBusMessageIter it, dict;
    dbus_message_iter_init_append(reply, &it);
    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &dict);
    append_variant_props(&dict, props);
    dbus_message_iter_close_container(&it, &dict);
    dbus_connection_send(conn, reply, nullptr);
    dbus_message_unref(reply);
}

inline std::vector<uint8_t> read_byte_array(DBusMessageIter* iter) {
    std::vector<uint8_t> out;
    DBusMessageIter sub;
    dbus_message_iter_recurse(iter, &sub);
    while (dbus_message_iter_get_arg_type(&sub) == DBUS_TYPE_BYTE) {
        unsigned char b;
        dbus_message_iter_get_basic(&sub, &b);
        out.push_back(b);
        dbus_message_iter_next(&sub);
    }
    return out;
}

inline uint16_t read_offset_option(DBusMessageIter* options_iter) {
    uint16_t offset = 0;
    DBusMessageIter arr;
    dbus_message_iter_recurse(options_iter, &arr);
    while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry;
        dbus_message_iter_recurse(&arr, &entry);
        const char* key = nullptr;
        dbus_message_iter_get_basic(&entry, &key);
        if (key && std::string(key) == "offset") {
            dbus_message_iter_next(&entry);
            DBusMessageIter variant;
            dbus_message_iter_recurse(&entry, &variant);
            if (dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_UINT16) {
                uint16_t v = 0;
                dbus_message_iter_get_basic(&variant, &v);
                offset = v;
            }
        }
        dbus_message_iter_next(&arr);
    }
    return offset;
}

// One-shot outbound method call (used only during startup registration).
inline DBusMessage* call_method(DBusConnection* conn, const std::string& dest, const std::string& path,
                                 const std::string& iface, const std::string& method,
                                 const std::function<void(DBusMessageIter&)>& build_args,
                                 DBusError* err) {
    DBusMessage* msg = dbus_message_new_method_call(dest.c_str(), path.c_str(), iface.c_str(), method.c_str());
    if (build_args) {
        DBusMessageIter it;
        dbus_message_iter_init_append(msg, &it);
        build_args(it);
    }

    DBusPendingCall* pending = nullptr;
    if (!dbus_connection_send_with_reply(conn, msg, &pending, 10000) || !pending) {
        dbus_message_unref(msg);
        dbus_set_error_const(err, DBUS_ERROR_FAILED, "dbus_connection_send_with_reply failed");
        return nullptr;
    }
    dbus_message_unref(msg);

    // Deliberately not a blocking send-and-wait: BlueZ's RegisterApplication
    // and RegisterAdvertisement both call back into objects *we* export
    // (GetManagedObjects on the app root, Properties.GetAll on the
    // advertisement) before they reply. dbus_connection_send_with_reply_and_block
    // doesn't service incoming requests while it waits, so it would
    // deadlock against that callback -- BlueZ waiting on us, us waiting on
    // BlueZ -- until this call's own timeout fires. Pumping
    // read_write_dispatch here keeps our own registered object paths
    // (via the vtables from dbus_connection_try_register_object_path)
    // live while we wait for the pending call to complete.
    while (!dbus_pending_call_get_completed(pending)) {
        dbus_connection_read_write_dispatch(conn, 50);
    }

    DBusMessage* reply = dbus_pending_call_steal_reply(pending);
    dbus_pending_call_unref(pending);

    if (reply && dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR) {
        dbus_set_error_from_message(err, reply);
    }

    return reply;
}

} // namespace detail

class GattServer {
public:
    GattServer(DBusConnection* conn, std::string adapter_path, std::string app_root,
               std::string service_uuid, std::string device_name)
        : conn_(conn), adapter_path_(std::move(adapter_path)), app_root_(std::move(app_root)),
          service_path_(app_root_ + "/service0"), service_uuid_(std::move(service_uuid)),
          device_name_(std::move(device_name)), adv_path_(app_root_ + "/advertising0"),
          agent_path_(app_root_ + "/agent0") {}

    // Path is assigned automatically as <service_path>/charN in add order.
    Characteristic* add_characteristic(const std::string& uuid, std::vector<std::string> flags,
                                        std::function<std::vector<uint8_t>()> on_read,
                                        std::function<void(const std::vector<uint8_t>&)> on_write) {
        auto c = std::make_unique<Characteristic>();
        c->path = service_path_ + "/char" + std::to_string(chars_.size());
        c->uuid = uuid;
        c->service_path = service_path_;
        c->flags = std::move(flags);
        c->on_read = std::move(on_read);
        c->on_write = std::move(on_write);
        chars_.push_back(std::move(c));
        return chars_.back().get();
    }

    bool start(const std::string& agent_capability, std::string& err_out) {
        DBusError err;
        dbus_error_init(&err);

        static DBusObjectPathVTable vtable = {nullptr, &GattServer::on_message_trampoline, nullptr, nullptr, nullptr, nullptr};

        ctx_root_ = std::make_unique<ObjCtx>(ObjCtx{ObjKind::Root, this, nullptr});
        dbus_connection_try_register_object_path(conn_, app_root_.c_str(), &vtable, ctx_root_.get(), &err);

        ctx_service_ = std::make_unique<ObjCtx>(ObjCtx{ObjKind::Service, this, nullptr});
        dbus_connection_try_register_object_path(conn_, service_path_.c_str(), &vtable, ctx_service_.get(), &err);

        for (auto& c : chars_) {
            auto ctx = std::make_unique<ObjCtx>(ObjCtx{ObjKind::Characteristic, this, c.get()});
            dbus_connection_try_register_object_path(conn_, c->path.c_str(), &vtable, ctx.get(), &err);
            char_ctxs_.push_back(std::move(ctx));
        }

        ctx_adv_ = std::make_unique<ObjCtx>(ObjCtx{ObjKind::Advertisement, this, nullptr});
        dbus_connection_try_register_object_path(conn_, adv_path_.c_str(), &vtable, ctx_adv_.get(), &err);

        ctx_agent_ = std::make_unique<ObjCtx>(ObjCtx{ObjKind::Agent, this, nullptr});
        dbus_connection_try_register_object_path(conn_, agent_path_.c_str(), &vtable, ctx_agent_.get(), &err);

        if (!set_adapter_bool("Powered", true, err_out)) return false;
        if (!set_adapter_string("Alias", device_name_, err_out)) return false;
        if (!set_adapter_bool("Pairable", true, err_out)) return false;
        if (!set_adapter_bool("Discoverable", true, err_out)) return false;

        DBusMessage* reply;

        reply = detail::call_method(conn_, "org.bluez", "/org/bluez", IFACE_AGENT_MGR, "RegisterAgent",
            [&](DBusMessageIter& it) {
                const char* p = agent_path_.c_str();
                dbus_message_iter_append_basic(&it, DBUS_TYPE_OBJECT_PATH, &p);
                const char* cap = agent_capability.c_str();
                dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &cap);
            }, &err);
        if (!check(reply, err, "RegisterAgent", err_out)) return false;

        reply = detail::call_method(conn_, "org.bluez", "/org/bluez", IFACE_AGENT_MGR, "RequestDefaultAgent",
            [&](DBusMessageIter& it) {
                const char* p = agent_path_.c_str();
                dbus_message_iter_append_basic(&it, DBUS_TYPE_OBJECT_PATH, &p);
            }, &err);
        if (!check(reply, err, "RequestDefaultAgent", err_out)) return false;

        reply = detail::call_method(conn_, "org.bluez", adapter_path_, IFACE_GATT_MGR, "RegisterApplication",
            [&](DBusMessageIter& it) {
                const char* p = app_root_.c_str();
                dbus_message_iter_append_basic(&it, DBUS_TYPE_OBJECT_PATH, &p);
                DBusMessageIter dict;
                dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &dict);
                dbus_message_iter_close_container(&it, &dict);
            }, &err);
        if (!check(reply, err, "RegisterApplication", err_out)) return false;

        reply = detail::call_method(conn_, "org.bluez", adapter_path_, IFACE_LE_ADV_MGR, "RegisterAdvertisement",
            [&](DBusMessageIter& it) {
                const char* p = adv_path_.c_str();
                dbus_message_iter_append_basic(&it, DBUS_TYPE_OBJECT_PATH, &p);
                DBusMessageIter dict;
                dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &dict);
                dbus_message_iter_close_container(&it, &dict);
            }, &err);
        if (!check(reply, err, "RegisterAdvertisement", err_out)) return false;

        return true;
    }

    void notify(Characteristic* chr, const std::vector<uint8_t>& value) {
        std::lock_guard<std::mutex> lk(send_mu_);
        DBusMessage* sig = dbus_message_new_signal(chr->path.c_str(), IFACE_PROPS, "PropertiesChanged");
        DBusMessageIter it, dict, entry, variant, arr, inval;
        dbus_message_iter_init_append(sig, &it);

        const char* iface_c = IFACE_GATT_CHAR;
        dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &iface_c);

        dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &dict);
        dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
        const char* key = "Value";
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "ay", &variant);
        dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY, "y", &arr);
        for (uint8_t b : value) dbus_message_iter_append_basic(&arr, DBUS_TYPE_BYTE, &b);
        dbus_message_iter_close_container(&variant, &arr);
        dbus_message_iter_close_container(&entry, &variant);
        dbus_message_iter_close_container(&dict, &entry);
        dbus_message_iter_close_container(&it, &dict);

        dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "s", &inval);
        dbus_message_iter_close_container(&it, &inval);

        dbus_connection_send(conn_, sig, nullptr);
        dbus_message_unref(sig);
    }

private:
    enum class ObjKind { Root, Service, Characteristic, Advertisement, Agent };
    struct ObjCtx {
        ObjKind kind;
        GattServer* self;
        Characteristic* chr;
    };

    bool set_adapter_bool(const std::string& prop, bool value, std::string& err_out) {
        DBusError err;
        dbus_error_init(&err);
        DBusMessage* reply = detail::call_method(conn_, "org.bluez", adapter_path_, IFACE_PROPS, "Set",
            [&](DBusMessageIter& it) {
                const char* iface_c = IFACE_ADAPTER;
                dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &iface_c);
                const char* prop_c = prop.c_str();
                dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &prop_c);
                DBusMessageIter variant;
                dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, "b", &variant);
                dbus_bool_t v = value ? 1 : 0;
                dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &v);
                dbus_message_iter_close_container(&it, &variant);
            }, &err);
        return check(reply, err, "Set " + prop, err_out);
    }

    bool set_adapter_string(const std::string& prop, const std::string& value, std::string& err_out) {
        DBusError err;
        dbus_error_init(&err);
        DBusMessage* reply = detail::call_method(conn_, "org.bluez", adapter_path_, IFACE_PROPS, "Set",
            [&](DBusMessageIter& it) {
                const char* iface_c = IFACE_ADAPTER;
                dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &iface_c);
                const char* prop_c = prop.c_str();
                dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &prop_c);
                DBusMessageIter variant;
                dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, "s", &variant);
                const char* v = value.c_str();
                dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &v);
                dbus_message_iter_close_container(&it, &variant);
            }, &err);
        return check(reply, err, "Set " + prop, err_out);
    }

    static bool check(DBusMessage* reply, DBusError& err, const std::string& what, std::string& err_out) {
        if (dbus_error_is_set(&err)) {
            err_out = what + ": " + err.message;
            dbus_error_free(&err);
            if (reply) dbus_message_unref(reply);
            return false;
        }
        if (reply) dbus_message_unref(reply);
        return true;
    }

    std::vector<PropVal> props_for(ObjKind kind, Characteristic* chr) const {
        switch (kind) {
            case ObjKind::Service:
                return {PropVal::str("UUID", service_uuid_), PropVal::boolean("Primary", true)};
            case ObjKind::Characteristic:
                return {PropVal::str("UUID", chr->uuid),
                        PropVal::objpath("Service", chr->service_path),
                        PropVal::str_array("Flags", chr->flags)};
            case ObjKind::Advertisement:
                return {PropVal::str("Type", "peripheral"),
                        PropVal::str("LocalName", device_name_),
                        PropVal::str_array("ServiceUUIDs", {service_uuid_})};
            default:
                return {};
        }
    }

    void handle_get_managed_objects(DBusMessage* msg) {
        DBusMessage* reply = dbus_message_new_method_return(msg);
        DBusMessageIter it, objs;
        dbus_message_iter_init_append(reply, &it);
        dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{oa{sa{sv}}}", &objs);

        auto emit_object = [&](const std::string& path, const std::string& iface, const std::vector<PropVal>& props) {
            DBusMessageIter obj, ifaces, iface_entry, props_dict;
            dbus_message_iter_open_container(&objs, DBUS_TYPE_DICT_ENTRY, nullptr, &obj);
            const char* path_c = path.c_str();
            dbus_message_iter_append_basic(&obj, DBUS_TYPE_OBJECT_PATH, &path_c);
            dbus_message_iter_open_container(&obj, DBUS_TYPE_ARRAY, "{sa{sv}}", &ifaces);
            dbus_message_iter_open_container(&ifaces, DBUS_TYPE_DICT_ENTRY, nullptr, &iface_entry);
            const char* iface_c = iface.c_str();
            dbus_message_iter_append_basic(&iface_entry, DBUS_TYPE_STRING, &iface_c);
            dbus_message_iter_open_container(&iface_entry, DBUS_TYPE_ARRAY, "{sv}", &props_dict);
            detail::append_variant_props(&props_dict, props);
            dbus_message_iter_close_container(&iface_entry, &props_dict);
            dbus_message_iter_close_container(&ifaces, &iface_entry);
            dbus_message_iter_close_container(&obj, &ifaces);
            dbus_message_iter_close_container(&objs, &obj);
        };

        emit_object(service_path_, IFACE_GATT_SVC, props_for(ObjKind::Service, nullptr));
        for (auto& c : chars_) emit_object(c->path, IFACE_GATT_CHAR, props_for(ObjKind::Characteristic, c.get()));

        dbus_message_iter_close_container(&it, &objs);
        dbus_connection_send(conn_, reply, nullptr);
        dbus_message_unref(reply);
    }

    DBusHandlerResult on_message(DBusConnection* conn, DBusMessage* msg, ObjCtx* ctx) {
        const char* iface = dbus_message_get_interface(msg);
        const char* member = dbus_message_get_member(msg);
        if (!iface || !member) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        std::string ifc(iface), mem(member);

        if (ctx->kind == ObjKind::Root && ifc == IFACE_OM && mem == "GetManagedObjects") {
            handle_get_managed_objects(msg);
            return DBUS_HANDLER_RESULT_HANDLED;
        }

        if (ifc == IFACE_PROPS) {
            if (mem == "GetAll") {
                detail::reply_props_dict(conn, msg, props_for(ctx->kind, ctx->chr));
                return DBUS_HANDLER_RESULT_HANDLED;
            }
            if (mem == "Get") {
                DBusMessageIter it;
                dbus_message_iter_init(msg, &it);
                dbus_message_iter_next(&it); // skip interface name arg
                const char* prop_name = nullptr;
                dbus_message_iter_get_basic(&it, &prop_name);
                for (auto& p : props_for(ctx->kind, ctx->chr)) {
                    if (p.name == prop_name) {
                        std::vector<PropVal> single{p};
                        DBusMessage* reply = dbus_message_new_method_return(msg);
                        DBusMessageIter rit, dict;
                        dbus_message_iter_init_append(reply, &rit);
                        dbus_message_iter_open_container(&rit, DBUS_TYPE_ARRAY, "{sv}", &dict);
                        detail::append_variant_props(&dict, single);
                        dbus_message_iter_close_container(&rit, &dict);
                        dbus_connection_send(conn, reply, nullptr);
                        dbus_message_unref(reply);
                        return DBUS_HANDLER_RESULT_HANDLED;
                    }
                }
                detail::reply_error(conn, msg, DBUS_ERROR_UNKNOWN_PROPERTY, "no such property");
                return DBUS_HANDLER_RESULT_HANDLED;
            }
            if (mem == "Set") {
                detail::reply_empty(conn, msg);
                return DBUS_HANDLER_RESULT_HANDLED;
            }
        }

        if (ctx->kind == ObjKind::Characteristic && ifc == IFACE_GATT_CHAR) {
            Characteristic* chr = ctx->chr;
            if (mem == "ReadValue") {
                DBusMessageIter it;
                uint16_t offset = 0;
                if (dbus_message_iter_init(msg, &it)) offset = detail::read_offset_option(&it);
                std::vector<uint8_t> value = chr->on_read ? chr->on_read() : std::vector<uint8_t>{};
                if (offset > value.size()) offset = static_cast<uint16_t>(value.size());
                std::vector<uint8_t> slice(value.begin() + offset, value.end());
                detail::reply_bytes(conn, msg, slice);
                return DBUS_HANDLER_RESULT_HANDLED;
            }
            if (mem == "WriteValue") {
                DBusMessageIter it;
                dbus_message_iter_init(msg, &it);
                std::vector<uint8_t> value = detail::read_byte_array(&it);
                if (chr->on_write) chr->on_write(value);
                detail::reply_empty(conn, msg);
                return DBUS_HANDLER_RESULT_HANDLED;
            }
            if (mem == "StartNotify") {
                chr->notifying = true;
                detail::reply_empty(conn, msg);
                return DBUS_HANDLER_RESULT_HANDLED;
            }
            if (mem == "StopNotify") {
                chr->notifying = false;
                detail::reply_empty(conn, msg);
                return DBUS_HANDLER_RESULT_HANDLED;
            }
        }

        if (ctx->kind == ObjKind::Advertisement && ifc == IFACE_LE_ADV && mem == "Release") {
            detail::reply_empty(conn, msg);
            return DBUS_HANDLER_RESULT_HANDLED;
        }

        // Pairing agent: accept everything (NoInputNoOutput / "Just Works").
        if (ctx->kind == ObjKind::Agent && ifc == IFACE_AGENT) {
            if (mem == "RequestPasskey") {
                DBusMessage* reply = dbus_message_new_method_return(msg);
                DBusMessageIter it;
                dbus_message_iter_init_append(reply, &it);
                dbus_uint32_t passkey = 0;
                dbus_message_iter_append_basic(&it, DBUS_TYPE_UINT32, &passkey);
                dbus_connection_send(conn, reply, nullptr);
                dbus_message_unref(reply);
            } else {
                // Release, Cancel, RequestConfirmation, RequestAuthorization,
                // AuthorizeService, DisplayPasskey, DisplayPinCode: all
                // accepted/acknowledged with an empty reply.
                detail::reply_empty(conn, msg);
            }
            return DBUS_HANDLER_RESULT_HANDLED;
        }

        detail::reply_error(conn, msg, DBUS_ERROR_UNKNOWN_METHOD, ifc + "." + mem + " not implemented");
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    static DBusHandlerResult on_message_trampoline(DBusConnection* conn, DBusMessage* msg, void* user_data) {
        auto* ctx = static_cast<ObjCtx*>(user_data);
        return ctx->self->on_message(conn, msg, ctx);
    }

    DBusConnection* conn_;
    std::string adapter_path_;
    std::string app_root_;
    std::string service_path_;
    std::string service_uuid_;
    std::string device_name_;
    std::string adv_path_;
    std::string agent_path_;
    std::mutex send_mu_;

    std::vector<std::unique_ptr<Characteristic>> chars_;
    std::unique_ptr<ObjCtx> ctx_root_;
    std::unique_ptr<ObjCtx> ctx_service_;
    std::vector<std::unique_ptr<ObjCtx>> char_ctxs_;
    std::unique_ptr<ObjCtx> ctx_adv_;
    std::unique_ptr<ObjCtx> ctx_agent_;
};

} // namespace gattsrv
