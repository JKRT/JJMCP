#!/bin/bash
# Integration test: feed canned JSON-RPC frames into build/jjmcp serve and assert response shape.
# Tests the framing (newline-delimited, no Content-Length) and the headline methods that the MCP
# spec requires every server to implement: initialize, ping, tools/list, resources/list.
#
# Run with `make test-integration` from the project root, or directly:
#   JJMCP_INTEGRATION=1 ./tests/integration/test_mcp_conformance.sh
#
# Requires: build/jjmcp present, jq on PATH.

set -eu

PROJECT_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
JJMCP="$PROJECT_ROOT/build/jjmcp"

if [ ! -x "$JJMCP" ]; then
    echo "FAIL: $JJMCP not built. Run 'make' first." >&2
    exit 1
fi
if ! command -v jq >/dev/null; then
    echo "SKIP: jq not on PATH" >&2
    exit 0
fi

failures=0
fail() { echo "  FAIL: $*" >&2; failures=$((failures + 1)); }
pass() { echo "  ok: $*"; }

# Run jjmcp from a fresh temp cwd so a pre-existing $PROJECT_ROOT/.jjmcp/config.json (left over
# from another session) cannot bleed in and pre-bind us to a real pane.
TEST_CWD=$(mktemp -d)
trap 'rm -rf "$TEST_CWD"' EXIT
run_jjmcp() {
    (cd "$TEST_CWD" && "$JJMCP" serve 2>/dev/null)
}

echo "== mcp conformance: initialize + ping + tools/list + resources/list =="
out=$(printf '%s\n' \
    '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}' \
    '{"jsonrpc":"2.0","id":2,"method":"ping"}' \
    '{"jsonrpc":"2.0","id":3,"method":"tools/list"}' \
    '{"jsonrpc":"2.0","id":4,"method":"resources/list"}' \
    | run_jjmcp)

# Each line should be valid JSON.
echo "$out" | while IFS= read -r line; do
    [ -z "$line" ] && continue
    echo "$line" | jq . >/dev/null || { echo "BAD JSON: $line"; exit 1; }
done

initialize=$(echo "$out" | jq -c 'select(.id==1)')
ping=$(echo "$out" | jq -c 'select(.id==2)')
tools_list=$(echo "$out" | jq -c 'select(.id==3)')
resources_list=$(echo "$out" | jq -c 'select(.id==4)')

[ "$(echo "$initialize" | jq -r .result.protocolVersion)" = "2024-11-05" ] \
    && pass "initialize.result.protocolVersion=2024-11-05" \
    || fail "initialize protocolVersion missing or wrong"

[ "$(echo "$initialize" | jq -r .result.serverInfo.name)" = "JohnJuliaMCP" ] \
    && pass "initialize.result.serverInfo.name=JohnJuliaMCP" \
    || fail "serverInfo.name missing or wrong"

[ "$(echo "$initialize" | jq -r '.result.capabilities | has("tools")')" = "true" ] \
    && pass "initialize advertises tools capability" \
    || fail "tools capability missing"

[ "$(echo "$initialize" | jq -r '.result.capabilities | has("resources")')" = "true" ] \
    && pass "initialize advertises resources capability" \
    || fail "resources capability missing"

[ "$(echo "$ping" | jq -r .result)" = "{}" ] \
    && pass "ping returns empty result" \
    || fail "ping result wrong"

tool_count=$(echo "$tools_list" | jq -r '.result.tools | length')
[ "$tool_count" -ge 10 ] \
    && pass "tools/list returns $tool_count tools (>=10)" \
    || fail "tools/list returned $tool_count, expected >=10"

# Verify jjmcp_eval has the expected schema fields.
eval_tool=$(echo "$tools_list" | jq -c '.result.tools[] | select(.name=="jjmcp_eval")')
for field in code timeout_ms capture_lines force validate_syntax transport; do
    has=$(echo "$eval_tool" | jq -r ".inputSchema.properties | has(\"$field\")")
    [ "$has" = "true" ] \
        && pass "jjmcp_eval inputSchema has $field" \
        || fail "jjmcp_eval inputSchema missing $field"
done

# Verify the transport enum.
transport_enum=$(echo "$eval_tool" | jq -c '.inputSchema.properties.transport.enum')
[ "$transport_enum" = '["auto","tmux","socket"]' ] \
    && pass "jjmcp_eval.transport enum is auto/tmux/socket" \
    || fail "jjmcp_eval.transport enum unexpected: $transport_enum"

# Verify outputSchema includes transport.
output_has_transport=$(echo "$eval_tool" | jq -r '.outputSchema.properties | has("transport")')
[ "$output_has_transport" = "true" ] \
    && pass "jjmcp_eval outputSchema includes transport" \
    || fail "jjmcp_eval outputSchema missing transport"

# Verify outputSchema is present.
has_output=$(echo "$eval_tool" | jq -r 'has("outputSchema")')
[ "$has_output" = "true" ] \
    && pass "jjmcp_eval has outputSchema" \
    || fail "jjmcp_eval missing outputSchema"

# Verify pkg_status tool is present.
echo "$tools_list" | jq -e '.result.tools[] | select(.name=="jjmcp_pkg_status")' >/dev/null \
    && pass "jjmcp_pkg_status tool present" \
    || fail "jjmcp_pkg_status tool missing"

echo "$tools_list" | jq -e '.result.tools[] | select(.name=="jjmcp_capture_test_results")' >/dev/null \
    && pass "jjmcp_capture_test_results tool present" \
    || fail "jjmcp_capture_test_results tool missing"

capture_tool=$(echo "$tools_list" | jq -c '.result.tools[] | select(.name=="jjmcp_capture_test_results")')
for field in lines require_summary include_raw; do
    has=$(echo "$capture_tool" | jq -r ".inputSchema.properties | has(\"$field\")")
    [ "$has" = "true" ] \
        && pass "jjmcp_capture_test_results inputSchema has $field" \
        || fail "jjmcp_capture_test_results inputSchema missing $field"
done

has_output=$(echo "$capture_tool" | jq -r 'has("outputSchema")')
[ "$has_output" = "true" ] \
    && pass "jjmcp_capture_test_results has outputSchema" \
    || fail "jjmcp_capture_test_results missing outputSchema"

# Resources list returns an array (empty when no project_root bound).
resources_count=$(echo "$resources_list" | jq -r '.result.resources | length')
[ "$resources_count" -ge 0 ] \
    && pass "resources/list returns array (count=$resources_count)" \
    || fail "resources/list missing array"

echo "== mcp conformance: error paths =="
err_out=$(printf '%s\n' \
    '{"jsonrpc":"2.0","id":10,"method":"tools/call","params":{"name":"jjmcp_eval","arguments":{"code":"1+1"}}}' \
    '{"jsonrpc":"2.0","id":11,"method":"unknown/method"}' \
    '{"jsonrpc":"2.0","id":12,"method":"tools/call","params":{}}' \
    | run_jjmcp)

# id=10: eval without bind should return isError=true.
no_bind=$(echo "$err_out" | jq -c 'select(.id==10)')
[ "$(echo "$no_bind" | jq -r .result.isError)" = "true" ] \
    && pass "tools/call without bind returns isError=true" \
    || fail "tools/call without bind should be an error"

# id=11: unknown method returns -32601.
unknown=$(echo "$err_out" | jq -c 'select(.id==11)')
[ "$(echo "$unknown" | jq -r .error.code)" = "-32601" ] \
    && pass "unknown method returns -32601" \
    || fail "unknown method code wrong: $(echo "$unknown" | jq -r .error.code)"

# id=12: malformed tools/call returns -32602.
bad_call=$(echo "$err_out" | jq -c 'select(.id==12)')
[ "$(echo "$bad_call" | jq -r .error.code)" = "-32602" ] \
    && pass "malformed tools/call returns -32602" \
    || fail "malformed tools/call code wrong: $(echo "$bad_call" | jq -r .error.code)"

if [ "$failures" -ne 0 ]; then
    echo "FAIL: $failures conformance assertions failed" >&2
    exit 1
fi
echo "all conformance assertions passed"
