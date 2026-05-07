#!/bin/bash
# Integration test: spawn a private tmux session, install JJMCPHelper.jl in a temp Pkg env, start
# the helper, and verify that jjmcp_eval transport=socket round-trips structured content faster
# than the marker path. Tears down the helper, the tmux session, and the temp Pkg env on exit.
#
# Run with `make test-integration` from the project root, or directly:
#   JJMCP_INTEGRATION=1 ./tests/integration/test_socket_fixture.sh
#
# Requires: build/jjmcp present, tmux on PATH, julia on PATH, jq on PATH, network access for the
# initial Revise + JSON3 download (cached after first run).

set -eu

PROJECT_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
JJMCP="$PROJECT_ROOT/build/jjmcp"
HELPER_PATH="$PROJECT_ROOT/julia/JJMCPHelper"
SESSION="jjmcp-sock-itest-$$"
TEST_CWD=$(mktemp -d)

cleanup() {
    tmux kill-session -t "$SESSION" 2>/dev/null || true
    rm -rf "$TEST_CWD"
    rm -f /run/user/$(id -u)/jjmcp-*.sock 2>/dev/null || true
}
trap cleanup EXIT

if [ ! -x "$JJMCP" ]; then echo "FAIL: $JJMCP not built. Run 'make' first." >&2; exit 1; fi
for tool in tmux julia jq; do
    if ! command -v "$tool" >/dev/null; then echo "SKIP: $tool not on PATH" >&2; exit 0; fi
done

failures=0
fail() { echo "  FAIL: $*" >&2; failures=$((failures + 1)); }
pass() { echo "  ok: $*"; }

run_jjmcp() {
    (cd "$TEST_CWD" && "$JJMCP" serve 2>/dev/null)
}

echo "== spawn tmux session with julia and start JJMCPHelper =="
tmux new-session -d -s "$SESSION" "julia --startup-file=no"
for i in $(seq 1 30); do
    tmux capture-pane -t "$SESSION" -p 2>/dev/null | grep -q "julia>" && break
    sleep 1
done
tmux capture-pane -t "$SESSION" -p 2>/dev/null | grep -q "julia>" || { fail "julia did not start"; exit 1; }
PANE_ID=$(tmux list-panes -t "$SESSION" -F '#{pane_id}' | head -1)
echo "  pane: $PANE_ID"

# Install Revise + helper via temp Pkg env. We add Revise BEFORE developing the helper so any
# subsequent code edits survive the running session, which matches the documented user workflow.
tmux send-keys -t "$SESSION" "using Pkg; Pkg.activate(; temp=true); Pkg.add(\"Revise\"); using Revise; Pkg.develop(path=\"$HELPER_PATH\"); using JJMCPHelper; JJMCPHelper.start()" Enter

EXPECTED_SOCK="/run/user/$(id -u)/jjmcp-$(echo "$PANE_ID" | tr -d %).sock"
for i in $(seq 1 120); do
    [ -S "$EXPECTED_SOCK" ] && break
    sleep 1
done
[ -S "$EXPECTED_SOCK" ] && pass "helper socket present at $EXPECTED_SOCK" \
    || { fail "helper did not create socket within 120s"; tmux capture-pane -t "$SESSION" -p -S - | tail -30 >&2; exit 1; }

echo "== eval via transport=socket =="
out=$(printf '%s\n' \
    '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}' \
    "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{\"name\":\"jjmcp_bind\",\"arguments\":{\"target\":\"$PANE_ID\"}}}" \
    '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"jjmcp_eval","arguments":{"code":"println(\"sock_test\"); 12*12","validate_syntax":false,"transport":"socket"}}}' \
    | run_jjmcp)

eval_resp=$(echo "$out" | jq -c 'select(.id==3)')
sc=$(echo "$eval_resp" | jq -c .result.structuredContent)

[ "$(echo "$sc" | jq -r .transport)" = "socket" ] && pass "transport=socket" || fail "transport not socket: $(echo "$sc" | jq -r .transport)"
[ "$(echo "$sc" | jq -r .julia_error)" = "false" ] && pass "julia_error=false" || fail "julia_error wrong"
expected_stdout=$(printf 'sock_test\n')
[ "$(echo "$sc" | jq -r .stdout)" = "$expected_stdout" ] && pass "stdout=sock_test" || fail "stdout wrong: $(echo "$sc" | jq -r .stdout | xxd | head -2)"
[ "$(echo "$sc" | jq -r .value_repr)" = "144" ] && pass "value_repr=144" || fail "value_repr wrong: $(echo "$sc" | jq -r .value_repr)"

elapsed=$(echo "$sc" | jq -r .elapsed_ms)
[ "$elapsed" -lt 500 ] && pass "elapsed_ms=$elapsed (under 500 ms)" || fail "elapsed_ms=$elapsed exceeds 500 ms"

echo "== eval via transport=socket (error path) =="
out=$(printf '%s\n' \
    '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}' \
    "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{\"name\":\"jjmcp_bind\",\"arguments\":{\"target\":\"$PANE_ID\"}}}" \
    '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"jjmcp_eval","arguments":{"code":"error(\"sock_intentional\")","validate_syntax":false,"transport":"socket"}}}' \
    | run_jjmcp)

eval_resp=$(echo "$out" | jq -c 'select(.id==3)')
sc=$(echo "$eval_resp" | jq -c .result.structuredContent)

[ "$(echo "$eval_resp" | jq -r .result.isError)" = "true" ] && pass "isError=true" || fail "isError wrong"
[ "$(echo "$sc" | jq -r .julia_error)" = "true" ] && pass "julia_error=true" || fail "julia_error wrong"
[ "$(echo "$sc" | jq -r .error_message)" = "sock_intentional" ] && pass "error_message=sock_intentional" || fail "error_message wrong"
echo "$sc" | jq -r .backtrace | grep -q "Stacktrace:" && pass "backtrace contains Stacktrace:" || fail "backtrace missing Stacktrace:"

echo "== fallback: stop helper and verify transport=auto falls back to tmux =="
tmux send-keys -t "$SESSION" "JJMCPHelper.stop()" Enter
for i in $(seq 1 10); do
    [ -S "$EXPECTED_SOCK" ] || break
    sleep 1
done
[ ! -S "$EXPECTED_SOCK" ] && pass "socket removed after stop()" || fail "socket still present"

out=$(printf '%s\n' \
    '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}' \
    "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{\"name\":\"jjmcp_bind\",\"arguments\":{\"target\":\"$PANE_ID\"}}}" \
    '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"jjmcp_eval","arguments":{"code":"77","validate_syntax":false,"transport":"auto"}}}' \
    | run_jjmcp)
eval_resp=$(echo "$out" | jq -c 'select(.id==3)')
sc=$(echo "$eval_resp" | jq -c .result.structuredContent)
[ "$(echo "$sc" | jq -r .transport)" = "tmux" ] && pass "auto fell back to tmux when socket gone" || fail "fallback transport not tmux: $(echo "$sc" | jq -r .transport)"
[ "$(echo "$sc" | jq -r .value_repr)" = "77" ] && pass "tmux fallback value_repr=77" || fail "fallback value wrong"

echo "== transport=socket with no socket returns a structured error =="
out=$(printf '%s\n' \
    '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}' \
    "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{\"name\":\"jjmcp_bind\",\"arguments\":{\"target\":\"$PANE_ID\"}}}" \
    '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"jjmcp_eval","arguments":{"code":"99","validate_syntax":false,"transport":"socket"}}}' \
    | run_jjmcp)
eval_resp=$(echo "$out" | jq -c 'select(.id==3)')
[ "$(echo "$eval_resp" | jq -r .result.isError)" = "true" ] && pass "isError=true when socket required but missing" || fail "should be error"
echo "$eval_resp" | jq -r '.result.content[0].text' | grep -q "no JJMCPHelper socket" && pass "error mentions JJMCPHelper" || fail "error text unexpected"

if [ "$failures" -ne 0 ]; then
    echo "FAIL: $failures socket fixture assertions failed" >&2
    exit 1
fi
echo "all socket fixture assertions passed"
