# RFID 사용자 인식 + 사용자별 통계 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** MFRC522 RFID 리더로 사용자를 전환하고, 대시보드에서 사용자별 운동 통계를 분리해서 관리한다.

**Architecture:** Sensor 보드가 MFRC522(SPI0)로 UID를 읽어 `fitpico/rfid/uid` 토픽으로 발행하면, Dashboard 보드가 수신해 사용자를 전환하고 `fitpico/rfid/user`로 브로드캐스트한다. 사용자 등록 정보는 Dashboard 보드 플래시에 저장하고, 운동 통계는 RAM에 유지한다.

**Tech Stack:** C (Pico SDK), MFRC522 SPI, hardware_flash, lwip MQTT, embedded HTML/JS

---

## 파일 변경 목록

| 파일 | 변경 |
|------|------|
| `common/config.h` | PIR 핀/타임아웃 삭제, RFID 핀·토픽 추가 |
| `src/main_sensor.c` | PIR 코드 삭제, MFRC522 드라이버 + UID 발행 추가 |
| `src/main_display.c` | `fitpico/rfid/user` 구독, LCD 사용자명 표시 추가 |
| `src/main_iot-dashboard.c` | RFID 구독, 사용자 관리, 플래시 저장, API, HTML UI 추가 |
| `CMakeLists.txt` | `hardware_spi`, `hardware_flash`, `hardware_sync` 추가 |

---

## Task 1: PIR 코드 제거

**Files:**
- Modify: `common/config.h`
- Modify: `src/main_sensor.c`

- [ ] **Step 1: config.h에서 PIR 관련 정의 삭제**

`common/config.h` 에서 아래 두 줄 삭제:
```c
// 삭제
#define PIR_PIN           16
#define PIR_TIMEOUT_MS        30000
```

- [ ] **Step 2: main_sensor.c 헤더 주석 수정**

파일 상단 주석 블록에서 PIR 언급 삭제:
```c
/*
 *   - HC-SR04  : 초음파 거리 측정
 *   - MFRC522  : RFID 사용자 인식
 *   - Web/MQTT : 운동 저장 시작/종료 제어
 *
 * 핀 배치 (config.h 참조)
 *   GP14=TRIG  GP15=ECHO
 */
```

- [ ] **Step 3: g_last_pir_ms 변수 제거**

line 59의 `static uint32_t  g_last_pir_ms = 0;` 삭제.

- [ ] **Step 4: init_sensor_gpio()에서 PIR GPIO 초기화 삭제**

```c
// 삭제 대상 (line 286~288)
gpio_init(PIR_PIN);
gpio_set_dir(PIR_PIN, GPIO_IN);
```

- [ ] **Step 5: reset_motion_state()에서 g_last_pir_ms 제거**

```c
static void reset_motion_state(void) {
    g_pushup_pos = POS_UP;
    g_rep_down_ms = 0;
    // g_last_pir_ms = 0;  ← 이 줄 삭제
}
```

- [ ] **Step 6: start_tracking_session()에서 PIR 관련 코드 제거 및 g_active 즉시 true로 변경**

```c
static void start_tracking_session(uint32_t current_ms) {
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
```

- [ ] **Step 7: handle_pushup_detection() 세트 완료 부분에서 g_active=false와 g_last_pir_ms 제거**

세트 완료 블록 (`if (g_reps < REPS_PER_SET) return;` 이후):
```c
    g_sets++;
    g_daily_sets++;
    g_reps = 0;
    g_set_end_ms = current_ms;
    // g_active = false;      ← 삭제 (PIR 없이 계속 활성 유지)
    // g_last_pir_ms = 0;     ← 삭제

    publish_rest_state(DEFAULT_REST_SEC);
    publish_immediate_status(true);
    printf("[SET] %d/%d complete\n", g_sets, TARGET_SETS);
```

- [ ] **Step 8: handle_pir_activity() 함수 전체 삭제** (line 290~308)

- [ ] **Step 9: poll_inputs_and_motion()에서 handle_pir_activity 호출 삭제**

```c
static void poll_inputs_and_motion(uint32_t current_ms) {
    // handle_pir_activity(current_ms);  ← 삭제

    float cm = hcsr04_read_cm();
    if (g_tracking_enabled) {
        handle_pushup_detection(cm, current_ms);
    }
}
```

- [ ] **Step 10: 빌드 확인**

```bash
cd /Users/chanpark/fitness-pico/Fit-pico
mkdir -p ../../build/sensor && cd ../../build/sensor
cmake ../../Fit-pico -DCMAKE_BUILD_TYPE=Debug
make fitpico_sensor 2>&1 | tail -20
```
Expected: `fitpico_sensor.uf2` 생성, PIR 관련 경고 없음.

- [ ] **Step 11: 커밋**

```bash
cd /Users/chanpark/fitness-pico
git add Fit-pico/common/config.h Fit-pico/src/main_sensor.c
git commit -m "refactor: remove PIR sensor code, activate immediately on start"
```

---

## Task 2: config.h에 RFID 핀·토픽 추가

**Files:**
- Modify: `common/config.h`

- [ ] **Step 1: RFID SPI 핀 정의 추가**

`config.h`의 핀 정의 섹션에 추가:
```c
// ─── RFID (MFRC522 — 센서 노드 SPI0) ───────────────────────────────────────
#define RFID_MISO_PIN     4   // SPI0 RX
#define RFID_SCK_PIN      2   // SPI0 SCK
#define RFID_MOSI_PIN     3   // SPI0 TX
#define RFID_CS_PIN       5   // Chip Select (software)
#define RFID_RST_PIN      0   // Reset

#define RFID_SCAN_COOLDOWN_MS  2000  // 같은 카드 재인식 방지 (ms)
```

- [ ] **Step 2: RFID MQTT 토픽 추가**

`config.h`의 MQTT 토픽 섹션에 추가:
```c
#define TOPIC_RFID_UID   "fitpico/rfid/uid"    // Sensor → Dashboard: UID 감지
#define TOPIC_RFID_USER  "fitpico/rfid/user"   // Dashboard → 전체: 사용자 전환
```

- [ ] **Step 3: 커밋**

```bash
cd /Users/chanpark/fitness-pico
git add Fit-pico/common/config.h
git commit -m "feat: add RFID SPI pins and MQTT topics to config.h"
```

---

## Task 3: CMakeLists.txt에 하드웨어 라이브러리 추가

**Files:**
- Modify: `Fit-pico/CMakeLists.txt`

- [ ] **Step 1: fitpico_sensor에 hardware_spi 추가**

```cmake
target_link_libraries(fitpico_sensor
    pico_stdlib
    hardware_gpio
    hardware_spi           # ← 추가
    pico_cyw43_arch_lwip_threadsafe_background
    pico_lwip_mqtt
)
```

- [ ] **Step 2: fitpico_dashboard에 hardware_flash, hardware_sync 추가**

```cmake
target_link_libraries(fitpico_dashboard
    pico_stdlib
    hardware_flash         # ← 추가
    hardware_sync          # ← 추가
    pico_cyw43_arch_lwip_threadsafe_background
    pico_lwip_mqtt
    pico_lwip_mdns
)
```

- [ ] **Step 3: 커밋**

```bash
cd /Users/chanpark/fitness-pico
git add Fit-pico/CMakeLists.txt
git commit -m "build: add hardware_spi, hardware_flash, hardware_sync libraries"
```

---

## Task 4: MFRC522 드라이버 + UID 발행 (main_sensor.c)

**Files:**
- Modify: `src/main_sensor.c`

- [ ] **Step 1: include 추가**

파일 상단 include 블록에 추가:
```c
#include "hardware/spi.h"
```

- [ ] **Step 2: MFRC522 레지스터·명령 상수 정의 추가**

`#define DEFAULT_REST_SEC 45` 바로 위에 추가:
```c
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
```

- [ ] **Step 3: RFID 상태 변수 추가**

전역 변수 섹션 (`static Position g_pushup_pos = POS_UP;` 아래) 에 추가:
```c
static char     g_last_uid[12]     = {0};
static uint32_t g_last_uid_ms      = 0;
```

- [ ] **Step 4: MFRC522 드라이버 함수 추가**

`init_sensor_gpio()` 함수 바로 위에 삽입:
```c
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

    uint32_t deadline = time_us_32() + 25000u;
    while (time_us_32() < deadline) {
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

    uint32_t deadline = time_us_32() + 25000u;
    while (time_us_32() < deadline) {
        uint8_t irq = mfrc_read(MFRC_ComIrqReg);
        if (irq & 0x30) break;
        if (irq & 0x01) return false;
    }
    uint8_t err = mfrc_read(MFRC_ErrorReg);
    if (err & 0x1B) return false;
    if (mfrc_read(MFRC_FIFOLevelReg) < 4) return false;

    for (int i = 0; i < 4; i++) uid[i] = mfrc_read(MFRC_FIFODataReg);
    return true;
}
```

- [ ] **Step 5: UID → 문자열 변환 + MQTT 발행 함수 추가**

`mfrc_read_uid()` 바로 아래에 추가:
```c
static void rfid_uid_to_str(const uint8_t uid[4], char out[12]) {
    snprintf(out, 12, "%02X:%02X:%02X:%02X",
             uid[0], uid[1], uid[2], uid[3]);
}

static void handle_rfid_scan(uint32_t current_ms) {
    if (!mfrc_detect_card()) return;

    uint8_t uid[4];
    if (!mfrc_read_uid(uid)) return;

    char uid_str[12];
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
```

- [ ] **Step 6: init_sensor_gpio()에 mfrc_init() 호출 추가**

```c
static void init_sensor_gpio(void) {
    gpio_init(HCSR04_TRIG_PIN);
    gpio_set_dir(HCSR04_TRIG_PIN, GPIO_OUT);
    gpio_put(HCSR04_TRIG_PIN, 0);

    gpio_init(HCSR04_ECHO_PIN);
    gpio_set_dir(HCSR04_ECHO_PIN, GPIO_IN);

    mfrc_init();   // ← 추가
}
```

- [ ] **Step 7: poll_inputs_and_motion()에 handle_rfid_scan 추가**

```c
static void poll_inputs_and_motion(uint32_t current_ms) {
    handle_rfid_scan(current_ms);   // ← 추가

    float cm = hcsr04_read_cm();
    if (g_tracking_enabled) {
        handle_pushup_detection(cm, current_ms);
    }
}
```

- [ ] **Step 8: 빌드 확인**

```bash
cd /Users/chanpark/fitness-pico/build/sensor
make fitpico_sensor 2>&1 | tail -20
```
Expected: 빌드 성공, `undefined reference` 없음.

- [ ] **Step 9: 하드웨어 검증 (선택)**

`fitpico_sensor.uf2` 플래시 후 USB 시리얼 모니터에서 카드 태그 시:
```
[RFID] MFRC522 초기화 완료
[RFID] 카드 감지: A3:B2:C1:D0
```

- [ ] **Step 10: 커밋**

```bash
cd /Users/chanpark/fitness-pico
git add Fit-pico/src/main_sensor.c
git commit -m "feat: add MFRC522 RFID driver and UID MQTT publish to sensor node"
```

---

## Task 5: Dashboard — 사용자 구조체·전역변수 + 플래시 저장

**Files:**
- Modify: `src/main_iot-dashboard.c`

- [ ] **Step 1: include 추가**

파일 상단 include 블록에 추가:
```c
#include "hardware/flash.h"
#include "hardware/sync.h"
```

- [ ] **Step 2: 상수 및 사용자 구조체 정의 추가**

`#define HTTP_PORT 80` 바로 아래에 추가:
```c
#define MAX_USERS           8
#define FLASH_MAGIC         0xFD5A1234U
#define FLASH_USER_SIZE     512    // 2 flash pages (각 256 bytes)
#define FLASH_USER_OFFSET   (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)

typedef struct __attribute__((packed)) {
    char    uid[12];      // "A3:B2:C1:D0\0"
    char    name[20];
    uint8_t weight_kg;
    uint8_t goal_sets;
    uint8_t _pad[2];
} user_t;   // 36 bytes

typedef struct {
    uint32_t magic;
    uint8_t  count;
    uint8_t  _pad[3];
    user_t   users[MAX_USERS];      // 8 * 36 = 288 bytes
    uint8_t  _fill[512 - 8 - 288];  // = 216 bytes, 총 512
} flash_user_block_t;

typedef struct {
    int      today_reps;
    int      today_sets;
    int      total_reps;
    int      total_sets;
    int      reps_at_login;   // 로그인 시점 g_daily_reps 스냅샷
    int      sets_at_login;
} user_stats_t;
```

- [ ] **Step 3: 전역 사용자 상태 변수 추가**

기존 전역 변수 블록 끝 (`static struct tcp_pcb *g_http_pcb = NULL;` 아래) 에 추가:
```c
static user_t       g_users[MAX_USERS]      = {0};
static user_stats_t g_stats[MAX_USERS]      = {0};
static int          g_user_count            = 0;
static int          g_current_user          = -1;   // -1 = 미로그인
static bool         g_scan_mode             = false;
static char         g_pending_uid[12]       = {0};
```

- [ ] **Step 4: 플래시 로드 함수 추가**

`on_publish()` 함수 바로 위에 추가:
```c
// ─── 플래시 사용자 저장소 ────────────────────────────────────────────────────

static void users_flash_load(void) {
    const flash_user_block_t *block =
        (const flash_user_block_t *)(XIP_BASE + FLASH_USER_OFFSET);
    if (block->magic != FLASH_MAGIC) {
        printf("[FLASH] 저장된 사용자 없음 (magic 불일치)\n");
        return;
    }
    g_user_count = block->count < MAX_USERS ? block->count : MAX_USERS;
    memcpy(g_users, block->users, (size_t)g_user_count * sizeof(user_t));
    printf("[FLASH] 사용자 %d명 로드\n", g_user_count);
}

static void users_flash_save(void) {
    static uint8_t buf[FLASH_USER_SIZE] __attribute__((aligned(4)));
    flash_user_block_t *block = (flash_user_block_t *)buf;

    memset(buf, 0xFF, sizeof(buf));
    block->magic = FLASH_MAGIC;
    block->count = (uint8_t)g_user_count;
    memcpy(block->users, g_users, (size_t)g_user_count * sizeof(user_t));

    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(FLASH_USER_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(FLASH_USER_OFFSET, buf, FLASH_USER_SIZE);
    restore_interrupts(ints);

    printf("[FLASH] 사용자 %d명 저장\n", g_user_count);
}
```

- [ ] **Step 5: main()에서 부팅 시 users_flash_load() 호출**

`mqtt_connect_broker();` 호출 바로 위에 추가:
```c
users_flash_load();
```

- [ ] **Step 6: 빌드 확인**

```bash
cd /Users/chanpark/fitness-pico/build/dashboard
cmake ../../Fit-pico -DCMAKE_BUILD_TYPE=Debug
make fitpico_dashboard 2>&1 | tail -20
```
Expected: 빌드 성공.

- [ ] **Step 7: 커밋**

```bash
cd /Users/chanpark/fitness-pico
git add Fit-pico/src/main_iot-dashboard.c
git commit -m "feat: add user data structures and flash persistence to dashboard"
```

---

## Task 6: Dashboard — RFID UID 구독 + 사용자 전환 로직

**Files:**
- Modify: `src/main_iot-dashboard.c`

- [ ] **Step 1: on_data()에 RFID UID 처리 추가**

`on_data()` 함수의 토픽 분기 마지막 (`TOPIC_DISPLAY_STATUS` 처리 바로 뒤) 에 추가:
```c
} else if (strcmp(g_cur_topic, TOPIC_RFID_UID) == 0) {
    char uid[12];
    json_str(g_payload, "uid", uid, sizeof(uid));
    if (uid[0] == '\0') return;

    // 등록 대기 모드: UID 캡처만 하고 사용자 전환 안 함
    if (g_scan_mode) {
        memcpy(g_pending_uid, uid, sizeof(g_pending_uid));
        g_scan_mode = false;
        printf("[RFID] 등록 대기 UID 캡처: %s\n", uid);
        return;
    }

    // 사용자 조회
    for (int i = 0; i < g_user_count; i++) {
        if (strcmp(g_users[i].uid, uid) == 0) {
            // 이전 사용자 통계 스냅샷 저장
            if (g_current_user >= 0) {
                int prev = g_current_user;
                g_stats[prev].today_reps  = g_daily_reps - g_stats[prev].reps_at_login;
                g_stats[prev].today_sets  = g_daily_sets - g_stats[prev].sets_at_login;
                g_stats[prev].total_reps += g_stats[prev].today_reps;
                g_stats[prev].total_sets += g_stats[prev].today_sets;
            }
            // 새 사용자 전환
            g_current_user = i;
            g_stats[i].reps_at_login = g_daily_reps;
            g_stats[i].sets_at_login = g_daily_sets;

            char payload[96];
            snprintf(payload, sizeof(payload),
                     "{\"name\":\"%s\",\"uid\":\"%s\","
                     "\"weight\":%d,\"goal_sets\":%d}",
                     g_users[i].name, uid,
                     g_users[i].weight_kg, g_users[i].goal_sets);
            mqtt_send_message(TOPIC_RFID_USER, payload);
            printf("[RFID] 사용자 전환: %s\n", g_users[i].name);
            return;
        }
    }
    printf("[RFID] 미등록 카드: %s\n", uid);
}
```

- [ ] **Step 2: subscribe_dashboard_topics()에 RFID 토픽 추가**

```c
static void subscribe_dashboard_topics(mqtt_client_t *client) {
    mqtt_subscribe(client, TOPIC_COUNT,          0, on_sub, NULL);
    mqtt_subscribe(client, TOPIC_SPEED,          0, on_sub, NULL);
    mqtt_subscribe(client, TOPIC_REST,           0, on_sub, NULL);
    mqtt_subscribe(client, TOPIC_DAILY,          0, on_sub, NULL);
    mqtt_subscribe(client, TOPIC_SENSOR_STATUS,  0, on_sub, NULL);
    mqtt_subscribe(client, TOPIC_DISPLAY_STATUS, 0, on_sub, NULL);
    mqtt_subscribe(client, TOPIC_RFID_UID,       0, on_sub, NULL);  // ← 추가
}
```

- [ ] **Step 3: 커밋**

```bash
cd /Users/chanpark/fitness-pico
git add Fit-pico/src/main_iot-dashboard.c
git commit -m "feat: subscribe RFID UID topic, implement user switch logic in dashboard"
```

---

## Task 7: Dashboard — 사용자 관리 API

**Files:**
- Modify: `src/main_iot-dashboard.c`

- [ ] **Step 1: API 핸들러 함수 추가**

`build_status_json()` 함수 바로 위에 추가:
```c
// ─── 사용자 관리 API 핸들러 ──────────────────────────────────────────────────

static void handle_get_users(struct tcp_pcb *tpcb) {
    char body[512];
    size_t pos = 0;
    pos += (size_t)snprintf(body + pos, sizeof(body) - pos, "[");
    for (int i = 0; i < g_user_count; i++) {
        pos += (size_t)snprintf(body + pos, sizeof(body) - pos,
            "%s{\"uid\":\"%s\",\"name\":\"%s\","
            "\"weight\":%d,\"goal_sets\":%d}",
            i ? "," : "",
            g_users[i].uid, g_users[i].name,
            g_users[i].weight_kg, g_users[i].goal_sets);
    }
    snprintf(body + pos, sizeof(body) - pos, "]");
    http_send_response(tpcb, "200 OK", "application/json; charset=utf-8", body);
}

static void handle_post_user(struct tcp_pcb *tpcb, const char *request) {
    const char *body = strstr(request, "\r\n\r\n");
    if (!body) { http_send_not_found(tpcb); return; }
    body += 4;

    if (g_user_count >= MAX_USERS) {
        http_send_control_result(tpcb, false, "max_users_reached");
        return;
    }

    user_t u = {0};
    json_str(body, "uid",  u.uid,  sizeof(u.uid));
    json_str(body, "name", u.name, sizeof(u.name));
    u.weight_kg = (uint8_t)json_int(body, "weight");
    u.goal_sets = (uint8_t)json_int(body, "goal_sets");

    if (u.uid[0] == '\0' || u.name[0] == '\0') {
        http_send_control_result(tpcb, false, "missing_fields");
        return;
    }

    // 중복 UID 확인
    for (int i = 0; i < g_user_count; i++) {
        if (strcmp(g_users[i].uid, u.uid) == 0) {
            http_send_control_result(tpcb, false, "uid_exists");
            return;
        }
    }

    g_users[g_user_count++] = u;
    users_flash_save();
    http_send_control_result(tpcb, true, "");
    printf("[USER] 등록: %s (%s)\n", u.name, u.uid);
}

static void handle_get_current_user(struct tcp_pcb *tpcb) {
    char body[128];
    if (g_current_user < 0) {
        snprintf(body, sizeof(body), "{\"logged_in\":false}");
    } else {
        user_t *u = &g_users[g_current_user];
        snprintf(body, sizeof(body),
                 "{\"logged_in\":true,\"name\":\"%s\","
                 "\"weight\":%d,\"goal_sets\":%d}",
                 u->name, u->weight_kg, u->goal_sets);
    }
    http_send_response(tpcb, "200 OK", "application/json; charset=utf-8", body);
}

static void handle_scan_mode(struct tcp_pcb *tpcb) {
    g_scan_mode = true;
    g_pending_uid[0] = '\0';
    http_send_control_result(tpcb, true, "");
    printf("[RFID] 등록 대기 모드 진입\n");
}

static void handle_get_current_stats(struct tcp_pcb *tpcb) {
    char body[256];
    if (g_current_user < 0) {
        snprintf(body, sizeof(body), "{\"logged_in\":false}");
    } else {
        user_t      *u = &g_users[g_current_user];
        user_stats_t *s = &g_stats[g_current_user];
        int session_reps = g_daily_reps - s->reps_at_login;
        int session_sets = g_daily_sets - s->sets_at_login;
        snprintf(body, sizeof(body),
                 "{\"logged_in\":true,\"name\":\"%s\","
                 "\"session_reps\":%d,\"session_sets\":%d,"
                 "\"total_reps\":%d,\"total_sets\":%d,"
                 "\"goal_sets\":%d,\"goal_achieved\":%s,"
                 "\"pending_uid\":\"%s\"}",
                 u->name,
                 session_reps < 0 ? 0 : session_reps,
                 session_sets < 0 ? 0 : session_sets,
                 s->total_reps + (session_reps > 0 ? session_reps : 0),
                 s->total_sets + (session_sets > 0 ? session_sets : 0),
                 u->goal_sets,
                 session_sets >= u->goal_sets ? "true" : "false",
                 g_pending_uid);
    }
    http_send_response(tpcb, "200 OK", "application/json; charset=utf-8", body);
}
```

- [ ] **Step 2: http_recv()에 새 라우트 추가**

`http_recv()` 내 기존 라우트 분기에서 `http_send_not_found` 바로 앞에 추가:
```c
    } else if (strncmp(state->request, "GET /api/users/current ", 23) == 0) {
        handle_get_current_user(tpcb);
    } else if (strncmp(state->request, "GET /api/users ", 15) == 0) {
        handle_get_users(tpcb);
    } else if (strncmp(state->request, "POST /api/users ", 16) == 0) {
        handle_post_user(tpcb, state->request);
    } else if (strncmp(state->request, "POST /api/rfid/scan-mode ", 25) == 0) {
        handle_scan_mode(tpcb);
    } else if (strncmp(state->request, "GET /api/stats/current ", 23) == 0) {
        handle_get_current_stats(tpcb);
    } else {
        http_send_not_found(tpcb);
```

> **주의:** `GET /api/users/current`는 반드시 `GET /api/users` 분기보다 먼저 위치해야 한다.

- [ ] **Step 3: build_status_json()에 scan_mode, pending_uid, current_user 필드 추가**

`build_status_json()` 함수의 snprintf 끝에 추가 (마지막 `}` 전):
```c
        "\"current_user\":\"%s\","
        "\"scan_mode\":%s,"
        "\"pending_uid\":\"%s\"",
        g_current_user >= 0 ? g_users[g_current_user].name : "",
        g_scan_mode ? "true" : "false",
        g_pending_uid
```

- [ ] **Step 4: 빌드 확인**

```bash
cd /Users/chanpark/fitness-pico/build/dashboard
make fitpico_dashboard 2>&1 | tail -20
```
Expected: 빌드 성공.

- [ ] **Step 5: 커밋**

```bash
cd /Users/chanpark/fitness-pico
git add Fit-pico/src/main_iot-dashboard.c
git commit -m "feat: add user management API endpoints (GET/POST /api/users, scan-mode, stats)"
```

---

## Task 8: Dashboard — HTML UI 확장 (사용자 섹션)

**Files:**
- Modify: `src/main_iot-dashboard.c`

- [ ] **Step 1: 현재 사용자 표시 카드 HTML 추가**

`DASHBOARD_HTML` 문자열에서 `</div>\n</div>\n<script>\n` (그리드 끝 바로 전) 위치에 추가:

```c
// 기존 마지막 카드 (history card) 끝 부분 찾아서 그 뒤, </div>(grid 닫힘) 전에 삽입
"<section class=\"card full\" id=\"rfid-section\">"
  "<div class=\"label\">RFID 사용자</div>"
  "<div id=\"current-user-name\" class=\"value\" style=\"font-size:28px\">-</div>"
  "<div class=\"controls\" style=\"margin-top:16px\">"
    "<button class=\"btn\" id=\"scan-btn\">카드 갖다대기 (등록)</button>"
  "</div>"
  "<div id=\"reg-form\" style=\"display:none;margin-top:16px\">"
    "<div class=\"controls\">"
      "<input id=\"reg-name\" placeholder=\"이름\" style=\"padding:10px;border-radius:12px;border:1px solid #ccc;font-size:16px\">"
      "<input id=\"reg-weight\" type=\"number\" placeholder=\"체중(kg)\" min=\"1\" max=\"300\" style=\"width:100px;padding:10px;border-radius:12px;border:1px solid #ccc;font-size:16px\">"
      "<input id=\"reg-goal\" type=\"number\" placeholder=\"목표세트\" min=\"1\" max=\"20\" style=\"width:100px;padding:10px;border-radius:12px;border:1px solid #ccc;font-size:16px\">"
      "<button class=\"btn\" id=\"reg-submit\">등록</button>"
    "</div>"
    "<div id=\"reg-uid-label\" class=\"meta\" style=\"margin-top:8px\"></div>"
  "</div>"
  "<div id=\"rfid-meta\" class=\"meta\" style=\"margin-top:8px\">카드를 리더기에 태그하면 사용자가 전환됩니다.</div>"
  "<section class=\"card\" style=\"margin-top:16px;background:#f3efe6\">"
    "<div class=\"label\">오늘 세션 통계</div>"
    "<div class=\"list\">"
      "<div class=\"chip\" id=\"user-session-reps\">렙: -</div>"
      "<div class=\"chip\" id=\"user-session-sets\">세트: -</div>"
      "<div class=\"chip\" id=\"user-total-reps\">누적 렙: -</div>"
      "<div class=\"chip\" id=\"user-total-sets\">누적 세트: -</div>"
      "<div class=\"chip\" id=\"user-goal\">목표: -</div>"
    "</div>"
  "</section>"
"</section>\n"
```

- [ ] **Step 2: JavaScript에 RFID 로직 추가**

기존 `<script>` 블록에서 `window.addEventListener('load', ...)` 함수 안에 추가:

```javascript
// RFID 사용자 섹션 업데이트 (refresh()에서 data를 받아서 호출)
// refresh() 함수의 try 블록 끝, catch 전에 추가:
"const uname=data.current_user||'';"
"document.getElementById('current-user-name').textContent=uname||'로그인 전';"
"if(data.pending_uid&&data.pending_uid.length>0&&document.getElementById('reg-form').style.display==='none'){"
  "document.getElementById('reg-form').style.display='block';"
  "document.getElementById('reg-uid-label').textContent='카드 UID: '+data.pending_uid;"
  "document.getElementById('rfid-meta').textContent='등록 정보를 입력하고 등록 버튼을 누르세요.';"
"}"
```

`window.addEventListener('load', ...)` 안에 이벤트 리스너 추가:
```javascript
"document.getElementById('scan-btn').addEventListener('click',async()=>{"
  "await fetch('/api/rfid/scan-mode',{method:'POST'});"
  "document.getElementById('rfid-meta').textContent='카드를 갖다대세요...';"
"});"
"document.getElementById('reg-submit').addEventListener('click',async()=>{"
  "const resp=await fetch('/api/status',{cache:'no-store'});"
  "const d=await resp.json();"
  "const uid=d.pending_uid;"
  "const name=document.getElementById('reg-name').value.trim();"
  "const weight=parseInt(document.getElementById('reg-weight').value)||70;"
  "const goal=parseInt(document.getElementById('reg-goal').value)||3;"
  "if(!uid||!name){alert('이름과 UID를 확인하세요');return;}"
  "const res=await fetch('/api/users',{method:'POST',"
    "headers:{'Content-Type':'application/json'},"
    "body:JSON.stringify({uid,name,weight,goal_sets:goal})});"
  "const result=await res.json();"
  "if(result.ok){document.getElementById('reg-form').style.display='none';"
    "document.getElementById('rfid-meta').textContent=name+' 등록 완료!';}"
  "else{alert('등록 실패: '+result.error);}"
"});"
```

stats 업데이트를 refresh() 내에 추가:
```javascript
"async function refreshStats(){try{"
  "const res=await fetch('/api/stats/current',{cache:'no-store'});"
  "const s=await res.json();"
  "if(s.logged_in){"
    "document.getElementById('user-session-reps').textContent='렙: '+s.session_reps;"
    "document.getElementById('user-session-sets').textContent='세트: '+s.session_sets;"
    "document.getElementById('user-total-reps').textContent='누적 렙: '+s.total_reps;"
    "document.getElementById('user-total-sets').textContent='누적 세트: '+s.total_sets;"
    "document.getElementById('user-goal').textContent='목표 '+s.goal_sets+'세트 '+(s.goal_achieved?'✓ 달성':'진행중');"
  "}}catch(e){}}"
"setInterval(refreshStats,1000);"
```

- [ ] **Step 3: 빌드 확인**

```bash
cd /Users/chanpark/fitness-pico/build/dashboard
make fitpico_dashboard 2>&1 | tail -20
```
Expected: 빌드 성공.

- [ ] **Step 4: 브라우저 검증**

`fitpico_dashboard.uf2` 플래시 후 `http://fitpico-dashboard.local/` 접속해서:
- "RFID 사용자" 섹션이 표시됨
- "카드 갖다대기" 버튼 클릭 → "카드를 갖다대세요..." 메시지
- 카드 태그 후 등록 폼 표시 확인

- [ ] **Step 5: 커밋**

```bash
cd /Users/chanpark/fitness-pico
git add Fit-pico/src/main_iot-dashboard.c
git commit -m "feat: add RFID user management UI and per-user stats to dashboard"
```

---

## Task 9: Display — 사용자명 LCD 표시

**Files:**
- Modify: `src/main_display.c`

- [ ] **Step 1: 사용자명 전역 변수 추가**

`main_display.c`의 전역 변수 섹션에 추가:
```c
static char g_current_user_name[20] = {0};
static uint32_t g_user_display_until_ms = 0;
```

- [ ] **Step 2: on_data()에 RFID_USER 토픽 처리 추가**

`main_display.c`의 MQTT 데이터 핸들러 (`on_data()` 혹은 동등한 함수)에서 토픽 분기 끝에 추가:
```c
} else if (strcmp(g_cur_topic, TOPIC_RFID_USER) == 0) {
    json_str(g_payload, "name", g_current_user_name, sizeof(g_current_user_name));
    g_user_display_until_ms = now_ms() + 3000;  // 3초간 표시
    printf("[RFID] 사용자 전환: %s\n", g_current_user_name);
}
```

`now_ms()` 함수가 없으면 파일 상단에 추가:
```c
static uint32_t now_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}
```

- [ ] **Step 3: LCD 업데이트 함수에 사용자명 표시 로직 추가**

LCD를 업데이트하는 함수 (보통 `update_display()` 또는 `render_lcd()` 형태) 첫 부분에 추가:
```c
// 사용자 이름 3초간 1행에 표시
if (g_current_user_name[0] != '\0' &&
    now_ms() < g_user_display_until_ms) {
    char row0[17];
    snprintf(row0, sizeof(row0), "HI %-13s", g_current_user_name);
    lcd_set_cursor(0, 0);
    lcd_print(row0);
    return;   // 이 시간 동안은 일반 표시 생략
}
```

- [ ] **Step 4: subscribe에 TOPIC_RFID_USER 추가**

구독 설정 함수 (보통 MQTT 연결 콜백 내부)에 추가:
```c
mqtt_subscribe(client, TOPIC_RFID_USER, 0, mqtt_sub_cb, NULL);
```

- [ ] **Step 5: 빌드 확인**

```bash
cd /Users/chanpark/fitness-pico/build/display
cmake ../../Fit-pico -DCMAKE_BUILD_TYPE=Debug
make fitpico_display 2>&1 | tail -20
```
Expected: 빌드 성공.

- [ ] **Step 6: 하드웨어 검증**

카드 태그 시 LCD 1행에 `HI 박찬웅       ` 형태로 3초간 표시 후 일반 화면 복귀.

- [ ] **Step 7: 커밋**

```bash
cd /Users/chanpark/fitness-pico
git add Fit-pico/src/main_display.c
git commit -m "feat: display username on LCD for 3 seconds when RFID card tapped"
```

---

## 전체 검증 체크리스트

- [ ] Sensor 보드: 카드 태그 → 시리얼 `[RFID] 카드 감지: A3:B2:C1:D0` 출력
- [ ] Sensor 보드: MQTT 브로커에 `fitpico/rfid/uid` 수신 확인 (mosquitto_sub 등으로)
- [ ] Dashboard: 대시보드 "카드 갖다대기" → 카드 태그 → 등록 폼 표시
- [ ] Dashboard: 이름/체중/목표 입력 후 등록 → 재시작 후 사용자 유지 확인
- [ ] Dashboard: 등록된 카드 태그 → RFID 사용자 섹션에 이름 표시
- [ ] Dashboard: 운동 후 오늘 세션 렙/세트 업데이트 확인
- [ ] Dashboard: 미등록 카드 태그 시 `[RFID] 미등록 카드:` 출력 (사용자 전환 없음)
- [ ] Display: 카드 태그 시 LCD 1행 `HI {이름}` 3초 표시 후 복귀
