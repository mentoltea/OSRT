#!/usr/bin/env bash
set -u
cd "$(dirname "$0")"

SOCK=/tmp/OSRT_lab1.sock
OUT=$(mktemp -d /tmp/osrt_test.XXXXXX)
SERVER_LOG=$OUT/server.log
SERVER_PID=

CMD_NOP=0
CMD_WAIT=1
CMD_ECHO=2
CMD_NEG=3
CMD_TIMER=4

PASS=0
FAIL=0
CLIENTS=0

GREEN=$'\e[32m'; RED=$'\e[31m'; CYAN=$'\e[36m'; RESET=$'\e[0m'

now_ms() { date +%s%3N; }
section() { echo; echo "${CYAN}=== $* ===${RESET}"; }
pass() { echo "  ${GREEN}[PASS]${RESET} $*"; PASS=$((PASS + 1)); }
fail() { echo "  ${RED}[FAIL]${RESET} $*"; FAIL=$((FAIL + 1)); }
check() {
    local desc=$1; shift
    if "$@"; then pass "$desc"; else fail "$desc"; fi
}

# "type value" из вывода клиента
answer_of() {
    sed -nE 's/.*Received Answer \{ type: (-?[0-9]+), value: (-?[0-9]+) \}.*/\1 \2/p' "$1"
}

# Синхронный запуск клиента: run_client <name> <priority> <command> <value>
run_client() {
    CLIENTS=$((CLIENTS + 1))
    ./client "$2" "$3" "$4" > "$OUT/$1.out" 2>&1
}

# Фоновый запуск клиента; по завершении пишет метку времени в <name>.end
run_client_bg() {
    CLIENTS=$((CLIENTS + 1))
    ( ./client "$2" "$3" "$4" > "$OUT/$1.out" 2>&1; now_ms > "$OUT/$1.end" ) &
    BG_PIDS+=($!)
}

cleanup() {
    if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill -INT "$SERVER_PID" 2>/dev/null
        wait "$SERVER_PID" 2>/dev/null
    fi
}
trap cleanup EXIT

# ---------------------------------------------------------------------------
section "Сборка"
make clear > /dev/null 2>&1
if make all > "$OUT/build.log" 2>&1; then
    pass "make all"
else
    fail "make all"; cat "$OUT/build.log"; exit 1
fi

# ---------------------------------------------------------------------------
section "Запуск сервера"
./server > "$SERVER_LOG" 2>&1 &
SERVER_PID=$!
for _ in $(seq 30); do
    grep -q "Listening on" "$SERVER_LOG" && [ -S "$SOCK" ] && break
    sleep 0.1
done
check "сервер слушает $SOCK (pid $SERVER_PID)" test -S "$SOCK"
[ -S "$SOCK" ] || { cat "$SERVER_LOG"; exit 1; }

# ---------------------------------------------------------------------------
section "Часть 1. Синхронный запрос/ответ (Unix domain socket)"

run_client echo 0 $CMD_ECHO 42
check "ECHO 42 -> {0, 42}"        [ "$(answer_of "$OUT/echo.out")" = "0 42" ]

run_client neg 0 $CMD_NEG 7
check "NEG 7 -> {0, -7}"          [ "$(answer_of "$OUT/neg.out")" = "0 -7" ]

run_client nop 0 $CMD_NOP 123
check "NOP -> {0, 0}"             [ "$(answer_of "$OUT/nop.out")" = "0 0" ]

run_client bad 0 99 1
check "неизвестная команда -> {-1, 0}" [ "$(answer_of "$OUT/bad.out")" = "-1 0" ]

t0=$(now_ms)
run_client wait 0 $CMD_WAIT 2
dt=$(( $(now_ms) - t0 ))
check "WAIT 2: клиент заблокирован до ответа (${dt} мс >= 2000)" [ "$dt" -ge 2000 ]
check "WAIT 2 -> {0, 0}"          [ "$(answer_of "$OUT/wait.out")" = "0 0" ]

# ---------------------------------------------------------------------------
section "Часть 2. Приоритетная очередь"
# Блокер занимает единственного воркера на 3 с, остальные копятся в очереди
# и должны быть обработаны в порядке убывания приоритета.
log_start=$(wc -l < "$SERVER_LOG")
BG_PIDS=()
run_client_bg blocker 0 $CMD_WAIT 3
sleep 0.5
for p in 1 5 10 3; do
    run_client_bg "prio$p" "$p" $CMD_ECHO "$p"
    sleep 0.2
done
wait "${BG_PIDS[@]}"

order=$(tail -n +"$((log_start + 1))" "$SERVER_LOG" \
        | sed -nE 's/^\s*priority (-?[0-9]+)/\1/p' | tr '\n' ' ' | sed 's/ $//')
echo "  порядок обработки (priority): $order"
check "порядок 0 10 5 3 1 (блокер, затем по убыванию приоритета)" [ "$order" = "0 10 5 3 1" ]
for p in 1 5 10 3; do
    check "клиент priority=$p получил свой ответ {0, $p}" [ "$(answer_of "$OUT/prio$p.out")" = "0 $p" ]
done

# ---------------------------------------------------------------------------
section "Часть 3-4. Асинхронное уведомление через eventfd (CMD_TIMER)"

t0=$(now_ms)
run_client timer 0 $CMD_TIMER 2
dt=$(( $(now_ms) - t0 ))
check "клиент получил уведомление через eventfd" grep -q "ASYNC TIMER NOTIFICATION" "$OUT/timer.out"
check "счётчик eventfd = 1"       grep -q "Signal count in eventfd: 1" "$OUT/timer.out"
n_line=$(grep -n "ASYNC TIMER NOTIFICATION" "$OUT/timer.out" | head -1 | cut -d: -f1)
a_line=$(grep -n "Received Answer" "$OUT/timer.out" | head -1 | cut -d: -f1)
check "уведомление пришло раньше ответа" [ -n "$n_line" ] && [ -n "$a_line" ] && [ "$n_line" -lt "$a_line" ]
check "уведомление через ~2 с (${dt} мс >= 2000)" [ "$dt" -ge 2000 ]
check "TIMER 2 -> {0, 2}"         [ "$(answer_of "$OUT/timer.out")" = "0 2" ]
check "сервер передал eventfd (SCM_RIGHTS) и записал в него" \
    grep -q "Async notification sent to eventfd" "$SERVER_LOG"

# ---------------------------------------------------------------------------
section "Несколько клиентов одновременно"

# Три таймера параллельно: общее время ~3 с (а не 1+2+3 = 6 с),
# уведомления приходят в порядке длительности таймеров.
BG_PIDS=()
t0=$(now_ms)
run_client_bg t3 0 $CMD_TIMER 3
run_client_bg t1 0 $CMD_TIMER 1
run_client_bg t2 0 $CMD_TIMER 2
sleep 0.3
# Пока таймеры тикают, синхронный запрос обслуживается сразу
s0=$(now_ms)
run_client during 0 $CMD_ECHO 777
sdt=$(( $(now_ms) - s0 ))
wait "${BG_PIDS[@]}"
dt=$(( $(now_ms) - t0 ))

check "ECHO во время таймеров обслужен сразу (${sdt} мс < 1000)" [ "$sdt" -lt 1000 ]
check "ECHO во время таймеров -> {0, 777}" [ "$(answer_of "$OUT/during.out")" = "0 777" ]
check "3 таймера (1,2,3 с) выполнены параллельно за ${dt} мс (< 4500)" [ "$dt" -lt 4500 ]
for t in 1 2 3; do
    check "таймер $t с: уведомление получено" grep -q "ASYNC TIMER NOTIFICATION" "$OUT/t$t.out"
done
e1=$(cat "$OUT/t1.end"); e2=$(cat "$OUT/t2.end"); e3=$(cat "$OUT/t3.end")
check "уведомления пришли в порядке 1с < 2с < 3с" [ "$e1" -lt "$e2" ] && [ "$e2" -lt "$e3" ]

# Пачка из 10 одновременных ECHO: каждый клиент получает именно свой ответ
BG_PIDS=()
for i in $(seq 1 10); do
    run_client_bg "burst$i" $((i % 4)) $CMD_ECHO $((i * 11))
done
wait "${BG_PIDS[@]}"
ok=1
for i in $(seq 1 10); do
    [ "$(answer_of "$OUT/burst$i.out")" = "0 $((i * 11))" ] || { ok=0; echo "  burst$i: $(answer_of "$OUT/burst$i.out")"; }
done
check "10 одновременных клиентов получили корректные ответы" [ "$ok" -eq 1 ]

# ---------------------------------------------------------------------------
section "Логирование на сервере"
sleep 0.2
connected=$(grep -c "connected" "$SERVER_LOG")
picked=$(grep -c "picked up client" "$SERVER_LOG")
notified=$(grep -c "Async notification sent" "$SERVER_LOG")
timers=$(grep -c "Timer finished" "$SERVER_LOG")
check "залогированы все подключения ($connected из $CLIENTS)" [ "$connected" -eq "$CLIENTS" ]
check "залогирована обработка всех запросов ($picked из $CLIENTS)" [ "$picked" -eq "$CLIENTS" ]
check "залогированы все уведомления ($notified из 4)" [ "$notified" -eq 4 ]
check "залогированы завершения таймеров ($timers из 4)" [ "$timers" -eq 4 ]

# ---------------------------------------------------------------------------
section "Завершение сервера по SIGINT"
kill -INT "$SERVER_PID"
wait "$SERVER_PID" 2>/dev/null
SERVER_PID=
check "в логе есть 'Shutting down'" grep -q "Shutting down" "$SERVER_LOG"
check "файл сокета удалён"         test ! -e "$SOCK"

# ---------------------------------------------------------------------------
section "Итог"
echo "  ${GREEN}PASS: $PASS${RESET}   ${RED}FAIL: $FAIL${RESET}"
echo "  Логи сервера и клиентов: $OUT"
[ "$FAIL" -eq 0 ]
