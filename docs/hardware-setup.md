# Hardware Setup

## 보드 역할

| 보드 | 주요 하드웨어 | 역할 |
| --- | --- | --- |
| Sensor board | HC-SR04, MFRC522 | 푸시업 감지, RFID UID 읽기 |
| Display board | 16x2 I2C LCD, buzzer | 실시간 상태 표시와 알림 |
| Dashboard board | Pico 2 W 단독 사용 | 웹 대시보드, MQTT 집계, 사용자 관리 |

## Sensor board 배선

### HC-SR04

| 모듈 핀 | Pico 핀 |
| --- | --- |
| TRIG | `GP14` |
| ECHO | `GP15` |

### MFRC522

| 모듈 핀 | Pico 핀 |
| --- | --- |
| SDA / CS | `GP5` |
| SCK | `GP2` |
| MOSI | `GP3` |
| MISO | `GP4` |
| RST | `GP0` |
| 3.3V / GND | 3.3V / GND |

## Display board 배선

| 모듈 핀 | Pico 핀 |
| --- | --- |
| LCD SDA | `GP4` |
| LCD SCL | `GP5` |
| Buzzer | `GP18` |

Display board의 `GP4`, `GP5`는 I2C LCD용이고 Sensor board의 `GP4`, `GP5`는 RFID용입니다. 서로 다른 Pico 보드이므로 핀 번호가 같아도 충돌하지 않습니다.

## 네트워크 / MQTT 설정

빌드 시 아래 CMake 캐시 값을 넘기면 네트워크 환경에 맞게 설정할 수 있습니다.

```bash
cmake -S Fit-pico -B build/fitpico \
  -DWIFI_SSID="YOUR_WIFI_SSID" \
  -DWIFI_PASSWORD="YOUR_WIFI_PASSWORD" \
  -DMQTT_SERVER="BROKER_IP" \
  -DMQTT_USERNAME="OPTIONAL_USERNAME" \
  -DMQTT_PASSWORD="OPTIONAL_PASSWORD"
```

기본 MQTT 포트는 `1883`입니다. 로컬 브로커 테스트용 설정은 루트의 `mosquitto-lan.conf`를 사용하면 됩니다.

```bash
mosquitto -c mosquitto-lan.conf
```

브로커 로그는 `mosquitto-lan.log`에 기록되며 저장소에는 포함하지 않습니다.

## 빌드 산출물

펌웨어 산출물은 빌드 디렉터리에 생성됩니다.

- `build/fitpico/`
- `Fit-pico/build/`
- `Fit-pico/build-lan/`

문서에서는 생성 위치만 안내하고, 해당 디렉터리는 저장소에 커밋하지 않습니다.
