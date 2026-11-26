#!/usr/bin/env bash
# End-to-end Wine test: fake EOSSDK + two harness processes + real sn2lan.dll,
# exercising the MinHook detours, UDP tunnel, HELLO learning and reassembly.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="${BUILD_DIR:-$root/build/mingw}"
work="$root/build/integration"
cxx="${CXX:-x86_64-w64-mingw32-g++}"

"$root/scripts/build.sh" >/dev/null

rm -rf "$work"
mkdir -p "$work"
cp "$build/sn2lan.dll" "$work/"

"$cxx" -O2 -static -shared -o "$work/EOSSDK-Win64-Shipping.dll" "$root/tests/fake_eos/fake_eos.cpp"
"$cxx" -O2 -static -o "$work/harness.exe" "$root/tests/integration/harness.cpp"

cat > "$work/host.ini" <<'INI'
[tunnel]
enabled = true
role = host
bind_address = 127.0.0.1
bind_port = 27101
overlay = false
debug = true
INI

cat > "$work/client.ini" <<'INI'
[tunnel]
enabled = true
role = client
bind_address = 127.0.0.1
bind_port = 0
host = 127.0.0.1
host_port = 27101
overlay = false
debug = true
INI

cd "$work"
SN2LAN_CONFIG="$work/host.ini" SN2LAN_LOG="$work/host.log" wine ./harness.exe host >host.out 2>/dev/null &
host_pid=$!
sleep 1.5
set +e
SN2LAN_CONFIG="$work/client.ini" SN2LAN_LOG="$work/client.log" wine ./harness.exe client >client.out 2>/dev/null
client_code=$?
wait $host_pid
host_code=$?
set -e

echo "--- host ---"; cat host.out
echo "--- client ---"; cat client.out
echo "--- host log ---"; grep -E "learned|tunnel|hook|ready|ERROR|WARN" host.log || true
echo "--- client log ---"; grep -E "learned|alive|hook|ready|ERROR|WARN" client.log || true

fail=0
[ "$client_code" -eq 0 ] || { echo "FAIL: client exit $client_code"; fail=1; }
[ "$host_code" -eq 0 ] || { echo "FAIL: host exit $host_code"; fail=1; }
grep -q "peer 22222222222222222222222222222222 learned" host.log || { echo "FAIL: host did not learn client"; fail=1; }
grep -q "direct tunnel to host is alive" client.log || { echo "FAIL: client never saw the host directly"; fail=1; }

if [ "$fail" -ne 0 ]; then
    echo "integration test FAILED"
    exit 1
fi
echo "all integration tests passed"
