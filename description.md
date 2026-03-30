# 🏋️ FitPico — 팀 역할 분담표
갯수 카운트. 타이머, lcd 출력. 


> Raspberry Pi Pico 2 W 기반 IoT 운동 트래커 프로젝트  
> 기간: 4일 스프린트 | 팀원: 3명

---

## 👥 팀 구성 및 역할

| 구분 | 팀원 A | 팀원 B | 팀원 C |
|------|--------|--------|--------|
| **담당** | 센서 & 하드웨어 | 디스플레이 & UX | IoT & 대시보드 |
| **Pico 역할** | 센서부 (입력) | 출력부 (피드백) | 서버부 (통신/저장) |

---

## 🔵 팀원 A — 센서 & 하드웨어

**담당: 운동 동작 감지 & 측정**

### 사용 부품
- HC-SR04 초음파 센서 (거리 측정 → 운동 횟수 카운트)
- PIR 센서 (움직임 감지 → 운동 시작/정지 자동 인식)
- DHT 온습도 센서 (운동 환경 측정)
- 4×4 키패드 (운동 모드 입력)
- LED (세트 완료 피드백)

### 담당 파일
```
sensor/
├── main.py          # 메인 루프
├── hcsr04.py        # 초음파 센서 드라이버
├── pir.py           # PIR 센서 모듈
├── dht.py           # 온습도 센서 모듈
└── keypad.py        # 키패드 입력 처리
```

### 구현 목표
- [ ] HC-SR04 거리 측정 → 스쿼트/푸시업 횟수 카운트 로직
- [ ] PIR 센서로 운동 시작/정지 자동 감지
- [ ] 키패드로 운동 종목 선택 (스쿼트 / 푸시업 / 플랭크)
- [ ] MQTT로 측정 데이터 publish

---

## 🟢 팀원 B — 디스플레이 & UX

**담당: 운동 결과 표시 & 실시간 피드백**

### 사용 부품
- LCD 디스플레이 (실시간 세트/횟수/시간 표시)
- 서보모터 (달성률 게이지 — 0°~180°)
- 7세그먼트 (카운트다운 타이머)
- 부저 (세트 완료 알림음)
- IR 리모컨 (운동 시작/정지/초기화)
- DC 모터 + 팬 (쿨링 피드백)

### 담당 파일
```
display/
├── main.py          # 메인 루프
├── lcd.py           # LCD 디스플레이 드라이버
├── servo.py         # 서보모터 게이지 제어
├── segment.py       # 7세그먼트 드라이버
└── ir_remote.py     # IR 리모컨 수신
```

### 구현 목표
- [ ] LCD에 실시간 운동 현황 표시 (종목 / 횟수 / 세트 / 경과시간)
- [ ] MQTT subscribe → 센서 데이터 수신 후 화면 갱신
- [ ] 서보모터로 목표 달성률 시각적 게이지 구현
- [ ] IR 리모컨으로 전체 시스템 시작/정지 제어

---

## 🟠 팀원 C — IoT & 대시보드

**담당: WiFi 통신 & 데이터 시각화**

### 사용 부품
- Pico 2 W 내장 WiFi (MQTT 통신)
- RFID 모듈 (사용자 인식 → 개인별 기록)
- 수분 센서 (수분 보충 알림)

### 담당 파일
```
server/
├── main.py          # 메인 루프
├── mqtt_broker.py   # MQTT 브로커 연결
├── rfid.py          # RFID 사용자 인식
├── water_sensor.py  # 수분 센서
└── dashboard/
    ├── app.py       # Flask/HTTP 서버
    ├── index.html   # 웹 대시보드
    └── style.css    # 스타일
```

### 구현 목표
- [ ] Pico 2 W WiFi 연결 및 MQTT 브로커 설정
- [ ] 웹 대시보드 구축 (운동 기록 실시간 차트)
- [ ] RFID로 사용자 구분 → 개인별 기록 저장


---

## 🏗️ 공통 규칙

### 브랜치 전략
```
main          ← 최종 배포 (Day 4에만 머지)
  └── develop ← 통합 브랜치
        ├── feature/sensor-hcsr04    (팀원 A)
        ├── feature/display-lcd      (팀원 B)
        └── feature/iot-dashboard    (팀원 C)
```

### 커밋 메시지 컨벤션
```
feat:     새 기능 추가
fix:      버그 수정
docs:     문서 수정
refactor: 코드 리팩터링
test:     테스트 코드
```

예시:
```
feat: HC-SR04 거리 측정 함수 추가
fix: PIR 센서 오감지 필터링
docs: README 회로도 추가
```

### 공통 설정 파일 (`shared/config.py`)
```python
# MQTT 토픽 (Day 1 싱크에서 확정)
MQTT_BROKER_IP   = "192.168.x.x"
TOPIC_COUNT      = "fitpico/sensor/count"
TOPIC_TEMP       = "fitpico/sensor/temp"
TOPIC_HUMIDITY   = "fitpico/sensor/humidity"
TOPIC_CONTROL    = "fitpico/control"

# WiFi
WIFI_SSID        = ""   # secrets.py에서 불러올 것
WIFI_PASSWORD    = ""   # secrets.py에서 불러올 것
```

> ⚠️ `secrets.py`는 `.gitignore`에 추가하여 절대 커밋하지 않습니다.

---

## 📅 4일 스프린트 일정

| 일차 | 팀원 A | 팀원 B | 팀원 C |
|------|--------|--------|--------|
| **Day 1** | HC-SR04 / PIR / DHT 개별 테스트 | LCD / 서보 / 7세그 테스트 | WiFi 연결 + MQTT 브로커 셋업 |
| **Day 2** | 운동 카운터 로직 + MQTT publish | MQTT subscribe + LCD 실시간 갱신 | 웹 대시보드 v1 + 데이터 저장 |
| **Day 3** | A+B 통합 테스트 및 오차 보정 | B+C UI 연동 + IR 리모컨 전체 제어 | RFID 프로필 + 수분 알림 + 히스토리 |
| **Day 4** | 엣지케이스 처리 + 코드 정리 | UI 최종 polish + 데모 시나리오 | 발표 자료 + README + 아키텍처 문서화 |

### 팀 싱크 일정
- **Day 1 저녁**: MQTT 토픽 구조 확정, JSON 데이터 포맷 합의, 각 Pico IP 고정
- **Day 3 저녁**: 전체 시스템 연결 데모, 버그 리스트업, Day 4 분담 확정

---

## 🔌 시스템 아키텍처

```
[센서 Pico A] --MQTT publish--> [서버 Pico C] --HTTP--> [웹 대시보드]
                                      |
                               MQTT subscribe
                                      |
                              [출력 Pico B]
                          (LCD / 서보 / 부저)
```

---

## 📦 레포지토리 구조

```
fitpico/
├── sensor/           # 팀원 A
├── display/          # 팀원 B
├── server/           # 팀원 C
├── shared/
│   └── config.py     # 공통 상수 (MQTT 토픽 등)
├── .gitignore
└── README.md
```

### `.gitignore` 필수 항목
```
*.pyc
__pycache__/
secrets.py
*.env
```