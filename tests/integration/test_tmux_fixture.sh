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

PREV_HISTORY_LIMIT=$(tmux show-options -g history-limit 2>/dev/null | awk '{print $2}')
[ -n "$PREV_HISTORY_LIMIT" ] || PREV_HISTORY_LIMIT=2000

cleanup() {
    tmux kill-session -t "$SESSION" 2>/dev/null || true
    tmux set-option -g history-limit "$PREV_HISTORY_LIMIT" 2>/dev/null || true
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
# tmux snapshots history-limit into a pane's grid at creation time; setting it on the session
# afterward does not affect an already-created pane. Bump the global default before creating the
# pane, restored on exit above, so the large single-line test below (which wraps to thousands of
# 80-column rows) is not silently truncated by history eviction before capture-pane sees it.
# `set-option -g` requires a running server, and a fresh `start-server` can exit-empty again
# before the next command connects, so bootstrap with a throwaway session first.
BOOT_SESSION="jjmcp-itest-boot-$$"
tmux new-session -d -s "$BOOT_SESSION" 2>/dev/null || true
tmux set-option -g history-limit 50000
tmux new-session -d -s "$SESSION" "julia --startup-file=no"
tmux kill-session -t "$BOOT_SESSION" 2>/dev/null || true

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
trap 'tmux kill-session -t "$SESSION" 2>/dev/null || true; tmux set-option -g history-limit "$PREV_HISTORY_LIMIT" 2>/dev/null || true; rm -rf "$TEST_CWD"' EXIT
run_jjmcp() {
    (cd "$TEST_CWD" && "$JJMCP" serve 2>/dev/null)
}

failures=0
fail() { echo "  FAIL: $*" >&2; failures=$((failures + 1)); }
pass() { echo "  ok: $*"; }

echo "== bind + simple eval (success path) =="
out=$(printf '%s\n' \
    '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}' \
    '{"jsonrpc":"2.0","id":2,"method":"tools/list"}' \
    "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{\"name\":\"jjmcp_bind\",\"arguments\":{\"target\":\"$PANE_ID\"}}}" \
    '{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"jjmcp_capture_test_results","arguments":{"include_raw":false}}}' \
    '{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"jjmcp_capture_test_results","arguments":{"require_summary":true,"include_raw":false}}}' \
    '{"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"jjmcp_eval","arguments":{"code":"println(\"itest_marker\"); 7*6","validate_syntax":false}}}' \
    | run_jjmcp)

tools_list=$(echo "$out" | jq -c 'select(.id==2)')
bind_resp=$(echo "$out" | jq -c 'select(.id==3)')
fresh_capture=$(echo "$out" | jq -c 'select(.id==4)')
fresh_capture_with_required_summary=$(echo "$out" | jq -c 'select(.id==5)')
eval_resp=$(echo "$out" | jq -c 'select(.id==6)')
fresh_sc=$(echo "$fresh_capture" | jq -c .result.structuredContent)
sc=$(echo "$eval_resp" | jq -c .result.structuredContent)

echo "$tools_list" | jq -e '.result.tools[] | select(.name=="jjmcp_capture_test_results")' >/dev/null \
    && pass "tools/list includes jjmcp_capture_test_results" \
    || fail "tools/list missing jjmcp_capture_test_results"
[ "$(echo "$fresh_sc" | jq -r .found_summary)" = "false" ] \
    && pass "fresh capture has found_summary=false" \
    || fail "fresh capture found_summary expected false"
[ "$(echo "$fresh_capture_with_required_summary" | jq -r .result.isError)" = "true" ] \
    && pass "fresh capture with require_summary=true returns error" \
    || fail "fresh capture with require_summary=true should fail"
[ "$(echo "$bind_resp" | jq -r '.result.isError == true')" = "false" ] \
    && pass "bind call succeeds" \
    || fail "bind call failed"

[ "$(echo "$sc" | jq -r .julia_error)" = "false" ] && pass "julia_error=false" || fail "julia_error wrong"
[ "$(echo "$sc" | jq -r .found_begin)" = "true" ] && pass "found_begin=true" || fail "found_begin wrong"
[ "$(echo "$sc" | jq -r .found_end)" = "true" ] && pass "found_end=true" || fail "found_end wrong"
[ "$(echo "$sc" | jq -r .stdout)" = "itest_marker" ] && pass "stdout=itest_marker" || fail "stdout wrong: $(echo "$sc" | jq -r .stdout)"
[ "$(echo "$sc" | jq -r .value_repr)" = "42" ] && pass "value_repr=42" || fail "value_repr wrong: $(echo "$sc" | jq -r .value_repr)"

echo "== synthetic test run + parsed summary =="
out=$(printf '%s\n' \
    '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}' \
    "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{\"name\":\"jjmcp_bind\",\"arguments\":{\"target\":\"$PANE_ID\"}}}" \
    '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"jjmcp_eval","arguments":{"code":"using Test\n@testset \"jjmcp_itest\" begin\n@test 1 == 1\n@test 2 == 2\nend","validate_syntax":false}}}' \
    '{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"jjmcp_capture_test_results","arguments":{"require_summary":true,"include_raw":false}}}' \
    | run_jjmcp)

summary_resp=$(echo "$out" | jq -c 'select(.id==4)')
summary_sc=$(echo "$summary_resp" | jq -c .result.structuredContent)
[ "$(echo "$summary_resp" | jq -r '.result.isError == true')" = "false" ] \
    && pass "capture_test_results succeeds after test execution" \
    || fail "capture_test_results failed for synthetic tests"
[ "$(echo "$summary_sc" | jq -r .found_summary)" = "true" ] && pass "synthetic run found_summary=true" || fail "synthetic run found_summary=false"
[ "$(echo "$summary_sc" | jq -r .test_pass)" = "2" ] && pass "synthetic run parsed test_pass=2" || fail "synthetic run wrong test_pass: $(echo "$summary_sc" | jq -r .test_pass)"
[ "$(echo "$summary_sc" | jq -r .test_fail)" = "0" ] && pass "synthetic run parsed test_fail=0" || fail "synthetic run wrong test_fail: $(echo "$summary_sc" | jq -r .test_fail)"
[ "$(echo "$summary_sc" | jq -r .test_total)" = "2" ] && pass "synthetic run parsed test_total=2" || fail "synthetic run wrong test_total: $(echo "$summary_sc" | jq -r .test_total)"

echo "== eval preserves global assignments in bound REPL =="
out=$(printf '%s\n' \
    '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}' \
    "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{\"name\":\"jjmcp_bind\",\"arguments\":{\"target\":\"$PANE_ID\"}}}" \
    '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"jjmcp_eval","arguments":{"code":"jjmcp_itest_global = x -> x + 10","validate_syntax":false,"transport":"tmux"}}}' \
    '{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"jjmcp_eval","arguments":{"code":"jjmcp_itest_global(32)","validate_syntax":false,"transport":"tmux"}}}' \
    | run_jjmcp)

global_set_resp=$(echo "$out" | jq -c 'select(.id==3)')
global_read_resp=$(echo "$out" | jq -c 'select(.id==4)')
global_read_sc=$(echo "$global_read_resp" | jq -c .result.structuredContent)
[ "$(echo "$global_set_resp" | jq -r '.result.isError == true')" = "false" ] \
    && pass "global assignment eval succeeds" \
    || fail "global assignment eval failed"
[ "$(echo "$global_read_sc" | jq -r .value_repr)" = "42" ] \
    && pass "global assignment persists across tmux evals" \
    || fail "global assignment did not persist: $(echo "$global_read_sc" | jq -r .value_repr)"

echo "== eval returns tail when tmux output is line-truncated =="
out=$(printf '%s\n' \
    '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}' \
    "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{\"name\":\"jjmcp_bind\",\"arguments\":{\"target\":\"$PANE_ID\"}}}" \
    '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"jjmcp_eval","arguments":{"code":"for i in 1:5\n    println(\"tmux_tail_line_$i\")\nend\nnothing","capture_lines":2,"validate_syntax":false,"transport":"tmux"}}}' \
    | run_jjmcp)

tail_resp=$(echo "$out" | jq -c 'select(.id==3)')
tail_sc=$(echo "$tail_resp" | jq -c .result.structuredContent)
tail_stdout=$(echo "$tail_sc" | jq -r .stdout)
if echo "$tail_stdout" | grep -q "tmux_tail_line_5" \
    && ! echo "$tail_stdout" | grep -q "tmux_tail_line_1" \
    && echo "$tail_stdout" | grep -q "earlier line"; then
    pass "tmux stdout truncation keeps newest lines"
else
    fail "tmux stdout truncation did not keep newest lines: $tail_stdout"
fi

echo "== eval truncates large single-line tmux output =="
out=$(printf '%s\n' \
    '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}' \
    "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{\"name\":\"jjmcp_bind\",\"arguments\":{\"target\":\"$PANE_ID\"}}}" \
    '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"jjmcp_eval","arguments":{"code":"print(repeat(\"x\", 300000)); nothing","validate_syntax":false,"transport":"tmux"}}}' \
    | run_jjmcp)

large_resp=$(echo "$out" | jq -c 'select(.id==3)')
large_sc=$(echo "$large_resp" | jq -c .result.structuredContent)
echo "$large_sc" | jq -r .stdout | grep -q "JJMCP truncated" \
    && pass "large tmux stdout is truncated" \
    || fail "large tmux stdout missing truncation marker"

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
if echo "$text" | grep -Eq "syntax error before eval|ParseError|Expected"; then
    pass "error text mentions syntax or parse failure"
else
    fail "error text format unexpected: $text"
fi

if [ "$failures" -ne 0 ]; then
    echo "FAIL: $failures tmux fixture assertions failed" >&2
    exit 1
fi
echo "all tmux fixture assertions passed"
