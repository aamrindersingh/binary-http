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


# A fixed sleep is a guess about how fast a machine is, and this suite has
# already been bitten once by assuming how a peer behaves. These wait for
# the thing itself. The sanitized build is slow enough to start and stop
# that the difference is not theoretical: with `disown` plus a bare
# `wait`, the server restart below raced its own predecessor and four
# checks failed under `make asan` while passing on the plain build.
port_open() { # port_open <port>
    (exec 3<>/dev/tcp/127.0.0.1/"$1") 2>/dev/null && exec 3<&- 2>/dev/null
}

wait_for_port() { # wait_for_port <port>
    local i=0
    while [ "$i" -lt 150 ]; do
        port_open "$1" && return 0
        sleep 0.1
        i=$((i + 1))
    done
    return 1
}

stop_server() {
    [ -n "${SRV:-}" ] || return 0
    kill "$SRV" 2>/dev/null
    { wait "$SRV"; } 2>/dev/null
    SRV=""
}

start_server() { # start_server <logfile> [extra bserve args...]
    local log=$1
    shift
    ./bserve ./www "$PORT" -v "$@" > "$log" 2>&1 &
    SRV=$!
    if ! wait_for_port "$PORT"; then
        echo "server failed to start on port $PORT:" >&2
        cat "$log" >&2
        exit 1
    fi
}

cleanup() {
    [ -n "${SRV:-}" ] && kill "$SRV" 2>/dev/null
    [ -n "${PYS:-}" ] && kill "$PYS" 2>/dev/null
    rm -f tests/tmp-py.out tests/tmp-c.out
    { wait; } 2>/dev/null
}
trap cleanup EXIT

if [ ! -x ./bserve ] || [ ! -x ./bcurl ]; then
    echo "build first: make" >&2
    exit 2
fi

start_server tests/server.log

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

WRAP=$(./bcurl -v --start-id 16777214 -n 4 "localhost:$PORT/hello.txt" 2>&1 >/dev/null \
       | grep -o 'REQUEST id=[0-9]*' | grep -o '[0-9]*' | tr '\n' ',' | sed 's/,$//')
check "IDs wrap to 1, not to 0" "16777214,16777215,1,2" "$WRAP" "SPEC 1"

./bcurl --start-id 16777215 -n 2 "localhost:$PORT/hello.txt" >/dev/null 2>&1
check "and the server accepts a wrapped ID" "0" "$?" "SPEC 1"

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

stop_server
start_server tests/server-inject.log --inject-unknown
SKIPPED=$(./bcurl -v -n 3 "localhost:$PORT/hello.txt" 2>&1 >/dev/null | grep -c "per SPEC 8")
check "client skips unknown type" "3" "$SKIPPED" "SPEC 8"

./bcurl "localhost:$PORT/index.html" >/dev/null 2>&1
check "and still gets a correct body" "0" "$?" "SPEC 8"

echo
echo "pipelining"
got=$(python3 tests/rawframe.py "$PORT" pipelined 2>/dev/null)
check "3 in flight, IDs preserved" "1,2,3" "$got" "SPEC 1"

echo
echo "bcurl against a server it has never met"
PORT2=$((PORT + 1))
python3 tests/pyserve.py "$PORT2" ./www > tests/pyserve.log 2>&1 &
PYS=$!

if wait_for_port "$PORT2"; then
    ./bcurl "localhost:$PORT2/index.html" > tests/tmp-py.out 2>/dev/null
    check "2xx from pyserve exits zero" "0" "$?" "SPEC 6"

    cmp -s tests/tmp-py.out ./www/index.html
    check "body survives a 2-frame split" "0" "$?" "SPEC 6"

    ./bcurl "localhost:$PORT/index.html" > tests/tmp-c.out 2>/dev/null
    cmp -s tests/tmp-py.out tests/tmp-c.out
    check "both servers give identical bytes" "0" "$?" "SPEC 6"

    NDATA=$(./bcurl -v "localhost:$PORT2/index.html" 2>&1 >/dev/null | grep -c '^< DATA')
    check "pyserve really splits the body" "2" "$NDATA" "SPEC 6"

    NCL=$(./bcurl -v "localhost:$PORT2/index.html" 2>&1 >/dev/null | grep -c 'content-length')
    check "and sends no content-length" "0" "$NCL" "SPEC 6"

    LIT=$(./bcurl -v "localhost:$PORT2/index.html" 2>&1 >/dev/null | grep -c 'x-served-by: python')
    check "literal header name decoded" "1" "$LIT" "SPEC 4"

    ./bcurl "localhost:$PORT2/missing.html" >/dev/null 2>&1
    check "4xx from pyserve exits non-zero" "1" "$?" "SPEC 6"

    # The slide says 4xx AND 5xx. A file server will not produce a 5xx
    # on demand, so pyserve has a path that does.
    ./bcurl "localhost:$PORT2/500" >/dev/null 2>&1
    check "5xx exits non-zero too" "1" "$?" "SPEC 6"

    S5=$(./bcurl -v "localhost:$PORT2/500" 2>&1 >/dev/null | grep -o 'status: 500')
    check "and it really was a 500" "status: 500" "$S5" "SPEC 6"

    ./bcurl --send-unknown-frame "localhost:$PORT2/index.html" >/dev/null 2>&1
    check "pyserve skips an unknown type" "0" "$?" "SPEC 8"

    kill "$PYS" 2>/dev/null
    { wait "$PYS"; } 2>/dev/null
    PYS=""
    rm -f tests/tmp-py.out tests/tmp-c.out
else
    echo "  pyserve never started listening, see tests/pyserve.log" >&2
    cat tests/pyserve.log >&2
    FAIL=$((FAIL + 1))
fi

echo
echo "one wire format, five documents"
python3 tests/statictable.py >/dev/null 2>&1
check "spec and all 4 impls share a table" "0" "$?" "SPEC 4"

echo
echo "independence of the two codecs"
python3 tests/independence.py >/dev/null 2>&1
check "server and client share no code" "0" "$?" "README"

echo
printf '  %d passed, %d failed\n\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
