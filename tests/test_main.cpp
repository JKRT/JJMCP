#include "julia_wrap.hpp"
#include "tmux.hpp"

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
    check(wrapped.find("show(stdout, MIME(\"text/plain\"), __jjmcp_result)") != std::string::npos, "wrapper displays expression result");
    check(wrapped.find("nothing\nend\n") != std::string::npos, "wrapper suppresses REPL display of wrapper result");
}

void test_extract_between_markers()
{
    const auto marker = jjmcp::make_marker("42");
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

} // namespace

int main()
{
    test_julia_string_literal();
    test_wrap_contains_markers_and_result_display();
    test_extract_between_markers();
    test_truncate_lines();
    test_tmux_argv();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All tests passed\n";
    return EXIT_SUCCESS;
}
