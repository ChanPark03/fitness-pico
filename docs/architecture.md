# Architecture

`fitness-pico`는 MQTT를 중심으로 연결된 3보드 구조입니다. 센서 보드는 운동 데이터를 만들고, 대시보드 보드는 상태를 집계해 웹 UI를 제공하며, 디스플레이 보드는 즉시 피드백을 보여줍니다.

![fitness-pico workflow](assets/fitness-pico-workflow.svg)

## 컴포넌트 역할

| 구성요소 | 역할 | 구현 위치 |
| --- | --- | --- |
| Sensor board | HC-SR04 거리 측정, 푸시업 카운트, RFID UID 발행, 제어 토픽 구독 | `Fit-pico/src/main_sensor.c` |
| Dashboard board | MQTT 집계, HTTP API, 내장 대시보드, 사용자 매핑 저장, mDNS 광고 | `Fit-pico/src/main_iot-dashboard.c` |
| Display board | LCD 상태 표시, 부저 피드백, RFID 사용자 전환 표시 | `Fit-pico/src/main_display.c` |
| MQTT broker | 보드 간 메시지 전달 | 외부 브로커 또는 `mosquitto-lan.conf` |
| Browser | 시작/종료 제어, RFID 등록, 현재 사용자 통계 확인 | Dashboard board의 내장 HTML |

## 데이터 흐름

### 운동 데이터

1. Sensor board가 HC-SR04 거리값으로 푸시업 반복 수와 세트를 계산합니다.
2. Sensor board가 `fitpico/sensor/*` 토픽으로 상태를 발행합니다.
3. Dashboard board와 Display board가 해당 토픽을 구독해 UI를 갱신합니다.

### 제어 흐름

1. Browser가 Dashboard board의 HTTP API를 호출합니다.
2. Dashboard board가 `fitpico/control` 토픽으로 `start` 또는 `stop` 명령을 발행합니다.
3. Sensor board가 명령을 받아 세션 추적 상태를 바꿉니다.

### RFID 흐름

1. Sensor board가 MFRC522로 카드 UID를 읽습니다.
2. UID를 `fitpico/rfid/uid`로 발행합니다.
3. Dashboard board가 등록된 UID인지 확인하고 현재 사용자를 전환합니다.
4. Dashboard board가 `fitpico/rfid/user`를 발행하면 Display board가 사용자명을 잠시 표시합니다.

## MQTT 토픽

| 토픽 | 발행자 | 구독자 | 용도 |
| --- | --- | --- | --- |
| `fitpico/sensor/count` | Sensor | Dashboard, Display | 반복 수, 세트 수, tracking/active 상태 |
| `fitpico/sensor/rest` | Sensor | Dashboard | 세트 후 휴식 정보 |
| `fitpico/sensor/daily` | Sensor | Dashboard, Display | 일일 누적 반복/세트/활동 시간 |
| `fitpico/sensor/speed` | Sensor | Dashboard, Display | 최근 반복 속도와 경고 |
| `fitpico/control` | Dashboard | Sensor | 운동 시작/종료 명령 |
| `fitpico/sensor/status` | Sensor | Dashboard | 센서 보드 heartbeat |
| `fitpico/display/status` | Display | Dashboard | 디스플레이 보드 heartbeat |
| `fitpico/rfid/uid` | Sensor | Dashboard | RFID UID 감지 |
| `fitpico/rfid/user` | Dashboard | Display | 현재 사용자 전환 알림 |

## 브라우저 접근

Dashboard board는 HTTP 서버와 mDNS를 함께 올립니다. 같은 네트워크라면 보통 `http://fitpico-dashboard.local/`로 접속할 수 있고, 동작하지 않으면 보드 IP를 직접 사용하면 됩니다.
