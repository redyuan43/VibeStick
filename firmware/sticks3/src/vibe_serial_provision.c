#include "vibe_serial_provision.h"

#include <string.h>

#include "cJSON.h"
#include "driver/usb_serial_jtag.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define PROVISION_LINE_MAX 1536
#define PROVISION_RESPONSE_MAX 256
#define PROVISION_READ_TIMEOUT_MS 50
#define PROVISION_CONNECT_POLL_MS 250

static const char *TAG = "vibe_prov";

typedef struct {
    vibe_wifi_runtime_t *wifi;
    vibe_bridge_registry_t *registry;
} provision_context_t;

static void provision_write_line(const char *line)
{
    if (!line) {
        return;
    }
    size_t len = strlen(line);
    if (len > PROVISION_RESPONSE_MAX - 1) {
        len = PROVISION_RESPONSE_MAX - 1;
    }
    // Blocking write; the USB CDC drains quickly once a host reads it.
    (void)usb_serial_jtag_write_bytes(line, len, pdMS_TO_TICKS(2000));
    (void)usb_serial_jtag_write_bytes("\n", 1, pdMS_TO_TICKS(1000));
}

static void append_escaped(char *out, size_t out_size, size_t *offset,
                           const char *value)
{
    if (!out || !offset || !value) {
        return;
    }
    size_t pos = *offset;
    for (const char *cursor = value; *cursor != '\0' && pos + 6 < out_size;
         ++cursor) {
        unsigned char ch = (unsigned char)*cursor;
        if (ch == '"' || ch == '\\') {
            out[pos++] = '\\';
            out[pos++] = (char)ch;
        } else if (ch < 0x20) {
            int written =
                snprintf(out + pos, out_size - pos, "\\u%04x", (unsigned)ch);
            if (written <= 0) {
                break;
            }
            pos += (size_t)written;
        } else {
            out[pos++] = (char)ch;
        }
    }
    *offset = pos;
}

static void respond_error(const char *error)
{
    char response[PROVISION_RESPONSE_MAX];
    size_t offset = 0;
    int written =
        snprintf(response, sizeof(response), "VSPROV_ERR {\"error\":\"");
    if (written < 0) {
        return;
    }
    offset = (size_t)written;
    append_escaped(response, sizeof(response), &offset, error);
    if (offset < sizeof(response)) {
        response[offset++] = '"';
    }
    if (offset < sizeof(response)) {
        response[offset++] = '}';
    }
    if (offset < sizeof(response)) {
        response[offset] = '\0';
    }
    provision_write_line(response);
}

static bool bridge_profile_from_json(const cJSON *bridge,
                                     bridge_discovered_profile_t *profile)
{
    memset(profile, 0, sizeof(*profile));
    if (!bridge || !cJSON_IsObject(bridge)) {
        return false;
    }
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(bridge, "id");
    const cJSON *label = cJSON_GetObjectItemCaseSensitive(bridge, "label");
    const cJSON *host = cJSON_GetObjectItemCaseSensitive(bridge, "host");
    const cJSON *port = cJSON_GetObjectItemCaseSensitive(bridge, "port");
    const cJSON *token = cJSON_GetObjectItemCaseSensitive(bridge, "token");
    if (!cJSON_IsString(id) || id->valuestring[0] == '\0' ||
        !cJSON_IsString(host) || host->valuestring[0] == '\0') {
        return false;
    }
    snprintf(profile->id, sizeof(profile->id), "%s", id->valuestring);
    if (cJSON_IsString(label) && label->valuestring[0] != '\0') {
        snprintf(profile->label, sizeof(profile->label), "%s",
                 label->valuestring);
    } else {
        snprintf(profile->label, sizeof(profile->label), "%s",
                 id->valuestring);
    }
    snprintf(profile->host, sizeof(profile->host), "%s", host->valuestring);
    profile->port = cJSON_IsNumber(port) ? (int32_t)port->valueint : 8765;
    if (cJSON_IsString(token)) {
        snprintf(profile->token, sizeof(profile->token), "%s",
                 token->valuestring);
    }
    return true;
}

static size_t clamp_offset(size_t offset, size_t out_size)
{
    return offset < out_size ? offset : (out_size > 0 ? out_size - 1 : 0);
}

static void respond_status(const provision_context_t *context)
{
    char response[PROVISION_RESPONSE_MAX];
    char ssid[VIBE_WIFI_PROFILE_SSID_LEN] = {0};
    char ip[16] = {0};
    vibe_bridge_target_t target = {0};
    bool connected = vibe_wifi_runtime_connected(context->wifi);
    vibe_wifi_runtime_ssid(context->wifi, ssid, sizeof(ssid));
    vibe_wifi_runtime_ip(context->wifi, ip, sizeof(ip));
    vibe_bridge_registry_target(context->registry, &target);

    char port_text[16];
    snprintf(port_text, sizeof(port_text), "%d", (int)target.port);

    size_t offset = 0;
    offset += (size_t)snprintf(response + offset, sizeof(response) - offset,
                               "VSOK {\"connected\":%s,\"ssid\":\"",
                               connected ? "true" : "false");
    offset = clamp_offset(offset, sizeof(response));
    append_escaped(response, sizeof(response), &offset, ssid);
    offset = clamp_offset(offset, sizeof(response));
    offset += (size_t)snprintf(response + offset, sizeof(response) - offset,
                               "\",\"ip\":\"");
    offset = clamp_offset(offset, sizeof(response));
    append_escaped(response, sizeof(response), &offset, ip);
    offset = clamp_offset(offset, sizeof(response));
    offset += (size_t)snprintf(response + offset, sizeof(response) - offset,
                               "\",\"bridge_host\":\"");
    offset = clamp_offset(offset, sizeof(response));
    append_escaped(response, sizeof(response), &offset, target.host);
    offset = clamp_offset(offset, sizeof(response));
    offset += (size_t)snprintf(response + offset, sizeof(response) - offset,
                               "\",\"bridge_port\":%s}", port_text);
    offset = clamp_offset(offset, sizeof(response));
    response[offset] = '\0';
    provision_write_line(response);
}

static void handle_provision(const provision_context_t *context,
                             const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        respond_error("bad_json");
        return;
    }
    const cJSON *ssid_item = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    const cJSON *password_item =
        cJSON_GetObjectItemCaseSensitive(root, "password");
    const cJSON *bridge_item =
        cJSON_GetObjectItemCaseSensitive(root, "bridge");
    const cJSON *apply_item = cJSON_GetObjectItemCaseSensitive(root, "apply");

    if (!cJSON_IsString(ssid_item) || ssid_item->valuestring[0] == '\0') {
        cJSON_Delete(root);
        respond_error("missing_ssid");
        return;
    }
    const char *ssid = ssid_item->valuestring;
    const char *password =
        cJSON_IsString(password_item) ? password_item->valuestring : "";

    vibe_wifi_profile_t profile = {0};
    snprintf(profile.ssid, sizeof(profile.ssid), "%s", ssid);
    snprintf(profile.password, sizeof(profile.password), "%s", password);

    size_t profile_index = 0;
    esp_err_t err =
        vibe_wifi_runtime_store_profile(context->wifi, &profile, &profile_index);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "store Wi-Fi profile failed: %s", esp_err_to_name(err));
        cJSON_Delete(root);
        respond_error("store_failed");
        return;
    }
    ESP_LOGI(TAG, "stored Wi-Fi profile ssid=%s index=%u", ssid,
             (unsigned)profile_index);

    bool bridge_written = false;
    bridge_discovered_profile_t bridge_profile;
    if (bridge_profile_from_json(bridge_item, &bridge_profile)) {
        err = vibe_bridge_registry_upsert_manual(
            context->registry, ssid, &bridge_profile, "serial");
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "bridge upsert failed: %s", esp_err_to_name(err));
        } else {
            bridge_written = true;
            ESP_LOGI(TAG, "bridge profile upserted host=%s port=%d",
                     bridge_profile.host, (int)bridge_profile.port);
        }
    }

    bool apply = !cJSON_IsBool(apply_item) || cJSON_IsTrue(apply_item);
    bool connected = vibe_wifi_runtime_connected(context->wifi);
    if (apply) {
        err = vibe_wifi_runtime_connect_profile(context->wifi, profile_index);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "connect profile failed: %s", esp_err_to_name(err));
            cJSON_Delete(root);
            respond_error("connect_failed");
            return;
        }
        int waited_ms = 0;
        while (waited_ms < VIBE_SERIAL_PROVISION_CONNECT_WAIT_MS) {
            if (vibe_wifi_runtime_connected(context->wifi)) {
                connected = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(PROVISION_CONNECT_POLL_MS));
            waited_ms += PROVISION_CONNECT_POLL_MS;
        }
    }

    char response[PROVISION_RESPONSE_MAX];
    char ip[16] = {0};
    vibe_wifi_runtime_ip(context->wifi, ip, sizeof(ip));
    size_t offset = 0;
    offset += (size_t)snprintf(
        response + offset, sizeof(response) - offset,
        "VSPROV_OK {\"connected\":%s,\"ip\":\"",
        vibe_wifi_runtime_connected(context->wifi) ? "true" : "false");
    offset = clamp_offset(offset, sizeof(response));
    append_escaped(response, sizeof(response), &offset, ip);
    offset = clamp_offset(offset, sizeof(response));
    offset += (size_t)snprintf(
        response + offset, sizeof(response) - offset,
        "\",\"profiles_stored\":%u,\"bridge_written\":%s,\"ssid\":\"",
        (unsigned)context->wifi->profile_count,
        bridge_written ? "true" : "false");
    offset = clamp_offset(offset, sizeof(response));
    append_escaped(response, sizeof(response), &offset, ssid);
    offset = clamp_offset(offset, sizeof(response));
    offset += (size_t)snprintf(response + offset, sizeof(response) - offset,
                               "\",\"was_connected_before\":%s}",
                               connected ? "true" : "false");
    offset = clamp_offset(offset, sizeof(response));
    response[offset] = '\0';
    provision_write_line(response);
    cJSON_Delete(root);
}

static void handle_line(provision_context_t *context, const char *line)
{
    if (strncmp(line, "VSPROV ", 7) == 0) {
        handle_provision(context, line + 7);
        return;
    }
    if (strcmp(line, "VSGET") == 0) {
        respond_status(context);
        return;
    }
    // Unknown input (debug noise, shell echo) is silently ignored so the
    // protocol stays usable even when log output lands on the same CDC.
}

static void provision_task(void *arg)
{
    provision_context_t *context = (provision_context_t *)arg;

    usb_serial_jtag_driver_config_t usb_config = {
        .rx_buffer_size = 512,
        .tx_buffer_size = 256,
    };
    esp_err_t usb_err = usb_serial_jtag_driver_install(&usb_config);
    if (usb_err != ESP_OK && usb_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "USB-JTAG driver install failed: %s",
                 esp_err_to_name(usb_err));
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "serial provision listener ready");

    static char line[PROVISION_LINE_MAX];
    size_t line_len = 0;
    while (true) {
        uint8_t chunk[128];
        int received = usb_serial_jtag_read_bytes(
            chunk, sizeof(chunk), pdMS_TO_TICKS(PROVISION_READ_TIMEOUT_MS));
        for (int index = 0; index < received; ++index) {
            char ch = (char)chunk[index];
            if (ch == '\r') {
                continue;
            }
            if (ch == '\n') {
                line[line_len] = '\0';
                if (line_len > 0) {
                    handle_line(context, line);
                }
                line_len = 0;
                continue;
            }
            if (line_len < PROVISION_LINE_MAX - 1) {
                line[line_len++] = ch;
            } else {
                // Overflowed without a newline; drop the garbage line.
                line_len = 0;
            }
        }
        if (received < 0) {
            vTaskDelay(pdMS_TO_TICKS(PROVISION_READ_TIMEOUT_MS));
        }
    }
}

esp_err_t vibe_serial_provision_start(
    const vibe_serial_provision_config_t *config)
{
    if (!config || !config->wifi || !config->registry) {
        return ESP_ERR_INVALID_ARG;
    }
    static provision_context_t context;
    context.wifi = config->wifi;
    context.registry = config->registry;

    int stack = config->task_stack_bytes > 0
                    ? config->task_stack_bytes
                    : VIBE_SERIAL_PROVISION_DEFAULT_STACK_BYTES;
    int priority = config->task_priority > 0
                       ? config->task_priority
                       : VIBE_SERIAL_PROVISION_DEFAULT_PRIORITY;
    BaseType_t ok = xTaskCreate(provision_task, "serial_prov", stack, &context,
                                priority, NULL);
    return ok == pdPASS ? ESP_OK : ESP_FAIL;
}
