
 /*
 * 역할
 *   모든 센서 MQTT 토픽을 구독하여 USB 시리얼에
 *   실시간 대시보드를 출력하고 세션 통계를 집계
 *
 * 출력 예시 (2초마다 갱신)
 *   ╔══════════════════════════════╗
 *   ║     FitPico Dashboard        ║
 *   ╠══════════════════════════════╣
 *   ║ STATUS : ACTIVE              ║
 *   ║ REPS   : 5   SETS : 2 / 3   ║
 *   ║ SPEED  : 1200 ms  [ok]       ║
 *   ║ REST   : 45 sec (after set2) ║
 *   ╠══════════════════════════════╣
 *   ║ DAILY TOTAL                  ║
 *   ║  Reps : 45   Sets : 5        ║
 *   ╚══════════════════════════════╝
 *
 * MQTT 구독 토픽
 *   fitpico/sensor/count   → 현재 세션 상태
 *   fitpico/sensor/speed   → 1회당 속도
 *   fitpico/sensor/rest    → 세트 간 휴식
 *   fitpico/sensor/daily   → 일일 누적
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"

#include "lwip/apps/mqtt.h"
#include "lwip/ip_addr.h"

#include "config.h"

// ─────────────────────────────────────────────────────────────────────────────
// 전역 상태
// ─────────────────────────────────────────────────────────────────────────────

static mqtt_client_t *g_mqtt_client = NULL;
static bool           g_mqtt_ready  = false;

static char g_cur_topic[64] = {0};
static char g_payload[256]  = {0};
static int  g_pay_len       = 0;

// 현재 세션
static int  g_reps        = 0;
static int  g_sets        = 0;
static bool g_active      = false;

// 속도
static int  g_speed_ms    = 0;
static char g_warn[8]     = "---";
static int  g_speed_rep   = 0;

// 휴식
static int  g_rest_sec    = 0;
static int  g_rest_after  = 0;

// 일일 누적
static int  g_daily_reps  = 0;
static int  g_daily_sets  = 0;

// 속도 이력 (최근 5회)
#define SPEED_HISTORY 5
static int  g_speed_hist[SPEED_HISTORY] = {0};
static int  g_hist_idx   = 0;
static int  g_hist_count = 0;

static bool g_updated = false;

// ─────────────────────────────────────────────────────────────────────────────
// JSON 파싱 헬퍼
// ─────────────────────────────────────────────────────────────────────────────

static int json_int(const char *json, const char *key) {
    char search[32];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return 0;
    p += strlen(search);
    while (*p == ' ') p++;
    return atoi(p);
}

static void json_str(const char *json, const char *key, char *out, int sz) {
    char search[32];
    snprintf(search, sizeof(search), "\"%s\":\"", key);
    const char *p = strstr(json, search);
    if (!p) { out[0] = '\0'; return; }
    p += strlen(search);
    int i = 0;
    while (*p && *p != '"' && i < sz - 1) out[i++] = *p++;
    out[i] = '\0';
}

// ─────────────────────────────────────────────────────────────────────────────
// MQTT 콜백
// ─────────────────────────────────────────────────────────────────────────────

static void on_publish(void *arg, const char *topic, u32_t tot_len) {
    (void)arg; (void)tot_len;
    strncpy(g_cur_topic, topic, sizeof(g_cur_topic) - 1);
    g_pay_len = 0;
}

static void on_data(void *arg, const u8_t *data, u16_t len, u8_t flags) {
    (void)arg;
    int copy = len;
    if (g_pay_len + copy >= (int)sizeof(g_payload) - 1)
        copy = (int)sizeof(g_payload) - 1 - g_pay_len;
    memcpy(g_payload + g_pay_len, data, copy);
    g_pay_len += copy;
    if (!(flags & MQTT_DATA_FLAG_LAST)) return;
    g_payload[g_pay_len] = '\0';

    if (strcmp(g_cur_topic, TOPIC_COUNT) == 0) {
        g_reps = json_int(g_payload, "reps");
        g_sets = json_int(g_payload, "sets");
        char act[8]; json_str(g_payload, "active", act, sizeof(act));
        g_active = (strcmp(act, "true") == 0);

    } else if (strcmp(g_cur_topic, TOPIC_SPEED) == 0) {
        g_speed_ms  = json_int(g_payload, "speed_ms");
        g_speed_rep = json_int(g_payload, "rep");
        json_str(g_payload, "warn", g_warn, sizeof(g_warn));
        // 속도 이력 기록
        g_speed_hist[g_hist_idx] = g_speed_ms;
        g_hist_idx = (g_hist_idx + 1) % SPEED_HISTORY;
        if (g_hist_count < SPEED_HISTORY) g_hist_count++;

    } else if (strcmp(g_cur_topic, TOPIC_REST) == 0) {
        g_rest_sec   = json_int(g_payload, "rest_sec");
        g_rest_after = json_int(g_payload, "set");

    } else if (strcmp(g_cur_topic, TOPIC_DAILY) == 0) {
        g_daily_reps = json_int(g_payload, "total_reps");
        g_daily_sets = json_int(g_payload, "total_sets");
    }
    g_updated = true;
}

static void on_sub(void *arg, err_t result) {
    (void)arg;
    if (result != ERR_OK) printf("[MQTT] 구독 실패: %d\n", result);
    else printf("[MQTT] 구독 성공\n");
}

static void on_connect(mqtt_client_t *client, void *arg,
                       mqtt_connection_status_t status) {
    (void)arg;
    if (status != MQTT_CONNECT_ACCEPTED) {
        printf("[MQTT] 연결 실패 (status=%d)\n", status);
        g_mqtt_ready = false;
        return;
    }
    printf("[MQTT] 브로커 연결 성공\n");
    g_mqtt_ready = true;
    mqtt_set_inpub_callback(client, on_publish, on_data, NULL);
    mqtt_subscribe(client, TOPIC_COUNT, 0, on_sub, NULL);
    mqtt_subscribe(client, TOPIC_SPEED, 0, on_sub, NULL);
    mqtt_subscribe(client, TOPIC_REST,  0, on_sub, NULL);
    mqtt_subscribe(client, TOPIC_DAILY, 0, on_sub, NULL);
}

static void mqtt_connect_broker(void) {
    ip_addr_t broker;
    if (!ip4addr_aton(MQTT_BROKER_IP, &broker)) return;
    g_mqtt_client = mqtt_client_new();
    if (!g_mqtt_client) return;
    struct mqtt_connect_client_info_t ci = {
        .client_id = "fitpico_dashboard", .keep_alive = 60,
    };
    mqtt_client_connect(g_mqtt_client, &broker, MQTT_BROKER_PORT,
                        on_connect, NULL, &ci);
}

// ─────────────────────────────────────────────────────────────────────────────
// 대시보드 출력
// ─────────────────────────────────────────────────────────────────────────────

static int speed_avg(void) {
    if (g_hist_count == 0) return 0;
    int sum = 0;
    for (int i = 0; i < g_hist_count; i++) sum += g_speed_hist[i];
    return sum / g_hist_count;
}

static void print_dashboard(void) {
    // 터미널 커서 위쪽으로 이동 (ANSI 이스케이프)
    printf("\033[H\033[2J");

    printf("╔══════════════════════════════╗\n");
    printf("║     FitPico Dashboard        ║\n");
    printf("╠══════════════════════════════╣\n");
    printf("║ STATUS : %-20s║\n", g_active ? "ACTIVE  [운동 중]" : "IDLE    [대기 중]");
    printf("║ REPS   : %-3d  SETS : %d / %d    ║\n",
           g_reps, g_sets, TARGET_SETS);
    printf("║ SPEED  : %-4d ms  [%-4s]      ║\n", g_speed_ms, g_warn);
    if (g_rest_sec > 0)
        printf("║ REST   : %-3d sec  (set %d 후)  ║\n", g_rest_sec, g_rest_after);
    else
        printf("║ REST   : -                    ║\n");
    printf("╠══════════════════════════════╣\n");
    printf("║ DAILY TOTAL                  ║\n");
    printf("║  Reps : %-4d   Sets : %-4d   ║\n", g_daily_reps, g_daily_sets);
    if (g_hist_count > 0)
        printf("║  Avg Speed : %-4d ms         ║\n", speed_avg());
    else
        printf("║  Avg Speed : ---  ms         ║\n");
    printf("╚══════════════════════════════╝\n");

    if (g_hist_count > 0) {
        printf("\n최근 %d회 속도 (ms): ", g_hist_count);
        for (int i = 0; i < g_hist_count; i++) {
            int idx = (g_hist_idx - g_hist_count + i + SPEED_HISTORY) % SPEED_HISTORY;
            printf("%d", g_speed_hist[idx]);
            if (i < g_hist_count - 1) printf(", ");
        }
        printf("\n");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main(void) {
    stdio_init_all();
    sleep_ms(2000);

    printf("=== FitPico IoT Dashboard ===\n");
    printf("브로커: %s:%d\n", MQTT_BROKER_IP, MQTT_BROKER_PORT);

    if (cyw43_arch_init()) { printf("[WiFi] 초기화 실패\n"); return 1; }
    cyw43_arch_enable_sta_mode();
    printf("[WiFi] %s 연결 중...\n", WIFI_SSID);
    if (cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD,
                                           CYW43_AUTH_WPA2_AES_PSK, 15000)) {
        printf("[WiFi] 연결 실패\n");
        return 1;
    }
    printf("[WiFi] 연결 완료\n");
    mqtt_connect_broker();
    sleep_ms(1000);

    uint32_t last_print_ms = 0;

    while (true) {
        cyw43_arch_poll();
        uint32_t now = to_ms_since_boot(get_absolute_time());

        if (g_mqtt_ready && (g_updated || now - last_print_ms > 2000)) {
            print_dashboard();
            last_print_ms = now;
            g_updated = false;
        }

        sleep_ms(50);
    }

    cyw43_arch_deinit();
    return 0;
}
