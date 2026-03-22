#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "secrets.h"

#define TAG                 "SECURITY"
#define NUM_SUBCARRIERS     64
#define TOP_SUBCARRIERS     12
#define CALIB_SAMPLES       200
#define IIR_ALPHA           0.15f
#define HAMPEL_WINDOW       7
#define HAMPEL_THRESHOLD    3.0f
#define MVS_WINDOW          15
#define MVS_MOTION_THR      0.20f
#define CONFIRM_COUNT       3
#define RELEASE_COUNT       30

static float iir_state[NUM_SUBCARRIERS];
static float hampel_buf[NUM_SUBCARRIERS][HAMPEL_WINDOW];
static int   hampel_idx[NUM_SUBCARRIERS];
static float mvs_buf[MVS_WINDOW];
static int   mvs_idx   = 0;
static int   mvs_count = 0;
static float calib_buf[CALIB_SAMPLES][NUM_SUBCARRIERS];
static float baseline_mean[NUM_SUBCARRIERS];
static float baseline_var[NUM_SUBCARRIERS];
static int   calib_count = 0;
static bool  calibrated  = false;
static int   best_carriers[TOP_SUBCARRIERS];
static int   detect_count    = 0;
static int   no_motion_count = 0;
static bool  human_present   = false;
static QueueHandle_t tg_queue;

// ── Butterworth IIR ────────────────────────────────────────────────
static float butterworth_iir(int sc, float new_val) {
    iir_state[sc] = IIR_ALPHA * new_val +
                    (1.0f - IIR_ALPHA) * iir_state[sc];
    return iir_state[sc];
}

// ── Hampel filter ──────────────────────────────────────────────────
static float hampel_filter(int sc, float new_val) {
    hampel_buf[sc][hampel_idx[sc]] = new_val;
    hampel_idx[sc] = (hampel_idx[sc] + 1) % HAMPEL_WINDOW;
    float sorted[HAMPEL_WINDOW];
    memcpy(sorted, hampel_buf[sc], sizeof(sorted));
    for (int i = 1; i < HAMPEL_WINDOW; i++) {
        float key = sorted[i]; int j = i-1;
        while (j >= 0 && sorted[j] > key) { sorted[j+1]=sorted[j]; j--; }
        sorted[j+1] = key;
    }
    float median = sorted[HAMPEL_WINDOW/2];
    float abs_dev[HAMPEL_WINDOW];
    for (int i = 0; i < HAMPEL_WINDOW; i++)
        abs_dev[i] = fabsf(hampel_buf[sc][i] - median);
    for (int i = 1; i < HAMPEL_WINDOW; i++) {
        float key = abs_dev[i]; int j = i-1;
        while (j >= 0 && abs_dev[j] > key) { abs_dev[j+1]=abs_dev[j]; j--; }
        abs_dev[j+1] = key;
    }
    float mad = abs_dev[HAMPEL_WINDOW/2];
    if (mad > 0.001f &&
        fabsf(new_val-median) > HAMPEL_THRESHOLD*mad*1.4826f)
        return median;
    return new_val;
}

// ── Amplitude ──────────────────────────────────────────────────────
static float get_amplitude(int8_t real, int8_t imag) {
    return sqrtf((float)(real*real) + (float)(imag*imag));
}

// ── NBVI ───────────────────────────────────────────────────────────
static void select_best_subcarriers(void) {
    float variances[NUM_SUBCARRIERS];
    for (int sc = 0; sc < NUM_SUBCARRIERS; sc++) {
        float sum = 0;
        for (int i = 0; i < CALIB_SAMPLES; i++) sum += calib_buf[i][sc];
        float mean = sum / CALIB_SAMPLES;
        baseline_mean[sc] = mean;
        float var = 0;
        for (int i = 0; i < CALIB_SAMPLES; i++) {
            float d = calib_buf[i][sc] - mean; var += d*d;
        }
        variances[sc] = var / CALIB_SAMPLES;
        baseline_var[sc] = variances[sc];
    }
    variances[0] = 0;
    variances[1] = 0;
    bool used[NUM_SUBCARRIERS] = {false};
    for (int k = 0; k < TOP_SUBCARRIERS; k++) {
        int best = -1;
        for (int sc = 0; sc < NUM_SUBCARRIERS; sc++)
            if (!used[sc] && (best==-1 || variances[sc]>variances[best]))
                best = sc;
        best_carriers[k] = best;
        used[best] = true;
    }
    ESP_LOGI(TAG, "NBVI done — top subcarriers selected");
    for (int k = 0; k < TOP_SUBCARRIERS; k++)
        ESP_LOGI(TAG, "  sc=%d var=%.3f",
                 best_carriers[k], variances[best_carriers[k]]);
}

// ── MVS rolling variance ───────────────────────────────────────────
static float mvs_update(float score) {
    mvs_buf[mvs_idx] = score;
    mvs_idx = (mvs_idx + 1) % MVS_WINDOW;
    if (mvs_count < MVS_WINDOW) mvs_count++;
    float sum = 0;
    for (int i = 0; i < mvs_count; i++) sum += mvs_buf[i];
    float mean = sum / mvs_count;
    float var = 0;
    for (int i = 0; i < mvs_count; i++) {
        float d = mvs_buf[i] - mean; var += d*d;
    }
    return var / mvs_count;
}

// ── Telegram task ──────────────────────────────────────────────────
static void telegram_task(void *pvParameters) {
    int msg;
    while (1) {
        if (xQueueReceive(tg_queue, &msg, portMAX_DELAY)) {
            const char *text =
                (msg == 1) ? "INTRUDER DETECTED! Someone entered the room." :
                (msg == 2) ? "All clear. Intruder has left." :
                             "Security system armed and monitoring!";
            char url[128];
            snprintf(url, sizeof(url),
                "https://api.telegram.org/bot%s/sendMessage", BOT_TOKEN);
            char post[256];
            snprintf(post, sizeof(post),
                "{\"chat_id\":\"%s\",\"text\":\"%s\"}", CHAT_ID, text);
            esp_http_client_config_t config = {
                .url               = url,
                .method            = HTTP_METHOD_POST,
                .crt_bundle_attach = esp_crt_bundle_attach,
            };
            esp_http_client_handle_t client =
                esp_http_client_init(&config);
            esp_http_client_set_header(client,
                "Content-Type", "application/json");
            esp_http_client_set_post_field(client, post, strlen(post));
            esp_err_t err = esp_http_client_perform(client);
            if (err == ESP_OK)
                ESP_LOGI(TAG, "Telegram sent: %s", text);
            else
                ESP_LOGE(TAG, "Telegram failed: %s",
                         esp_err_to_name(err));
            esp_http_client_cleanup(client);
        }
    }
}

// ── CSI callback ───────────────────────────────────────────────────
static void csi_callback(void *ctx, wifi_csi_info_t *info) {
    if (!info || !info->buf) return;
    float amplitudes[NUM_SUBCARRIERS];
    for (int sc = 0; sc < NUM_SUBCARRIERS && (sc*2+1) < info->len; sc++) {
        float amp = get_amplitude(info->buf[sc*2], info->buf[sc*2+1]);
        float iir = butterworth_iir(sc, amp);
        amplitudes[sc] = hampel_filter(sc, iir);
    }
    if (!calibrated) {
        if (calib_count < CALIB_SAMPLES) {
            for (int sc = 0; sc < NUM_SUBCARRIERS; sc++)
                calib_buf[calib_count][sc] = amplitudes[sc];
            calib_count++;
            if (calib_count % 20 == 0)
                ESP_LOGI(TAG, "Calibrating... %d/%d",
                         calib_count, CALIB_SAMPLES);
        } else {
            select_best_subcarriers();
            calibrated = true;
            ESP_LOGI(TAG, "Calibration done! Threshold=%.4f",
                     MVS_MOTION_THR);
            ESP_LOGI(TAG, "System armed. Monitoring started.");
            int m = 0; xQueueSend(tg_queue, &m, 0);
        }
        return;
    }
    float total = 0;
    for (int k = 0; k < TOP_SUBCARRIERS; k++) {
        int sc = best_carriers[k];
        float diff = fabsf(amplitudes[sc] - baseline_mean[sc]);
        float norm = (baseline_var[sc] > 0.01f)
                     ? diff / sqrtf(baseline_var[sc]) : 0;
        total += norm;
    }
    float score = total / TOP_SUBCARRIERS;
    float mvs_var = mvs_update(score);
    if (mvs_var > MVS_MOTION_THR) {
        detect_count++;
        no_motion_count = 0;
        if (detect_count >= CONFIRM_COUNT && !human_present) {
            human_present = true;
            ESP_LOGW(TAG, "=== INTRUDER DETECTED! mvs=%.4f ===", mvs_var);
            int m = 1; xQueueSend(tg_queue, &m, 0);
        } else if (human_present) {
            ESP_LOGW(TAG, "Intruder present. mvs=%.4f", mvs_var);
        }
    } else {
        no_motion_count++;
        detect_count = 0;
        if (no_motion_count >= RELEASE_COUNT && human_present) {
            human_present = false;
            ESP_LOGI(TAG, "--- Intruder left. mvs=%.4f ---", mvs_var);
            int m = 2; xQueueSend(tg_queue, &m, 0);
        } else if (!human_present) {
            ESP_LOGI(TAG, "Clear. mvs=%.4f", mvs_var);
        }
    }
}

// ── UDP ping ───────────────────────────────────────────────────────
static void udp_ping_task(void *pvParameters) {
    vTaskDelay(pdMS_TO_TICKS(2000));
    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port   = htons(5555);
    inet_aton(ROUTER_IP, &dest.sin_addr);
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { vTaskDelete(NULL); }
    ESP_LOGI(TAG, "UDP ping → %s", ROUTER_IP);
    char buf[] = "csi";
    while (1) {
        sendto(sock, buf, sizeof(buf), 0,
               (struct sockaddr *)&dest, sizeof(dest));
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// ── WiFi event handler ─────────────────────────────────────────────
static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t event_id, void *data) {
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "WiFi connected! Calibrating...");
        ESP_LOGI(TAG, "Sit at desk and keep STILL!");
        esp_wifi_set_ps(WIFI_PS_NONE);
        wifi_csi_config_t cfg = {
            .lltf_en=true, .htltf_en=true,
            .stbc_htltf2_en=true, .ltf_merge_en=true,
            .channel_filter_en=true,
            .manu_scale=false, .shift=false,
        };
        esp_wifi_set_csi_config(&cfg);
        esp_wifi_set_csi_rx_cb(csi_callback, NULL);
        esp_wifi_set_csi(true);
        xTaskCreate(udp_ping_task, "udp_ping", 4096, NULL, 5, NULL);
        xTaskCreate(telegram_task, "telegram", 8192, NULL, 3, NULL);
    } else if (base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
    }
}

// ── app_main ───────────────────────────────────────────────────────
void app_main(void) {
    memset(iir_state,  0, sizeof(iir_state));
    memset(hampel_buf, 0, sizeof(hampel_buf));
    memset(hampel_idx, 0, sizeof(hampel_idx));
    memset(mvs_buf,    0, sizeof(mvs_buf));

    tg_queue = xQueueCreate(5, sizeof(int));

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase(); nvs_flash_init();
    }
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                &wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                &wifi_event_handler, NULL);
    wifi_config_t wifi_config = {
        .sta = { .ssid=WIFI_SSID, .password=WIFI_PASS },
    };
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();
    ESP_LOGI(TAG, "Home security system starting...");
}