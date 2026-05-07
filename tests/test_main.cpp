#include "julia_wrap.hpp"
#include "socket_client.hpp"
#include "tmux.hpp"

#include <cstdlib>

#include <cstdlib>
#include <iostream>
#include <string>
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
    const auto literal = jjmcp::julia_string_literal("a\"b\\c\n");
    check(literal == "\"a\\\"b\\\\c\\n\"", "julia string literal escapes quotes, slash, newline");
}

void test_wrap_contains_markers_and_result_display()
{
    const auto marker = jjmcp::make_marker("abc");
    const auto wrapped = jjmcp::wrap_julia_code("1 + 1", marker);
    check(wrapped.find(marker.begin) != std::string::npos, "wrapper contains begin marker");
    check(wrapped.find(marker.end) != std::string::npos, "wrapper contains end marker");
    check(wrapped.find(marker.error) != std::string::npos, "wrapper contains error marker");
    check(wrapped.find(marker.out_end) != std::string::npos, "wrapper contains out_end marker");
    check(wrapped.find(marker.val_end) != std::string::npos, "wrapper contains val_end marker");
    check(wrapped.find(marker.bt) != std::string::npos, "wrapper contains backtrace marker");
    check(wrapped.find("show(stdout, MIME(\"text/plain\"), __jjmcp_result)") != std::string::npos, "wrapper displays expression result");
    check(wrapped.find("showerror(stdout, e)") != std::string::npos, "wrapper showerror without backtrace");
    check(wrapped.find("Base.show_backtrace(stdout, catch_backtrace())") != std::string::npos, "wrapper emits backtrace separately");
    check(wrapped.find("nothing\nend\n") != std::string::npos, "wrapper suppresses REPL display of wrapper result");
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

} // namespace

int main()
{
    test_julia_string_literal();
    test_wrap_contains_markers_and_result_display();
    test_extract_between_markers();
    test_extract_structured_success();
    test_extract_structured_error();
    test_truncate_lines();
    test_tmux_argv();
    test_compute_capture_overlap();
    test_make_test_code();
    test_socket_default_path();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All tests passed\n";
    return EXIT_SUCCESS;
}
