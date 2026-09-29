#include <skyrc-bt.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *error_name(enum skyrc_bt_error error)
{
    switch (error) {
    case SKYRC_BT_OK: return "success";
    case SKYRC_BT_INVALID_ARGUMENT: return "invalid argument";
    case SKYRC_BT_INVALID_CONFIG: return "invalid protocol configuration";
    case SKYRC_BT_INVALID_FRAME: return "invalid frame";
    case SKYRC_BT_FRAME_TOO_LARGE: return "frame too large";
    case SKYRC_BT_INVALID_UPGRADE_CHUNK: return "invalid firmware chunk";
    case SKYRC_BT_DBUS_ERROR: return "D-Bus/BlueZ error";
    case SKYRC_BT_DEVICE_NOT_FOUND: return "device not found";
    case SKYRC_BT_TIMEOUT: return "operation timed out";
    case SKYRC_BT_NOT_CONNECTED: return "not connected";
    case SKYRC_BT_BUFFER_TOO_SMALL: return "buffer too small";
    case SKYRC_BT_OUT_OF_MEMORY: return "out of memory";
    }
    return "unknown error";
}

static void report_error(const char *command, enum skyrc_bt_error error)
{
    fprintf(stderr, "%s: %s\n", command, error_name(error));
}

static void usage(FILE *stream)
{
    fprintf(stream,
        "Usage: skyrc-bt <command> [arguments]\n\n"
        "Commands:\n"
        "  scan [--adapter hci0] [--duration MS]                 Discover BLE devices\n"
        "  connect <address> [--adapter hci0] [--timeout MS]     Connect and disconnect\n"
        "  read <address> <service-uuid> <characteristic-uuid> [options]\n"
        "  write <address> <service-uuid> <characteristic-uuid> <hex-bytes> [options]\n"
        "  notify <address> <service-uuid> <characteristic-uuid> [options]\n"
        "  decode <header-hex> <header-len> <command-offset> <length-offset> <adjust> <tail-len> <max-frame> <hex-bytes>\n"
        "  chunk-count <image-file>                              Count 20-byte firmware chunks\n"
        "  chunk <image-file> <index>                            Print one 20-byte firmware chunk\n"
        "  help                                                  Show this help\n\n"
        "Connection options: --adapter NAME (default: any powered adapter), --timeout MS (default: 10000)\n"
        "Hex bytes accept pairs separated by ':' or spaces, or contiguous pairs (example: 01:ab:ff).\n");
}

static const char *option_value(int argc, char **argv, const char *name)
{
    for (int i = 0; i + 1 < argc; ++i)
        if (strcmp(argv[i], name) == 0) return argv[i + 1];
    return NULL;
}

static bool parse_size(const char *s, size_t *value)
{
    char *end = NULL;
    if (!s || *s == '-') return false;
    errno = 0;
    unsigned long long n = strtoull(s, &end, 10);
    if (errno || end == s || *end || n > (unsigned long long)SIZE_MAX) return false;
    *value = (size_t)n;
    return true;
}

static bool parse_int(const char *s, int *value)
{
    size_t n;
    if (!parse_size(s, &n) || n > (size_t)2147483647) return false;
    *value = (int)n;
    return true;
}

static int parse_hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool parse_hex(const char *text, uint8_t **bytes, size_t *length)
{
    size_t count = 0;
    for (const char *p = text; *p; ++p)
        if (*p != ':' && *p != ' ' && *p != '-' && *p != ',') ++count;
    if (count == 0 || count % 2 != 0) return false;
    uint8_t *result = malloc(count / 2);
    if (!result) return false;
    int high = -1;
    size_t out = 0;
    for (const char *p = text; *p; ++p) {
        if (*p == ':' || *p == ' ' || *p == '-' || *p == ',') continue;
        int nibble = parse_hex_nibble(*p);
        if (nibble < 0) { free(result); return false; }
        if (high < 0) high = nibble;
        else { result[out++] = (uint8_t)((high << 4) | nibble); high = -1; }
    }
    *bytes = result;
    *length = out;
    return true;
}

static void print_hex(const uint8_t *bytes, size_t length)
{
    for (size_t i = 0; i < length; ++i)
        printf("%s%02x", i ? ":" : "", bytes[i]);
    putchar('\n');
}

static void scan_result(void *context, const skyrc_bt_scan_result *result)
{
    (void)context;
    int16_t rssi = 0;
    printf("%s\t%s\t", skyrc_bt_scan_result_get_address(result),
           skyrc_bt_scan_result_get_name(result) ? skyrc_bt_scan_result_get_name(result) : "(unnamed)");
    if (skyrc_bt_scan_result_get_rssi(result, &rssi)) printf("%d dBm", (int)rssi);
    else printf("(RSSI unavailable)");
    putchar('\n');
}

static int command_scan(int argc, char **argv)
{
    const char *adapter = option_value(argc, argv, "--adapter");
    const char *duration_text = option_value(argc, argv, "--duration");
    int duration = 10000;
    if (duration_text && (!parse_int(duration_text, &duration) || duration <= 0)) {
        fprintf(stderr, "scan: --duration must be a positive number of milliseconds\n"); return 2;
    }
    enum skyrc_bt_error error = skyrc_bt_scan(adapter, duration, scan_result, NULL);
    if (error != SKYRC_BT_OK) { report_error("scan", error); return 1; }
    return 0;
}

static enum skyrc_bt_error connect_device(int argc, char **argv, const char *address,
                                           skyrc_bt_device **device)
{
    const char *adapter = option_value(argc, argv, "--adapter");
    const char *timeout_text = option_value(argc, argv, "--timeout");
    int timeout = 10000;
    if (timeout_text && (!parse_int(timeout_text, &timeout) || timeout <= 0))
        return SKYRC_BT_INVALID_ARGUMENT;
    return skyrc_bt_connect(adapter, address, timeout, device);
}

static int command_connect(int argc, char **argv)
{
    if (argc < 1) { fprintf(stderr, "connect: <address> required\n"); return 2; }
    skyrc_bt_device *device = NULL;
    enum skyrc_bt_error error = connect_device(argc - 1, argv + 1, argv[0], &device);
    if (error != SKYRC_BT_OK) { report_error("connect", error); return 1; }
    printf("Connected to %s\n", argv[0]);
    error = skyrc_bt_disconnect(device);
    if (error != SKYRC_BT_OK) { report_error("disconnect", error); return 1; }
    return 0;
}

static int command_gatt(int argc, char **argv, bool write)
{
    if (argc < (write ? 4 : 3)) { fprintf(stderr, "%s: insufficient arguments\n", write ? "write" : "read"); return 2; }
    uint8_t *input = NULL;
    size_t input_length = 0;
    if (write && !parse_hex(argv[3], &input, &input_length)) {
        fprintf(stderr, "write: invalid hex byte string\n"); return 2;
    }
    int option_start = write ? 4 : 3;
    skyrc_bt_device *device = NULL;
    enum skyrc_bt_error error = connect_device(argc - option_start, argv + option_start, argv[0], &device);
    if (error != SKYRC_BT_OK) { free(input); report_error(write ? "write" : "read", error); return 1; }
    if (write) {
        bool with_response = option_value(argc - option_start, argv + option_start, "--command") == NULL;
        error = skyrc_bt_gatt_write(device, argv[1], argv[2], input, input_length, with_response);
        if (error == SKYRC_BT_OK) { printf("Wrote "); print_hex(input, input_length); }
    } else {
        uint8_t value[512];
        size_t value_length = 0;
        error = skyrc_bt_gatt_read(device, argv[1], argv[2], value, sizeof(value), &value_length);
        if (error == SKYRC_BT_OK) print_hex(value, value_length);
        else if (error == SKYRC_BT_BUFFER_TOO_SMALL)
            fprintf(stderr, "read: characteristic value is %zu bytes; CLI buffer holds %zu\n", value_length, sizeof(value));
    }
    enum skyrc_bt_error disconnect_error = skyrc_bt_disconnect(device);
    free(input);
    if (error != SKYRC_BT_OK) { report_error(write ? "write" : "read", error); return 1; }
    if (disconnect_error != SKYRC_BT_OK) { report_error("disconnect", disconnect_error); return 1; }
    return 0;
}

static void notification(void *context, const uint8_t *value, size_t length)
{
    (void)context;
    printf("notification[%zu]: ", length);
    print_hex(value, length);
    fflush(stdout);
}

static int command_notify(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "notify: <address> <service-uuid> <characteristic-uuid> required\n"); return 2; }
    int duration = 30000;
    const char *duration_text = option_value(argc - 3, argv + 3, "--duration");
    if (duration_text && (!parse_int(duration_text, &duration) || duration < 0)) {
        fprintf(stderr, "notify: --duration must be nonnegative milliseconds\n"); return 2;
    }
    skyrc_bt_device *device = NULL;
    enum skyrc_bt_error error = connect_device(argc - 3, argv + 3, argv[0], &device);
    if (error != SKYRC_BT_OK) { report_error("notify", error); return 1; }
    error = skyrc_bt_notify_start(device, argv[1], argv[2], notification, NULL);
    if (error == SKYRC_BT_OK) {
        const int step = 250;
        for (int elapsed = 0; duration == 0 || elapsed < duration; elapsed += step) {
            int wait = duration == 0 || duration - elapsed > step ? step : duration - elapsed;
            error = skyrc_bt_poll(device, wait);
            if (error != SKYRC_BT_OK) break;
        }
        enum skyrc_bt_error stop_error = skyrc_bt_notify_stop(device);
        if (error == SKYRC_BT_OK) error = stop_error;
    }
    enum skyrc_bt_error disconnect_error = skyrc_bt_disconnect(device);
    if (error != SKYRC_BT_OK) { report_error("notify", error); return 1; }
    if (disconnect_error != SKYRC_BT_OK) { report_error("disconnect", disconnect_error); return 1; }
    return 0;
}

typedef struct { size_t command_offset, header_length, tail_length; } decode_context;
static void decoded_frame(void *context, uint8_t command, const uint8_t *payload, size_t length)
{
    (void)context;
    printf("command=0x%02x payload=", command);
    print_hex(payload, length);
}

static int command_decode(int argc, char **argv)
{
    if (argc < 8) { fprintf(stderr, "decode: expected seven framing values and hex bytes\n"); return 2; }
    uint8_t header_bytes[2]; uint8_t *fragment = NULL; size_t fragment_length = 0;
    if (!parse_hex(argv[0], &fragment, &fragment_length) || fragment_length != 1) {
        free(fragment); fprintf(stderr, "decode: header must be one hex byte\n"); return 2;
    }
    header_bytes[0] = fragment[0]; free(fragment);
    size_t header_length, command_offset, length_offset, tail_length, maximum;
    int adjust;
    if (!parse_size(argv[1], &header_length) || !parse_size(argv[2], &command_offset) ||
        !parse_size(argv[3], &length_offset) || !parse_int(argv[4], &adjust) ||
        !parse_size(argv[5], &tail_length) || !parse_size(argv[6], &maximum) ||
        !parse_hex(argv[7], &fragment, &fragment_length)) {
        fprintf(stderr, "decode: invalid framing value or hex bytes\n"); return 2;
    }
    skyrc_bt_protocol_config *config = skyrc_bt_protocol_config_create(header_bytes[0], header_length,
        command_offset, length_offset, (int16_t)adjust, tail_length, NULL, 0, maximum);
    skyrc_bt_decoder *decoder = config ? skyrc_bt_decoder_create(config) : NULL;
    skyrc_bt_protocol_config_free(config);
    if (!decoder) { free(fragment); fprintf(stderr, "decode: invalid configuration\n"); return 2; }
    enum skyrc_bt_error error = skyrc_bt_decoder_feed(decoder, fragment, fragment_length, decoded_frame, NULL);
    free(fragment); skyrc_bt_decoder_free(decoder);
    if (error != SKYRC_BT_OK) { report_error("decode", error); return 1; }
    return 0;
}

static bool read_file(const char *path, uint8_t **data, size_t *length)
{
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return false; }
    long size = ftell(file);
    if (size < 0 || fseek(file, 0, SEEK_SET) != 0) { fclose(file); return false; }
    *length = (size_t)size;
    *data = malloc(*length ? *length : 1);
    if (!*data) { fclose(file); return false; }
    bool ok = fread(*data, 1, *length, file) == *length;
    fclose(file);
    if (!ok) { free(*data); *data = NULL; }
    return ok;
}

int main(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "help") == 0 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        usage(stdout); return argc < 2 ? 2 : 0;
    }
    const char *command = argv[1];
    int n = argc - 2; char **args = argv + 2;
    if (strcmp(command, "scan") == 0) return command_scan(n, args);
    if (strcmp(command, "connect") == 0) return command_connect(n, args);
    if (strcmp(command, "read") == 0) return command_gatt(n, args, false);
    if (strcmp(command, "write") == 0) return command_gatt(n, args, true);
    if (strcmp(command, "notify") == 0) return command_notify(n, args);
    if (strcmp(command, "decode") == 0) return command_decode(n, args);
    if (strcmp(command, "chunk") == 0 || strcmp(command, "chunk-count") == 0) {
        if (n < (strcmp(command, "chunk") == 0 ? 2 : 1)) { fprintf(stderr, "%s: image file required\n", command); return 2; }
        uint8_t *image = NULL; size_t image_length = 0;
        if (!read_file(args[0], &image, &image_length)) { perror(command); return 1; }
        size_t count = skyrc_bt_upgrade_chunk_count(image_length);
        if (strcmp(command, "chunk-count") == 0) printf("%zu\n", count);
        else {
            size_t index;
            if (!parse_size(args[1], &index)) { free(image); fprintf(stderr, "chunk: invalid index\n"); return 2; }
            uint8_t bytes[20]; size_t length = 0;
            enum skyrc_bt_error error = skyrc_bt_upgrade_get_chunk(image, image_length, index, bytes, &length);
            if (error != SKYRC_BT_OK) { free(image); report_error("chunk", error); return 1; }
            print_hex(bytes, length);
        }
        free(image); return 0;
    }
    fprintf(stderr, "unknown command: %s\n", command); usage(stderr); return 2;
}
