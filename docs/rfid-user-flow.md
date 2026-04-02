# RFID User Flow

## 목적

RFID는 운동 시작/종료를 대신하는 입력이 아니라, 현재 사용자를 전환하고 사용자별 통계를 분리해서 보여주기 위한 수단입니다. 운동 제어는 계속 Dashboard UI의 `운동 시작`과 `운동 종료` 버튼으로 수행합니다.

## 사용자 전환 흐름

1. 사용자가 Sensor board의 MFRC522에 카드를 태그합니다.
2. Sensor board가 UID를 읽어 `fitpico/rfid/uid`로 `{"uid":"A3:B2:C1:D0"}` 형태의 메시지를 발행합니다.
3. Dashboard board가 UID를 조회합니다.
4. 등록된 사용자면 현재 사용자를 전환하고 `fitpico/rfid/user`로 이름, UID, 체중, 목표 세트를 브로드캐스트합니다.
5. Display board는 해당 메시지를 받아 3초 동안 사용자명을 LCD에 표시합니다.

같은 카드는 짧은 시간 안에 반복 인식되지 않도록 Sensor board에서 쿨다운을 적용합니다.

## 등록 흐름

1. Browser에서 `카드 갖다대기 (등록)` 버튼을 누릅니다.
2. Dashboard board가 등록 대기 모드로 들어갑니다.
3. 카드가 태그되면 Dashboard board가 UID를 `pending_uid`로 보관합니다.
4. 사용자가 이름, 체중, 목표 세트를 입력하고 등록을 완료합니다.
5. Dashboard board가 사용자 목록을 플래시에 저장합니다.

등록 API는 `POST /api/users`이고, 등록 대기 시작은 `POST /api/rfid/scan-mode`입니다.

## 저장 / 복원 방식

### 사용자 목록

- 저장 위치: Dashboard board 플래시
- 저장 내용: UID, 이름, 체중, 목표 세트
- 복원 시점: Dashboard board 부팅 시 자동 로드

### 통계

- 저장 위치: 현재 구현은 Dashboard board RAM
- 포함 값: 현재 로그인 사용자 기준 오늘 세션 반복 수/세트 수, 누적 반복 수/세트 수
- 주의: 사용자 매핑과 달리 통계는 재부팅 후 유지되지 않습니다

즉, 현재 코드 기준으로 영속 저장되는 것은 RFID 사용자 매핑이고, 통계는 런타임 상태입니다.

## MQTT 토픽

| 토픽 | 방향 | 예시 |
| --- | --- | --- |
| `fitpico/rfid/uid` | Sensor → Dashboard | `{"uid":"A3:B2:C1:D0"}` |
| `fitpico/rfid/user` | Dashboard → Display | `{"name":"Chan","uid":"A3:B2:C1:D0","weight":70,"goal_sets":3}` |

## 주요 API

| 메서드 | 경로 | 설명 |
| --- | --- | --- |
| `GET` | `/api/status` | 대시보드 전체 상태와 `pending_uid` 확인 |
| `GET` | `/api/users` | 등록된 사용자 목록 조회 |
| `GET` | `/api/users/current` | 현재 로그인 사용자 조회 |
| `POST` | `/api/users` | UID와 사용자 정보 등록 |
| `POST` | `/api/rfid/scan-mode` | 카드 등록 대기 시작 |
| `GET` | `/api/stats/current` | 현재 로그인 사용자 기준 통계 조회 |

## UI 동작

- Dashboard UI는 현재 사용자명, 등록 대기 상태, 오늘 세션 통계를 함께 보여줍니다.
- 미등록 카드가 태그되면 등록 폼을 열어 이어서 사용자 정보를 입력할 수 있습니다.
- 등록 완료 후에는 현재 사용자 통계와 목표 달성 여부를 바로 확인할 수 있습니다.
