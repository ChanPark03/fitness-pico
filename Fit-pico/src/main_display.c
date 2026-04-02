
/*
 * 표시 레이아웃 (16×2 문자)
 *   Row 0: R:5  S:2/3 ACTV
 *   Row 1: 1200[ok]TD:45
 *
 * MQTT 구독 토픽
 *   fitpico/sensor/count   → reps, sets, active, tracking
 *   fitpico/sensor/speed   → speed_ms, warn
 *   fitpico/sensor/daily   → total_reps, total_sets
 *
 * MQTT 발행 토픽
 *   fitpico/display/status → display heartbeat
 *
 * 핀 배치
 *   GP4 = SDA (I2C0)
 *   GP5 = SCL (I2C0)
 *   GP18 = Buzzer
 *   PCF8574 I2C 주소 = 0x27  (점퍼 A0~A2 모두 High; 0x3F 이면 아래 수정)
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/gpio.h"
#include "hardware/i2c.h"

#include "lwip/apps/mqtt.h"
#include "lwip/ip_addr.h"

#include "config.h"

// ─────────────────────────────────────────────────────────────────────────────
// I2C LCD (PCF8574 백팩) 설정
// ─────────────────────────────────────────────────────────────────────────────

#define LCD_I2C   i2c0
#define LCD_SDA   4
#define LCD_SCL   5
#define LCD_ADDR  0x27   // PCF8574: 0x27 / PCF8574A: 0x3F

// PCF8574 → HD44780 비트 배치
#define LCD_RS  0x01   // P0
#define LCD_EN  0x04   // P2
#define LCD_BL  0x08   // P3 백라이트
// D4~D7 = P4~P7 (상위 니블)

// ─────────────────────────────────────────────────────────────────────────────
// LCD 드라이버 (HD44780 4-bit via PCF8574)
// ─────────────────────────────────────────────────────────────────────────────

static void lcd_write_pcf(uint8_t b) {
    i2c_write_blocking(LCD_I2C, LCD_ADDR, &b, 1, false);
}

/** EN 펄스 (data: D7..D4 + RS + BL, EN 제외) */
static void lcd_pulse_en(uint8_t data) {
    lcd_write_pcf(data | LCD_EN);   // EN ↑
    sleep_us(1);
    lcd_write_pcf(data & ~LCD_EN);  // EN ↓
    sleep_us(50);
}

/** 4비트 니블 전송 (nibble: 하위 4비트, flags: LCD_RS 또는 0) */
static void lcd_send_nibble(uint8_t nibble, uint8_t flags) {
    uint8_t data = ((nibble & 0x0F) << 4) | LCD_BL | flags;
    lcd_pulse_en(data);
}

/** HD44780 명령(RS=0) 전송 */
static void lcd_cmd(uint8_t cmd) {
    lcd_send_nibble(cmd >> 4,   0);
    lcd_send_nibble(cmd & 0x0F, 0);
    // Clear(0x01) / Return Home(0x02)는 실행 시간 길어 2ms 대기
    if (cmd <= 0x03) sleep_ms(2);
    else              sleep_us(100);
}

/** HD44780 데이터(RS=1) 전송 */
static void lcd_putc(char c) {
    lcd_send_nibble((uint8_t)c >> 4,   LCD_RS);
    lcd_send_nibble((uint8_t)c & 0x0F, LCD_RS);
    sleep_us(50);
}

/** HD44780 4-bit 모드 초기화 */
static void lcd_init(void) {
    sleep_ms(50);   // 전원 안정 대기

    // 8-bit 모드에서 0x3 세 번 → 4-bit 모드 진입
    uint8_t b = (0x3 << 4) | LCD_BL;
    lcd_pulse_en(b); sleep_ms(5);
    lcd_pulse_en(b); sleep_us(150);
    lcd_pulse_en(b); sleep_us(150);

    // 4-bit 모드 선택
    b = (0x2 << 4) | LCD_BL;
    lcd_pulse_en(b); sleep_us(150);

    // 이후 전체 명령 전송
    lcd_cmd(0x28);  // Function set: 4-bit, 2행, 5×8
    lcd_cmd(0x08);  // Display off
    lcd_cmd(0x01);  // Clear display
    lcd_cmd(0x06);  // Entry mode: 커서 오른쪽 이동
    lcd_cmd(0x0C);  // Display on, 커서 off, 깜박임 off
}

/** 커서 이동 (row: 0/1, col: 0~15) */
static void lcd_set_cursor(uint8_t row, uint8_t col) {
    uint8_t addr = (row == 0 ? 0x00 : 0x40) + col;
    lcd_cmd(0x80 | addr);
}

/** 지정 행에 문자열 출력 (16칸 미만이면 공백으로 채움) */
static void lcd_puts_line(uint8_t row, const char *s) {
    lcd_set_cursor(row, 0);
    int col = 0;
    for (; *s && col < 16; s++, col++) lcd_putc(*s);
    for (; col < 16; col++) lcd_putc(' ');
}

// ─────────────────────────────────────────────────────────────────────────────
// 전역 상태
// ─────────────────────────────────────────────────────────────────────────────

static mqtt_client_t *g_mqtt_client = NULL;
static bool           g_mqtt_ready  = false;
static bool           g_mqtt_connecting = false;
static uint32_t       g_last_mqtt_attempt_ms = 0;

static char g_cur_topic[64] = {0};
static char g_payload[128]  = {0};
static int  g_pay_len       = 0;

static int  g_reps        = 0;
static int  g_sets        = 0;
static bool g_active      = false;
static bool g_tracking    = false;
static int  g_speed_ms    = 0;
static int  g_speed_rep   = 0;
static char g_warn[8]     = "---";
static int  g_daily_reps  = 0;
static int  g_daily_sets  = 0;

static bool g_dirty = true;
static bool g_wifi_ready = false;

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

typedef struct display_state {
    int reps;
    int sets;
    bool active;
    bool tracking;
    int speed_ms;
    const char *warn;
    int daily_reps;
} display_state_t;

typedef struct session_snapshot {
    int reps;
    int sets;
    bool active;
    bool tracking;
} session_snapshot_t;

#define BUZZER_TONE_HZ 2400
#define BUZZER_BEEP_MS 120
#define BUZZER_GAP_MS  120

static uint32_t now_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

static const char *mqtt_status_name(mqtt_connection_status_t status) {
    switch (status) {
        case MQTT_CONNECT_ACCEPTED: return "ACCEPTED";
        case MQTT_CONNECT_REFUSED_PROTOCOL_VERSION: return "REFUSED_PROTOCOL_VERSION";
        case MQTT_CONNECT_REFUSED_IDENTIFIER: return "REFUSED_IDENTIFIER";
        case MQTT_CONNECT_REFUSED_SERVER: return "REFUSED_SERVER";
        case MQTT_CONNECT_REFUSED_USERNAME_PASS: return "REFUSED_USERNAME_PASS";
        case MQTT_CONNECT_REFUSED_NOT_AUTHORIZED_: return "REFUSED_NOT_AUTHORIZED";
        case MQTT_CONNECT_DISCONNECTED: return "DISCONNECTED";
        case MQTT_CONNECT_TIMEOUT: return "TIMEOUT";
        default: return "UNKNOWN";
    }
}

static void init_lcd_bus(void) {
    i2c_init(LCD_I2C, 100000);
    gpio_set_function(LCD_SDA, GPIO_FUNC_I2C);
    gpio_set_function(LCD_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(LCD_SDA);
    gpio_pull_up(LCD_SCL);

    lcd_init();
}

static void init_display_outputs(void) {
    gpio_init(BUZZER_PIN);
    gpio_set_dir(BUZZER_PIN, GPIO_OUT);
    gpio_put(BUZZER_PIN, 0);

    init_lcd_bus();
}

static display_state_t current_display_state(void) {
    display_state_t state = {
        .reps = g_reps,
        .sets = g_sets,
        .active = g_active,
        .tracking = g_tracking,
        .speed_ms = g_speed_ms,
        .warn = g_warn,
        .daily_reps = g_daily_reps,
    };
    return state;
}

static session_snapshot_t current_session_snapshot(void) {
    session_snapshot_t snapshot = {
        .reps = g_reps,
        .sets = g_sets,
        .active = g_active,
        .tracking = g_tracking,
    };
    return snapshot;
}

static void mqtt_send(const char *topic, const char *payload) {
    if (!g_mqtt_ready || !g_mqtt_client) return;
    mqtt_publish(g_mqtt_client, topic, payload,
                 (uint16_t)strlen(payload), 0, 0, NULL, NULL);
}

static void buzzer_tone(uint32_t frequency_hz, uint32_t duration_ms) {
    if (frequency_hz == 0 || duration_ms == 0) return;

    const uint32_t half_period_us = 500000u / frequency_hz;
    const uint32_t cycles = (frequency_hz * duration_ms) / 1000u;

    for (uint32_t i = 0; i < cycles; i++) {
        gpio_put(BUZZER_PIN, 1);
        sleep_us(half_period_us);
        gpio_put(BUZZER_PIN, 0);
        sleep_us(half_period_us);
    }
}

static void buzzer_beep(int count) {
    for (int i = 0; i < count; i++) {
        buzzer_tone(BUZZER_TONE_HZ, BUZZER_BEEP_MS);
        sleep_ms(BUZZER_GAP_MS);
    }
}

static bool connect_wifi_with_fallbacks(void) {
    for (size_t i = 0; i < sizeof(WIFI_AUTH_MODES) / sizeof(WIFI_AUTH_MODES[0]); i++) {
        printf("[WiFi] Trying %s...\n", WIFI_AUTH_NAMES[i]);
        int err = cyw43_arch_wifi_connect_timeout_ms(
            WIFI_SSID, WIFI_PASSWORD, WIFI_AUTH_MODES[i], 15000
        );
        if (err == 0) {
            printf("[WiFi] Connected (%s)\n", WIFI_AUTH_NAMES[i]);
            return true;
        }
        printf("[WiFi] Failed (%s, err=%d)\n", WIFI_AUTH_NAMES[i], err);
        sleep_ms(500);
    }
    return false;
}

static void publish_display_heartbeat(void) {
    char buf[160];
    snprintf(buf, sizeof(buf),
             "{\"device\":\"display\",\"online\":true,\"wifi\":%s,\"mqtt\":%s,\"active\":%s,\"tracking\":%s,\"warn\":\"%s\"}",
             g_wifi_ready ? "true" : "false",
             g_mqtt_ready ? "true" : "false",
             g_active ? "true" : "false",
             g_tracking ? "true" : "false",
             g_warn);
    mqtt_send(TOPIC_DISPLAY_STATUS, buf);
}

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

static bool json_bool(const char *json, const char *key, bool default_value) {
    char search[32];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return default_value;
    p += strlen(search);
    while (*p == ' ') p++;
    if (strncmp(p, "true", 4) == 0) return true;
    if (strncmp(p, "false", 5) == 0) return false;
    return default_value;
}

static void handle_session_output(session_snapshot_t previous,
                                  session_snapshot_t current) {
    if (!previous.active && current.active) {
        buzzer_beep(1);
        return;
    }

    if (previous.active && !current.active &&
        current.reps == 0 && current.sets == 0 && !current.tracking) {
        buzzer_beep(2);
        return;
    }

    if (current.sets > previous.sets && !current.active && current.reps == 0) {
        buzzer_beep(3);
        if (current.sets >= TARGET_SETS) {
            buzzer_beep(5);
        }
    }
}

static void handle_speed_output(int previous_speed_rep, int current_speed_rep) {
    if (current_speed_rep <= 0 || current_speed_rep == previous_speed_rep) return;
    buzzer_beep(1);
}

// ─────────────────────────────────────────────────────────────────────────────
// MQTT 콜백
// ─────────────────────────────────────────────────────────────────────────────

static void on_publish(void *arg, const char *topic, u32_t tot_len) {
    (void)arg; (void)tot_len;
    strncpy(g_cur_topic, topic, sizeof(g_cur_topic) - 1);
    g_cur_topic[sizeof(g_cur_topic) - 1] = '\0';
    g_pay_len = 0;
}

static void on_data(void *arg, const u8_t *data, u16_t len, u8_t flags) {
    (void)arg;
    session_snapshot_t previous_session = current_session_snapshot();
    int previous_speed_rep = g_speed_rep;

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
        g_active = json_bool(g_payload, "active", false);
        g_tracking = json_bool(g_payload, "tracking", false);
        handle_session_output(previous_session, current_session_snapshot());
    } else if (strcmp(g_cur_topic, TOPIC_SPEED) == 0) {
        g_speed_rep = json_int(g_payload, "rep");
        g_speed_ms = json_int(g_payload, "speed_ms");
        json_str(g_payload, "warn", g_warn, sizeof(g_warn));
        handle_speed_output(previous_speed_rep, g_speed_rep);
    } else if (strcmp(g_cur_topic, TOPIC_DAILY) == 0) {
        g_daily_reps = json_int(g_payload, "total_reps");
        g_daily_sets = json_int(g_payload, "total_sets");
    }
    g_dirty = true;
}

static void render_display(void) {
    char line[17];
    display_state_t state = current_display_state();
    const char *status = state.active ? "ACTV" : (state.tracking ? "PAUS" : "MONI");

    snprintf(line, sizeof(line), "R:%-2d S:%d/%d %s",
             state.reps, state.sets, TARGET_SETS,
             status);
    lcd_puts_line(0, line);

    snprintf(line, sizeof(line), "%d[%s]TD:%d",
             state.speed_ms, state.warn, state.daily_reps);
    lcd_puts_line(1, line);
}

static void on_sub(void *arg, err_t result) {
    (void)arg;
    if (result != ERR_OK) printf("[MQTT] Subscribe failed: %d\n", result);
}

static void on_connect(mqtt_client_t *client, void *arg,
                       mqtt_connection_status_t status) {
    (void)arg;
    g_mqtt_connecting = false;
    if (status != MQTT_CONNECT_ACCEPTED) {
        printf("[MQTT] Connection failed (%s, %d)\n",
               mqtt_status_name(status), status);
        g_mqtt_ready = false;
        return;
    }
    printf("[MQTT] Connected (%s, %d)\n",
           mqtt_status_name(status), status);
    g_mqtt_ready = true;
    mqtt_set_inpub_callback(client, on_publish, on_data, NULL);
    mqtt_subscribe(client, TOPIC_COUNT, 0, on_sub, NULL);
    mqtt_subscribe(client, TOPIC_SPEED, 0, on_sub, NULL);
    mqtt_subscribe(client, TOPIC_DAILY, 0, on_sub, NULL);
}

static void mqtt_connect_broker(void) {
    ip_addr_t broker;
    g_last_mqtt_attempt_ms = now_ms();
    printf("[MQTT] Connect request -> %s:%d\n", MQTT_BROKER_IP, MQTT_BROKER_PORT);
    if (!ip4addr_aton(MQTT_BROKER_IP, &broker)) {
        printf("[MQTT] Invalid broker IP: %s\n", MQTT_BROKER_IP);
        return;
    }
    if (!g_mqtt_client) {
        g_mqtt_client = mqtt_client_new();
    }
    if (!g_mqtt_client) {
        printf("[MQTT] Client allocation failed\n");
        return;
    }
    struct mqtt_connect_client_info_t ci = {
        .client_id = "fitpico_display", .keep_alive = 60,
    };
    g_mqtt_connecting = true;
    err_t err = mqtt_client_connect(g_mqtt_client, &broker, MQTT_BROKER_PORT,
                                    on_connect, NULL, &ci);
    printf("[MQTT] Connect request result: %d\n", err);
    if (err != ERR_OK) {
        g_mqtt_connecting = false;
        printf("[MQTT] Connect request failed immediately: %d\n", err);
    }
}

static void maintain_mqtt_connection(uint32_t current_ms) {
    if (!g_wifi_ready) return;
    if (g_mqtt_ready && g_mqtt_client && !mqtt_client_is_connected(g_mqtt_client)) {
        printf("[MQTT] Client lost connection, scheduling reconnect\n");
        g_mqtt_ready = false;
    }
    if (g_mqtt_ready || g_mqtt_connecting) return;
    if ((current_ms - g_last_mqtt_attempt_ms) < MQTT_RECONNECT_INTERVAL_MS) return;
    mqtt_connect_broker();
}

// ─────────────────────────────────────────────────────────────────────────────
// 디스플레이 갱신  (16×2)
//   Row 0: "R:5  S:2/3 ACTV"   (최대 16자)
//   Row 1: "1200[ok]TD:45  "   (최대 16자)
// ─────────────────────────────────────────────────────────────────────────────

static void display_update(void) {
    render_display();
    g_dirty = false;
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main(void) {
    stdio_init_all();
    sleep_ms(2000);
    printf("=== FitPico Display Node ===\n");
    printf("MQTT 브로커: %s:%d\n", MQTT_BROKER_IP, MQTT_BROKER_PORT);

    init_display_outputs();
    lcd_puts_line(0, "=== FitPico ===");
    lcd_puts_line(1, "WiFi connecting");

    if (cyw43_arch_init()) {
        printf("[WiFi] Init failed\n");
        lcd_puts_line(1, "WiFi init failed");
        return 1;
    }
    cyw43_arch_enable_sta_mode();
    printf("[WiFi] Connecting to %s...\n", WIFI_SSID);
    if (!connect_wifi_with_fallbacks()) {
        printf("[WiFi] Connection failed\n");
        lcd_puts_line(1, "WiFi failed     ");
        return 1;
    }
    printf("[WiFi] Connection complete\n");
    g_wifi_ready = true;
    lcd_puts_line(1, "WiFi connected ");

    mqtt_connect_broker();
    sleep_ms(1000);

    display_update();
    uint32_t last_heartbeat_ms = 0;

    while (true) {
        cyw43_arch_poll();
        uint32_t now = now_ms();
        maintain_mqtt_connection(now);
        if (g_dirty) display_update();
        if (g_mqtt_ready && (now - last_heartbeat_ms > DEVICE_HEARTBEAT_INTERVAL_MS)) {
            publish_display_heartbeat();
            last_heartbeat_ms = now;
        }
        sleep_ms(100);
    }

    cyw43_arch_deinit();
    return 0;
}
