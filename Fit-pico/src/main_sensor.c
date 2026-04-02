/*
 *   - HC-SR04  : 초음파 거리 측정
 *   - MFRC522  : RFID 사용자 인식
 *   - Web/MQTT : 운동 저장 시작/종료 제어
 *
 * 핀 배치 (config.h 참조)
 *   GP14=TRIG  GP15=ECHO
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"

#include "lwip/apps/mqtt.h"
#include "lwip/ip_addr.h"

#include "config.h"

#define DEFAULT_REST_SEC 45

// ─── MFRC522 레지스터 ────────────────────────────────────────────────────────
#define MFRC_CommandReg    0x01
#define MFRC_ComIrqReg     0x04
#define MFRC_ErrorReg      0x06
#define MFRC_FIFODataReg   0x09
#define MFRC_FIFOLevelReg  0x0A
#define MFRC_BitFramingReg 0x0D
#define MFRC_ModeReg       0x11
#define MFRC_TxControlReg  0x14
#define MFRC_TxASKReg      0x15
#define MFRC_TModeReg      0x2A
#define MFRC_TPrescalerReg 0x2B
#define MFRC_TReloadRegH   0x2C
#define MFRC_TReloadRegL   0x2D

#define PCD_Idle       0x00
#define PCD_Transceive 0x0C
#define PCD_SoftReset  0x0F

#define PICC_REQA      0x26
#define PICC_ANTICOLL  0x93

typedef enum { POS_UP = 0, POS_DOWN = 1 } Position;

typedef struct periodic_state {
    uint32_t last_publish_ms;
    uint32_t last_heartbeat_ms;
} periodic_state_t;

static mqtt_client_t *g_mqtt_client = NULL;
static bool           g_mqtt_ready  = false;

static char g_cur_topic[64] = {0};
static char g_payload[128]  = {0};
static int  g_pay_len       = 0;

static bool      g_sensing_enabled   = true;
static bool      g_tracking_enabled  = false;
static bool      g_manual_stop       = true;
static bool      g_active            = false;
static int       g_reps              = 0;
static int       g_sets              = 0;
static int       g_daily_reps        = 0;
static int       g_daily_sets        = 0;
static uint32_t  g_session_active_ms = 0;
static uint32_t  g_daily_active_ms   = 0;
static uint32_t  g_rep_down_ms       = 0;
static uint32_t  g_set_end_ms        = 0;
static Position  g_pushup_pos        = POS_UP;

static char     g_last_uid[12]     = {0};
static uint32_t g_last_uid_ms      = 0;

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

static uint32_t now_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

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
    if (!p) {
        out[0] = '\0';
        return;
    }
    p += strlen(search);
    int i = 0;
    while (*p && *p != '"' && i < sz - 1) out[i++] = *p++;
    out[i] = '\0';
}

static uint32_t session_active_sec(void) {
    return g_session_active_ms / 1000u;
}

static uint32_t daily_active_sec(void) {
    return g_daily_active_ms / 1000u;
}

static bool session_complete(void) {
    return g_sets >= TARGET_SETS;
}

static bool session_empty(void) {
    return g_reps == 0 && g_sets == 0;
}

static bool should_start_new_session(void) {
    return session_empty() || session_complete();
}

static void reset_motion_state(void) {
    g_pushup_pos = POS_UP;
    g_rep_down_ms = 0;
}

static void reset_session_progress(void) {
    g_reps = 0;
    g_sets = 0;
    g_session_active_ms = 0;
    g_set_end_ms = 0;
    reset_motion_state();
}

static void mqtt_pub_cb(void *arg, err_t result) {
    (void)arg;
    if (result != ERR_OK) printf("[MQTT] Publish error: %d\n", result);
}

static void mqtt_send(const char *topic, const char *payload) {
    if (!g_mqtt_ready || !g_mqtt_client) return;
    mqtt_publish(g_mqtt_client, topic, payload,
                 (uint16_t)strlen(payload), 0, 0, mqtt_pub_cb, NULL);
}

static void publish_rest_state(int rest_sec) {
    char buf[64];
    snprintf(buf, sizeof(buf),
             "{\"set\":%d,\"rest_sec\":%d}",
             g_sets, rest_sec);
    mqtt_send(TOPIC_REST, buf);
}

static void publish_session_state(void) {
    char buf[192];
    snprintf(buf, sizeof(buf),
             "{\"mode\":\"pushup\",\"reps\":%d,\"sets\":%d,\"active\":%s,"
             "\"tracking\":%s,\"sensing\":%s,\"session_active_sec\":%lu}",
             g_reps, g_sets,
             g_active ? "true" : "false",
             g_tracking_enabled ? "true" : "false",
             g_sensing_enabled ? "true" : "false",
             (unsigned long)session_active_sec());
    mqtt_send(TOPIC_COUNT, buf);
}

static void publish_daily_totals(void) {
    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"total_reps\":%d,\"total_sets\":%d,\"daily_active_sec\":%lu}",
             g_daily_reps, g_daily_sets,
             (unsigned long)daily_active_sec());
    mqtt_send(TOPIC_DAILY, buf);
}

static void publish_speed_result(uint32_t speed_ms, const char *warn) {
    char buf[64];
    snprintf(buf, sizeof(buf),
             "{\"rep\":%d,\"speed_ms\":%lu,\"warn\":\"%s\"}",
             g_reps, (unsigned long)speed_ms, warn);
    mqtt_send(TOPIC_SPEED, buf);
}

static void publish_heartbeat(void) {
    char buf[192];
    snprintf(buf, sizeof(buf),
             "{\"device\":\"sensor\",\"online\":true,\"active\":%s,"
             "\"tracking\":%s,\"sensing\":%s,\"reps\":%d,\"sets\":%d}",
             g_active ? "true" : "false",
             g_tracking_enabled ? "true" : "false",
             g_sensing_enabled ? "true" : "false",
             g_reps, g_sets);
    mqtt_send(TOPIC_SENSOR_STATUS, buf);
}

static void publish_immediate_status(bool include_daily_totals) {
    if (!g_mqtt_ready) return;

    publish_session_state();
    if (include_daily_totals) {
        publish_daily_totals();
    }
}

static void publish_all(void) {
    publish_session_state();
    publish_daily_totals();
    printf("[MQTT] tracking=%d active=%d reps=%d sets=%d | daily reps=%d sets=%d\n",
           g_tracking_enabled, g_active, g_reps, g_sets,
           g_daily_reps, g_daily_sets);
}

static void start_tracking_session(uint32_t current_ms) {
    (void)current_ms;
    bool new_session = should_start_new_session();
    if (new_session) {
        reset_session_progress();
    } else {
        reset_motion_state();
    }

    g_tracking_enabled = true;
    g_manual_stop = false;
    g_active = true;          // PIR 없이 즉시 활성화

    publish_rest_state(0);
    publish_all();
    publish_heartbeat();

    printf("[CONTROL] Tracking %s\n", new_session ? "started (new session)" : "resumed");
}

static void stop_tracking_session(uint32_t current_ms) {
    (void)current_ms;
    g_tracking_enabled = false;
    g_manual_stop = true;
    g_active = false;
    reset_motion_state();

    publish_rest_state(0);
    publish_all();
    publish_heartbeat();

    printf("[CONTROL] Tracking stopped, sensing only\n");
}

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

static int count_pushup_rep(float cm, uint32_t current_ms) {
    if (g_pushup_pos == POS_UP && cm < PUSHUP_DOWN_CM) {
        g_pushup_pos = POS_DOWN;
        g_rep_down_ms = current_ms;
    } else if (g_pushup_pos == POS_DOWN && cm > PUSHUP_UP_CM) {
        g_pushup_pos = POS_UP;
        return 1;
    }
    return 0;
}

// ─── MFRC522 SPI 드라이버 ───────────────────────────────────────────────────

static uint8_t mfrc_read(uint8_t reg) {
    uint8_t tx = (uint8_t)((reg << 1) | 0x80);
    uint8_t rx = 0;
    gpio_put(RFID_CS_PIN, 0);
    spi_write_blocking(spi0, &tx, 1);
    spi_read_blocking(spi0, 0x00, &rx, 1);
    gpio_put(RFID_CS_PIN, 1);
    return rx;
}

static void mfrc_write(uint8_t reg, uint8_t val) {
    uint8_t buf[2] = { (uint8_t)((reg << 1) & 0x7E), val };
    gpio_put(RFID_CS_PIN, 0);
    spi_write_blocking(spi0, buf, 2);
    gpio_put(RFID_CS_PIN, 1);
}

static void mfrc_set_bits(uint8_t reg, uint8_t mask) {
    mfrc_write(reg, mfrc_read(reg) | mask);
}

static void mfrc_init(void) {
    spi_init(spi0, 1000 * 1000);
    gpio_set_function(RFID_MISO_PIN, GPIO_FUNC_SPI);
    gpio_set_function(RFID_SCK_PIN,  GPIO_FUNC_SPI);
    gpio_set_function(RFID_MOSI_PIN, GPIO_FUNC_SPI);

    gpio_init(RFID_CS_PIN);
    gpio_set_dir(RFID_CS_PIN, GPIO_OUT);
    gpio_put(RFID_CS_PIN, 1);

    gpio_init(RFID_RST_PIN);
    gpio_set_dir(RFID_RST_PIN, GPIO_OUT);
    gpio_put(RFID_RST_PIN, 1);
    sleep_ms(50);

    mfrc_write(MFRC_CommandReg, PCD_SoftReset);
    sleep_ms(50);

    mfrc_write(MFRC_TModeReg,      0x80);
    mfrc_write(MFRC_TPrescalerReg, 0xA9);
    mfrc_write(MFRC_TReloadRegH,   0x03);
    mfrc_write(MFRC_TReloadRegL,   0xE8);
    mfrc_write(MFRC_TxASKReg,      0x40);
    mfrc_write(MFRC_ModeReg,       0x3D);
    mfrc_set_bits(MFRC_TxControlReg, 0x03);   // 안테나 ON

    printf("[RFID] MFRC522 초기화 완료\n");
}

// 카드 감지 (REQA). 카드 있으면 true.
static bool mfrc_detect_card(void) {
    mfrc_write(MFRC_BitFramingReg, 0x07);
    mfrc_write(MFRC_CommandReg,    PCD_Idle);
    mfrc_write(MFRC_ComIrqReg,     0x7F);
    mfrc_write(MFRC_FIFOLevelReg,  0x80);

    uint8_t cmd = PICC_REQA;
    mfrc_write(MFRC_FIFODataReg, cmd);
    mfrc_write(MFRC_CommandReg,  PCD_Transceive);
    mfrc_set_bits(MFRC_BitFramingReg, 0x80);

    uint32_t start = time_us_32();
    while ((time_us_32() - start) < 25000u) {
        uint8_t irq = mfrc_read(MFRC_ComIrqReg);
        if (irq & 0x30) break;
        if (irq & 0x01) return false;
    }
    uint8_t err = mfrc_read(MFRC_ErrorReg);
    if (err & 0x1B) return false;
    return (mfrc_read(MFRC_FIFOLevelReg) >= 2);
}

// UID 4바이트 읽기. 성공하면 true.
static bool mfrc_read_uid(uint8_t uid[4]) {
    mfrc_write(MFRC_BitFramingReg, 0x00);
    mfrc_write(MFRC_CommandReg,    PCD_Idle);
    mfrc_write(MFRC_ComIrqReg,     0x7F);
    mfrc_write(MFRC_FIFOLevelReg,  0x80);

    mfrc_write(MFRC_FIFODataReg, PICC_ANTICOLL);
    mfrc_write(MFRC_FIFODataReg, 0x20);
    mfrc_write(MFRC_CommandReg,  PCD_Transceive);
    mfrc_set_bits(MFRC_BitFramingReg, 0x80);

    uint32_t start = time_us_32();
    while ((time_us_32() - start) < 25000u) {
        uint8_t irq = mfrc_read(MFRC_ComIrqReg);
        if (irq & 0x30) break;
        if (irq & 0x01) return false;
    }
    uint8_t err = mfrc_read(MFRC_ErrorReg);
    if (err & 0x1B) return false;
    if (mfrc_read(MFRC_FIFOLevelReg) < 5) return false;

    for (int i = 0; i < 4; i++) uid[i] = mfrc_read(MFRC_FIFODataReg);
    return true;
}

static void rfid_uid_to_str(const uint8_t uid[4], char out[12]) {
    snprintf(out, 12, "%02X:%02X:%02X:%02X",
             uid[0], uid[1], uid[2], uid[3]);
}

static void handle_rfid_scan(uint32_t current_ms) {
    if (!mfrc_detect_card()) return;

    uint8_t uid[4];
    if (!mfrc_read_uid(uid)) return;

    char uid_str[12] = {0};
    rfid_uid_to_str(uid, uid_str);

    // 쿨다운: 같은 카드를 RFID_SCAN_COOLDOWN_MS 내에 재발행하지 않음
    if (strcmp(uid_str, g_last_uid) == 0 &&
        (current_ms - g_last_uid_ms) < RFID_SCAN_COOLDOWN_MS) {
        return;
    }

    memcpy(g_last_uid, uid_str, sizeof(g_last_uid));
    g_last_uid_ms = current_ms;

    char payload[32];
    snprintf(payload, sizeof(payload), "{\"uid\":\"%s\"}", uid_str);
    mqtt_send(TOPIC_RFID_UID, payload);
    printf("[RFID] 카드 감지: %s\n", uid_str);
}

static void init_sensor_gpio(void) {
    gpio_init(HCSR04_TRIG_PIN);
    gpio_set_dir(HCSR04_TRIG_PIN, GPIO_OUT);
    gpio_put(HCSR04_TRIG_PIN, 0);

    gpio_init(HCSR04_ECHO_PIN);
    gpio_set_dir(HCSR04_ECHO_PIN, GPIO_IN);

    mfrc_init();   // ← 추가
}

static void handle_pushup_detection(float cm, uint32_t current_ms) {
    if (!g_tracking_enabled || !g_active) return;
    if (cm <= 0.0f || !count_pushup_rep(cm, current_ms)) return;

    g_reps++;
    g_daily_reps++;

    uint32_t speed_ms = current_ms - g_rep_down_ms;
    g_session_active_ms += speed_ms;
    g_daily_active_ms += speed_ms;

    const char *warn = "ok";
    if (speed_ms < REP_TOO_FAST_MS) warn = "fast";
    else if (speed_ms > REP_TOO_SLOW_MS) warn = "slow";

    printf("[REP] Push-up %d (%.1fcm) speed=%lums [%s]\n",
           g_reps, cm, (unsigned long)speed_ms, warn);

    publish_speed_result(speed_ms, warn);
    publish_immediate_status(true);

    if (g_reps < REPS_PER_SET) return;

    g_sets++;
    g_daily_sets++;
    g_reps = 0;
    g_set_end_ms = current_ms;

    publish_rest_state(DEFAULT_REST_SEC);
    publish_immediate_status(true);

    printf("[SET] %d/%d complete\n", g_sets, TARGET_SETS);
    if (session_complete()) {
        printf("[DONE] Goal reached\n");
    }
}

static void handle_control_command(const char *command, uint32_t current_ms) {
    if (strcmp(command, "start") == 0) {
        start_tracking_session(current_ms);
        return;
    }
    if (strcmp(command, "stop") == 0) {
        stop_tracking_session(current_ms);
        return;
    }

    printf("[CONTROL] Unknown command: %s\n", command);
}

static void mqtt_incoming_pub_cb(void *arg, const char *topic, u32_t tot_len) {
    (void)arg;
    (void)tot_len;
    strncpy(g_cur_topic, topic, sizeof(g_cur_topic) - 1);
    g_cur_topic[sizeof(g_cur_topic) - 1] = '\0';
    g_pay_len = 0;
}

static void mqtt_incoming_data_cb(void *arg, const u8_t *data, u16_t len, u8_t flags) {
    (void)arg;
    int copy = len;
    if (g_pay_len + copy >= (int)sizeof(g_payload) - 1) {
        copy = (int)sizeof(g_payload) - 1 - g_pay_len;
    }
    memcpy(g_payload + g_pay_len, data, (size_t)copy);
    g_pay_len += copy;

    if (!(flags & MQTT_DATA_FLAG_LAST)) return;

    g_payload[g_pay_len] = '\0';
    if (strcmp(g_cur_topic, TOPIC_CONTROL) == 0) {
        char command[16];
        json_str(g_payload, "command", command, sizeof(command));
        if (command[0] != '\0') {
            handle_control_command(command, now_ms());
        }
    }
}

static void mqtt_sub_cb(void *arg, err_t result) {
    (void)arg;
    if (result != ERR_OK) printf("[MQTT] Subscribe failed: %d\n", result);
    else printf("[MQTT] Subscribe success\n");
}

static void mqtt_connection_cb(mqtt_client_t *client, void *arg,
                               mqtt_connection_status_t status) {
    (void)arg;
    if (status == MQTT_CONNECT_ACCEPTED) {
        printf("[MQTT] Broker connected\n");
        g_mqtt_ready = true;
        mqtt_set_inpub_callback(client, mqtt_incoming_pub_cb, mqtt_incoming_data_cb, NULL);
        mqtt_subscribe(client, TOPIC_CONTROL, 0, mqtt_sub_cb, NULL);
        publish_all();
        publish_rest_state(0);
        publish_heartbeat();
    } else {
        printf("[MQTT] Connect failed (status=%d)\n", status);
        g_mqtt_ready = false;
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

static void mqtt_connect_broker(void) {
    ip_addr_t broker;
    if (!ip4addr_aton(MQTT_BROKER_IP, &broker)) {
        printf("[MQTT] Invalid broker IP: %s\n", MQTT_BROKER_IP);
        return;
    }
    g_mqtt_client = mqtt_client_new();
    if (!g_mqtt_client) {
        printf("[MQTT] Client allocation failed\n");
        return;
    }

    struct mqtt_connect_client_info_t ci = {
        .client_id   = "fitpico_sensor",
        .client_user = NULL,
        .client_pass = NULL,
        .keep_alive  = 60,
        .will_topic  = NULL,
    };

    err_t err = mqtt_client_connect(g_mqtt_client, &broker, MQTT_BROKER_PORT,
                                    mqtt_connection_cb, NULL, &ci);
    if (err != ERR_OK) printf("[MQTT] Connect request failed: %d\n", err);
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
    handle_rfid_scan(current_ms);   // ← 추가

    float cm = hcsr04_read_cm();
    if (g_tracking_enabled) {
        handle_pushup_detection(cm, current_ms);
    }
}

int main(void) {
    stdio_init_all();
    sleep_ms(2000);

    printf("=== FitPico Sensor Node ===\n");
    printf("Broker: %s:%d\n", MQTT_BROKER_IP, MQTT_BROKER_PORT);

    init_sensor_gpio();

    if (cyw43_arch_init()) {
        printf("[WiFi] Init failed\n");
        return 1;
    }

    cyw43_arch_enable_sta_mode();
    printf("[WiFi] Connecting to %s...\n", WIFI_SSID);
    if (!connect_wifi_with_fallbacks()) {
        printf("[WiFi] Connection failed - MQTT disabled\n");
    } else {
        printf("[WiFi] Connected\n");
        mqtt_connect_broker();
        sleep_ms(1000);
    }

    periodic_state_t periodic = {0};
    printf("\nReady. Sensor keeps monitoring, dashboard controls tracking.\n");
    printf("Goal: %d sets x %d reps\n\n", TARGET_SETS, REPS_PER_SET);

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
