#!/bin/bash
# WoW Forever login probe against forever_fake.py (REST + Battle.net RPC + encrypted world session on this machine).
#   run_forever.sh [probe binary]   (default: builds it with net/forever/build.sh linux)
set -e
T=$(cd "$(dirname "$0")" && pwd); N="$T/.."
OUT=${OUT:-/tmp/w3d_forever}; mkdir -p "$OUT"
PROBE=${1:-}
if [ -z "$PROBE" ]; then bash "$N/forever/build.sh" linux "$OUT/build" > "$OUT/build.log" 2>&1 || { tail -20 "$OUT/build.log"; exit 1; }; PROBE="$OUT/build/w3dfprobe"; fi
openssl req -x509 -newkey rsa:2048 -nodes -days 2 -subj "/CN=trinity.actual.battle.net" -keyout "$OUT/key.pem" -out "$OUT/cert.pem" 2>/dev/null
B=$((20000 + RANDOM % 20000))
python3 "$T/forever_fake.py" "$OUT/cert.pem" "$OUT/key.pem" $B $((B+1)) $((B+2)) 70658 > "$OUT/fake.log" 2>&1 &
FAKE=$!
for i in $(seq 50); do grep -q "FAKE READY" "$OUT/fake.log" 2>/dev/null && break; sleep 0.1; done
set +e
echo "secret pass" | timeout 80 "$PROBE" 127.0.0.1 test@example.org --bnet-port $((B+1)) --stay 12 --out "$OUT/report.txt" --log "$OUT/packets.fwl" > "$OUT/probe.log" 2>&1
RC=$?
wait $FAKE; FRC=$?
set -e
cat "$OUT/fake.log" | grep -v "FAKE READY"
echo "probe exit $RC, fake exit $FRC"
grep -q "DONE: login" "$OUT/report.txt" && grep -q "Testchar" "$OUT/report.txt" && grep -q "Forever Test" "$OUT/report.txt" && grep -q "login page (web_auth_url): https://127.0.0.1:$B/" "$OUT/report.txt" && \
  grep -q "SMSG_SET_TIME_ZONE_INFORMATION .* (zlib)" "$OUT/report.txt" && grep -q "SMSG_ACCOUNT_DATA_TIMES .* (zlib)" "$OUT/report.txt" && \
  grep -q "SMSG_PONG" "$OUT/report.txt" && [ $RC = 0 ] && [ $FRC = 0 ] && echo "FOREVER PROBE: PASS" || { echo "FOREVER PROBE: FAIL"; tail -30 "$OUT/report.txt"; exit 1; }
# step 2: an account with no character: the probe creates one (--create) and enters the world (--enter)
python3 "$T/forever_fake.py" "$OUT/cert.pem" "$OUT/key.pem" $B $((B+1)) $((B+2)) 70658 create > "$OUT/fake2.log" 2>&1 &
FAKE=$!
for i in $(seq 50); do grep -q "FAKE READY" "$OUT/fake2.log" 2>/dev/null && break; sleep 0.1; done
set +e
echo "secret pass" | timeout 80 "$PROBE" 127.0.0.1 test@example.org --bnet-port $((B+1)) --create Probe --enter --stay 3 --out "$OUT/report2.txt" --log "$OUT/packets2.fwl" > "$OUT/probe2.log" 2>&1
RC=$?
wait $FAKE; FRC=$?
set -e
grep -v "FAKE READY" "$OUT/fake2.log"
echo "probe (create + enter) exit $RC, fake exit $FRC"
grep -q "SMSG_CREATE_CHAR code 24 (success)" "$OUT/report2.txt" && grep -q "SMSG_LOGIN_VERIFY_WORLD: map 0 at (-8950.0" "$OUT/report2.txt" && grep -q "the character is in the world" "$OUT/report2.txt" && \
  [ $RC = 0 ] && [ $FRC = 0 ] && echo "FOREVER CREATE + ENTER: PASS" || { echo "FOREVER CREATE + ENTER: FAIL"; tail -30 "$OUT/report2.txt"; exit 1; }
# the world stream of a real login: SMSG_UPDATE_OBJECT parsed (sanitizers on)
g++ -std=c++20 -O1 -fsanitize=address,undefined -Wall -Wextra "$T/forever_update_test.cpp" "$N/forever/fupdate.cpp" -o "$OUT/forever_update_test" && (cd "$T/.." && "$OUT/forever_update_test" "$T/data/forever_update.fwl") | tail -3
# the modern data formats (DB2 WDC5, WDT MAID, ADT chunks, M2 MD21) on synthetic files
python3 "$T/gen_forever_data.py" "$OUT/data" > /dev/null
g++ -std=c++17 -Wall -fsanitize=address,undefined -I"$N/forever" "$T/forever_data_test.cpp" "$N/forever/fdata.cpp" -o "$OUT/forever_data_test"
"$OUT/forever_data_test" "$OUT/data"
