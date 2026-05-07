#!/bin/bash
# Integration test: spawn a private tmux server with Julia inside, drive jjmcp_bind + jjmcp_eval
# end-to-end, verify the structured content shape matches expectations. Tears down the test tmux
# server on exit so it never leaks into the user's tmux scope.
#
# Run with `make test-integration` from the project root, or directly:
#   JJMCP_INTEGRATION=1 ./tests/integration/test_tmux_fixture.sh
#
# Requires: build/jjmcp present, tmux on PATH, julia on PATH, jq on PATH.

set -eu

PROJECT_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
JJMCP="$PROJECT_ROOT/build/jjmcp"
SESSION="jjmcp-itest-$$"

cleanup() {
    tmux kill-session -t "$SESSION" 2>/dev/null || true
}
trap cleanup EXIT

if [ ! -x "$JJMCP" ]; then
    echo "FAIL: $JJMCP not built. Run 'make' first." >&2
    exit 1
fi
for tool in tmux julia jq; do
    if ! command -v "$tool" >/dev/null; then
        echo "SKIP: $tool not on PATH" >&2
        exit 0
    fi
done

# Spawn an isolated session on the user's tmux server. jjmcp does not currently retarget the tmux
# control socket per-invocation, so a private -L socket would not work; we use a uniquely-named
# session (jjmcp-itest-$$) which is just as test-isolated and is cleaned up on exit.
tmux new-session -d -s "$SESSION" "julia --startup-file=no"

# Wait for the Julia prompt to appear in the pane.
for i in $(seq 1 30); do
    if tmux capture-pane -t "$SESSION" -p 2>/dev/null | grep -q "julia>"; then
        break
    fi
    sleep 1
done
if ! tmux capture-pane -t "$SESSION" -p 2>/dev/null | grep -q "julia>"; then
    echo "FAIL: julia did not start within 30s" >&2
    exit 1
fi
PANE_ID=$(tmux list-panes -t "$SESSION" -F '#{pane_id}' | head -1)
echo "test pane: $PANE_ID (session $SESSION)"

# Run jjmcp from a fresh temp cwd so a pre-existing .jjmcp/config.json from the project does not
# pre-bind us to a stale pane.
TEST_CWD=$(mktemp -d)
trap 'tmux kill-session -t "$SESSION" 2>/dev/null || true; rm -rf "$TEST_CWD"' EXIT
run_jjmcp() {
    (cd "$TEST_CWD" && "$JJMCP" serve 2>/dev/null)
}

failures=0
fail() { echo "  FAIL: $*" >&2; failures=$((failures + 1)); }
pass() { echo "  ok: $*"; }

echo "== bind + simple eval (success path) =="
out=$(printf '%s\n' \
    '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}' \
    "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{\"name\":\"jjmcp_bind\",\"arguments\":{\"target\":\"$PANE_ID\"}}}" \
    '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"jjmcp_eval","arguments":{"code":"println(\"itest_marker\"); 7*6","validate_syntax":false}}}' \
    | run_jjmcp)

eval_resp=$(echo "$out" | jq -c 'select(.id==3)')
sc=$(echo "$eval_resp" | jq -c .result.structuredContent)

[ "$(echo "$sc" | jq -r .julia_error)" = "false" ] && pass "julia_error=false" || fail "julia_error wrong"
[ "$(echo "$sc" | jq -r .found_begin)" = "true" ] && pass "found_begin=true" || fail "found_begin wrong"
[ "$(echo "$sc" | jq -r .found_end)" = "true" ] && pass "found_end=true" || fail "found_end wrong"
[ "$(echo "$sc" | jq -r .stdout)" = "itest_marker" ] && pass "stdout=itest_marker" || fail "stdout wrong: $(echo "$sc" | jq -r .stdout)"
[ "$(echo "$sc" | jq -r .value_repr)" = "42" ] && pass "value_repr=42" || fail "value_repr wrong: $(echo "$sc" | jq -r .value_repr)"

echo "== eval (error path) =="
out=$(printf '%s\n' \
    '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}' \
    "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{\"name\":\"jjmcp_bind\",\"arguments\":{\"target\":\"$PANE_ID\"}}}" \
    '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"jjmcp_eval","arguments":{"code":"error(\"itest_boom\")","validate_syntax":false}}}' \
    | run_jjmcp)

eval_resp=$(echo "$out" | jq -c 'select(.id==3)')
sc=$(echo "$eval_resp" | jq -c .result.structuredContent)

[ "$(echo "$eval_resp" | jq -r .result.isError)" = "true" ] && pass "isError=true" || fail "isError wrong"
[ "$(echo "$sc" | jq -r .julia_error)" = "true" ] && pass "julia_error=true" || fail "julia_error wrong"
[ "$(echo "$sc" | jq -r .error_message)" = "itest_boom" ] && pass "error_message=itest_boom" || fail "error_message wrong: $(echo "$sc" | jq -r .error_message)"
echo "$sc" | jq -r .backtrace | grep -q "Stacktrace:" && pass "backtrace contains Stacktrace:" || fail "backtrace missing Stacktrace:"

echo "== syntax pre-validation rejects bad code =="
out=$(printf '%s\n' \
    '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}' \
    "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{\"name\":\"jjmcp_bind\",\"arguments\":{\"target\":\"$PANE_ID\"}}}" \
    '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"jjmcp_eval","arguments":{"code":"function broken(\n  end"}}}' \
    | run_jjmcp)

eval_resp=$(echo "$out" | jq -c 'select(.id==3)')
[ "$(echo "$eval_resp" | jq -r .result.isError)" = "true" ] && pass "syntax error caught before paste" || fail "syntax pre-validation did not catch broken function"
text=$(echo "$eval_resp" | jq -r .result.content[0].text)
echo "$text" | grep -q "syntax error before eval" && pass "error text mentions syntax pre-validation" || fail "error text format unexpected: $text"

if [ "$failures" -ne 0 ]; then
    echo "FAIL: $failures tmux fixture assertions failed" >&2
    exit 1
fi
echo "all tmux fixture assertions passed"
