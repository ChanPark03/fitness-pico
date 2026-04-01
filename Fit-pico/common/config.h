#pragma once

// ─── MQTT 브로커 ────────────────────────────────────────────────────────────
#define MQTT_BROKER_IP    "163.152.213.101"
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

// ─── 핀 정의 (팀원 A — 센서 노드) ──────────────────────────────────────────
#define HCSR04_TRIG_PIN   14
#define HCSR04_ECHO_PIN   15
#define PIR_PIN           16

// 4×4 키패드: 행(출력) / 열(입력 풀업)
#define KEYPAD_ROW0       2
#define KEYPAD_ROW1       3
#define KEYPAD_ROW2       4
#define KEYPAD_ROW3       5
#define KEYPAD_COL0       6
#define KEYPAD_COL1       7
#define KEYPAD_COL2       8
#define KEYPAD_COL3       9

// ─── 디스플레이 보드 WS2812 상태등 ────────────────────────────────────────
#define BUZZER_PIN            18    // 디스플레이 보드 버저 핀
#define WS2812_PIN            12    // 디스플레이 보드 RGB strip 데이터 핀
#define WS2812_LED_COUNT      8     // 연결한 LED 개수에 맞게 조정
#define WS2812_IS_RGBW        0

// ─── 운동 파라미터 ───────────────────────────────────────────────────────────
#define PUSHUP_DOWN_CM        12.0f  // 내려간 기준 (cm)
#define PUSHUP_UP_CM          28.0f  // 올라온 기준 (cm)
#define REPS_PER_SET          10     // 한 세트 반복 횟수
#define TARGET_SETS           3      // 목표 세트 수

#define PIR_TIMEOUT_MS        30000  // PIR 무동작 자동 정지 타임아웃 (ms)

// ─── 운동 속도 기준 ──────────────────────────────────────────────────────────
#define REP_TOO_FAST_MS       800    // 이보다 빠르면 너무 빠름 경고 (ms)
#define REP_TOO_SLOW_MS       4000   // 이보다 느리면 너무 느림 경고 (ms)

// ─── 주기 설정 ──────────────────────────────────────────────────────────────
#define MQTT_PUBLISH_INTERVAL_MS     2000  // MQTT publish 주기 (ms)
#define DEVICE_HEARTBEAT_INTERVAL_MS 3000  // 각 보드 heartbeat 전송 주기
#define BOARD_OFFLINE_TIMEOUT_MS     8000  // heartbeat 미수신 시 오프라인 판정 기준
