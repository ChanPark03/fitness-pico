
 /*
 * 표시 레이아웃 (16×2 문자)
 *   Row 0: R:5  S:2/3 ACTV
 *   Row 1: 1200ms[ok]D:45R
 *
 * MQTT 구독 토픽
 *   fitpico/sensor/count   → reps, sets, active
 *   fitpico/sensor/speed   → speed_ms, warn
 *   fitpico/sensor/daily   → total_reps, total_sets
 *
 * 핀 배치
 *   GP4 = SDA (I2C0)
 *   GP5 = SCL (I2C0)
 *   PCF8574 I2C 주소 = 0x27  (점퍼 A0~A2 모두 High; 0x3F 이면 아래 수정)
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
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
#define LCD_RW  0x02   // P1 (항상 0 = 쓰기)
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

static char g_cur_topic[64] = {0};
static char g_payload[128]  = {0};
static int  g_pay_len       = 0;

static int  g_reps        = 0;
static int  g_sets        = 0;
static bool g_active      = false;
static int  g_speed_ms    = 0;
static char g_warn[8]     = "---";
static int  g_daily_reps  = 0;
static int  g_daily_sets  = 0;

static bool g_dirty = true;

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
        g_speed_ms = json_int(g_payload, "speed_ms");
        json_str(g_payload, "warn", g_warn, sizeof(g_warn));
    } else if (strcmp(g_cur_topic, TOPIC_DAILY) == 0) {
        g_daily_reps = json_int(g_payload, "total_reps");
        g_daily_sets = json_int(g_payload, "total_sets");
    }
    g_dirty = true;
}

static void on_sub(void *arg, err_t result) {
    (void)arg;
    if (result != ERR_OK) printf("[MQTT] 구독 실패: %d\n", result);
}

static void on_connect(mqtt_client_t *client, void *arg,
                       mqtt_connection_status_t status) {
    (void)arg;
    if (status != MQTT_CONNECT_ACCEPTED) {
        printf("[MQTT] 연결 실패 (status=%d)\n", status);
        g_mqtt_ready = false;
        return;
    }
    printf("[MQTT] 연결 성공\n");
    g_mqtt_ready = true;
    mqtt_set_inpub_callback(client, on_publish, on_data, NULL);
    mqtt_subscribe(client, TOPIC_COUNT, 0, on_sub, NULL);
    mqtt_subscribe(client, TOPIC_SPEED, 0, on_sub, NULL);
    mqtt_subscribe(client, TOPIC_DAILY, 0, on_sub, NULL);
}

static void mqtt_connect_broker(void) {
    ip_addr_t broker;
    if (!ip4addr_aton(MQTT_BROKER_IP, &broker)) return;
    g_mqtt_client = mqtt_client_new();
    if (!g_mqtt_client) return;
    struct mqtt_connect_client_info_t ci = {
        .client_id = "fitpico_display", .keep_alive = 60,
    };
    mqtt_client_connect(g_mqtt_client, &broker, MQTT_BROKER_PORT,
                        on_connect, NULL, &ci);
}

// ─────────────────────────────────────────────────────────────────────────────
// 디스플레이 갱신  (16×2)
//   Row 0: "R:5  S:2/3 ACTV"   (최대 16자)
//   Row 1: "1200ms[ok]D:45R "  (최대 16자)
// ─────────────────────────────────────────────────────────────────────────────

static void display_update(void) {
    char line[17];  // 16자 + '\0'

    // Row 0: 반복 횟수 / 세트 / 상태
    snprintf(line, sizeof(line), "R:%-2d S:%d/%d %s",
             g_reps, g_sets, TARGET_SETS,
             g_active ? "ACTV" : "IDLE");
    lcd_puts_line(0, line);

    // Row 1: 속도 [경고] 일일누적
    snprintf(line, sizeof(line), "%dms[%s]D:%dR",
             g_speed_ms, g_warn, g_daily_reps);
    lcd_puts_line(1, line);

    g_dirty = false;
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main(void) {
    stdio_init_all();
    sleep_ms(2000);
    printf("=== FitPico Display Node ===\n");

    // I2C 초기화
    i2c_init(LCD_I2C, 100000);  // PCF8574 표준 속도 100 kHz
    gpio_set_function(LCD_SDA, GPIO_FUNC_I2C);
    gpio_set_function(LCD_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(LCD_SDA);
    gpio_pull_up(LCD_SCL);

    lcd_init();
    lcd_puts_line(0, "=== FitPico ===");
    lcd_puts_line(1, "WiFi 연결 중...");

    // WiFi 연결
    if (cyw43_arch_init()) {
        printf("[WiFi] 초기화 실패\n");
        lcd_puts_line(1, "WiFi init fail  ");
        return 1;
    }
    cyw43_arch_enable_sta_mode();
    printf("[WiFi] %s 연결 중...\n", WIFI_SSID);
    if (cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD,
                                           CYW43_AUTH_WPA2_AES_PSK, 15000)) {
        printf("[WiFi] 연결 실패\n");
        lcd_puts_line(1, "WiFi failed     ");
        return 1;
    }
    printf("[WiFi] 연결 완료\n");
    lcd_puts_line(1, "MQTT 연결 중...");

    mqtt_connect_broker();
    sleep_ms(1000);

    display_update();

    while (true) {
        cyw43_arch_poll();
        if (g_dirty) display_update();
        sleep_ms(100);
    }

    cyw43_arch_deinit();
    return 0;
}
