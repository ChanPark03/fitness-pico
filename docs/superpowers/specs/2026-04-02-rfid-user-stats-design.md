# RFID 사용자 인식 + 사용자별 통계 설계

**날짜**: 2026-04-02  
**프로젝트**: fitness-pico (Raspberry Pi Pico 2W 팔굽혀펴기 트래커)

---

## 배경

현재 시스템은 HTML 대시보드의 Start/Stop 버튼으로 운동을 제어하지만, 사용자 구분이 없어 여러 명이 기기를 공유할 때 통계가 섞인다. MFRC522 RFID 리더기(키트 포함)를 추가해 카드 태그로 사용자를 전환하고, 사용자별 운동 통계를 대시보드에서 분리해서 보여준다.

---

## 결정 사항

| 항목 | 결정 |
|------|------|
| RFID 역할 | 사용자 전환(로그인)만. 운동 시작/종료는 기존 HTML 버튼 유지 |
| RFID 연결 보드 | Sensor 보드 (SPI) |
| 사용자 등록 | 대시보드에서 동적 등록, Dashboard 보드 플래시에 저장 |
| 통계 항목 | 오늘 요약 + 누적 기록 + 7일 히스토리 그래프 + 개인 목표 |

---

## 아키텍처

### 하드웨어 연결 (MFRC522 → Sensor 보드)

```
MFRC522   →   Pico 2W (GP 핀)
SDA(CS)   →   GP5
SCK       →   GP2
MOSI      →   GP3
MISO      →   GP4
RST       →   GP0
3.3V / GND
```

> Sensor 보드에는 LCD가 없으므로 GP4/GP5는 미사용 상태. Display 보드의 I2C LCD(GP4 SDA, GP5 SCL)와 보드가 달라 충돌 없음.

### MQTT 토픽 추가

| 토픽 | 발행자 | 내용 |
|------|--------|------|
| `fitpico/rfid/uid` | Sensor 보드 | `{"uid":"A3:B2:C1:D0"}` — 카드 감지 시 |
| `fitpico/rfid/user` | Dashboard 보드 | `{"name":"박찬웅","uid":"..."}` — 사용자 전환 시 |

### 데이터 흐름

```
카드 태그
  → Sensor 보드: UID 읽기 → fitpico/rfid/uid 발행
  → Dashboard 보드: UID 수신 → 플래시에서 사용자 조회
      → 매핑 있음: 현재 사용자 전환 → fitpico/rfid/user 발행
      → 매핑 없음: "미등록 카드" 상태 반환
  → Display 보드: fitpico/rfid/user 수신 → LCD 1행에 사용자명 표시
```

---

## 플래시 저장 구조 (Dashboard 보드)

### users.json — 카드 등록 정보
```json
[
  {"uid": "A3:B2:C1:D0", "name": "박찬웅", "weight": 70, "goal_sets": 3},
  {"uid": "F1:E2:D3:C4", "name": "이상수", "weight": 75, "goal_sets": 4}
]
```

### stats_{name}.json — 사용자별 통계
```json
{
  "total_reps": 1240,
  "total_sets": 124,
  "total_seconds": 18000,
  "days": {
    "2026-04-02": {"reps": 40, "sets": 4, "seconds": 600}
  }
}
```

---

## 대시보드 UI 변경

### 추가 탭 1 — 카드 등록
- "카드 갖다대기" 버튼 → Sensor 보드를 등록 대기 모드로 전환 (운동 카운팅은 일시 중단)
- 카드 태그 시 UID 수신 → 이름 / 몸무게 / 목표 세트수 입력 폼 표시
- 저장 버튼 → `POST /api/users` → 플래시에 기록 → Sensor 보드 일반 모드 복귀

### 추가 탭 2 — 사용자별 통계
- 현재 로그인된 사용자 표시 (카드 태그 기준)
- 오늘 요약: 렙수 / 세트수 / 운동 시간 / 소모 칼로리
- 누적 기록: 총 렙수 / 총 세트수 / 총 운동 시간
- 7일 히스토리: 날짜별 렙수 막대 그래프
- 목표 달성률: 목표 세트수 대비 오늘 세트수

---

## 변경 파일 목록

| 파일 | 변경 내용 |
|------|-----------|
| `common/config.h` | SPI 핀 정의, `fitpico/rfid/uid`, `fitpico/rfid/user` 토픽 추가 |
| `src/main_sensor.c` | MFRC522 SPI 초기화, UID 폴링 루프, `fitpico/rfid/uid` 발행 |
| `src/main_display.c` | `fitpico/rfid/user` 구독, LCD 사용자명 표시 |
| `src/main_iot-dashboard.c` | 플래시 read/write, 사용자 관리 API, 통계 집계, 대시보드 UI 확장 |
| `CMakeLists.txt` | MFRC522 라이브러리(또는 직접 구현 드라이버) 추가 |

---

## 새 API 엔드포인트

| 메서드 | 경로 | 설명 |
|--------|------|------|
| `GET` | `/api/users` | 등록된 사용자 목록 |
| `POST` | `/api/users` | 새 사용자 등록 (UID + 이름 + 몸무게 + 목표) |
| `DELETE` | `/api/users/{uid}` | 사용자 삭제 |
| `GET` | `/api/stats/{name}` | 사용자별 통계 조회 |
| `GET` | `/api/users/current` | 현재 로그인된 사용자 |
| `POST` | `/api/rfid/scan-mode` | 등록 대기 모드 전환 |

---

## 통계 저장 시점

- 운동 중 렙/세트 카운트는 Dashboard 보드 RAM에 누적
- HTML "Stop" 버튼 클릭 시 현재 사용자의 `stats_{name}.json`에 병합 저장
- 사용자 전환(카드 태그) 시에도 기존 사용자 통계 자동 저장 후 전환

---

## 검증 방법

1. MFRC522 배선 후 카드 태그 시 `fitpico/rfid/uid` 토픽에 UID 발행 확인
2. 대시보드에서 사용자 등록 → 재시작 후 플래시에서 복원 확인
3. 등록된 카드 태그 → LCD에 사용자명 표시 확인
4. 운동 후 통계 탭에서 사용자별 렙수/세트수 분리 확인
5. 미등록 카드 태그 시 "미등록 카드" 처리 확인
