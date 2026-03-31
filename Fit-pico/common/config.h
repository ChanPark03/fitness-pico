#pragma once

// ─── MQTT 브로커 ────────────────────────────────────────────────────────────
#define MQTT_BROKER_IP    "163.152.213.101"
#define MQTT_BROKER_PORT  1883

// ─── MQTT 토픽 ──────────────────────────────────────────────────────────────
#define TOPIC_COUNT       "fitpico/sensor/count"
#define TOPIC_TEMP        "fitpico/sensor/temp"
#define TOPIC_HUMIDITY    "fitpico/sensor/humidity"
#define TOPIC_CONTROL     "fitpico/control"

// ─── 핀 정의 (팀원 A — 센서 노드) ──────────────────────────────────────────
#define HCSR04_TRIG_PIN   14
#define HCSR04_ECHO_PIN   15
#define PIR_PIN           16
#define DHT11_PIN         17
#define LED_PIN           18

// 4×4 키패드: 행(출력) / 열(입력 풀업)
#define KEYPAD_ROW0       2
#define KEYPAD_ROW1       3
#define KEYPAD_ROW2       4
#define KEYPAD_ROW3       5
#define KEYPAD_COL0       6
#define KEYPAD_COL1       7
#define KEYPAD_COL2       8
#define KEYPAD_COL3       9

// ─── 운동 파라미터 ───────────────────────────────────────────────────────────
// ─── 푸시업 파라미터 ─────────────────────────────────────────────────────────
// HC-SR04 센서를 바닥에 위를 향해 설치
//   내려간 상태(가슴 근접) → 올라온 상태(팔 펴짐) = 1회
#define PUSHUP_DOWN_CM      12.0f  // 내려간 기준 (cm)
#define PUSHUP_UP_CM        28.0f  // 올라온 기준 (cm)
#define REPS_PER_SET        10     // 한 세트 반복 횟수
#define TARGET_SETS         3      // 목표 세트 수
#define REST_BETWEEN_SET_MS 30000  // 세트 간 휴식 시간 (ms)

#define PIR_TIMEOUT_MS      30000  // PIR 무동작 자동 정지 타임아웃 (ms)

// ─── 주기 설정 ──────────────────────────────────────────────────────────────
#define DHT_READ_INTERVAL_MS    5000   // DHT11 읽기 주기 (ms)
#define MQTT_PUBLISH_INTERVAL_MS 2000  // MQTT publish 주기 (ms)
