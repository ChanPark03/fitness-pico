# fitness-pico
FitPico_C_Project/
├── CMakeLists.txt          # 프로젝트 전체 빌드 설정 (가장 중요!)
├── pico_sdk_import.cmake   # SDK 연결을 위한 필수 파일
├── common/                 # 공통 헤더 및 설정
│   ├── config.h            # WiFi SSID, PW, MQTT 토픽 정의 (매크로)
│   └── lwipopts.h          # 네트워크(LwIP) 설정 파일
├── src/                    # 각 팀원별 소스 코드
│   ├── main_sensor.c       # 팀원 A: 초음파 센서 제어 및 MQTT 송신
│   ├── main_display.c      # 팀원 B: LCD/모터 제어 및 MQTT 수신
│   └── main_hub.c          # 팀원 C: 데이터 중계 및 로직 처리
└── lib/                    # 외부 라이브러리 (MQTT, LCD 드라이버 등)
    ├── paho_mqtt/          # C용 MQTT 클라이언트 라이브러리
    └── i2c_display/        # LCD 제어 라이브러리