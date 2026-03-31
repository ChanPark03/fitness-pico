#!/bin/bash
# FitPico — Mosquitto 테스트 스크립트
# 사용법: ./test.sh [pub|sub|all]

BROKER="163.152.213.101"
PORT=1883

case "$1" in
  # ─── 모든 토픽 수신 모니터링 ─────────────────────────────────────────────
  sub|"")
    echo "[FitPico] 브로커 $BROKER 구독 시작 (Ctrl+C 종료)"
    mosquitto_sub -h $BROKER -p $PORT -t "fitpico/#" -v
    ;;

  # ─── 테스트 메시지 발행 ───────────────────────────────────────────────────
  pub)
    echo "[TEST] count 발행"
    mosquitto_pub -h $BROKER -p $PORT \
      -t "fitpico/sensor/count" \
      -m '{"reps":5,"sets":2,"active":"true"}'

    echo "[TEST] speed 발행"
    mosquitto_pub -h $BROKER -p $PORT \
      -t "fitpico/sensor/speed" \
      -m '{"rep":5,"speed_ms":1200,"warn":"ok"}'

    echo "[TEST] rest 발행"
    mosquitto_pub -h $BROKER -p $PORT \
      -t "fitpico/sensor/rest" \
      -m '{"set":2,"rest_sec":60}'

    echo "[TEST] daily 발행"
    mosquitto_pub -h $BROKER -p $PORT \
      -t "fitpico/sensor/daily" \
      -m '{"total_reps":45,"total_sets":5}'
    ;;

  # ─── 구독 + 발행 동시 실행 ────────────────────────────────────────────────
  all)
    $0 sub &
    SUB_PID=$!
    sleep 1
    $0 pub
    wait $SUB_PID
    ;;

  *)
    echo "사용법: $0 [sub|pub|all]"
    exit 1
    ;;
esac
