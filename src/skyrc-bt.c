#include <skyrc-bt.h>

#include <gio/gio.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SKYRC_BT_UPGRADE_CHUNK_SIZE 20

struct skyrc_bt_protocol_config {
    uint8_t header;
    size_t header_length;
    size_t command_offset;
    size_t length_offset;
    int16_t length_adjust;
    size_t tail_length;
    const uint8_t *ignored_commands;
    size_t ignored_command_count;
    size_t maximum_frame_length;
    uint8_t *owned_ignored_commands;
};

struct skyrc_bt_scan_result {
    const char *address;
    const char *name;
    int16_t rssi;
    bool has_rssi;
};

struct skyrc_bt_decoder {
    struct skyrc_bt_protocol_config config;
    uint8_t *ignored_commands;
    uint8_t *frame;
    size_t frame_length;
    size_t expected_length;
};

const char *skyrc_bt_scan_result_get_address(const skyrc_bt_scan_result *result)
{
    return result ? result->address : NULL;
}

const char *skyrc_bt_scan_result_get_name(const skyrc_bt_scan_result *result)
{
    return result ? result->name : NULL;
}

bool skyrc_bt_scan_result_get_rssi(const skyrc_bt_scan_result *result,
                                   int16_t *rssi)
{
    if (!result || !result->has_rssi || !rssi) return false;
    *rssi = result->rssi;
    return true;
}

struct skyrc_bt_device {
    GDBusConnection *bus;
    GMainContext *main_context;
    char *adapter_path;
    char *device_path;
    char *notify_path;
    guint notify_signal_id;
    skyrc_bt_notify_callback notify_callback;
    void *notify_context;
};

static enum skyrc_bt_error skyrc_bt_dbus_error(GError *error)
{
    enum skyrc_bt_error result = SKYRC_BT_DBUS_ERROR;
    if (error && (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT) ||
                  g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_TIMEOUT)))
        result = SKYRC_BT_TIMEOUT;
    if (error) g_error_free(error);
    return result;
}

static GDBusConnection *skyrc_bt_get_system_bus(enum skyrc_bt_error *status)
{
    GError *error = NULL;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    if (!bus && status) *status = skyrc_bt_dbus_error(error);
    return bus;
}

static GVariant *skyrc_bt_get_managed_objects(GDBusConnection *bus,
                                               enum skyrc_bt_error *status)
{
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_sync(bus, "org.bluez", "/",
        "org.freedesktop.DBus.ObjectManager", "GetManagedObjects", NULL,
        G_VARIANT_TYPE("(a{oa{sa{sv}}})"), G_DBUS_CALL_FLAGS_NONE, 5000,
        NULL, &error);
    if (!reply) {
        if (status) *status = skyrc_bt_dbus_error(error);
        return NULL;
    }
    GVariant *objects = NULL;
    g_variant_get(reply, "(@a{oa{sa{sv}}})", &objects);
    g_variant_unref(reply);
    return objects;
}

static bool skyrc_bt_find_adapter(GVariant *objects, const char *adapter,
                                  char **adapter_path)
{
    GVariantIter iter;
    const char *path;
    GVariant *interfaces;
    bool found = false;
    *adapter_path = NULL;
    g_variant_iter_init(&iter, objects);
    while (g_variant_iter_next(&iter, "{&o@a{sa{sv}}}", &path, &interfaces)) {
        GVariant *properties = g_variant_lookup_value(interfaces,
            "org.bluez.Adapter1", G_VARIANT_TYPE("a{sv}"));
        if (properties && (!adapter ||
            (g_str_has_prefix(path, "/org/bluez/") &&
             strcmp(path + strlen("/org/bluez/"), adapter) == 0))) {
            gboolean powered = FALSE;
            if (g_variant_lookup(properties, "Powered", "b", &powered) && powered) {
                *adapter_path = g_strdup(path);
                found = true;
            }
        }
        if (properties) g_variant_unref(properties);
        g_variant_unref(interfaces);
        if (found) break;
    }
    return found;
}

static void skyrc_bt_report_scan_device(const char *path, GVariant *properties,
                                        skyrc_bt_scan_callback callback,
                                        void *context)
{
    const char *address = NULL, *name = NULL;
    int16_t rssi = 0;
    gboolean has_rssi;
    struct skyrc_bt_scan_result result;
    if (!g_str_has_prefix(path, "/org/bluez/") ||
        !g_strrstr(path, "/dev_") ||
        !g_variant_lookup(properties, "Address", "&s", &address))
        return;
    if (!g_variant_lookup(properties, "Name", "&s", &name))
        g_variant_lookup(properties, "Alias", "&s", &name);
    has_rssi = g_variant_lookup(properties, "RSSI", "n", &rssi);
    result.address = address;
    result.name = name ? name : "";
    result.rssi = rssi;
    result.has_rssi = has_rssi;
    callback(context, &result);
}

struct skyrc_bt_scan_context {
    skyrc_bt_scan_callback callback;
    void *user_context;
    const char *adapter_path;
};

static void skyrc_bt_scan_added_signal(GDBusConnection *bus, const char *sender,
    const char *object_path, const char *interface_name, const char *signal_name,
    GVariant *parameters, void *user_data)
{
    struct skyrc_bt_scan_context *context = user_data;
    (void)bus;
    (void)sender;
    (void)object_path;
    (void)interface_name;
    (void)signal_name;
    const char *device_path = NULL;
    g_variant_get_child(parameters, 0, "&o", &device_path);
    GVariant *interfaces = g_variant_get_child_value(parameters, 1);
    GVariant *properties = g_variant_lookup_value(interfaces, "org.bluez.Device1",
                                                   G_VARIANT_TYPE("a{sv}"));
    if (properties && device_path && g_str_has_prefix(device_path, context->adapter_path)) {
        skyrc_bt_report_scan_device(device_path, properties, context->callback,
                                    context->user_context);
    }
    if (properties) g_variant_unref(properties);
    g_variant_unref(interfaces);
}

static void skyrc_bt_scan_changed_signal(GDBusConnection *bus, const char *sender,
    const char *object_path, const char *interface_name, const char *signal_name,
    GVariant *parameters, void *user_data)
{
    struct skyrc_bt_scan_context *context = user_data;
    (void)bus;
    (void)sender;
    (void)interface_name;
    (void)signal_name;
    const char *changed_interface;
    GVariant *changed = NULL, *invalidated = NULL;
    g_variant_get(parameters, "(&s@a{sv}@as)", &changed_interface, &changed,
                  &invalidated);
    if (strcmp(changed_interface, "org.bluez.Device1") == 0 &&
        g_str_has_prefix(object_path, context->adapter_path))
        skyrc_bt_report_scan_device(object_path, changed, context->callback,
                                    context->user_context);
    g_variant_unref(changed);
    g_variant_unref(invalidated);
}

enum skyrc_bt_error skyrc_bt_scan(const char *adapter, int duration_ms,
                                  skyrc_bt_scan_callback callback, void *context)
{
    enum skyrc_bt_error status = SKYRC_BT_OK;
    GDBusConnection *bus = NULL;
    GVariant *objects = NULL, *reply = NULL;
    GError *error = NULL;
    char *adapter_path = NULL;
    guint added_id = 0, changed_id = 0;
    GMainContext *main_context = NULL;
    struct skyrc_bt_scan_context scan_context = {callback, context, NULL};

    if (duration_ms <= 0 || !callback) return SKYRC_BT_INVALID_ARGUMENT;
    bus = skyrc_bt_get_system_bus(&status);
    if (!bus) return status;
    objects = skyrc_bt_get_managed_objects(bus, &status);
    if (!objects) goto cleanup;
    if (!skyrc_bt_find_adapter(objects, adapter, &adapter_path)) {
        status = SKYRC_BT_DEVICE_NOT_FOUND;
        goto cleanup;
    }
    scan_context.adapter_path = adapter_path;
    main_context = g_main_context_new();
    g_main_context_push_thread_default(main_context);
    added_id = g_dbus_connection_signal_subscribe(bus, "org.bluez",
        "org.freedesktop.DBus.ObjectManager", "InterfacesAdded", "/", NULL,
        G_DBUS_SIGNAL_FLAGS_NONE, skyrc_bt_scan_added_signal, &scan_context, NULL);
    changed_id = g_dbus_connection_signal_subscribe(bus, "org.bluez",
        "org.freedesktop.DBus.Properties", "PropertiesChanged", NULL,
        "org.bluez.Device1", G_DBUS_SIGNAL_FLAGS_NONE,
        skyrc_bt_scan_changed_signal, &scan_context, NULL);
    g_main_context_pop_thread_default(main_context);

    /* Report devices BlueZ already knows about before discovery starts. */
    GVariantIter object_iter;
    const char *object_path;
    GVariant *interfaces;
    g_variant_iter_init(&object_iter, objects);
    while (g_variant_iter_next(&object_iter, "{&o@a{sa{sv}}}", &object_path,
                               &interfaces)) {
        if (g_str_has_prefix(object_path, adapter_path)) {
            GVariant *properties = g_variant_lookup_value(interfaces,
                "org.bluez.Device1", G_VARIANT_TYPE("a{sv}"));
            if (properties) {
                skyrc_bt_report_scan_device(object_path, properties, callback, context);
                g_variant_unref(properties);
            }
        }
        g_variant_unref(interfaces);
    }

    reply = g_dbus_connection_call_sync(bus, "org.bluez", adapter_path,
        "org.bluez.Adapter1", "StartDiscovery", NULL, G_VARIANT_TYPE("()"),
        G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error);
    if (!reply) {
        status = skyrc_bt_dbus_error(error);
        error = NULL;
        goto cleanup;
    }
    g_variant_unref(reply);
    reply = NULL;
    gint64 deadline = g_get_monotonic_time() + (gint64)duration_ms * 1000;
    while (g_get_monotonic_time() < deadline)
        if (!g_main_context_iteration(main_context, FALSE))
            g_usleep(1000);

    reply = g_dbus_connection_call_sync(bus, "org.bluez", adapter_path,
        "org.bluez.Adapter1", "StopDiscovery", NULL, G_VARIANT_TYPE("()"),
        G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error);
    if (!reply) {
        status = skyrc_bt_dbus_error(error);
        error = NULL;
    }

cleanup:
    if (reply) g_variant_unref(reply);
    if (added_id) g_dbus_connection_signal_unsubscribe(bus, added_id);
    if (changed_id) g_dbus_connection_signal_unsubscribe(bus, changed_id);
    if (main_context) g_main_context_unref(main_context);
    if (objects) g_variant_unref(objects);
    g_free(adapter_path);
    if (bus) g_object_unref(bus);
    if (error) g_error_free(error);
    return status;
}

static bool skyrc_bt_make_device_path(const char *adapter_path,
                                      const char *address, char **device_path)
{
    unsigned char address_component[18];
    size_t length;
    if (!address) return false;
    length = strlen(address);
    if (length != 17) return false;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)address[i];
        if (i % 3 == 2) {
            if (c != ':' && c != '-') return false;
            address_component[i] = '_';
        } else {
            if (!isxdigit(c)) return false;
            address_component[i] = (unsigned char)g_ascii_toupper((char)c);
        }
    }
    address_component[length] = '\0';
    *device_path = g_strdup_printf("%s/dev_%s", adapter_path, (char *)address_component);
    return *device_path != NULL;
}

static bool skyrc_bt_device_services_resolved(GDBusConnection *bus,
                                               const char *device_path)
{
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_sync(bus, "org.bluez", device_path,
        "org.freedesktop.DBus.Properties", "Get",
        g_variant_new("(ss)", "org.bluez.Device1", "ServicesResolved"),
        G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 1000, NULL, &error);
    if (!reply) {
        if (error) g_error_free(error);
        return false;
    }
    GVariant *boxed = NULL;
    g_variant_get(reply, "(v)", &boxed);
    gboolean resolved = g_variant_get_boolean(boxed);
    g_variant_unref(boxed);
    g_variant_unref(reply);
    return resolved;
}

enum skyrc_bt_error skyrc_bt_connect(const char *adapter, const char *address,
                                     int timeout_ms, skyrc_bt_device **device)
{
    enum skyrc_bt_error status = SKYRC_BT_OK;
    GDBusConnection *bus = NULL;
    GVariant *objects = NULL, *reply = NULL;
    GError *error = NULL;
    char *adapter_path = NULL, *device_path = NULL;
    bool attempted_connect = false;

    if (!address || !device || timeout_ms <= 0)
        return SKYRC_BT_INVALID_ARGUMENT;
    *device = NULL;
    bus = skyrc_bt_get_system_bus(&status);
    if (!bus) return status;
    objects = skyrc_bt_get_managed_objects(bus, &status);
    if (!objects) goto cleanup;
    if (!skyrc_bt_find_adapter(objects, adapter, &adapter_path)) {
        status = SKYRC_BT_DEVICE_NOT_FOUND;
        goto cleanup;
    }
    if (!skyrc_bt_make_device_path(adapter_path, address, &device_path)) {
        status = SKYRC_BT_INVALID_ARGUMENT;
        goto cleanup;
    }
    reply = g_dbus_connection_call_sync(bus, "org.bluez", device_path,
        "org.bluez.Device1", "Connect", NULL, G_VARIANT_TYPE("()"),
        G_DBUS_CALL_FLAGS_NONE, timeout_ms, NULL, &error);
    if (!reply) {
        status = skyrc_bt_dbus_error(error);
        error = NULL;
        goto cleanup;
    }
    g_variant_unref(reply);
    reply = NULL;
    attempted_connect = true;
    gint64 deadline = g_get_monotonic_time() + (gint64)timeout_ms * 1000;
    while (!skyrc_bt_device_services_resolved(bus, device_path) &&
           g_get_monotonic_time() < deadline)
        g_usleep(100000);
    if (!skyrc_bt_device_services_resolved(bus, device_path)) {
        status = SKYRC_BT_TIMEOUT;
        goto cleanup;
    }

    skyrc_bt_device *connected = calloc(1, sizeof(*connected));
    if (!connected) {
        status = SKYRC_BT_OUT_OF_MEMORY;
        goto cleanup;
    }
    connected->bus = bus;
    bus = NULL;
    connected->main_context = g_main_context_new();
    connected->adapter_path = adapter_path;
    adapter_path = NULL;
    connected->device_path = device_path;
    device_path = NULL;
    *device = connected;

cleanup:
    if (attempted_connect && !*device && bus && device_path) {
        GVariant *disconnect_reply = g_dbus_connection_call_sync(bus, "org.bluez",
            device_path, "org.bluez.Device1", "Disconnect", NULL,
            G_VARIANT_TYPE("()"), G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL);
        if (disconnect_reply) g_variant_unref(disconnect_reply);
    }
    if (reply) g_variant_unref(reply);
    if (objects) g_variant_unref(objects);
    g_free(adapter_path);
    g_free(device_path);
    if (bus) g_object_unref(bus);
    if (error) g_error_free(error);
    return status;
}

static bool skyrc_bt_normalize_uuid(const char *input, char uuid[37])
{
    size_t length;
    if (!input) return false;
    length = strlen(input);
    if (length == 4) {
        if (snprintf(uuid, 37, "0000%s-0000-1000-8000-00805f9b34fb", input) != 36)
            return false;
    } else if (length == 8) {
        if (snprintf(uuid, 37, "%s-0000-1000-8000-00805f9b34fb", input) != 36)
            return false;
    } else if (length == 36) {
        memcpy(uuid, input, 36);
        uuid[36] = '\0';
    } else {
        return false;
    }
    for (size_t i = 0; i < 36; ++i)
        uuid[i] = (char)g_ascii_tolower(uuid[i]);
    return true;
}

static char *skyrc_bt_find_characteristic(skyrc_bt_device *device,
    const char *service_uuid, const char *characteristic_uuid,
    enum skyrc_bt_error *status)
{
    char normalized_service[37], normalized_characteristic[37];
    GVariant *objects = NULL;
    GVariantIter iter;
    const char *path;
    GVariant *interfaces;
    char *service_path = NULL, *characteristic_path = NULL;

    if (!device || !device->bus) {
        *status = SKYRC_BT_NOT_CONNECTED;
        return NULL;
    }
    if (!skyrc_bt_normalize_uuid(service_uuid, normalized_service) ||
        !skyrc_bt_normalize_uuid(characteristic_uuid, normalized_characteristic)) {
        *status = SKYRC_BT_INVALID_ARGUMENT;
        return NULL;
    }
    objects = skyrc_bt_get_managed_objects(device->bus, status);
    if (!objects) return NULL;
    g_variant_iter_init(&iter, objects);
    while (g_variant_iter_next(&iter, "{&o@a{sa{sv}}}", &path, &interfaces)) {
        if (g_str_has_prefix(path, device->device_path) && !service_path) {
            GVariant *props = g_variant_lookup_value(interfaces,
                "org.bluez.GattService1", G_VARIANT_TYPE("a{sv}"));
            const char *uuid = NULL, *service_device = NULL;
            if (props && g_variant_lookup(props, "UUID", "&s", &uuid) &&
                g_variant_lookup(props, "Device", "&o", &service_device) &&
                strcmp(service_device, device->device_path) == 0) {
                char normalized[37];
                if (skyrc_bt_normalize_uuid(uuid, normalized) &&
                    strcmp(normalized, normalized_service) == 0)
                    service_path = g_strdup(path);
            }
            if (props) g_variant_unref(props);
        }
        g_variant_unref(interfaces);
        if (service_path) break;
    }
    if (service_path) {
        g_variant_iter_init(&iter, objects);
        while (g_variant_iter_next(&iter, "{&o@a{sa{sv}}}", &path, &interfaces)) {
            if (g_str_has_prefix(path, device->device_path)) {
                GVariant *props = g_variant_lookup_value(interfaces,
                    "org.bluez.GattCharacteristic1", G_VARIANT_TYPE("a{sv}"));
                const char *uuid = NULL, *service = NULL;
                if (props && g_variant_lookup(props, "UUID", "&s", &uuid) &&
                    g_variant_lookup(props, "Service", "&o", &service) &&
                    strcmp(service, service_path) == 0) {
                    char normalized[37];
                    if (skyrc_bt_normalize_uuid(uuid, normalized) &&
                        strcmp(normalized, normalized_characteristic) == 0)
                        characteristic_path = g_strdup(path);
                }
                if (props) g_variant_unref(props);
            }
            g_variant_unref(interfaces);
            if (characteristic_path) break;
        }
    }
    g_variant_unref(objects);
    g_free(service_path);
    if (!characteristic_path) *status = SKYRC_BT_DEVICE_NOT_FOUND;
    return characteristic_path;
}

enum skyrc_bt_error skyrc_bt_gatt_read(skyrc_bt_device *device,
    const char *service_uuid, const char *characteristic_uuid,
    uint8_t *value, size_t capacity, size_t *value_length)
{
    enum skyrc_bt_error status = SKYRC_BT_OK;
    char *path;
    GVariantBuilder options;
    GVariant *reply, *bytes;
    GError *error = NULL;
    gsize length = 0;
    const guint8 *data;
    if (!value_length || (!value && capacity)) return SKYRC_BT_INVALID_ARGUMENT;
    *value_length = 0;
    path = skyrc_bt_find_characteristic(device, service_uuid, characteristic_uuid, &status);
    if (!path) return status;
    g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
    reply = g_dbus_connection_call_sync(device->bus, "org.bluez", path,
        "org.bluez.GattCharacteristic1", "ReadValue",
        g_variant_new("(@a{sv})", g_variant_builder_end(&options)),
        G_VARIANT_TYPE("(ay)"), G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error);
    g_free(path);
    if (!reply) return skyrc_bt_dbus_error(error);
    g_variant_get(reply, "(@ay)", &bytes);
    data = g_variant_get_fixed_array(bytes, &length, sizeof(guint8));
    *value_length = length;
    if (length > capacity) {
        status = SKYRC_BT_BUFFER_TOO_SMALL;
    } else if (length && value) {
        memcpy(value, data, length);
    }
    g_variant_unref(bytes);
    g_variant_unref(reply);
    return status;
}

enum skyrc_bt_error skyrc_bt_gatt_write(skyrc_bt_device *device,
    const char *service_uuid, const char *characteristic_uuid,
    const uint8_t *value, size_t value_length, bool with_response)
{
    enum skyrc_bt_error status = SKYRC_BT_OK;
    char *path;
    GVariantBuilder options;
    GVariant *reply;
    GError *error = NULL;
    if ((!value && value_length) || value_length > UINT16_MAX)
        return SKYRC_BT_INVALID_ARGUMENT;
    path = skyrc_bt_find_characteristic(device, service_uuid, characteristic_uuid, &status);
    if (!path) return status;
    g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&options, "{sv}", "type",
                          g_variant_new_string(with_response ? "request" : "command"));
    GVariant *bytes = g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE,
                                                value, value_length, sizeof(guint8));
    reply = g_dbus_connection_call_sync(device->bus, "org.bluez", path,
        "org.bluez.GattCharacteristic1", "WriteValue",
        g_variant_new("(@aya{sv})", bytes, g_variant_builder_end(&options)),
        G_VARIANT_TYPE("()"), G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error);
    g_free(path);
    if (!reply) return skyrc_bt_dbus_error(error);
    g_variant_unref(reply);
    return SKYRC_BT_OK;
}

static void skyrc_bt_notify_changed(GDBusConnection *bus, const char *sender,
    const char *object_path, const char *interface_name, const char *signal_name,
    GVariant *parameters, void *user_data)
{
    skyrc_bt_device *device = user_data;
    (void)bus;
    (void)sender;
    (void)object_path;
    (void)interface_name;
    (void)signal_name;
    const char *changed_interface;
    GVariant *changed = NULL, *invalidated = NULL, *value = NULL;
    g_variant_get(parameters, "(&s@a{sv}@as)", &changed_interface, &changed,
                  &invalidated);
    if (strcmp(changed_interface, "org.bluez.GattCharacteristic1") == 0 &&
        device->notify_callback) {
        value = g_variant_lookup_value(changed, "Value", G_VARIANT_TYPE("ay"));
        if (value) {
            gsize length = 0;
            const guint8 *bytes = g_variant_get_fixed_array(value, &length,
                                                            sizeof(guint8));
            device->notify_callback(device->notify_context, bytes, length);
            g_variant_unref(value);
        }
    }
    g_variant_unref(changed);
    g_variant_unref(invalidated);
}

enum skyrc_bt_error skyrc_bt_notify_start(skyrc_bt_device *device,
    const char *service_uuid, const char *characteristic_uuid,
    skyrc_bt_notify_callback callback, void *context)
{
    enum skyrc_bt_error status = SKYRC_BT_OK;
    char *path;
    GVariant *reply;
    GError *error = NULL;
    if (!device || !callback) return SKYRC_BT_INVALID_ARGUMENT;
    if (device->notify_signal_id) {
        status = skyrc_bt_notify_stop(device);
        if (status != SKYRC_BT_OK) return status;
    }
    path = skyrc_bt_find_characteristic(device, service_uuid,
                                        characteristic_uuid, &status);
    if (!path) return status;
    device->notify_path = path;
    device->notify_callback = callback;
    device->notify_context = context;
    g_main_context_push_thread_default(device->main_context);
    device->notify_signal_id = g_dbus_connection_signal_subscribe(device->bus,
        "org.bluez", "org.freedesktop.DBus.Properties", "PropertiesChanged",
        path, "org.bluez.GattCharacteristic1", G_DBUS_SIGNAL_FLAGS_NONE,
        skyrc_bt_notify_changed, device, NULL);
    g_main_context_pop_thread_default(device->main_context);
    reply = g_dbus_connection_call_sync(device->bus, "org.bluez", path,
        "org.bluez.GattCharacteristic1", "StartNotify", NULL,
        G_VARIANT_TYPE("()"), G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error);
    if (!reply) {
        if (device->notify_signal_id)
            g_dbus_connection_signal_unsubscribe(device->bus, device->notify_signal_id);
        device->notify_signal_id = 0;
        g_clear_pointer(&device->notify_path, g_free);
        device->notify_callback = NULL;
        device->notify_context = NULL;
        return skyrc_bt_dbus_error(error);
    }
    g_variant_unref(reply);
    return SKYRC_BT_OK;
}

enum skyrc_bt_error skyrc_bt_notify_stop(skyrc_bt_device *device)
{
    GVariant *reply;
    GError *error = NULL;
    if (!device) return SKYRC_BT_INVALID_ARGUMENT;
    if (!device->notify_signal_id) return SKYRC_BT_OK;
    reply = g_dbus_connection_call_sync(device->bus, "org.bluez",
        device->notify_path, "org.bluez.GattCharacteristic1", "StopNotify",
        NULL, G_VARIANT_TYPE("()"), G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error);
    g_dbus_connection_signal_unsubscribe(device->bus, device->notify_signal_id);
    device->notify_signal_id = 0;
    g_clear_pointer(&device->notify_path, g_free);
    device->notify_callback = NULL;
    device->notify_context = NULL;
    if (!reply) return skyrc_bt_dbus_error(error);
    g_variant_unref(reply);
    return SKYRC_BT_OK;
}

enum skyrc_bt_error skyrc_bt_poll(skyrc_bt_device *device, int timeout_ms)
{
    if (!device || !device->bus) return SKYRC_BT_NOT_CONNECTED;
    if (timeout_ms < 0) return SKYRC_BT_INVALID_ARGUMENT;
    if (timeout_ms == 0) {
        while (g_main_context_iteration(device->main_context, FALSE)) { }
        return SKYRC_BT_OK;
    }
    gint64 deadline = g_get_monotonic_time() + (gint64)timeout_ms * 1000;
    while (g_get_monotonic_time() < deadline) {
        if (!g_main_context_iteration(device->main_context, FALSE)) {
            if (g_get_monotonic_time() >= deadline) break;
            g_usleep(1000);
        }
    }
    return SKYRC_BT_OK;
}

enum skyrc_bt_error skyrc_bt_disconnect(skyrc_bt_device *device)
{
    enum skyrc_bt_error status = SKYRC_BT_OK;
    GVariant *reply = NULL;
    GError *error = NULL;
    if (!device) return SKYRC_BT_OK;
    if (device->notify_signal_id)
        status = skyrc_bt_notify_stop(device);
    reply = g_dbus_connection_call_sync(device->bus, "org.bluez",
        device->device_path, "org.bluez.Device1", "Disconnect", NULL,
        G_VARIANT_TYPE("()"), G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error);
    if (!reply && status == SKYRC_BT_OK)
        status = skyrc_bt_dbus_error(error);
    else if (reply)
        g_variant_unref(reply);
    else if (error)
        g_error_free(error);
    g_object_unref(device->bus);
    g_main_context_unref(device->main_context);
    g_free(device->adapter_path);
    g_free(device->device_path);
    g_free(device->notify_path);
    free(device);
    return status;
}

static bool skyrc_bt_is_ignored(const skyrc_bt_decoder *decoder, uint8_t command)
{
    for (size_t i = 0; i < decoder->config.ignored_command_count; ++i)
        if (decoder->ignored_commands[i] == command)
            return true;
    return false;
}

skyrc_bt_protocol_config *skyrc_bt_protocol_config_create(
    uint8_t header, size_t header_length, size_t command_offset,
    size_t length_offset, int16_t length_adjust, size_t tail_length,
    const uint8_t *ignored_commands, size_t ignored_command_count,
    size_t maximum_frame_length)
{
    if (ignored_command_count && !ignored_commands) return NULL;
    skyrc_bt_protocol_config *config = calloc(1, sizeof(*config));
    if (!config) return NULL;
    if (ignored_command_count) {
        config->owned_ignored_commands = malloc(ignored_command_count);
        if (!config->owned_ignored_commands) {
            free(config);
            return NULL;
        }
        memcpy(config->owned_ignored_commands, ignored_commands,
               ignored_command_count);
    }
    config->header = header;
    config->header_length = header_length;
    config->command_offset = command_offset;
    config->length_offset = length_offset;
    config->length_adjust = length_adjust;
    config->tail_length = tail_length;
    config->ignored_commands = config->owned_ignored_commands;
    config->ignored_command_count = ignored_command_count;
    config->maximum_frame_length = maximum_frame_length;
    return config;
}

void skyrc_bt_protocol_config_free(skyrc_bt_protocol_config *config)
{
    if (!config) return;
    free(config->owned_ignored_commands);
    free(config);
}

skyrc_bt_decoder *skyrc_bt_decoder_create(const skyrc_bt_protocol_config *config)
{
    skyrc_bt_decoder *decoder;

    if (!config || config->header == 0 || config->header_length == 0 ||
        config->command_offset == 0 || config->length_offset == 0 ||
        config->maximum_frame_length == 0 || config->maximum_frame_length > UINT16_MAX ||
        config->header_length > config->maximum_frame_length ||
        config->command_offset >= config->maximum_frame_length ||
        config->length_offset >= config->maximum_frame_length ||
        config->tail_length >= config->maximum_frame_length ||
        config->header_length + config->tail_length >= config->maximum_frame_length ||
        (config->ignored_command_count && !config->ignored_commands))
        return NULL;

    decoder = calloc(1, sizeof(*decoder));
    if (!decoder) return NULL;
    decoder->frame = malloc(config->maximum_frame_length);
    if (!decoder->frame) {
        free(decoder);
        return NULL;
    }
    if (config->ignored_command_count) {
        decoder->ignored_commands = malloc(config->ignored_command_count);
        if (!decoder->ignored_commands) {
            free(decoder->frame);
            free(decoder);
            return NULL;
        }
        memcpy(decoder->ignored_commands, config->ignored_commands,
               config->ignored_command_count);
    }
    decoder->config = *config;
    decoder->config.ignored_commands = decoder->ignored_commands;
    return decoder;
}

void skyrc_bt_decoder_free(skyrc_bt_decoder *decoder)
{
    if (!decoder) return;
    free(decoder->ignored_commands);
    free(decoder->frame);
    free(decoder);
}

enum skyrc_bt_error skyrc_bt_decoder_reset(skyrc_bt_decoder *decoder)
{
    if (!decoder) return SKYRC_BT_INVALID_ARGUMENT;
    decoder->frame_length = 0;
    decoder->expected_length = 0;
    return SKYRC_BT_OK;
}

enum skyrc_bt_error skyrc_bt_decoder_feed(skyrc_bt_decoder *decoder,
                                           const uint8_t *fragment,
                                           size_t fragment_length,
                                           skyrc_bt_frame_callback callback,
                                           void *context)
{
    if (!decoder || (!fragment && fragment_length) || !callback)
        return SKYRC_BT_INVALID_ARGUMENT;

    for (size_t i = 0; i < fragment_length; ++i) {
        uint8_t byte = fragment[i];
        if (decoder->frame_length == 0) {
            if (byte != decoder->config.header) continue;
            decoder->frame[decoder->frame_length++] = byte;
            continue;
        }

        if (decoder->frame_length >= decoder->config.maximum_frame_length) {
            skyrc_bt_decoder_reset(decoder);
            return SKYRC_BT_FRAME_TOO_LARGE;
        }
        decoder->frame[decoder->frame_length++] = byte;

        if (decoder->frame_length == decoder->config.length_offset + 1) {
            int expected = (int)byte + decoder->config.length_adjust;
            if (expected <= 0 || (size_t)expected > decoder->config.maximum_frame_length ||
                (size_t)expected < decoder->config.header_length + decoder->config.tail_length ||
                decoder->config.command_offset >= (size_t)expected - decoder->config.tail_length) {
                skyrc_bt_decoder_reset(decoder);
                return SKYRC_BT_INVALID_FRAME;
            }
            decoder->expected_length = (size_t)expected;
            if (decoder->frame_length > decoder->expected_length) {
                skyrc_bt_decoder_reset(decoder);
                return SKYRC_BT_INVALID_FRAME;
            }
        }

        if (decoder->expected_length && decoder->frame_length == decoder->expected_length) {
            size_t payload_length = decoder->expected_length -
                                    decoder->config.header_length - decoder->config.tail_length;
            uint8_t command = decoder->frame[decoder->config.command_offset];
            if (!skyrc_bt_is_ignored(decoder, command))
                callback(context, command,
                         decoder->frame + decoder->config.header_length,
                         payload_length);
            decoder->frame_length = 0;
            decoder->expected_length = 0;
        }
    }
    return SKYRC_BT_OK;
}

size_t skyrc_bt_upgrade_chunk_count(size_t image_length)
{
    return image_length / SKYRC_BT_UPGRADE_CHUNK_SIZE +
           (image_length % SKYRC_BT_UPGRADE_CHUNK_SIZE != 0);
}

enum skyrc_bt_error skyrc_bt_upgrade_get_chunk(const uint8_t *image,
                                                size_t image_length,
                                                size_t chunk_index,
                                                uint8_t chunk[20],
                                                size_t *chunk_length)
{
    size_t offset, length;
    if (!image || !chunk || !chunk_length)
        return SKYRC_BT_INVALID_ARGUMENT;
    if (chunk_index >= skyrc_bt_upgrade_chunk_count(image_length))
        return SKYRC_BT_INVALID_UPGRADE_CHUNK;
    offset = chunk_index * SKYRC_BT_UPGRADE_CHUNK_SIZE;
    length = image_length - offset;
    if (length > SKYRC_BT_UPGRADE_CHUNK_SIZE)
        length = SKYRC_BT_UPGRADE_CHUNK_SIZE;
    memcpy(chunk, image + offset, length);
    *chunk_length = length;
    return SKYRC_BT_OK;
}
