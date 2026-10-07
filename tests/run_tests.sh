#!/bin/sh
# Тесты: sh tests/run_tests.sh (или make test).
# Каждый tests/*.cfg запускается без задержки; проверяются код завершения
# (строка «# exit: N», по умолчанию 0), строки «# expect: текст» в выводе
# и отсутствие нарушений инвариантов. Отдельно проверяется остановка по Ctrl+C.
cd "$(dirname "$0")/.." || exit 1
OUT=/tmp/restaurant_test.out
LOG=/tmp/restaurant_test.log
pass=0
fail=0

result() {   # result ОК? ОПИСАНИЕ
    if [ "$1" -eq 1 ]; then pass=$((pass + 1)); echo "  OK    $2"
    else fail=$((fail + 1)); echo "  FAIL  $2"; fi
}

for cfg in tests/*.cfg; do
    want=$(sed -n 's/^# exit: //p' "$cfg")
    [ -n "$want" ] || want=0
    ./restaurant -c "$cfg" -d 0 -l "$LOG" > "$OUT" 2>&1
    code=$?
    ok=1
    [ "$code" -eq "$want" ] || ok=0
    sed -n 's/^# expect: //p' "$cfg" > "$OUT.expect"
    while IFS= read -r line; do
        grep -qF -- "$line" "$OUT" || ok=0
    done < "$OUT.expect"
    grep -q "нарушений: [1-9]" "$OUT" && ok=0
    result $ok "$cfg (код $code)"
done

# Режим без ограничения посетителей: остановка по Ctrl+C (SIGINT) с итогами и кодом 0
./restaurant -n 0 -d 5 -l "$LOG" > "$OUT" 2>&1 &
pid=$!
sleep 1
kill -INT $pid
wait $pid
code=$?
ok=0
[ "$code" -eq 0 ] && grep -q "SIGINT" "$LOG" && grep -q "Итоги" "$LOG" && ok=1
result $ok "Ctrl+C в режиме без ограничения (код $code, итоги в журнале)"

rm -f "$OUT" "$OUT.expect" "$LOG"
echo "Пройдено: $pass, не пройдено: $fail"
[ "$fail" -eq 0 ]
