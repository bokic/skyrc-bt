#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef libskyrc_bt_EXPORTS
 #if defined(_MSC_VER)
  #define EXPORT_SKYRC_BT __declspec(dllexport)
 #else
  #define EXPORT_SKYRC_BT __attribute__((visibility("default")))
 #endif
#else
 #define EXPORT_SKYRC_BT
#endif

/**
 * Linux BLE access through the BlueZ system D-Bus, plus transport-independent
 * helpers for the SkyRC APK's notification framing and firmware chunking.
 * Service and characteristic UUIDs are supplied by the caller because the APK
 * receives them from its host application at runtime.
 */

/** Errors reported by the Bluetooth protocol helpers. */
enum skyrc_bt_error {
    SKYRC_BT_OK,
    SKYRC_BT_INVALID_ARGUMENT,
    SKYRC_BT_INVALID_CONFIG,
    SKYRC_BT_INVALID_FRAME,
    SKYRC_BT_FRAME_TOO_LARGE,
    SKYRC_BT_INVALID_UPGRADE_CHUNK,
    SKYRC_BT_DBUS_ERROR,
    SKYRC_BT_DEVICE_NOT_FOUND,
    SKYRC_BT_TIMEOUT,
    SKYRC_BT_NOT_CONNECTED,
    SKYRC_BT_BUFFER_TOO_SMALL,
    SKYRC_BT_OUT_OF_MEMORY,
};

/** Opaque notification framing configuration. */
typedef struct skyrc_bt_protocol_config skyrc_bt_protocol_config;

/** Opaque incremental BLE notification decoder. */
typedef struct skyrc_bt_decoder skyrc_bt_decoder;

/** Called for each complete, non-ignored notification frame.
 * @param context Caller-provided callback context.
 * @param command Command byte from the configured frame offset.
 * @param payload Frame bytes between the configured header and tail.
 * @param payload_length Payload length in bytes; payload is valid only during the callback.
 */
typedef void (*skyrc_bt_frame_callback)(void *context, uint8_t command,
                                         const uint8_t *payload, size_t payload_length);

/** Opaque BLE device details valid only while a scan callback is running. */
typedef struct skyrc_bt_scan_result skyrc_bt_scan_result;

/** Called when a BLE device is discovered or its advertisement changes. */
typedef void (*skyrc_bt_scan_callback)(void *context,
                                       const skyrc_bt_scan_result *result);

/** Get the scanned device's Bluetooth address.
 * @param result Scan result supplied to the callback.
 * @return Borrowed colon-separated address, valid only during the callback; NULL for NULL.
 */
EXPORT_SKYRC_BT const char *skyrc_bt_scan_result_get_address(
    const skyrc_bt_scan_result *result);

/** Get the scanned device's advertised name.
 * @param result Scan result supplied to the callback.
 * @return Borrowed name, valid only during the callback; NULL for NULL.
 */
EXPORT_SKYRC_BT const char *skyrc_bt_scan_result_get_name(
    const skyrc_bt_scan_result *result);

/** Get the received signal strength indication.
 * @param result Scan result supplied to the callback.
 * @param rssi Receives RSSI when available.
 * @return true when RSSI is present and copied; false otherwise.
 */
EXPORT_SKYRC_BT bool skyrc_bt_scan_result_get_rssi(
    const skyrc_bt_scan_result *result, int16_t *rssi);

/** Opaque connection to a remote BlueZ BLE device. */
typedef struct skyrc_bt_device skyrc_bt_device;

/** Called with a characteristic notification value. Data is valid only in callback. */
typedef void (*skyrc_bt_notify_callback)(void *context, const uint8_t *value,
                                          size_t value_length);

/** Scan using the Linux BlueZ system D-Bus interface.
 * @param adapter Adapter name such as "hci0", or NULL to select a powered adapter.
 * @param duration_ms Scan duration in milliseconds; must be positive.
 * @param callback Receives matching advertisements during the scan.
 * @param context Caller context passed to callback.
 * @return SKYRC_BT_OK on success, otherwise a skyrc_bt_error value.
 */
EXPORT_SKYRC_BT enum skyrc_bt_error skyrc_bt_scan(const char *adapter,
    int duration_ms, skyrc_bt_scan_callback callback, void *context);

/** Connect to a discovered device over BlueZ GATT.
 * @param adapter Adapter name such as "hci0", or NULL for a powered adapter.
 * @param address Bluetooth address in colon-separated form.
 * @param timeout_ms Connection and service-discovery timeout in milliseconds.
 * @param device Receives the connected device handle.
 * @return SKYRC_BT_OK on success, otherwise a skyrc_bt_error value.
 */
EXPORT_SKYRC_BT enum skyrc_bt_error skyrc_bt_connect(const char *adapter,
    const char *address, int timeout_ms, skyrc_bt_device **device);

/** Disconnect and free a BlueZ device handle.
 * @param device Connected device; NULL is allowed.
 * @return SKYRC_BT_OK on success, otherwise a skyrc_bt_error value.
 */
EXPORT_SKYRC_BT enum skyrc_bt_error skyrc_bt_disconnect(skyrc_bt_device *device);

/** Read a GATT characteristic by service and characteristic UUID.
 * @param device Connected device.
 * @param service_uuid Service UUID, full or short form.
 * @param characteristic_uuid Characteristic UUID, full or short form.
 * @param value Output byte buffer.
 * @param capacity Capacity of value in bytes.
 * @param value_length Receives required/output length.
 * @return SKYRC_BT_OK on success, SKYRC_BT_BUFFER_TOO_SMALL if capacity is insufficient, otherwise a skyrc_bt_error value.
 */
EXPORT_SKYRC_BT enum skyrc_bt_error skyrc_bt_gatt_read(skyrc_bt_device *device,
    const char *service_uuid, const char *characteristic_uuid,
    uint8_t *value, size_t capacity, size_t *value_length);

/** Write a value to a GATT characteristic.
 * @param device Connected device.
 * @param service_uuid Service UUID, full or short form.
 * @param characteristic_uuid Characteristic UUID, full or short form.
 * @param value Bytes to write.
 * @param value_length Number of bytes to write.
 * @param with_response True for a write request; false for a write command.
 * @return SKYRC_BT_OK on success, otherwise a skyrc_bt_error value.
 */
EXPORT_SKYRC_BT enum skyrc_bt_error skyrc_bt_gatt_write(skyrc_bt_device *device,
    const char *service_uuid, const char *characteristic_uuid,
    const uint8_t *value, size_t value_length, bool with_response);

/** Subscribe to GATT characteristic notifications.
 * @param device Connected device.
 * @param service_uuid Service UUID, full or short form.
 * @param characteristic_uuid Notify characteristic UUID, full or short form.
 * @param callback Receives notification values after skyrc_bt_poll() dispatches them.
 * @param context Caller context passed to callback.
 * @return SKYRC_BT_OK on success, otherwise a skyrc_bt_error value.
 */
EXPORT_SKYRC_BT enum skyrc_bt_error skyrc_bt_notify_start(skyrc_bt_device *device,
    const char *service_uuid, const char *characteristic_uuid,
    skyrc_bt_notify_callback callback, void *context);

/** Stop the active notification subscription, if any.
 * @param device Connected device.
 * @return SKYRC_BT_OK on success, otherwise a skyrc_bt_error value.
 */
EXPORT_SKYRC_BT enum skyrc_bt_error skyrc_bt_notify_stop(skyrc_bt_device *device);

/** Dispatch BlueZ D-Bus notification events for a bounded period.
 * @param device Connected device.
 * @param timeout_ms Maximum time to wait in milliseconds; zero only drains pending events.
 * @return SKYRC_BT_OK on success, otherwise a skyrc_bt_error value.
 */
EXPORT_SKYRC_BT enum skyrc_bt_error skyrc_bt_poll(skyrc_bt_device *device,
    int timeout_ms);

/** Create an immutable protocol configuration.
 * The ignored command bytes are copied and may be released by the caller.
 * @return New configuration, or NULL if allocation fails.
 */
EXPORT_SKYRC_BT skyrc_bt_protocol_config *skyrc_bt_protocol_config_create(
    uint8_t header, size_t header_length, size_t command_offset,
    size_t length_offset, int16_t length_adjust, size_t tail_length,
    const uint8_t *ignored_commands, size_t ignored_command_count,
    size_t maximum_frame_length);

/** Destroy a protocol configuration; NULL is allowed. */
EXPORT_SKYRC_BT void skyrc_bt_protocol_config_free(
    skyrc_bt_protocol_config *config);

/** Create a decoder using a copy of the supplied protocol configuration.
 * @param config Valid frame layout; ignored command bytes are copied.
 * @return New decoder, or NULL if the configuration is invalid or allocation fails.
 */
EXPORT_SKYRC_BT skyrc_bt_decoder *skyrc_bt_decoder_create(
    const skyrc_bt_protocol_config *config);

/** Destroy a notification decoder.
 * @param decoder Decoder to free; NULL is allowed.
 */
EXPORT_SKYRC_BT void skyrc_bt_decoder_free(skyrc_bt_decoder *decoder);

/** Discard any partially received frame.
 * @param decoder Decoder to reset.
 * @return SKYRC_BT_OK, or SKYRC_BT_INVALID_ARGUMENT for NULL.
 */
EXPORT_SKYRC_BT enum skyrc_bt_error skyrc_bt_decoder_reset(skyrc_bt_decoder *decoder);

/** Add a BLE notification fragment and dispatch every complete frame it contains.
 * Partial frames are retained for the next call. Bytes preceding a frame header
 * are ignored. A malformed or oversized frame resets the partial frame.
 * @param decoder Decoder created by skyrc_bt_decoder_create().
 * @param fragment Received notification bytes.
 * @param fragment_length Number of bytes in fragment.
 * @param callback Called synchronously for each complete non-ignored frame.
 * @param context Value passed unchanged to callback.
 * @return SKYRC_BT_OK on success, otherwise a skyrc_bt_error value.
 */
EXPORT_SKYRC_BT enum skyrc_bt_error skyrc_bt_decoder_feed(
    skyrc_bt_decoder *decoder, const uint8_t *fragment, size_t fragment_length,
    skyrc_bt_frame_callback callback, void *context);

/** Get the number of 20-byte writes needed for a firmware image.
 * @param image_length Firmware image size in bytes.
 * @return Number of chunks, or zero for an empty image.
 */
EXPORT_SKYRC_BT size_t skyrc_bt_upgrade_chunk_count(size_t image_length);

/** Copy one firmware image chunk using the APK's 20-byte chunk size.
 * @param image Complete firmware image.
 * @param image_length Image size in bytes.
 * @param chunk_index Zero-based chunk index.
 * @param chunk Output buffer of at least 20 bytes.
 * @param chunk_length Receives the actual chunk size (1 to 20 bytes).
 * @return SKYRC_BT_OK on success, otherwise SKYRC_BT_INVALID_ARGUMENT or SKYRC_BT_INVALID_UPGRADE_CHUNK.
 */
EXPORT_SKYRC_BT enum skyrc_bt_error skyrc_bt_upgrade_get_chunk(
    const uint8_t *image, size_t image_length, size_t chunk_index,
    uint8_t chunk[20], size_t *chunk_length);
