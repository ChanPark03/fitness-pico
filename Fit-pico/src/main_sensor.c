/*
 *   - HC-SR04  : 초음파 거리 측정 → 푸시업 횟수 카운트
 *   - PIR      : 움직임 감지 → 운동 자동 시작/정지
 *   - 4×4 키패드: A=시작  D=정지  #=세트완료
 * MQTT publish 토픽
 *   fitpico/sensor/count   → {"mode":"pushup","reps":5,"sets":2,"active":true}
 *   fitpico/sensor/rest    → {"set":2,"rest_sec":45}
 *   fitpico/sensor/speed   → {"rep":5,"speed_ms":1200,"warn":"ok"}
 *   fitpico/sensor/daily   → {"total_reps":45,"total_sets":5}
 *
 * 핀 배치 (config.h 참조)
 *   GP14=TRIG  GP15=ECHO  GP16=PIR
 *   GP2-5=Keypad Rows(OUT)   GP6-9=Keypad Cols(IN, 풀업)
 *
 * HC-SR04 센서 위치
 *   바닥에 위를 향하게 설치
 *   내려간 상태: 가슴 ~ 센서 12cm 이내
 *   올라온 상태: 가슴 ~ 센서 28cm 이상  → 1회 카운트
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/gpio.h"

#include "lwip/apps/mqtt.h"
#include "lwip/ip_addr.h"

#include "config.h"

// ─────────────────────────────────────────────────────────────────────────────
// 타입 정의
// ─────────────────────────────────────────────────────────────────────────────

typedef enum { POS_UP = 0, POS_DOWN = 1 } Position;

// ─────────────────────────────────────────────────────────────────────────────
// 전역 상태
// ─────────────────────────────────────────────────────────────────────────────

static mqtt_client_t *g_mqtt_client = NULL;
static bool           g_mqtt_ready  = false;

static bool      g_active       = false;
static int       g_reps         = 0;
static int       g_sets         = 0;
static uint32_t  g_set_end_ms   = 0;   // 세트 완료 시각 (휴식 추적)

// 일일 누적 기록 (D키로 초기화되지 않음)
static int       g_daily_reps   = 0;
static int       g_daily_sets   = 0;

static Position  g_pushup_pos   = POS_UP;
static uint32_t  g_rep_down_ms  = 0;   // DOWN 진입 시각 (속도 측정)
static uint32_t  g_last_pir_ms  = 0;

// 키패드
static const char KEYMAP[4][4] = {
    {'1', '2', '3', 'A'},
    {'4', '5', '6', 'B'},
    {'7', '8', '9', 'C'},
    {'*', '0', '#', 'D'}
};
static const uint ROW_PINS[4] = {KEYPAD_ROW0, KEYPAD_ROW1, KEYPAD_ROW2, KEYPAD_ROW3};
static const uint COL_PINS[4] = {KEYPAD_COL0, KEYPAD_COL1, KEYPAD_COL2, KEYPAD_COL3};

static const uint32_t WIFI_AUTH_MODES[] = {
    CYW43_AUTH_WPA2_AES_PSK,
    CYW43_AUTH_WPA2_MIXED_PSK,
    CYW43_AUTH_WPA3_WPA2_AES_PSK,
};

static const char *const WIFI_AUTH_NAMES[] = {
    "WPA2_AES",
    "WPA2_MIXED",
    "WPA3_WPA2",
};

typedef struct periodic_state {
    uint32_t last_publish_ms;
    uint32_t last_heartbeat_ms;
} periodic_state_t;

// ─────────────────────────────────────────────────────────────────────────────
// HC-SR04: 초음파 거리 측정
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief HC-SR04로 거리(cm) 측정
 *   1. TRIG 핀에 10µs HIGH 펄스 전송
 *   2. ECHO 핀 HIGH 구간 = 초음파 왕복 시간
 *   3. 거리(cm) = 왕복시간(µs) / 58
 * @return 거리(cm), 타임아웃 시 -1.0f
 */
static float hcsr04_read_cm(void) {
    gpio_put(HCSR04_TRIG_PIN, 0);
    sleep_us(2);
    gpio_put(HCSR04_TRIG_PIN, 1);
    sleep_us(10);
    gpio_put(HCSR04_TRIG_PIN, 0);

    uint32_t t = time_us_32();
    while (!gpio_get(HCSR04_ECHO_PIN)) {
        if (time_us_32() - t > 30000u) return -1.0f;
    }
    uint32_t echo_start = time_us_32();
    while (gpio_get(HCSR04_ECHO_PIN)) {
        if (time_us_32() - echo_start > 30000u) return -1.0f;
    }
    return (float)(time_us_32() - echo_start) / 58.0f;
}

// ─────────────────────────────────────────────────────────────────────────────
// 4×4 키패드
// ─────────────────────────────────────────────────────────────────────────────

static void keypad_init(void) {
    for (int r = 0; r < 4; r++) {
        gpio_init(ROW_PINS[r]);
        gpio_set_dir(ROW_PINS[r], GPIO_OUT);
        gpio_put(ROW_PINS[r], 1);
    }
    for (int c = 0; c < 4; c++) {
        gpio_init(COL_PINS[c]);
        gpio_set_dir(COL_PINS[c], GPIO_IN);
        gpio_pull_up(COL_PINS[c]);
    }
}

/** @brief 눌린 키 반환 (없으면 '\0'), 20ms 디바운스 포함 */
static char keypad_scan(void) {
    for (int r = 0; r < 4; r++) {
        gpio_put(ROW_PINS[r], 0);
        sleep_us(10);
        for (int c = 0; c < 4; c++) {
            if (!gpio_get(COL_PINS[c])) {
                sleep_ms(20);
                if (!gpio_get(COL_PINS[c])) {
                    gpio_put(ROW_PINS[r], 1);
                    while (!gpio_get(COL_PINS[c])) tight_loop_contents();
                    return KEYMAP[r][c];
                }
            }
        }
        gpio_put(ROW_PINS[r], 1);
    }
    return '\0';
}

static uint32_t now_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

// ─────────────────────────────────────────────────────────────────────────────
// 푸시업 카운터 로직
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief 거리 값으로 푸시업 1회 완료 여부 판정
 *   DOWN 진입 시각을 g_rep_down_ms에 기록 (속도 계산용)
 * @return 1: 1회 완료, 0: 대기 중
 */
static int count_pushup_rep(float cm, uint32_t now_ms) {
    if (g_pushup_pos == POS_UP && cm < PUSHUP_DOWN_CM) {
        g_pushup_pos = POS_DOWN;
        g_rep_down_ms = now_ms;
    } else if (g_pushup_pos == POS_DOWN && cm > PUSHUP_UP_CM) {
        g_pushup_pos = POS_UP;
        return 1;
    }
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// MQTT
// ─────────────────────────────────────────────────────────────────────────────

static void mqtt_connection_cb(mqtt_client_t *client, void *arg,
                               mqtt_connection_status_t status) {
    (void)client; (void)arg;
    if (status == MQTT_CONNECT_ACCEPTED) {
        printf("[MQTT] 브로커 연결 성공\n");
        g_mqtt_ready = true;
    } else {
        printf("[MQTT] 연결 실패 (status=%d)\n", status);
        g_mqtt_ready = false;
    }
}

static void mqtt_pub_cb(void *arg, err_t result) {
    (void)arg;
    if (result != ERR_OK) printf("[MQTT] Publish 오류: %d\n", result);
}

static void mqtt_send(const char *topic, const char *payload) {
    if (!g_mqtt_ready || !g_mqtt_client) return;
    mqtt_publish(g_mqtt_client, topic, payload,
                 (uint16_t)strlen(payload), 0, 0, mqtt_pub_cb, NULL);
}

static bool connect_wifi_with_fallbacks(void) {
    for (size_t i = 0; i < sizeof(WIFI_AUTH_MODES) / sizeof(WIFI_AUTH_MODES[0]); i++) {
        printf("[WiFi] %s 연결 시도...\n", WIFI_AUTH_NAMES[i]);
        int err = cyw43_arch_wifi_connect_timeout_ms(
            WIFI_SSID, WIFI_PASSWORD, WIFI_AUTH_MODES[i], 15000
        );
        if (err == 0) {
            printf("[WiFi] 연결 성공 (%s)\n", WIFI_AUTH_NAMES[i]);
            return true;
        }
        printf("[WiFi] 연결 실패 (%s, err=%d)\n", WIFI_AUTH_NAMES[i], err);
        sleep_ms(500);
    }
    return false;
}

static void mqtt_connect_broker(void) {
    ip_addr_t broker;
    if (!ip4addr_aton(MQTT_BROKER_IP, &broker)) {
        printf("[MQTT] 잘못된 브로커 IP: %s\n", MQTT_BROKER_IP);
        return;
    }
    g_mqtt_client = mqtt_client_new();
    if (!g_mqtt_client) { printf("[MQTT] 클라이언트 생성 실패\n"); return; }

    struct mqtt_connect_client_info_t ci = {
        .client_id   = "fitpico_sensor_a",
        .client_user = NULL,
        .client_pass = NULL,
        .keep_alive  = 60,
        .will_topic  = NULL,
    };
    err_t err = mqtt_client_connect(g_mqtt_client, &broker, MQTT_BROKER_PORT,
                                    mqtt_connection_cb, NULL, &ci);
    if (err != ERR_OK) printf("[MQTT] 연결 요청 실패: %d\n", err);
}

static void init_sensor_gpio(void) {
    gpio_init(HCSR04_TRIG_PIN);
    gpio_set_dir(HCSR04_TRIG_PIN, GPIO_OUT);
    gpio_put(HCSR04_TRIG_PIN, 0);

    gpio_init(HCSR04_ECHO_PIN);
    gpio_set_dir(HCSR04_ECHO_PIN, GPIO_IN);

    gpio_init(PIR_PIN);
    gpio_set_dir(PIR_PIN, GPIO_IN);

    keypad_init();
}

// ─────────────────────────────────────────────────────────────────────────────
// 키 입력 처리
// ─────────────────────────────────────────────────────────────────────────────

static void handle_key(char key, uint32_t now_ms) {
    char buf[64];
    switch (key) {
        case 'A':   // 시작 / 재시작
            // 세트 완료 후 재시작이면 휴식 시간 publish
            if (g_set_end_ms > 0) {
                uint32_t rest_sec = (now_ms - g_set_end_ms) / 1000;
                snprintf(buf, sizeof(buf),
                         "{\"set\":%d,\"rest_sec\":%lu}", g_sets, (unsigned long)rest_sec);
                mqtt_send(TOPIC_REST, buf);
                printf("[REST] 세트 %d 후 휴식: %lu초\n", g_sets, (unsigned long)rest_sec);
                g_set_end_ms = 0;
            }
            g_active     = true;
            g_pushup_pos = POS_UP;
            printf("[KEY] 푸시업 시작\n");
            break;

        case 'D':   // 정지 및 세션 초기화
            g_active     = false;
            g_reps       = 0;
            g_sets       = 0;
            g_set_end_ms = 0;
            printf("[KEY] 정지 및 초기화\n");
            break;

        case '#':   // 세트 수동 완료
            if (g_active) {
                g_sets++;
                g_daily_sets++;
                g_reps       = 0;
                g_set_end_ms = now_ms;
                g_active     = false;
                printf("[KEY] 세트 %d 수동 완료\n", g_sets);
            }
            break;

        default:
            break;
    }
}

static void handle_pir_activity(uint32_t now_ms) {
    bool pir = (bool)gpio_get(PIR_PIN);
    if (pir) {
        g_last_pir_ms = now_ms;
        if (!g_active) {
            g_active = true;
            printf("[PIR] 움직임 감지 — 자동 시작\n");
        }
    }

    if (g_active && g_last_pir_ms > 0 &&
        (now_ms - g_last_pir_ms) > PIR_TIMEOUT_MS) {
        g_active = false;
        g_last_pir_ms = 0;
        printf("[PIR] %d초간 움직임 없음 — 자동 정지\n", PIR_TIMEOUT_MS / 1000);
    }
}

static void publish_speed_result(uint32_t speed_ms, const char *warn) {
    char buf[64];
    snprintf(buf, sizeof(buf),
             "{\"rep\":%d,\"speed_ms\":%lu,\"warn\":\"%s\"}",
             g_reps, (unsigned long)speed_ms, warn);
    mqtt_send(TOPIC_SPEED, buf);
}

static void handle_pushup_detection(uint32_t now_ms) {
    if (!g_active) return;

    float cm = hcsr04_read_cm();
    if (cm <= 0.0f || !count_pushup_rep(cm, now_ms)) return;

    g_reps++;
    g_daily_reps++;

    uint32_t speed_ms = now_ms - g_rep_down_ms;
    const char *warn = "ok";
    if (speed_ms < REP_TOO_FAST_MS) warn = "fast";
    else if (speed_ms > REP_TOO_SLOW_MS) warn = "slow";

    printf("[REP] 푸시업 %d회 (%.1fcm) 속도=%lums [%s]\n",
           g_reps, cm, (unsigned long)speed_ms, warn);
    publish_speed_result(speed_ms, warn);
    if (warn[0] != 'o') {
        printf("[SPEED] 속도 경고: %s\n", warn);
    }

    if (g_reps < REPS_PER_SET) return;

    g_sets++;
    g_daily_sets++;
    g_reps = 0;
    g_set_end_ms = now_ms;
    g_active = false;
    printf("[SET] %d/%d 세트 완료! A키로 다음 세트 시작\n", g_sets, TARGET_SETS);

    if (g_sets >= TARGET_SETS) {
        printf("[DONE] 목표 달성! 수고하셨습니다.\n");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// MQTT 데이터 전송
// ─────────────────────────────────────────────────────────────────────────────

static void publish_session_state(void) {
    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"mode\":\"pushup\",\"reps\":%d,\"sets\":%d,\"active\":%s}",
             g_reps, g_sets, g_active ? "true" : "false");
    mqtt_send(TOPIC_COUNT, buf);
}

static void publish_daily_totals(void) {
    char buf[96];
    snprintf(buf, sizeof(buf),
             "{\"total_reps\":%d,\"total_sets\":%d}",
             g_daily_reps, g_daily_sets);
    mqtt_send(TOPIC_DAILY, buf);
}

static void publish_all(void) {
    publish_session_state();
    publish_daily_totals();
    printf("[MQTT] reps=%d sets=%d | daily reps=%d sets=%d\n",
           g_reps, g_sets, g_daily_reps, g_daily_sets);
}

static void publish_heartbeat(void) {
    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"device\":\"sensor\",\"online\":true,\"active\":%s,\"reps\":%d,\"sets\":%d}",
             g_active ? "true" : "false", g_reps, g_sets);
    mqtt_send(TOPIC_SENSOR_STATUS, buf);
}

static void publish_periodic_updates(uint32_t current_ms, periodic_state_t *state) {
    if (!g_mqtt_ready) return;

    if ((current_ms - state->last_publish_ms) > MQTT_PUBLISH_INTERVAL_MS) {
        publish_all();
        state->last_publish_ms = current_ms;
    }

    if ((current_ms - state->last_heartbeat_ms) > DEVICE_HEARTBEAT_INTERVAL_MS) {
        publish_heartbeat();
        state->last_heartbeat_ms = current_ms;
    }
}

static void poll_inputs_and_motion(uint32_t current_ms) {
    char key = keypad_scan();
    if (key != '\0') {
        handle_key(key, current_ms);
    }

    handle_pir_activity(current_ms);
    handle_pushup_detection(current_ms);
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main(void) {
    stdio_init_all();
    sleep_ms(2000);

    printf("=== FitPico Sensor Node (팀원 A) — 푸시업 ===\n");
    printf("브로커: %s:%d\n", MQTT_BROKER_IP, MQTT_BROKER_PORT);

    init_sensor_gpio();

    if (cyw43_arch_init()) {
        printf("[WiFi] 초기화 실패\n");
        return 1;
    }
    cyw43_arch_enable_sta_mode();
    printf("[WiFi] %s 연결 중...\n", WIFI_SSID);
    if (!connect_wifi_with_fallbacks()) {
        printf("[WiFi] 연결 실패 — MQTT 비활성\n");
    } else {
        printf("[WiFi] 연결 완료\n");
        mqtt_connect_broker();
        sleep_ms(1000);
    }

    periodic_state_t periodic = {0};

    printf("\n준비 완료. A=시작  D=정지  #=세트완료\n");
    printf("목표: %d세트 x %d회  |  센서: 바닥에 위로 향하게 설치\n\n",
           TARGET_SETS, REPS_PER_SET);

    while (true) {
        cyw43_arch_poll();
        uint32_t current_ms = now_ms();

        poll_inputs_and_motion(current_ms);
        publish_periodic_updates(current_ms, &periodic);

        sleep_ms(50);
    }

    cyw43_arch_deinit();
    return 0;
}
