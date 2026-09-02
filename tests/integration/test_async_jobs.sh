#!/bin/bash
# Integration test: asynchronous, recoverable evaluation. Drives one long-lived jjmcp process over a
# FIFO so requests can be sequenced, then checks that a job outlives its waiting window, that the
# control plane answers while Julia is busy, and that the result is recoverable afterwards.
#
# Run with `make test-integration` from the project root, or directly:
#   JJMCP_INTEGRATION=1 ./tests/integration/test_async_jobs.sh
#
# Requires: build/jjmcp present, tmux on PATH, julia on PATH, jq on PATH.

set -eu

PROJECT_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
JJMCP="$PROJECT_ROOT/build/jjmcp"
SESSION="jjmcp-async-itest-$$"
TEST_CWD=""
SERVER_PID=""

cleanup() {
    [ -n "$SERVER_PID" ] && kill "$SERVER_PID" 2>/dev/null || true
    tmux kill-session -t "$SESSION" 2>/dev/null || true
    [ -n "$TEST_CWD" ] && rm -rf "$TEST_CWD"
    return 0
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

tmux new-session -d -s "$SESSION" "julia --startup-file=no"
for _ in $(seq 1 30); do
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

# One server process for the whole test: jobs live in that process, so every request has to reach
# the same instance. A FIFO keeps stdin open, which lets the test send one request at a time and
# read each response before deciding what to send next.
TEST_CWD=$(mktemp -d)
IN_FIFO="$TEST_CWD/in"
OUT_FILE="$TEST_CWD/out.jsonl"
mkfifo "$IN_FIFO"
touch "$OUT_FILE"
(cd "$TEST_CWD" && "$JJMCP" serve <"$IN_FIFO" >"$OUT_FILE" 2>"$TEST_CWD/err.log") &
SERVER_PID=$!
exec 3>"$IN_FIFO"

failures=0
fail() { echo "  FAIL: $*" >&2; failures=$((failures + 1)); }
pass() { echo "  ok: $*"; }

send() { printf '%s\n' "$1" >&3; }

# Wait for the response frame carrying this JSON-RPC id and print it.
await() {
    local id="$1" limit="${2:-40}" i
    for i in $(seq 1 $((limit * 10))); do
        if grep -q "\"id\":$id," "$OUT_FILE" 2>/dev/null; then
            grep "\"id\":$id," "$OUT_FILE" | head -1
            return 0
        fi
        sleep 0.1
    done
    echo "  FAIL: no response for id $id within ${limit}s" >&2
    return 1
}

sc_of() { echo "$1" | jq -c '.result.structuredContent'; }

send '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}'
await 1 >/dev/null
send "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{\"name\":\"jjmcp_bind\",\"arguments\":{\"target\":\"$PANE_ID\"}}}"
bind_resp=$(await 2)
[ "$(echo "$bind_resp" | jq -r '.result.isError == true')" = "false" ] \
    && pass "bind call succeeds" || fail "bind call failed"

echo "== eval_async returns a job id while Julia is still busy =="
send '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"jjmcp_eval_async","arguments":{"code":"sleep(12); println(\"async_marker\"); 6*7","validate_syntax":false}}}'
submit=$(await 3)
submit_sc=$(sc_of "$submit")
JOB_ID=$(echo "$submit_sc" | jq -r .job_id)
[ "$(echo "$submit" | jq -r '.result.isError == true')" = "false" ] \
    && pass "eval_async is not an error" || fail "eval_async returned an error"
[ "$(echo "$submit_sc" | jq -r .state)" = "running" ] \
    && pass "eval_async reports state=running" || fail "eval_async state: $(echo "$submit_sc" | jq -r .state)"
[ -n "$JOB_ID" ] && [ "$JOB_ID" != "null" ] \
    && pass "eval_async returns job_id $JOB_ID" || fail "eval_async returned no job_id"
[ "$(echo "$submit_sc" | jq -r .marker_id)" = "$JOB_ID" ] \
    && pass "job_id equals marker_id" || fail "job_id and marker_id differ"

echo "== control plane answers while the REPL is busy =="
send "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\",\"params\":{\"name\":\"jjmcp_job_status\",\"arguments\":{\"job_id\":\"$JOB_ID\"}}}"
status=$(await 4 10)
status_sc=$(sc_of "$status")
[ "$(echo "$status_sc" | jq -r .state)" = "running" ] \
    && pass "job_status reports running" || fail "job_status state: $(echo "$status_sc" | jq -r .state)"
[ "$(echo "$status_sc" | jq -r .proc_available)" = "true" ] \
    && pass "job_status reads the REPL process" || fail "job_status could not read /proc"
[ "$(echo "$status_sc" | jq -r '.rss_bytes > 0')" = "true" ] \
    && pass "job_status reports RSS" || fail "job_status RSS missing"
[ "$(echo "$status_sc" | jq -r '.cpu_seconds >= 0')" = "true" ] \
    && pass "job_status reports cpu seconds" || fail "job_status cpu seconds missing"
[ "$(echo "$status_sc" | jq -r '.elapsed_ms > 0')" = "true" ] \
    && pass "job_status reports elapsed time" || fail "job_status elapsed_ms missing"

send "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/call\",\"params\":{\"name\":\"jjmcp_eval_async\",\"arguments\":{\"code\":\"1+1\",\"validate_syntax\":false}}}"
busy=$(await 5 10)
[ "$(echo "$busy" | jq -r .result.isError)" = "true" ] \
    && pass "a second job in the same pane is refused" || fail "a second concurrent job was accepted"

echo "== a short wait leaves the job running =="
send "{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"tools/call\",\"params\":{\"name\":\"jjmcp_wait\",\"arguments\":{\"job_id\":\"$JOB_ID\",\"timeout_ms\":2000}}}"
short_wait=$(await 6 15)
short_sc=$(sc_of "$short_wait")
[ "$(echo "$short_wait" | jq -r '.result.isError == true')" = "false" ] \
    && pass "an expired wait is not an error" || fail "an expired wait returned an error"
[ "$(echo "$short_sc" | jq -r .state)" = "running" ] \
    && pass "an expired wait still reports running" || fail "short wait state: $(echo "$short_sc" | jq -r .state)"

echo "== a longer wait returns the structured result =="
send "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/call\",\"params\":{\"name\":\"jjmcp_wait\",\"arguments\":{\"job_id\":\"$JOB_ID\",\"timeout_ms\":40000}}}"
long_wait=$(await 7 60)
long_sc=$(sc_of "$long_wait")
[ "$(echo "$long_sc" | jq -r .state)" = "completed" ] \
    && pass "wait returns state=completed" || fail "long wait state: $(echo "$long_sc" | jq -r .state)"
[ "$(echo "$long_sc" | jq -r .value_repr)" = "42" ] \
    && pass "wait returns value_repr=42" || fail "wait value_repr: $(echo "$long_sc" | jq -r .value_repr)"
echo "$long_sc" | jq -r .stdout | grep -q "async_marker" \
    && pass "wait returns the job stdout" || fail "wait stdout missing async_marker"

echo "== the result is retrievable again after the wait =="
send "{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"tools/call\",\"params\":{\"name\":\"jjmcp_result\",\"arguments\":{\"job_id\":\"$JOB_ID\"}}}"
result=$(await 8 10)
result_sc=$(sc_of "$result")
[ "$(echo "$result_sc" | jq -r .value_repr)" = "42" ] \
    && pass "jjmcp_result replays the value" || fail "jjmcp_result value_repr: $(echo "$result_sc" | jq -r .value_repr)"

send "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"tools/call\",\"params\":{\"name\":\"jjmcp_capture_job\",\"arguments\":{\"job_id\":\"$JOB_ID\"}}}"
capture=$(await 9 10)
echo "$capture" | jq -r .result.content[0].text | grep -q "async_marker" \
    && pass "capture_job returns this job's output" || fail "capture_job missing async_marker"

send '{"jsonrpc":"2.0","id":10,"method":"tools/call","params":{"name":"jjmcp_list_jobs","arguments":{}}}'
listed=$(await 10 10)
[ "$(echo "$listed" | jq -r --arg id "$JOB_ID" '[.result.structuredContent.jobs[] | select(.job_id==$id)] | length')" = "1" ] \
    && pass "list_jobs includes the job" || fail "list_jobs is missing the job"

echo "== a stored result survives eviction from memory =="
if [ -f "$TEST_CWD/.jjmcp/jobs/$JOB_ID.json" ]; then
    pass "the completed job is written to the result store"
else
    fail "no stored result at $TEST_CWD/.jjmcp/jobs/$JOB_ID.json"
fi

echo "== jjmcp_eval hands a timed-out run to the background instead of failing =="
send '{"jsonrpc":"2.0","id":11,"method":"tools/call","params":{"name":"jjmcp_eval","arguments":{"code":"sleep(10); 5*5","timeout_ms":2000,"validate_syntax":false,"transport":"tmux"}}}'
detached=$(await 11 20)
detached_sc=$(sc_of "$detached")
DETACHED_ID=$(echo "$detached_sc" | jq -r .job_id)
[ "$(echo "$detached" | jq -r '.result.isError == true')" = "false" ] \
    && pass "an expired eval window is not an MCP error" || fail "expired eval window returned an error"
[ "$(echo "$detached_sc" | jq -r .state)" = "running" ] \
    && pass "the expired eval reports state=running" || fail "expired eval state: $(echo "$detached_sc" | jq -r .state)"
[ "$(echo "$detached_sc" | jq -r .detached)" = "true" ] \
    && pass "the expired eval is polled in the background" || fail "expired eval was not detached"
# The pasted macro stays readable for the human: it shows the timeout the caller asked for, not the
# longer lifetime the detached job is given internally.
tmux capture-pane -t "$PANE_ID" -p -S -400 | grep -q "@JJMCP_COMMAND \"$DETACHED_ID\" 2000" \
    && pass "the pane shows the caller timeout in the pasted macro" \
    || fail "the pasted macro does not show the caller timeout"

send "{\"jsonrpc\":\"2.0\",\"id\":12,\"method\":\"tools/call\",\"params\":{\"name\":\"jjmcp_wait\",\"arguments\":{\"job_id\":\"$DETACHED_ID\",\"timeout_ms\":40000}}}"
recovered=$(await 12 60)
recovered_sc=$(sc_of "$recovered")
[ "$(echo "$recovered_sc" | jq -r .state)" = "completed" ] \
    && pass "the detached run completes" || fail "detached run state: $(echo "$recovered_sc" | jq -r .state)"
[ "$(echo "$recovered_sc" | jq -r .value_repr)" = "25" ] \
    && pass "the detached run keeps its value" || fail "detached value_repr: $(echo "$recovered_sc" | jq -r .value_repr)"

echo "== wait on an unknown marker falls back to the pane =="
send '{"jsonrpc":"2.0","id":13,"method":"tools/call","params":{"name":"jjmcp_wait","arguments":{"job_id":"1788247633165392_273954_478","timeout_ms":2000}}}'
unknown=$(await 13 20)
unknown_sc=$(sc_of "$unknown")
[ "$(echo "$unknown_sc" | jq -r .recovered_from_scrollback)" = "true" ] \
    && pass "an untracked marker is looked up in the pane" || fail "no scrollback fallback for an untracked marker"
[ "$(echo "$unknown_sc" | jq -r .state)" = "timed_out" ] \
    && pass "an absent marker times out rather than failing the call" \
    || fail "untracked marker state: $(echo "$unknown_sc" | jq -r .state)"

send "{\"jsonrpc\":\"2.0\",\"id\":14,\"method\":\"tools/call\",\"params\":{\"name\":\"jjmcp_eval\",\"arguments\":{\"code\":\"7*7\",\"validate_syntax\":false,\"transport\":\"tmux\"}}}"
after=$(await 14 30)
[ "$(echo "$(sc_of "$after")" | jq -r .value_repr)" = "49" ] \
    && pass "the pane is usable again once its jobs end" || fail "the pane did not accept a new eval"

exec 3>&-
wait "$SERVER_PID" 2>/dev/null || true
SERVER_PID=""

if [ "$failures" -ne 0 ]; then
    echo "FAIL: $failures async job assertions failed" >&2
    exit 1
fi
echo "all async job assertions passed"
