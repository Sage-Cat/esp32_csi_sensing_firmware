/*
 * SPDX-FileCopyrightText: 2026 Sage-Cat
 * SPDX-License-Identifier: Apache-2.0
 *
 * Router CSI acquisition follows Espressif's Apache-2.0/CC0
 * examples/get-started/csi_recv_router example. This version adds stable node
 * identity, a persistent boot epoch, timing/health records, AP-roaming updates,
 * and an output lock suitable for long autonomous serial captures.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "driver/temperature_sensor.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#if CONFIG_IDF_TARGET_ESP32C5
#include "esp_csi_gain_ctrl.h"
#endif
#include "lwip/inet.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "ping/ping_sock.h"
#include "protocol_examples_common.h"

#include "cws_control_protocol.h"
#include "cws_legacy_commands.h"

#if CONFIG_IDF_TARGET_ESP32C5
#define CWS_FIRMWARE_PROFILE "cooperative-router-csi-c5-v1"
#define CWS_FIRMWARE_VERSION "1.0.0"
#else
#define CWS_FIRMWARE_PROFILE "cooperative-router-csi-s3-v1"
#define CWS_FIRMWARE_VERSION "1.4.1"
#endif

static const char *TAG = "cws_csi_node";

static SemaphoreHandle_t s_output_lock;
static SemaphoreHandle_t s_ping_lock;
static SemaphoreHandle_t s_csi_control_lock;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_csi_line[4096];
static uint8_t s_ap_bssid[6];
static uint8_t s_station_mac[6];
static uint8_t s_ap_channel;
static bool s_connected;
static uint32_t s_boot_epoch;
static uint64_t s_csi_count;
static uint64_t s_csi_callback_seen_count;
static uint64_t s_csi_mac_mismatch_count;
static uint64_t s_csi_invalid_count;
static uint64_t s_output_drop_count;
static uint32_t s_csi_reinit_count;
static uint32_t s_csi_reinit_failure_count;
static uint32_t s_csi_stall_count;
static bool s_csi_stalled;
static bool s_csi_initialized;
static uint32_t s_ping_restart_count;
static uint32_t s_ping_restart_failure_count;
static uint64_t s_ping_success_count;
static uint64_t s_ping_timeout_count;
static temperature_sensor_handle_t s_temperature_sensor;
static esp_ping_handle_t s_ping_handle;
static uint32_t s_ping_hz;
static uint32_t s_config_epoch;
static cws_control_context_t s_control_protocol;

static esp_err_t csi_reinitialize(const char *reason);
static esp_err_t ping_restart_current(const char *reason);

static void ping_on_success(esp_ping_handle_t handle, void *args)
{
    (void)handle;
    (void)args;
    portENTER_CRITICAL(&s_state_lock);
    s_ping_success_count++;
    portEXIT_CRITICAL(&s_state_lock);
}

static void ping_on_timeout(esp_ping_handle_t handle, void *args)
{
    (void)handle;
    (void)args;
    portENTER_CRITICAL(&s_state_lock);
    s_ping_timeout_count++;
    portEXIT_CRITICAL(&s_state_lock);
}

static bool parse_mac(const char *value, uint8_t mac[6])
{
    unsigned int parsed[6];
    if (sscanf(value, "%x:%x:%x:%x:%x:%x", &parsed[0], &parsed[1],
               &parsed[2], &parsed[3], &parsed[4], &parsed[5]) != 6) {
        return false;
    }
    for (size_t index = 0; index < 6; index++) {
        if (parsed[index] > UINT8_MAX) {
            return false;
        }
        mac[index] = (uint8_t)parsed[index];
    }
    return true;
}

static uint32_t next_boot_epoch(void)
{
    nvs_handle_t handle;
    uint32_t epoch = 0;

    ESP_ERROR_CHECK(nvs_open("cws", NVS_READWRITE, &handle));
    esp_err_t err = nvs_get_u32(handle, "boot_epoch", &epoch);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_ERROR_CHECK(err);
    }
    epoch++;
    ESP_ERROR_CHECK(nvs_set_u32(handle, "boot_epoch", epoch));
    ESP_ERROR_CHECK(nvs_commit(handle));
    nvs_close(handle);
    return epoch;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        esp_err_t err = ping_restart_current("sta_got_ip");
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "probe restart after IP acquisition failed: %s",
                     esp_err_to_name(err));
        }
        return;
    }
    if (event_base != WIFI_EVENT) {
        return;
    }

    if (event_id == WIFI_EVENT_STA_CONNECTED) {
        const wifi_event_sta_connected_t *connected = event_data;
        portENTER_CRITICAL(&s_state_lock);
        memcpy(s_ap_bssid, connected->bssid, sizeof(s_ap_bssid));
        s_ap_channel = connected->channel;
        s_connected = true;
        portEXIT_CRITICAL(&s_state_lock);
        ESP_LOGI(TAG, "associated bssid=" MACSTR " channel=%u",
                 MAC2STR(connected->bssid), connected->channel);
        esp_err_t err = csi_reinitialize("sta_connected");
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "CSI reconnect initialization failed: %s",
                     esp_err_to_name(err));
        }
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        portENTER_CRITICAL(&s_state_lock);
        s_connected = false;
        s_csi_stalled = false;
        portEXIT_CRITICAL(&s_state_lock);
        ESP_LOGW(TAG, "station disconnected; connection helper will retry");
    }
}

static void csi_rx_callback(void *ctx, wifi_csi_info_t *info)
{
    if (info == NULL || info->buf == NULL) {
        portENTER_CRITICAL(&s_state_lock);
        s_csi_invalid_count++;
        portEXIT_CRITICAL(&s_state_lock);
        return;
    }

    portENTER_CRITICAL(&s_state_lock);
    s_csi_callback_seen_count++;
    portEXIT_CRITICAL(&s_state_lock);

    uint8_t expected_bssid[6];
    portENTER_CRITICAL(&s_state_lock);
    memcpy(expected_bssid, s_ap_bssid, sizeof(expected_bssid));
    portEXIT_CRITICAL(&s_state_lock);
    if (memcmp(info->mac, expected_bssid, sizeof(expected_bssid)) != 0) {
        portENTER_CRITICAL(&s_state_lock);
        s_csi_mac_mismatch_count++;
        portEXIT_CRITICAL(&s_state_lock);
        return;
    }

    if (xSemaphoreTake(s_output_lock, 0) != pdTRUE) {
        portENTER_CRITICAL(&s_state_lock);
        s_output_drop_count++;
        portEXIT_CRITICAL(&s_state_lock);
        return;
    }

    const wifi_pkt_rx_ctrl_t *rx = &info->rx_ctrl;
    portENTER_CRITICAL(&s_state_lock);
    uint64_t seq = s_csi_count++;
    portEXIT_CRITICAL(&s_state_lock);

    size_t used = 0;
#if CONFIG_IDF_TARGET_ESP32C5
    uint8_t agc_gain = 0;
    int8_t fft_gain = 0;
    esp_csi_gain_ctrl_get_rx_gain(rx, &agc_gain, &fft_gain);
    int written = snprintf(
        s_csi_line, sizeof(s_csi_line),
        "CSI_DATA,%" PRIu64 "," MACSTR ",%d,%d,%d,%d,%u,%d,%d,%d,%d",
        seq, MAC2STR(info->mac), rx->rssi, rx->rate, rx->noise_floor,
        fft_gain, agc_gain, rx->channel, rx->timestamp, rx->sig_len,
        rx->cur_bb_format);
#else
    int written = snprintf(
        s_csi_line, sizeof(s_csi_line),
        "CSI_DATA,%" PRIu64 "," MACSTR
        ",%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d",
        seq, MAC2STR(info->mac), rx->rssi, rx->rate, rx->sig_mode, rx->mcs,
        rx->cwb, rx->smoothing, rx->not_sounding, rx->aggregation, rx->stbc,
        rx->fec_coding, rx->sgi, rx->noise_floor, rx->ampdu_cnt, rx->channel,
        rx->secondary_channel, rx->timestamp, rx->ant, rx->sig_len,
        rx->rx_state);
#endif
    if (written < 0 || (size_t)written >= sizeof(s_csi_line)) {
        goto output_overflow;
    }
    used = (size_t)written;

    written = snprintf(s_csi_line + used, sizeof(s_csi_line) - used,
                       ",%d,%d,\"[%d", info->len,
                       info->first_word_invalid, info->buf[0]);
    if (written < 0 || (size_t)written >= sizeof(s_csi_line) - used) {
        goto output_overflow;
    }
    used += (size_t)written;
    for (int i = 1; i < info->len; i++) {
        written = snprintf(s_csi_line + used, sizeof(s_csi_line) - used,
                           ",%d", info->buf[i]);
        if (written < 0 || (size_t)written >= sizeof(s_csi_line) - used) {
            goto output_overflow;
        }
        used += (size_t)written;
    }
    written = snprintf(s_csi_line + used, sizeof(s_csi_line) - used,
                       "]\"\n");
    if (written < 0 || (size_t)written >= sizeof(s_csi_line) - used) {
        goto output_overflow;
    }
    used += (size_t)written;
    if (fwrite(s_csi_line, 1, used, stdout) != used) {
        goto output_overflow;
    }

    xSemaphoreGive(s_output_lock);
    return;

output_overflow:
    portENTER_CRITICAL(&s_state_lock);
    s_output_drop_count++;
    portEXIT_CRITICAL(&s_state_lock);
    xSemaphoreGive(s_output_lock);
}

static esp_err_t csi_reinitialize(const char *reason)
{
#if CONFIG_IDF_TARGET_ESP32C5
    wifi_csi_config_t config = {
        .enable = true,
        .acquire_csi_legacy = true,
        .acquire_csi_force_lltf = false,
        .acquire_csi_ht20 = true,
        .acquire_csi_ht40 = true,
        .acquire_csi_vht = false,
        .acquire_csi_su = false,
        .acquire_csi_mu = false,
        .acquire_csi_dcm = false,
        .acquire_csi_beamformed = false,
        .acquire_csi_he_stbc_mode = 2,
        .val_scale_cfg = 0,
        .dump_ack_en = false,
        .lltf_bit_mode = 0,
        .reserved = 0,
    };
#else
    wifi_csi_config_t config = {
        .lltf_en = true,
        .htltf_en = true,
        .stbc_htltf2_en = true,
        .ltf_merge_en = true,
        .channel_filter_en = true,
        .manu_scale = false,
        .shift = false,
    };
#endif

    if (xSemaphoreTake(s_csi_control_lock, pdMS_TO_TICKS(2000)) != pdTRUE) {
        portENTER_CRITICAL(&s_state_lock);
        s_csi_reinit_failure_count++;
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = ESP_OK;
    if (s_csi_initialized) {
        err = esp_wifi_set_csi(false);
    }
    if (err == ESP_OK) {
        err = esp_wifi_set_csi_config(&config);
    }
    if (err == ESP_OK) {
        err = esp_wifi_set_csi_rx_cb(csi_rx_callback, NULL);
    }
    if (err == ESP_OK) {
        err = esp_wifi_set_csi(true);
    }

    portENTER_CRITICAL(&s_state_lock);
    if (err == ESP_OK) {
        s_csi_initialized = true;
        s_csi_reinit_count++;
    } else {
        s_csi_initialized = false;
        s_csi_reinit_failure_count++;
    }
    portEXIT_CRITICAL(&s_state_lock);
    xSemaphoreGive(s_csi_control_lock);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "CSI initialized reason=%s", reason);
    } else {
        ESP_LOGE(TAG, "CSI initialization failed reason=%s error=%s",
                 reason, esp_err_to_name(err));
    }
    return err;
}

static esp_err_t select_preferred_ap(void)
{
    if (CONFIG_CWS_PREFERRED_BSSID[0] == '\0') {
        return ESP_OK;
    }

    uint8_t preferred_bssid[6];
    if (!parse_mac(CONFIG_CWS_PREFERRED_BSSID, preferred_bssid)) {
        ESP_LOGE(TAG, "invalid preferred BSSID: %s",
                 CONFIG_CWS_PREFERRED_BSSID);
        return ESP_ERR_INVALID_ARG;
    }

    wifi_ap_record_t current_ap = {0};
    esp_err_t err = esp_wifi_sta_get_ap_info(&current_ap);
    if (err == ESP_OK &&
        memcmp(current_ap.bssid, preferred_bssid, sizeof(preferred_bssid)) == 0 &&
        (CONFIG_CWS_PREFERRED_CHANNEL == 0 ||
         current_ap.primary == CONFIG_CWS_PREFERRED_CHANNEL)) {
        return ESP_OK;
    }

    wifi_config_t station_config = {0};
    ESP_RETURN_ON_ERROR(esp_wifi_get_config(WIFI_IF_STA, &station_config),
                        TAG, "cannot read station configuration");
    memcpy(station_config.sta.bssid, preferred_bssid,
           sizeof(station_config.sta.bssid));
    station_config.sta.bssid_set = true;
    station_config.sta.channel = CONFIG_CWS_PREFERRED_CHANNEL;
    station_config.sta.scan_method = WIFI_FAST_SCAN;
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &station_config),
                        TAG, "cannot set preferred AP");
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(250));
    ESP_RETURN_ON_ERROR(esp_wifi_connect(), TAG,
                        "cannot start preferred AP association");

    for (int attempt = 0; attempt < 300; attempt++) {
        esp_netif_ip_info_t ip = {0};
        esp_netif_t *station =
            esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (esp_wifi_sta_get_ap_info(&current_ap) == ESP_OK &&
            memcmp(current_ap.bssid, preferred_bssid,
                   sizeof(preferred_bssid)) == 0 &&
            esp_netif_get_ip_info(station, &ip) == ESP_OK &&
            ip.ip.addr != 0) {
            ESP_LOGI(TAG, "preferred AP selected bssid=" MACSTR
                          " channel=%u rssi=%d",
                     MAC2STR(current_ap.bssid), current_ap.primary,
                     current_ap.rssi);
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGE(TAG, "preferred AP association timed out bssid=" MACSTR
                  " channel=%d",
             MAC2STR(preferred_bssid), CONFIG_CWS_PREFERRED_CHANNEL);
    return ESP_ERR_TIMEOUT;
}

static esp_err_t ping_router_start(uint32_t frequency_hz,
                                   esp_ping_handle_t *new_handle)
{
    *new_handle = NULL;
    if (frequency_hz == 0) {
        return ESP_OK;
    }
    esp_netif_ip_info_t local_ip;
    esp_netif_t *station = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_err_t err = esp_netif_get_ip_info(station, &local_ip);
    if (err != ESP_OK) {
        return err;
    }

    esp_ping_config_t config = ESP_PING_DEFAULT_CONFIG();
    config.count = 0;
    config.interval_ms = 1000 / frequency_hz;
    config.task_stack_size = 3072;
    config.data_size = CONFIG_CWS_PROBE_PAYLOAD_BYTES;
    config.target_addr.u_addr.ip4.addr = ip4_addr_get_u32(&local_ip.gw);
    config.target_addr.type = ESP_IPADDR_TYPE_V4;

    esp_ping_callbacks_t callbacks = {
        .on_ping_success = ping_on_success,
        .on_ping_timeout = ping_on_timeout,
    };
    err = esp_ping_new_session(&config, &callbacks, new_handle);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_ping_start(*new_handle);
    if (err != ESP_OK) {
        esp_ping_delete_session(*new_handle);
        *new_handle = NULL;
        return err;
    }
    ESP_LOGI(TAG, "continuous router ping started at %" PRIu32
                  " Hz, gateway=" IPSTR,
             frequency_hz, IP2STR(&local_ip.gw));
    return ESP_OK;
}

static esp_err_t ping_restart_current(const char *reason)
{
    if (xSemaphoreTake(s_ping_lock, pdMS_TO_TICKS(2000)) != pdTRUE) {
        portENTER_CRITICAL(&s_state_lock);
        s_ping_restart_failure_count++;
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_TIMEOUT;
    }
    uint32_t frequency_hz;
    portENTER_CRITICAL(&s_state_lock);
    frequency_hz = s_ping_hz;
    portEXIT_CRITICAL(&s_state_lock);
    if (s_ping_handle != NULL) {
        esp_ping_stop(s_ping_handle);
        esp_ping_delete_session(s_ping_handle);
        s_ping_handle = NULL;
    }
    esp_err_t err = ping_router_start(frequency_hz, &s_ping_handle);
    portENTER_CRITICAL(&s_state_lock);
    if (err == ESP_OK) {
        s_ping_restart_count++;
    } else {
        s_ping_restart_failure_count++;
    }
    portEXIT_CRITICAL(&s_state_lock);
    xSemaphoreGive(s_ping_lock);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "probe restarted reason=%s", reason);
    } else {
        ESP_LOGE(TAG, "probe restart failed reason=%s error=%s",
                 reason, esp_err_to_name(err));
    }
    return err;
}

static esp_err_t ping_frequency_set(uint32_t frequency_hz)
{
    if (frequency_hz > 50) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_ping_lock, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    uint32_t previous_hz;
    portENTER_CRITICAL(&s_state_lock);
    previous_hz = s_ping_hz;
    portEXIT_CRITICAL(&s_state_lock);
    if (frequency_hz == previous_hz) {
        xSemaphoreGive(s_ping_lock);
        return ESP_OK;
    }
    if (s_ping_handle != NULL) {
        esp_ping_stop(s_ping_handle);
        esp_ping_delete_session(s_ping_handle);
        s_ping_handle = NULL;
    }
    esp_err_t err = ping_router_start(frequency_hz, &s_ping_handle);
    if (err == ESP_OK) {
        portENTER_CRITICAL(&s_state_lock);
        s_ping_hz = frequency_hz;
        portEXIT_CRITICAL(&s_state_lock);
    } else {
        ESP_LOGE(TAG, "cannot apply ping rate=%" PRIu32 ": %s; restoring=%" PRIu32,
                 frequency_hz, esp_err_to_name(err), previous_hz);
        esp_err_t restore_err = ping_router_start(previous_hz, &s_ping_handle);
        if (restore_err != ESP_OK) {
            ESP_LOGE(TAG, "cannot restore ping rate: %s",
                     esp_err_to_name(restore_err));
            portENTER_CRITICAL(&s_state_lock);
            s_ping_hz = 0;
            portEXIT_CRITICAL(&s_state_lock);
        }
    }
    xSemaphoreGive(s_ping_lock);
    return err;
}

static int control_apply_ping(void *context, uint32_t frequency_hz)
{
    (void)context;
    if (ping_frequency_set(frequency_hz) == ESP_OK) {
        return 0;
    }
    uint32_t effective_hz;
    portENTER_CRITICAL(&s_state_lock);
    effective_hz = s_ping_hz;
    portEXIT_CRITICAL(&s_state_lock);
    if (effective_hz == 0) {
        cws_control_sync_external_ping(&s_control_protocol, 0);
        return -2;
    }
    return -1;
}

static size_t control_describe_state(void *context, char *out, size_t out_size)
{
    (void)context;
    bool connected;
    uint8_t channel;
    uint8_t bssid[6];
    uint64_t csi_accepted;
    uint64_t csi_invalid;
    uint64_t output_drops;
    uint64_t ping_success;
    uint64_t ping_timeout;
    portENTER_CRITICAL(&s_state_lock);
    connected = s_connected;
    channel = s_ap_channel;
    memcpy(bssid, s_ap_bssid, sizeof(bssid));
    csi_accepted = s_csi_count;
    csi_invalid = s_csi_invalid_count;
    output_drops = s_output_drop_count;
    ping_success = s_ping_success_count;
    ping_timeout = s_ping_timeout_count;
    portEXIT_CRITICAL(&s_state_lock);
    if (connected) {
        return (size_t)snprintf(
            out, out_size,
            "firmware_profile=%s firmware_version=%s node_id=%s rate_min_hz=0"
            " rate_max_hz=50 band=%s channel=%u bssid=%02x%02x%02x%02x%02x%02x"
            " csi_accepted=%" PRIu64 " csi_invalid=%" PRIu64
            " output_drops=%" PRIu64 " ping_success=%" PRIu64
            " ping_timeouts=%" PRIu64,
            CWS_FIRMWARE_PROFILE, CWS_FIRMWARE_VERSION, CONFIG_CWS_NODE_LABEL,
            channel <= 14 ? "2g" : "5g", channel, bssid[0], bssid[1], bssid[2],
            bssid[3], bssid[4], bssid[5], csi_accepted, csi_invalid,
            output_drops, ping_success, ping_timeout);
    }
    return (size_t)snprintf(out, out_size,
                            "firmware_profile=%s firmware_version=%s node_id=%s"
                            " rate_min_hz=0 rate_max_hz=50 csi_accepted=%" PRIu64
                            " csi_invalid=%" PRIu64 " output_drops=%" PRIu64
                            " ping_success=%" PRIu64 " ping_timeouts=%" PRIu64,
                            CWS_FIRMWARE_PROFILE, CWS_FIRMWARE_VERSION,
                            CONFIG_CWS_NODE_LABEL, csi_accepted, csi_invalid,
                            output_drops, ping_success, ping_timeout);
}

static void control_emit_reply(const char *reply)
{
    if (xSemaphoreTake(s_output_lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
        fputs(reply, stdout);
        xSemaphoreGive(s_output_lock);
    }
}

static void command_task(void *arg)
{
    char line[CWS_CONTROL_MAX_LINE + 2];
    while (fgets(line, sizeof(line), stdin) != NULL) {
        size_t line_size = strlen(line);
        bool overlong = line_size == sizeof(line) - 1 &&
                        line[line_size - 1] != '\n';
        if (overlong) {
            int current;
            while ((current = fgetc(stdin)) != '\n' && current != EOF) {
            }
            if (strncmp(line, CWS_CONTROL_PROTOCOL,
                        strlen(CWS_CONTROL_PROTOCOL)) == 0) {
                char reply[CWS_CONTROL_MAX_REPLY];
                cws_control_handle(&s_control_protocol, line,
                                   CWS_CONTROL_MAX_LINE + 1, reply, sizeof(reply));
                control_emit_reply(reply);
            } else if (xSemaphoreTake(s_output_lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
                printf("CWS_CONFIG_REJECTED requested_ping_hz=%" PRIu32
                       " current_ping_hz=%" PRIu32 " error=ESP_ERR_INVALID_SIZE\n",
                       UINT32_MAX, s_ping_hz);
                xSemaphoreGive(s_output_lock);
            }
            continue;
        }
        uint32_t requested_hz = UINT32_MAX;
        cws_legacy_command_t legacy_command = cws_legacy_parse(line, &requested_hz);
        if (legacy_command == CWS_LEGACY_REBOOT) {
            if (xSemaphoreTake(s_output_lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
                printf("CWS_CONFIG_APPLIED action=reboot\n");
                xSemaphoreGive(s_output_lock);
            }
            vTaskDelay(pdMS_TO_TICKS(250));
            esp_restart();
        }
        if (strncmp(line, CWS_CONTROL_PROTOCOL,
                    strlen(CWS_CONTROL_PROTOCOL)) == 0) {
            char reply[CWS_CONTROL_MAX_REPLY];
            size_t protocol_size = overlong ? CWS_CONTROL_MAX_LINE + 1 : line_size;
            cws_control_handle(&s_control_protocol, line, protocol_size, reply,
                               sizeof(reply));
            portENTER_CRITICAL(&s_state_lock);
            s_config_epoch = cws_control_config_epoch(&s_control_protocol);
            portEXIT_CRITICAL(&s_state_lock);
            control_emit_reply(reply);
            continue;
        }
        esp_err_t err = legacy_command == CWS_LEGACY_SET_PING_HZ ? ping_frequency_set(requested_hz)
                                   : ESP_ERR_INVALID_ARG;
        uint32_t current_hz;
        portENTER_CRITICAL(&s_state_lock);
        current_hz = s_ping_hz;
        portEXIT_CRITICAL(&s_state_lock);
        if (err == ESP_OK || current_hz == 0) {
            cws_control_sync_external_ping(&s_control_protocol, current_hz);
            portENTER_CRITICAL(&s_state_lock);
            s_config_epoch = cws_control_config_epoch(&s_control_protocol);
            portEXIT_CRITICAL(&s_state_lock);
        }
        if (xSemaphoreTake(s_output_lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
            if (err == ESP_OK) {
                printf("CWS_CONFIG_APPLIED ping_hz=%" PRIu32 "\n", current_hz);
            } else {
                printf("CWS_CONFIG_REJECTED requested_ping_hz=%" PRIu32
                       " current_ping_hz=%" PRIu32 " error=%s\n",
                       requested_hz, current_hz, esp_err_to_name(err));
            }
            xSemaphoreGive(s_output_lock);
        }
    }
    ESP_LOGW(TAG, "serial command input ended");
    vTaskDelete(NULL);
}

static void heartbeat_task(void *arg)
{
    uint8_t station_mac[6];
    ESP_ERROR_CHECK(esp_read_mac(station_mac, ESP_MAC_WIFI_STA));

    if (xSemaphoreTake(s_output_lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
        printf(
            "CSI_PROFILE fw_profile=%s fw_version=%s fw_role=live "
            "node_label=%s boot_epoch=%" PRIu32 " config_epoch=%" PRIu32 " station_mac=" MACSTR
            " ping_hz=%" PRIu32 " probe_payload_bytes=%d\n",
            CWS_FIRMWARE_PROFILE, CWS_FIRMWARE_VERSION, CONFIG_CWS_NODE_LABEL,
            s_boot_epoch, s_config_epoch, MAC2STR(station_mac), s_ping_hz,
            CONFIG_CWS_PROBE_PAYLOAD_BYTES);
#if CONFIG_IDF_TARGET_ESP32C5
        printf(
            "type,seq,mac,rssi,rate,noise_floor,fft_gain,agc_gain,channel,"
            "local_timestamp,sig_len,rx_format,len,first_word,data\n");
#else
        printf(
            "type,id,mac,rssi,rate,sig_mode,mcs,bandwidth,smoothing,"
            "not_sounding,aggregation,stbc,fec_coding,sgi,noise_floor,"
            "ampdu_cnt,channel,secondary_channel,local_timestamp,ant,"
            "sig_len,rx_state,len,first_word,data\n");
#endif
        xSemaphoreGive(s_output_lock);
    }

    uint64_t previous_csi_count = UINT64_MAX;
    uint32_t stagnant_heartbeats = 0;
    bool stalled_episode = false;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(CONFIG_CWS_HEARTBEAT_INTERVAL_MS));

        wifi_ap_record_t current_ap = {0};
        if (esp_wifi_sta_get_ap_info(&current_ap) == ESP_OK) {
            portENTER_CRITICAL(&s_state_lock);
            memcpy(s_ap_bssid, current_ap.bssid, sizeof(s_ap_bssid));
            s_ap_channel = current_ap.primary;
            s_connected = true;
            portEXIT_CRITICAL(&s_state_lock);
        }

        bool connected;
        uint8_t channel;
        uint8_t bssid[6];
        uint64_t csi_count;
        uint64_t csi_callback_seen_count;
        uint64_t csi_mac_mismatch_count;
        uint64_t csi_invalid_count;
        uint64_t output_drop_count;
        uint32_t csi_reinit_count;
        uint32_t csi_reinit_failure_count;
        uint32_t csi_stall_count;
        uint32_t ping_restart_count;
        uint32_t ping_restart_failure_count;
        uint64_t ping_success_count;
        uint64_t ping_timeout_count;
        bool csi_stalled;
        uint32_t ping_hz;
        uint32_t config_epoch;
        int32_t chip_temp_millicelsius = INT32_MIN;
        float chip_temp_celsius;
        if (temperature_sensor_get_celsius(s_temperature_sensor,
                                           &chip_temp_celsius) == ESP_OK) {
            chip_temp_millicelsius = (int32_t)(chip_temp_celsius * 1000.0f);
        }
        portENTER_CRITICAL(&s_state_lock);
        connected = s_connected;
        channel = s_ap_channel;
        memcpy(bssid, s_ap_bssid, sizeof(bssid));
        csi_count = s_csi_count;
        csi_callback_seen_count = s_csi_callback_seen_count;
        csi_mac_mismatch_count = s_csi_mac_mismatch_count;
        csi_invalid_count = s_csi_invalid_count;
        output_drop_count = s_output_drop_count;
        csi_reinit_count = s_csi_reinit_count;
        csi_reinit_failure_count = s_csi_reinit_failure_count;
        csi_stall_count = s_csi_stall_count;
        ping_restart_count = s_ping_restart_count;
        ping_restart_failure_count = s_ping_restart_failure_count;
        ping_success_count = s_ping_success_count;
        ping_timeout_count = s_ping_timeout_count;
        csi_stalled = s_csi_stalled;
        ping_hz = s_ping_hz;
        config_epoch = s_config_epoch;
        portEXIT_CRITICAL(&s_state_lock);

        if (xSemaphoreTake(s_output_lock, pdMS_TO_TICKS(250)) == pdTRUE) {
            printf(
                "CWSLAB_TIMING_HEARTBEAT uptime_ms=%" PRIu64
                " boot_epoch=%" PRIu32 " csi_count=%" PRIu64
                " config_epoch=%" PRIu32
                " csi_callback_seen=%" PRIu64
                " csi_mac_mismatch=%" PRIu64
                " csi_invalid=%" PRIu64
                " output_drops=%" PRIu64 " connected=%d channel=%u"
                " bssid=" MACSTR " station_mac=" MACSTR
                " node_label=%s fw_profile=%s fw_version=%s"
                " ping_hz=%" PRIu32 " probe_payload_bytes=%d"
                " csi_reinit_count=%" PRIu32
                " csi_reinit_failures=%" PRIu32
                " csi_stall_count=%" PRIu32
                " ping_restart_count=%" PRIu32
                " ping_restart_failures=%" PRIu32
                " ping_success_count=%" PRIu64
                " ping_timeout_count=%" PRIu64
                " csi_stalled=%d"
                " chip_temp_millicelsius=%" PRId32 "\n",
                (uint64_t)(esp_timer_get_time() / 1000), s_boot_epoch,
                csi_count, config_epoch, csi_callback_seen_count, csi_mac_mismatch_count,
                csi_invalid_count, output_drop_count, connected ? 1 : 0, channel,
                MAC2STR(bssid), MAC2STR(station_mac), CONFIG_CWS_NODE_LABEL,
                CWS_FIRMWARE_PROFILE, CWS_FIRMWARE_VERSION,
                ping_hz, CONFIG_CWS_PROBE_PAYLOAD_BYTES,
                csi_reinit_count, csi_reinit_failure_count, csi_stall_count,
                ping_restart_count, ping_restart_failure_count,
                ping_success_count, ping_timeout_count,
                csi_stalled ? 1 : 0,
                chip_temp_millicelsius);
            xSemaphoreGive(s_output_lock);
        }

        if (!connected) {
            stagnant_heartbeats = 0;
            stalled_episode = false;
        } else if (previous_csi_count != UINT64_MAX &&
                   csi_count == previous_csi_count) {
            stagnant_heartbeats++;
        } else {
            stagnant_heartbeats = 0;
            stalled_episode = false;
            portENTER_CRITICAL(&s_state_lock);
            s_csi_stalled = false;
            portEXIT_CRITICAL(&s_state_lock);
        }
        previous_csi_count = csi_count;
        if (connected && stagnant_heartbeats >= 3 && !stalled_episode) {
            portENTER_CRITICAL(&s_state_lock);
            s_csi_stall_count++;
            s_csi_stalled = true;
            portEXIT_CRITICAL(&s_state_lock);
            stalled_episode = true;
            ESP_LOGW(TAG, "CSI watchdog detected a stalled counter; "
                          "awaiting event-driven recovery");
        }
    }
}

void app_main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stdin, NULL, _IONBF, 0);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    temperature_sensor_config_t temperature_config =
        TEMPERATURE_SENSOR_CONFIG_DEFAULT(20, 100);
    ESP_ERROR_CHECK(temperature_sensor_install(&temperature_config,
                                               &s_temperature_sensor));
    ESP_ERROR_CHECK(temperature_sensor_enable(s_temperature_sensor));

    s_boot_epoch = next_boot_epoch();
    s_output_lock = xSemaphoreCreateMutex();
    s_ping_lock = xSemaphoreCreateMutex();
    s_csi_control_lock = xSemaphoreCreateMutex();
    if (s_output_lock == NULL || s_ping_lock == NULL ||
        s_csi_control_lock == NULL) {
        ESP_LOGE(TAG, "failed to create runtime mutex");
        abort();
    }
    s_ping_hz = CONFIG_CWS_PING_FREQUENCY_HZ;
    s_config_epoch = 0;
    ESP_ERROR_CHECK(esp_read_mac(s_station_mac, ESP_MAC_WIFI_STA));
    cws_control_config_t control_config = {
        .boot_epoch = s_boot_epoch,
        .config_epoch = s_config_epoch,
        .ping_hz = s_ping_hz,
        .apply_ping = control_apply_ping,
        .describe_state = control_describe_state,
        .callback_context = NULL,
    };
    cws_control_init(&s_control_protocol, &control_config);

    /*
     * Start health output before association.  A missing or unsuitable AP must
     * remain observable to the failure-aware collector instead of making the
     * USB source appear completely silent while example_connect() waits.  The
     * command task starts here as well so a stalled association remains
     * remotely rebootable through the collector-owned serial channel.
     */
    xTaskCreate(heartbeat_task, "cws_heartbeat", 4096, NULL, 4, NULL);
    xTaskCreate(command_task, "cws_command", 4096, NULL, 4, NULL);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               wifi_event_handler, NULL));
    ESP_ERROR_CHECK(example_connect());
    ESP_ERROR_CHECK(select_preferred_ap());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    wifi_ap_record_t ap = {0};
    ESP_ERROR_CHECK(esp_wifi_sta_get_ap_info(&ap));
    portENTER_CRITICAL(&s_state_lock);
    memcpy(s_ap_bssid, ap.bssid, sizeof(s_ap_bssid));
    s_ap_channel = ap.primary;
    s_connected = true;
    portEXIT_CRITICAL(&s_state_lock);

    ESP_LOGI(TAG, "ready node=%s boot_epoch=%" PRIu32
                  " bssid=" MACSTR " channel=%u rssi=%d",
             CONFIG_CWS_NODE_LABEL, s_boot_epoch, MAC2STR(ap.bssid),
             ap.primary, ap.rssi);

    ESP_ERROR_CHECK(csi_reinitialize("startup_verify"));
    ESP_ERROR_CHECK(ping_restart_current("startup_verify"));
}
