
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

static bool      g_active      = false;
static int       g_reps        = 0;
static int       g_sets        = 0;
static uint32_t  g_set_end_ms  = 0;   // 세트 완료 시각 (휴식 추적)

static float     g_temp        = 0.0f;
static float     g_humidity    = 0.0f;

static Position  g_pushup_pos  = POS_UP;
static uint32_t  g_last_pir_ms = 0;

// 키패드
static const char KEYMAP[4][4] = {
    {'1', '2', '3', 'A'},
    {'4', '5', '6', 'B'},
    {'7', '8', '9', 'C'},
    {'*', '0', '#', 'D'}
};
static const uint ROW_PINS[4] = {KEYPAD_ROW0, KEYPAD_ROW1, KEYPAD_ROW2, KEYPAD_ROW3};
static const uint COL_PINS[4] = {KEYPAD_COL0, KEYPAD_COL1, KEYPAD_COL2, KEYPAD_COL3};

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
// DHT11: 온도 / 습도 읽기 (bit-bang)
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief DHT11 40비트 데이터 읽기
 *   프로토콜: 18ms LOW → 응답(80µs L + 80µs H) → 40비트
 *   비트 판별: HIGH > 28µs → '1', 이하 → '0'
 * @return true: 성공, false: 체크섬 오류 또는 타임아웃
 */
static bool dht11_read(float *temp, float *humidity) {
    uint8_t data[5] = {0};

    gpio_set_dir(DHT11_PIN, GPIO_OUT);
    gpio_put(DHT11_PIN, 0);
    sleep_ms(18);
    gpio_put(DHT11_PIN, 1);
    sleep_us(30);
    gpio_set_dir(DHT11_PIN, GPIO_IN);
    gpio_pull_up(DHT11_PIN);

    uint32_t t = time_us_32();
    while (gpio_get(DHT11_PIN))  { if (time_us_32() - t > 100u) return false; }
    t = time_us_32();
    while (!gpio_get(DHT11_PIN)) { if (time_us_32() - t > 100u) return false; }
    t = time_us_32();
    while (gpio_get(DHT11_PIN))  { if (time_us_32() - t > 100u) return false; }

    for (int i = 0; i < 40; i++) {
        t = time_us_32();
        while (!gpio_get(DHT11_PIN)) { if (time_us_32() - t > 60u) return false; }
        t = time_us_32();
        while (gpio_get(DHT11_PIN))  { if (time_us_32() - t > 80u) return false; }
        data[i / 8] <<= 1;
        if (time_us_32() - t > 28u) data[i / 8] |= 1;
    }

    if (((data[0] + data[1] + data[2] + data[3]) & 0xFF) != data[4]) return false;

    *humidity = (float)data[0] + (float)data[1] * 0.1f;
    *temp     = (float)data[2] + (float)data[3] * 0.1f;
    return true;
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

// ─────────────────────────────────────────────────────────────────────────────
// LED 피드백
// ─────────────────────────────────────────────────────────────────────────────

static void led_blink(int n) {
    for (int i = 0; i < n; i++) {
        gpio_put(LED_PIN, 1); sleep_ms(200);
        gpio_put(LED_PIN, 0); sleep_ms(200);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 푸시업 카운터 로직
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief 거리 값으로 푸시업 1회 완료 여부 판정
 *
 * 상태 머신:
 *   POS_UP → (cm < PUSHUP_DOWN_CM) → POS_DOWN
 *          → (cm > PUSHUP_UP_CM)   → POS_UP + 1 반환
 *
 * @return 1: 1회 완료, 0: 대기 중
 */
static int count_pushup_rep(float cm) {
    if (g_pushup_pos == POS_UP && cm < PUSHUP_DOWN_CM) {
        g_pushup_pos = POS_DOWN;
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

// ─────────────────────────────────────────────────────────────────────────────
// 키 입력 처리
// ─────────────────────────────────────────────────────────────────────────────

static void handle_key(char key) {
    switch (key) {
        case 'A':   // 시작 / 재시작
            g_active      = true;
            g_pushup_pos  = POS_UP;
            printf("[KEY] 푸시업 시작\n");
            led_blink(1);
            break;

        case 'D':   // 정지 및 초기화
            g_active = false;
            g_reps   = 0;
            g_sets   = 0;
            printf("[KEY] 정지 및 초기화\n");
            led_blink(2);
            break;

        case '#':   // 세트 수동 완료
            if (g_active) {
                g_sets++;
                g_reps = 0;
                led_blink(3);
                printf("[KEY] 세트 %d 수동 완료\n", g_sets);
            }
            break;

        default:
            break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// MQTT 데이터 전송
// ─────────────────────────────────────────────────────────────────────────────

static void publish_all(void) {
    char buf[128];

    snprintf(buf, sizeof(buf),
             "{\"mode\":\"pushup\",\"reps\":%d,\"sets\":%d,\"active\":%s}",
             g_reps, g_sets, g_active ? "true" : "false");
    mqtt_send(TOPIC_COUNT, buf);

    snprintf(buf, sizeof(buf), "%.1f", g_temp);
    mqtt_send(TOPIC_TEMP, buf);

    snprintf(buf, sizeof(buf), "%.1f", g_humidity);
    mqtt_send(TOPIC_HUMIDITY, buf);

    printf("[MQTT] reps=%d sets=%d temp=%.1f hum=%.1f\n",
           g_reps, g_sets, g_temp, g_humidity);
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main(void) {
    stdio_init_all();
    sleep_ms(2000);

    printf("=== FitPico Sensor Node (팀원 A) — 푸시업 ===\n");
    printf("브로커: %s:%d\n", MQTT_BROKER_IP, MQTT_BROKER_PORT);

    // GPIO 초기화
    gpio_init(HCSR04_TRIG_PIN); gpio_set_dir(HCSR04_TRIG_PIN, GPIO_OUT);
    gpio_put(HCSR04_TRIG_PIN, 0);
    gpio_init(HCSR04_ECHO_PIN); gpio_set_dir(HCSR04_ECHO_PIN, GPIO_IN);
    gpio_init(PIR_PIN);         gpio_set_dir(PIR_PIN, GPIO_IN);
    gpio_init(LED_PIN);         gpio_set_dir(LED_PIN, GPIO_OUT);
    gpio_put(LED_PIN, 0);
    keypad_init();

    // WiFi 초기화
    if (cyw43_arch_init()) { printf("[WiFi] 초기화 실패\n"); return 1; }
    cyw43_arch_enable_sta_mode();
    printf("[WiFi] %s 연결 중...\n", WIFI_SSID);
    if (cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD,
                                           CYW43_AUTH_WPA2_AES_PSK, 15000)) {
        printf("[WiFi] 연결 실패 — MQTT 비활성\n");
    } else {
        printf("[WiFi] 연결 완료\n");
        mqtt_connect_broker();
        sleep_ms(1000);
    }

    uint32_t last_dht_ms = 0;
    uint32_t last_pub_ms = 0;

    printf("\n준비 완료. A=시작  D=정지  #=세트완료\n");
    printf("목표: %d세트 x %d회  |  센서: 바닥에 위로 향하게 설치\n\n",
           TARGET_SETS, REPS_PER_SET);

    while (true) {
        cyw43_arch_poll();
        uint32_t now = to_ms_since_boot(get_absolute_time());

        // 키패드
        char key = keypad_scan();
        if (key != '\0') handle_key(key);

        // PIR: 움직임 감지 → 자동 시작
        bool pir = (bool)gpio_get(PIR_PIN);
        if (pir) {
            g_last_pir_ms = now;
            if (!g_active) {
                g_active = true;
                printf("[PIR] 움직임 감지 — 자동 시작\n");
            }
        }

        // PIR: 30초 무동작 → 자동 정지
        if (g_active && g_last_pir_ms > 0 &&
            (now - g_last_pir_ms) > PIR_TIMEOUT_MS) {
            g_active      = false;
            g_last_pir_ms = 0;
            printf("[PIR] %d초간 움직임 없음 — 자동 정지\n", PIR_TIMEOUT_MS / 1000);
        }

        // 푸시업 횟수 카운트
        if (g_active) {
            float cm = hcsr04_read_cm();
            if (cm > 0.0f && count_pushup_rep(cm)) {
                g_reps++;
                printf("[REP] 푸시업 %d회 (%.1fcm)\n", g_reps, cm);

                gpio_put(LED_PIN, 1); sleep_ms(80); gpio_put(LED_PIN, 0);

                if (g_reps >= REPS_PER_SET) {
                    g_sets++;
                    g_reps       = 0;
                    g_set_end_ms = now;
                    g_active     = false;
                    led_blink(3);
                    printf("[SET] %d/%d 세트 완료! A키로 다음 세트 시작\n",
                           g_sets, TARGET_SETS);
                    if (g_sets >= TARGET_SETS) {
                        printf("[DONE] 목표 달성! 수고하셨습니다.\n");
                        led_blink(5);
                    }
                }
            }
        }

        // DHT11 온습도 (5초마다)
        if (now - last_dht_ms > DHT_READ_INTERVAL_MS) {
            float t, h;
            if (dht11_read(&t, &h)) {
                g_temp = t; g_humidity = h;
                printf("[DHT11] %.1f°C  %.1f%%\n", t, h);
            }
            last_dht_ms = now;
        }

        // MQTT publish (2초마다)
        if (g_mqtt_ready && (now - last_pub_ms > MQTT_PUBLISH_INTERVAL_MS)) {
            publish_all();
            last_pub_ms = now;
        }

        sleep_ms(50);
    }

    cyw43_arch_deinit();
    return 0;
}
