#!/bin/bash
# Integration test: a Julia REPL that exits and is restarted inside the SAME tmux pane must still be
# usable by the SAME jjmcp process. The runtime bootstrap is cached per pane, so a cache that does
# not notice the new REPL process pastes @JJMCP_COMMAND into a REPL that never got the macro.
#
# Run with `make test-integration` from the project root, or directly:
#   JJMCP_INTEGRATION=1 ./tests/integration/test_repl_restart.sh
#
# Requires: build/jjmcp present, tmux on PATH, julia on PATH, jq on PATH.

set -eu

PROJECT_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
JJMCP="$PROJECT_ROOT/build/jjmcp"
SESSION="jjmcp-restart-itest-$$"

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

# capture-pane pads the pane to its full height, so test the last non-empty line: after a restart
# the scrollback still holds the previous session's prompts.
wait_for_prompt() {
    for _ in $(seq 1 60); do
        if tmux capture-pane -t "$SESSION" -p 2>/dev/null \
            | grep -v '^[[:space:]]*$' | tail -1 | grep -q "julia>"; then
            return 0
        fi
        sleep 1
    done
    return 1
}

# The REPL must be a child of the pane shell, not the pane process itself, so that `exit()` returns
# to the shell instead of killing the pane.
tmux new-session -d -s "$SESSION"
tmux send-keys -t "$SESSION" 'julia --startup-file=no' Enter
if ! wait_for_prompt; then
    echo "FAIL: julia did not start within 60s" >&2
    exit 1
fi
PANE_ID=$(tmux list-panes -t "$SESSION" -F '#{pane_id}' | head -1)
echo "test pane: $PANE_ID (session $SESSION)"

TEST_CWD=$(mktemp -d)
trap 'tmux kill-session -t "$SESSION" 2>/dev/null || true; rm -rf "$TEST_CWD"' EXIT

failures=0
fail() { echo "  FAIL: $*" >&2; failures=$((failures + 1)); }
pass() { echo "  ok: $*"; }

echo "== eval, restart the REPL in place, eval again on one jjmcp process =="
# The requests are fed slowly on purpose: both evals must reach the same server process, with the
# REPL restart happening between them.
out=$({
    printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}'
    printf '%s\n' "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{\"name\":\"jjmcp_bind\",\"arguments\":{\"target\":\"$PANE_ID\"}}}"
    printf '%s\n' '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"jjmcp_eval","arguments":{"code":"1+1","validate_syntax":false,"timeout_ms":30000}}}'
    sleep 12
    tmux send-keys -t "$PANE_ID" 'exit()' Enter
    sleep 4
    tmux send-keys -t "$PANE_ID" 'julia --startup-file=no' Enter
    wait_for_prompt || true
    sleep 2
    printf '%s\n' '{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"jjmcp_eval","arguments":{"code":"6*7","validate_syntax":false,"timeout_ms":30000}}}'
    sleep 40
} | (cd "$TEST_CWD" && "$JJMCP" serve 2>/dev/null))

before=$(echo "$out" | jq -c 'select(.id==3) | .result.structuredContent')
after=$(echo "$out" | jq -c 'select(.id==4) | .result.structuredContent')

[ "$(echo "$before" | jq -r .value_repr)" = "2" ] \
    && pass "eval before the restart returns 2" \
    || fail "eval before the restart wrong: $(echo "$before" | jq -r .value_repr)"
[ "$(echo "$after" | jq -r .found_end)" = "true" ] \
    && pass "eval after the restart completes" \
    || fail "eval after the restart did not complete: $after"
[ "$(echo "$after" | jq -r .julia_error)" = "false" ] \
    && pass "eval after the restart has no julia error" \
    || fail "eval after the restart reported a julia error: $after"
[ "$(echo "$after" | jq -r .value_repr)" = "42" ] \
    && pass "eval after the restart returns 42" \
    || fail "eval after the restart wrong: $(echo "$after" | jq -r .value_repr)"

if [ "$failures" -ne 0 ]; then
    echo "$failures assertion(s) failed" >&2
    exit 1
fi
echo "all repl restart assertions passed"
