#pragma once

// ─── MQTT 브로커 ────────────────────────────────────────────────────────────
// 우선순위:
//   1) 빌드 시 -DMQTT_SERVER="x.x.x.x" 또는 환경 변수 MQTT_SERVER
//   2) 아래 기본값
#ifndef MQTT_BROKER_IP
#ifdef MQTT_SERVER
#define MQTT_BROKER_IP    MQTT_SERVER
#else
#define MQTT_BROKER_IP    "163.152.213.101"
#endif
#endif
#ifdef MQTT_USERNAME
#define MQTT_BROKER_USERNAME MQTT_USERNAME
#else
#define MQTT_BROKER_USERNAME ""
#endif
#ifdef MQTT_PASSWORD
#define MQTT_BROKER_PASSWORD MQTT_PASSWORD
#else
#define MQTT_BROKER_PASSWORD ""
#endif
#define MQTT_BROKER_PORT  1883

// ─── MQTT 토픽 ──────────────────────────────────────────────────────────────
#define TOPIC_COUNT          "fitpico/sensor/count"
#define TOPIC_REST           "fitpico/sensor/rest"
#define TOPIC_DAILY          "fitpico/sensor/daily"
#define TOPIC_SPEED          "fitpico/sensor/speed"
#define TOPIC_CONTROL        "fitpico/control"

// 보드 heartbeat / 상태 토픽
#define TOPIC_SENSOR_STATUS  "fitpico/sensor/status"
#define TOPIC_DISPLAY_STATUS "fitpico/display/status"

// RFID 토픽
#define TOPIC_RFID_UID   "fitpico/rfid/uid"    // Sensor → Dashboard: UID 감지
#define TOPIC_RFID_USER  "fitpico/rfid/user"   // Dashboard → 전체: 사용자 전환

// ─── 핀 정의 (팀원 A — 센서 노드) ──────────────────────────────────────────
#define HCSR04_TRIG_PIN   14
#define HCSR04_ECHO_PIN   15

// ─── 디스플레이 보드 출력 장치 ─────────────────────────────────────────────
#define BUZZER_PIN            18    // 디스플레이 보드 버저 핀

// ─── RFID (MFRC522 — 센서 노드 SPI0) ───────────────────────────────────────
#define RFID_MISO_PIN     4   // SPI0 RX
#define RFID_SCK_PIN      2   // SPI0 SCK
#define RFID_MOSI_PIN     3   // SPI0 TX
#define RFID_CS_PIN       5   // Chip Select (software)
#define RFID_RST_PIN      0   // Reset

#define RFID_SCAN_COOLDOWN_MS  2000  // 같은 카드 재인식 방지 (ms)

// ─── 운동 파라미터 ───────────────────────────────────────────────────────────
#define PUSHUP_DOWN_CM        12.0f  // 내려간 기준 (cm)
#define PUSHUP_UP_CM          28.0f  // 올라온 기준 (cm)
#define REPS_PER_SET          10     // 한 세트 반복 횟수
#define TARGET_SETS           3      // 목표 세트 수

// ─── 운동 속도 기준 ──────────────────────────────────────────────────────────
#define REP_TOO_FAST_MS       800    // 이보다 빠르면 너무 빠름 경고 (ms)
#define REP_TOO_SLOW_MS       4000   // 이보다 느리면 너무 느림 경고 (ms)

// ─── 주기 설정 ──────────────────────────────────────────────────────────────
#define MQTT_PUBLISH_INTERVAL_MS     2000  // MQTT publish 주기 (ms)
#define MQTT_RECONNECT_INTERVAL_MS   5000  // MQTT 재연결 시도 간격
#define DEVICE_HEARTBEAT_INTERVAL_MS 3000  // 각 보드 heartbeat 전송 주기
#define BOARD_OFFLINE_TIMEOUT_MS     8000  // heartbeat 미수신 시 오프라인 판정 기준
