#include "jobs.hpp"
#include "julia_wrap.hpp"
#include "mcp.hpp"
#include "socket_client.hpp"
#include "tmux.hpp"
#include "tools.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const std::string& message)
{
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void test_julia_string_literal()
{
    const auto literal = jjmcp::julia_string_literal("a\"b\\c\n$x");
    check(literal == "\"a\\\"b\\\\c\\n\\$x\"", "julia string literal escapes quotes, slash, newline, dollar");
}

void test_wrap_contains_markers_and_result_display()
{
    const auto marker = jjmcp::make_marker("abc");
    const auto bootstrap = jjmcp::make_jjmcp_runtime_bootstrap_code(marker);
    check(bootstrap.find(marker.begin) != std::string::npos, "bootstrap contains begin marker");
    check(bootstrap.find(marker.end) != std::string::npos, "bootstrap contains end marker");
    check(bootstrap.find(marker.error) != std::string::npos, "bootstrap contains error marker");
    check(bootstrap.find(marker.out_end) != std::string::npos, "bootstrap contains out_end marker");
    check(bootstrap.find(marker.val_end) != std::string::npos, "bootstrap contains val_end marker");
    check(bootstrap.find(marker.bt) != std::string::npos, "bootstrap contains backtrace marker");
    check(bootstrap.find("module JJMCPRuntime") != std::string::npos,
          "bootstrap defines the Julia runtime module");
    check(bootstrap.find("macro JJMCP_COMMAND(marker_id, timeout_ms, body)") != std::string::npos,
          "bootstrap defines the command macro");
    check(bootstrap.find("Core.eval(mod, stmt)") != std::string::npos,
          "bootstrap evals top-level body statements in the caller module");
    check(bootstrap.find("show(stdout, MIME(\"text/plain\"), result)") != std::string::npos,
          "bootstrap runtime displays expression result");
    check(bootstrap.find("showerror(stdout, err)") != std::string::npos,
          "bootstrap runtime showerror without backtrace");
    check(bootstrap.find("Base.show_backtrace(stdout, catch_backtrace())") != std::string::npos,
          "bootstrap runtime emits backtrace separately");

    const auto wrapped = jjmcp::wrap_julia_code("1 + 1", marker, 30000);
    check(wrapped.rfind("@JJMCP_COMMAND \"abc\" 30000 begin\n", 0) == 0,
          "wrapper is a clean macro command with timeout");
    check(wrapped.find("1 + 1\nend\n") != std::string::npos,
          "wrapper keeps the user code readable inside the macro body");
}

void test_extract_between_markers()
{
    const auto marker = jjmcp::make_marker("42");
    // Legacy capture (no internal sentinels): exercises the backwards-compatible extraction path.
    const std::string capture =
        "julia> println(\"__JJMCP_BEGIN_42__\")\n"
        "__JJMCP_BEGIN_42__\n"
        "hello\n"
        "__JJMCP_ERROR_42__\n"
        "problem\n"
        "__JJMCP_END_42__\n"
        "julia> ";

    const auto extracted = jjmcp::extract_between_markers(capture, marker);
    check(extracted.found_begin, "extract found begin");
    check(extracted.found_end, "extract found end");
    check(extracted.julia_error, "extract found error marker");
    check(extracted.text == "hello\nproblem", "extract excludes markers and keeps text");
}

void test_extract_structured_success()
{
    const auto marker = jjmcp::make_marker("ok9");
    const std::string capture =
        "__JJMCP_BEGIN_ok9__\n"
        "user printed line A\n"
        "user printed line B\n"
        "__JJMCP_OUT_END_ok9__\n"
        "42\n"
        "__JJMCP_VAL_END_ok9__\n"
        "__JJMCP_END_ok9__\n";

    const auto e = jjmcp::extract_between_markers(capture, marker);
    check(e.found_begin && e.found_end, "structured success: begin and end found");
    check(!e.julia_error, "structured success: no error flag");
    check(e.stdout_text == "user printed line A\nuser printed line B", "structured success: stdout split");
    check(e.value_repr == "42", "structured success: value repr");
    check(e.error_message.empty() && e.backtrace.empty(), "structured success: no error fields populated");
    check(e.text == "user printed line A\nuser printed line B\n42", "structured success: legacy text preserved");
}

void test_extract_structured_success_with_glued_marker()
{
    const auto marker = jjmcp::make_marker("glued");
    const std::string capture =
        "julia> print(\"the marker literal in pasted code should not count: __JJMCP_BEGIN_glued__\")\n"
        "__JJMCP_BEGIN_glued__\n"
        "unterminated stdout"
        "__JJMCP_OUT_END_glued__\n"
        "__JJMCP_VAL_END_glued__\n"
        "__JJMCP_END_glued__\n";

    const auto e = jjmcp::extract_between_markers(capture, marker);
    check(e.found_begin && e.found_end, "structured glued marker: begin and end found");
    check(!e.julia_error, "structured glued marker: no error flag");
    check(e.stdout_text == "unterminated stdout", "structured glued marker: stdout before marker kept");
    check(e.value_repr.empty(), "structured glued marker: empty value repr");
    check(e.text == "unterminated stdout", "structured glued marker: legacy text kept");
}

void test_extract_structured_error()
{
    const auto marker = jjmcp::make_marker("ko9");
    const std::string capture =
        "__JJMCP_BEGIN_ko9__\n"
        "before the throw\n"
        "__JJMCP_ERROR_ko9__\n"
        "MethodError: no method matching f()\n"
        "Closest candidates are:\n"
        "  f(::Int) at REPL[1]:1\n"
        "__JJMCP_BT_ko9__\n"
        "Stacktrace:\n"
        "  [1] top-level scope\n"
        "__JJMCP_END_ko9__\n";

    const auto e = jjmcp::extract_between_markers(capture, marker);
    check(e.julia_error, "structured error: julia_error flag set");
    check(e.stdout_text == "before the throw", "structured error: stdout before ERROR captured");
    check(e.error_message == "MethodError: no method matching f()\nClosest candidates are:\n  f(::Int) at REPL[1]:1",
          "structured error: error_message between ERROR and BT");
    check(e.backtrace == "Stacktrace:\n  [1] top-level scope",
          "structured error: backtrace between BT and END");
    check(e.value_repr.empty(), "structured error: value_repr empty");
}

void test_truncate_lines()
{
    const auto truncated = jjmcp::truncate_lines("a\nb\nc\nd", 2);
    check(truncated == "a\nb\n[JJMCP truncated: omitted 2 line(s)]", "truncate_lines adds omission note");

    const auto tail = jjmcp::truncate_lines_tail("a\nb\nc\nd", 2);
    check(tail == "[JJMCP truncated: omitted 2 earlier line(s)]\nc\nd", "truncate_lines_tail keeps newest lines");

    const auto bytes = jjmcp::truncate_bytes("abcdef", 3);
    check(bytes == "abc\n[JJMCP truncated: omitted 3 byte(s)]", "truncate_bytes caps long single lines");

    const auto bytes_tail = jjmcp::truncate_bytes_tail("abcdef", 3);
    check(bytes_tail == "[JJMCP truncated: omitted 3 earlier byte(s)]\ndef", "truncate_bytes_tail keeps newest bytes");
}

void test_tmux_argv()
{
    const auto argv = jjmcp::make_tmux_argv({"display-message", "-p"});
    check(argv.size() == 3, "tmux argv size");
    check(argv[0] == "tmux" && argv[1] == "display-message" && argv[2] == "-p", "tmux argv contents");
}

void test_socket_default_path()
{
    using jjmcp::SocketClient;

    setenv("XDG_RUNTIME_DIR", "/run/user/1000", 1);
    check(SocketClient::default_socket_path("%19") == "/run/user/1000/jjmcp-19.sock",
          "default_socket_path strips % from pane id");
    check(SocketClient::default_socket_path("") == "/run/user/1000/jjmcp-default.sock",
          "default_socket_path uses 'default' when pane id empty");

    unsetenv("XDG_RUNTIME_DIR");
    check(SocketClient::default_socket_path("%42") == "/tmp/jjmcp-42.sock",
          "default_socket_path falls back to /tmp when XDG_RUNTIME_DIR unset");

    check(!SocketClient::socket_exists("/tmp/jjmcp-this-path-does-not-exist.sock"),
          "socket_exists returns false for missing path");
}

void test_parse_test_summary_pass_only()
{
    const std::string capture =
        "julia> using Test\n"
        "Test Summary:\n"
        "|      | Pass  Total  Time\n"
        "|      |    2      2  0.42s\n";
    const auto parsed = jjmcp::parse_test_summary(capture);
    check(parsed.found_summary, "parse summary detects pass-only block");
    check(parsed.test_pass == 2, "parsed pass count = 2");
    check(parsed.test_fail == 0, "parsed fail count defaults to 0");
    check(parsed.test_total == 2, "parsed total count = 2");
    check(parsed.test_time == "0.42s", "parsed summary time");
    check(parsed.status == "pass", "parsed status = pass");
    check(parsed.failures.empty(), "no failure snippets for passing run");
}

void test_parse_test_summary_with_failure()
{
    const std::string capture =
        "julia> using Test\n"
        "Test Failed at /tmp/runtests.jl:12\n"
        "Test Summary:\n"
        "|      | Pass  Fail  Total  Time\n"
        "|      |    1     1     2  0.01 s\n";
    const auto parsed = jjmcp::parse_test_summary(capture);
    check(parsed.found_summary, "parse summary detects failing block");
    check(parsed.test_pass == 1, "parsed fail-case pass count");
    check(parsed.test_fail == 1, "parsed fail-case fail count");
    check(parsed.test_total == 2, "parsed fail-case total count");
    check(parsed.status == "fail", "parsed status = fail");
    check(parsed.failures.size() == 1, "one failure snippet captured");
    check(parsed.failures[0].find("Test Failed at /tmp/runtests.jl:12") != std::string::npos,
          "failure snippet includes failing location");
}

void test_parse_test_summary_malformed()
{
    const std::string capture =
        "julia> println(\"hello\")\n"
        "Test Summary:\n"
        "No tests were run\n";
    const auto parsed = jjmcp::parse_test_summary(capture);
    check(!parsed.found_summary, "malformed summary does not mark found_summary");
    check(parsed.status == "unknown", "malformed summary status remains unknown");
}

void test_make_test_code()
{
    using jjmcp::make_test_code;

    check(make_test_code("@test 1 == 1", "", "") == "@test 1 == 1", "test_expr precedence");
    check(make_test_code("", "/tmp/runtests.jl", "") == "include(\"/tmp/runtests.jl\")", "file branch");
    check(make_test_code("", "", "myfeature") ==
              "using TestItemRunner\n@run_package_tests filter=ti->occursin(\"myfeature\", ti.name)",
          "test_item_pattern branch");
    check(make_test_code("", "", "") == "using Pkg\nPkg.test()", "default Pkg.test branch");
    check(make_test_code("@test true", "/tmp/r.jl", "p") == "@test true",
          "test_expr beats file and pattern");
}

void test_compute_capture_overlap()
{
    using jjmcp::compute_capture_overlap;

    check(compute_capture_overlap("", "abc") == 0, "overlap empty prior");
    check(compute_capture_overlap("abc", "") == 0, "overlap empty current");

    check(compute_capture_overlap("abc", "abc") == 3, "overlap identical strings");
    check(compute_capture_overlap("xyz", "abc") == 0, "overlap disjoint strings");
    check(compute_capture_overlap("xxxabc", "abcdef") == 3, "overlap suffix matches prefix");
    check(compute_capture_overlap("xxxabcxx", "abcdef") == 0, "overlap inner match without suffix is rejected");
    check(compute_capture_overlap("abcab", "ababc") == 2, "overlap longest of multiple candidates");

    // The scrollback splice case: pane buffer slid one line forward.
    const std::string prior = "BEGIN\nline1\nline2\nline3\n";
    const std::string current = "line1\nline2\nline3\nline4\n";
    const std::size_t k = compute_capture_overlap(prior, current);
    check(k == current.size() - std::string("line4\n").size(),
          "overlap splices scrolled-by-one capture without dup or loss");

    // Pane buffer fully consumed (BEGIN scrolled off entirely): no overlap, but the algorithm must not
    // crash and should return 0 so the caller appends current and preserves what is in the accumulator.
    check(compute_capture_overlap("BEGIN\nline1\n", "lineZ\nlineW\n") == 0,
          "overlap zero when pane scroll exceeds capture window");

    // Small-pane case: pane buffer has not reached the capture-pane window size, so consecutive
    // captures both return the FULL pane and prior is a prefix of current. Without correct handling
    // here the accumulator would duplicate everything in prior because the suffix-of-prior does not
    // match the prefix-of-current.
    const std::string small_prior = "banner\njulia> 1+1\n2\n";
    const std::string small_current = "banner\njulia> 1+1\n2\njulia> 3+4\n7\n";
    check(compute_capture_overlap(small_prior, small_current) == small_prior.size(),
          "overlap returns prior.size() when prior is a strict prefix of current (no scroll case)");
}

void test_runtime_bootstrap_cache()
{
    jjmcp::RuntimeBootstrapCache cache;
    const std::string pane = "%7";

    check(!cache.is_current(pane, "100:200"), "unknown pane is not bootstrapped");

    cache.mark(pane, "100:200");
    check(cache.is_current(pane, "100:200"), "same pane and same REPL process stays bootstrapped");
    check(!cache.is_current(pane, "100:300"), "restarted REPL in the same pane needs bootstrapping");
    check(!cache.is_current("%8", "100:200"), "other pane is not bootstrapped");

    cache.invalidate(pane);
    check(!cache.is_current(pane, "100:200"), "invalidate forces re-injection");

    // Unknown generation on either side degrades to caching by pane key alone.
    cache.mark(pane, "");
    check(cache.is_current(pane, "100:200"), "unknown stored generation trusts the pane entry");
    cache.mark(pane, "100:200");
    check(cache.is_current(pane, ""), "unknown probed generation trusts the pane entry");
}

void test_pane_foreground_generation()
{
    using jjmcp::pane_foreground_generation;

    check(pane_foreground_generation("").empty(), "generation empty for empty pid");
    check(pane_foreground_generation("bash").empty(), "generation empty for non-numeric pid");
    check(pane_foreground_generation("2147483646").empty(), "generation empty for unused pid");

    const std::string self = std::to_string(static_cast<long long>(::getpid()));
    const auto generation = pane_foreground_generation(self);
    check(generation.empty() || generation.rfind(self + ":", 0) == 0,
          "generation for a live pid is prefixed with that pid");
}

std::shared_ptr<jjmcp::EvalJob> make_test_job(const std::string& id, const std::string& target)
{
    auto job = std::make_shared<jjmcp::EvalJob>();
    job->id = id;
    job->marker = jjmcp::make_marker(id);
    job->target = target;
    job->code = "1 + 1";
    job->timeout_ms = 1000;
    job->submitted_at = jjmcp::timestamp_now();
    job->started = std::chrono::steady_clock::now();
    return job;
}

void test_truncate_tool_tail()
{
    const auto tail = jjmcp::truncate_tool_tail("keep_one\nkeep_two\nkeep_three\nkeep_four\n", 2);
    check(tail.find("keep_four") != std::string::npos, "tool tail keeps the newest line");
    check(tail.find("keep_one") == std::string::npos, "tool tail drops the oldest line");
}

void test_read_proc_stats()
{
    using jjmcp::read_proc_stats;

    check(!read_proc_stats(0).available, "proc stats unavailable for pid 0");
    check(!read_proc_stats(-1).available, "proc stats unavailable for a negative pid");

    const auto self = read_proc_stats(static_cast<long long>(::getpid()));
    check(self.available, "proc stats available for the test process");
    check(self.rss_bytes > 0, "proc stats report a non-zero RSS");
    check(self.cpu_seconds >= 0.0, "proc stats report cpu seconds");
    check(self.threads >= 1, "proc stats report at least one thread");
    check(!self.state.empty(), "proc stats report a process state");
}

void test_pane_foreground_pid()
{
    using jjmcp::pane_foreground_pid;

    check(pane_foreground_pid("") == 0, "foreground pid is zero for an empty pid");
    check(pane_foreground_pid("bash") == 0, "foreground pid is zero for a non-numeric pid");
    check(pane_foreground_pid("2147483646") == 0, "foreground pid is zero for an unused pid");
}

void test_job_store_lifecycle()
{
    const auto dir = std::filesystem::temp_directory_path()
                     / ("jjmcp-jobs-" + std::to_string(static_cast<long long>(::getpid())));
    std::filesystem::remove_all(dir);
    jjmcp::JobStore store(dir);

    auto first = make_test_job("job_one", "%1");
    check(store.register_running(first).ok(), "first job registers");
    check(store.find("job_one") == first, "registered job is retrievable by id");
    check(store.running_in("%1") == first, "registered job occupies its pane");
    check(store.most_recent() == first, "most recent job is the one just registered");

    auto second = make_test_job("job_two", "%1");
    check(!store.register_running(second).ok(), "a second job in the same pane is refused");

    auto other_pane = make_test_job("job_three", "%2");
    check(store.register_running(other_pane).ok(), "a job in another pane is accepted");

    first->finish(jjmcp::JobState::Completed, {});
    check(store.running_in("%1") == nullptr, "a finished job frees its pane");
    check(store.register_running(second).ok(), "the pane accepts a new job once the old one ends");

    check(store.list().size() == 3, "the store lists every tracked job");
    store.abandon(second);
    check(store.find("job_two") == nullptr, "an abandoned job leaves no record");
    check(store.running_in("%1") == nullptr, "an abandoned job frees its pane");

    std::filesystem::remove_all(dir);
}

void test_job_snapshot_and_persistence()
{
    const auto dir = std::filesystem::temp_directory_path()
                     / ("jjmcp-persist-" + std::to_string(static_cast<long long>(::getpid())));
    std::filesystem::remove_all(dir);
    jjmcp::JobStore store(dir);

    auto job = make_test_job("persist_one", "%9");
    check(store.register_running(job).ok(), "job registers before it runs");

    const auto running = job->snapshot(100);
    check(running.value("state", "") == "running", "a fresh job reports state=running");
    check(running.value("job_id", "") == "persist_one", "snapshot carries the job id");
    check(running.value("marker_id", "") == "persist_one", "job id and marker id are the same value");
    check(running.contains("live_tail"), "a running job exposes a live tail");

    job->finish(jjmcp::JobState::Completed, {});
    const auto done = job->snapshot(100);
    check(done.value("state", "") == "completed", "a finished job reports state=completed");
    check(!done.contains("live_tail"), "a finished job drops the live tail");

    store.retire(job);
    const auto stored = store.load_persisted("persist_one");
    check(stored.has_value(), "a retired job is written to the result store");
    if (stored) {
        check(stored->value("job_id", "") == "persist_one", "the stored record keeps the job id");
        check(stored->value("state", "") == "completed", "the stored record keeps the final state");
        check(stored->contains("text"), "the stored record keeps the output text");
    }
    check(!store.load_persisted("../escape").has_value(), "a path-traversing job id is rejected");
    check(!store.load_persisted("never_ran").has_value(), "an unknown job id has no stored result");

    std::filesystem::remove_all(dir);
}

void test_job_failure_state()
{
    auto job = make_test_job("failed_one", "%3");
    jjmcp::finish_from_outcome(job, {jjmcp::PollStop::RuntimeMissing, {}});
    const auto snapshot = job->snapshot(10);
    check(snapshot.value("state", "") == "failed", "a missing runtime fails the job");
    check(snapshot.value("failure", "").find("@JJMCP_COMMAND") != std::string::npos,
          "the failure names the missing macro");

    auto timed = make_test_job("timed_one", "%4");
    jjmcp::finish_from_outcome(timed, {jjmcp::PollStop::Pending, {}});
    check(timed->snapshot(10).value("state", "") == "timed_out",
          "a poller that reaches its deadline times the job out");
    check(timed->snapshot(10).value("timed_out", false), "a timed out job sets timed_out");
}

void test_control_plane_tool_names()
{
    using jjmcp::ToolDispatcher;
    check(ToolDispatcher::is_control_plane_tool("jjmcp_job_status"), "job status is control plane");
    check(ToolDispatcher::is_control_plane_tool("jjmcp_result"), "result is control plane");
    check(ToolDispatcher::is_control_plane_tool("jjmcp_capture_job"), "capture_job is control plane");
    check(ToolDispatcher::is_control_plane_tool("jjmcp_list_jobs"), "list_jobs is control plane");
    // These drive the REPL, so they must stay on the worker thread.
    check(!ToolDispatcher::is_control_plane_tool("jjmcp_eval"), "eval is not control plane");
    check(!ToolDispatcher::is_control_plane_tool("jjmcp_eval_async"), "eval_async is not control plane");
    check(!ToolDispatcher::is_control_plane_tool("jjmcp_wait"), "wait is not control plane");
}

void test_encode_mcp_frame_tolerates_invalid_utf8()
{
    nlohmann::json message = {
        {"jsonrpc", "2.0"},
        {"result", {{"text", std::string("julia backtrace \xff\xfe tail")}}},
    };
    std::string encoded;
    bool threw = false;
    try {
        encoded = jjmcp::encode_mcp_frame(message);
    } catch (const std::exception&) {
        threw = true;
    }
    check(!threw, "frame encoding does not throw on invalid UTF-8 pane bytes");
    check(encoded.find("julia backtrace") != std::string::npos,
          "frame encoding keeps the valid part of the text");
}

} // namespace

int main()
{
    test_julia_string_literal();
    test_wrap_contains_markers_and_result_display();
    test_extract_between_markers();
    test_extract_structured_success();
    test_extract_structured_success_with_glued_marker();
    test_extract_structured_error();
    test_truncate_lines();
    test_tmux_argv();
    test_compute_capture_overlap();
    test_make_test_code();
    test_socket_default_path();
    test_parse_test_summary_pass_only();
    test_parse_test_summary_with_failure();
    test_parse_test_summary_malformed();
    test_runtime_bootstrap_cache();
    test_pane_foreground_generation();
    test_encode_mcp_frame_tolerates_invalid_utf8();
    test_truncate_tool_tail();
    test_read_proc_stats();
    test_pane_foreground_pid();
    test_job_store_lifecycle();
    test_job_snapshot_and_persistence();
    test_job_failure_state();
    test_control_plane_tool_names();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All tests passed\n";
    return EXIT_SUCCESS;
}
