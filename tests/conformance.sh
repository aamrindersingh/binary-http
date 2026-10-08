#!/usr/bin/env bash
# Conformance suite for BHTTP/1.
#
# Every assertion traces to a MUST in SPEC.md. The raw-byte cases go
# through tests/rawframe.py, which was written from the spec and shares
# no code with either implementation, so a bug common to both would still
# be caught here.

set -uo pipefail
cd "$(dirname "$0")/.."

PORT=${PORT:-9411}
PASS=0
FAIL=0

green() { printf '\033[32m%s\033[0m' "$1"; }
red()   { printf '\033[31m%s\033[0m' "$1"; }

check() { # check <name> <expected> <actual> <spec-ref>
    if [ "$2" = "$3" ]; then
        printf '  %s  %-34s %-28s %s\n' "$(green PASS)" "$1" "$3" "$4"
        PASS=$((PASS + 1))
    else
        printf '  %s  %-34s %-28s %s\n' "$(red FAIL)" "$1" "want=$2 got=$3" "$4"
        FAIL=$((FAIL + 1))
    fi
}

cleanup() { [ -n "${SRV:-}" ] && kill "$SRV" 2>/dev/null; wait 2>/dev/null; }
trap cleanup EXIT

if [ ! -x ./bserve ] || [ ! -x ./bcurl ]; then
    echo "build first: make" >&2
    exit 2
fi

./bserve ./www "$PORT" -v > tests/server.log 2>&1 &
SRV=$!
sleep 1

if ! kill -0 "$SRV" 2>/dev/null; then
    echo "server failed to start:" >&2
    cat tests/server.log >&2
    exit 1
fi

echo
echo "BHTTP/1 conformance, port $PORT"
echo

echo "client and server interop"
BODY=$(./bcurl "localhost:$PORT/index.html" 2>/dev/null | head -1)
check "fetch returns the body" "<!doctype html>" "$BODY" "SPEC 5,6"

./bcurl "localhost:$PORT/index.html" >/dev/null 2>&1
check "2xx exits zero" "0" "$?" "SPEC 6"

./bcurl "localhost:$PORT/missing.html" >/dev/null 2>&1
check "4xx exits non-zero" "1" "$?" "SPEC 6"

OUT=$(./bcurl -v -n 5 "localhost:$PORT/hello.txt" 2>&1 >/dev/null | grep -c "request(s) over 1 connection")
check "5 requests, one connection" "1" "$OUT" "SPEC 1"

IDS=$(./bcurl -v -n 3 "localhost:$PORT/hello.txt" 2>&1 >/dev/null \
      | grep -o 'REQUEST id=[0-9]*' | grep -o '[0-9]*' | tr '\n' ',' | sed 's/,$//')
check "request IDs increase from 1" "1,2,3" "$IDS" "SPEC 1"

echo
echo "status codes"
for c in ok:200 traversal:403 bad-method:405 no-path:400 request-id-zero:400; do
    name=${c%%:*}; want=${c##*:}
    got=$(python3 tests/rawframe.py "$PORT" "$name" 2>/dev/null)
    check "$name" "$want" "$got" "SPEC 6"
done
got=$(python3 tests/rawframe.py "$PORT" ok 2>/dev/null)   # keep numbering sane
NF=$(./bcurl "localhost:$PORT/nope" 2>/dev/null | head -1)
check "404 body" "404 not found" "$NF" "SPEC 6"

echo
echo "header block decoding, every case is a 400"
for name in truncated-block trailing-bytes unknown-index zero-literal-name; do
    got=$(python3 tests/rawframe.py "$PORT" "$name" 2>/dev/null)
    check "$name" "400" "$got" "SPEC 4"
done

echo
echo "limits"
got=$(python3 tests/rawframe.py "$PORT" oversized 2>/dev/null)
check "oversized frame -> GOAWAY(2)" "GOAWAY:2" "$got" "SPEC 7"

echo
echo "forward compatibility, the rule that may not be skipped"
got=$(python3 tests/rawframe.py "$PORT" unknown-frame-skipped 2>/dev/null)
check "server skips unknown type" "200" "$got" "SPEC 8"

./bcurl --send-unknown-frame "localhost:$PORT/index.html" >/dev/null 2>&1
check "server skips it from bcurl too" "0" "$?" "SPEC 8"

kill "$SRV" 2>/dev/null; wait 2>/dev/null
./bserve ./www "$PORT" -v --inject-unknown > tests/server-inject.log 2>&1 &
SRV=$!
sleep 1
SKIPPED=$(./bcurl -v -n 3 "localhost:$PORT/hello.txt" 2>&1 >/dev/null | grep -c "per SPEC 8")
check "client skips unknown type" "3" "$SKIPPED" "SPEC 8"

./bcurl "localhost:$PORT/index.html" >/dev/null 2>&1
check "and still gets a correct body" "0" "$?" "SPEC 8"

echo
echo "pipelining"
got=$(python3 tests/rawframe.py "$PORT" pipelined 2>/dev/null)
check "3 in flight, IDs preserved" "1,2,3" "$got" "SPEC 1"

echo
printf '  %d passed, %d failed\n\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
