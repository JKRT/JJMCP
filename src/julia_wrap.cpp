#include "julia_wrap.hpp"

#include <chrono>
#include <cstring>
#include <sstream>
#include <unistd.h>

namespace jjmcp {

namespace {

std::string join_lines(const std::vector<std::string>& lines, std::size_t first, std::size_t last)
{
    std::string out;
    for (std::size_t i = first; i < last; ++i) {
        if (!out.empty()) {
            out.push_back('\n');
        }
        out += lines[i];
    }
    return out;
}

std::string indent_code(const std::string& code)
{
    std::stringstream input(code);
    std::string line;
    std::string out;
    while (std::getline(input, line)) {
        out += "            ";
        out += line;
        out.push_back('\n');
    }
    if (!code.empty() && code.back() == '\n') {
        return out;
    }
    return out;
}

} // namespace

Marker make_marker(std::string id)
{
    Marker marker;
    marker.id = std::move(id);
    marker.begin = "__JJMCP_BEGIN_" + marker.id + "__";
    marker.end = "__JJMCP_END_" + marker.id + "__";
    marker.error = "__JJMCP_ERROR_" + marker.id + "__";
    marker.out_end = "__JJMCP_OUT_END_" + marker.id + "__";
    marker.val_end = "__JJMCP_VAL_END_" + marker.id + "__";
    marker.bt = "__JJMCP_BT_" + marker.id + "__";
    return marker;
}

std::string make_marker_id(unsigned long long sequence)
{
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(now).count();
    return std::to_string(micros) + "_" + std::to_string(static_cast<long long>(::getpid())) + "_" + std::to_string(sequence);
}

std::string julia_string_literal(const std::string& text)
{
    std::string out = "\"";
    for (const unsigned char c : text) {
        switch (c) {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20) {
                constexpr char hex[] = "0123456789abcdef";
                out += "\\x";
                out.push_back(hex[(c >> 4) & 0x0f]);
                out.push_back(hex[c & 0x0f]);
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
    }
    out += "\"";
    return out;
}

std::string wrap_julia_code(const std::string& code, const Marker& marker)
{
    // Markers are emitted to stderr in dim grey via printstyled+flush so that they appear in the
    // pane scrollback distinct from the user's stdout output (which keeps default coloring).
    // tmux capture-pane -J -p captures both streams, so line-anchored extraction is unaffected.
    auto emit = [](const std::string& m) {
        return "        printstyled(stderr, " + julia_string_literal(m)
               + ", '\\n'; color=:light_black); flush(stderr)\n";
    };
    auto emit_inline = [](const std::string& m) {
        return "    printstyled(stderr, " + julia_string_literal(m)
               + ", '\\n'; color=:light_black); flush(stderr)\n";
    };

    std::string out;
    out += "let\n";
    out += "    local __jjmcp_result\n";
    out += emit_inline(marker.begin);
    out += "    try\n";
    out += "        __jjmcp_result = begin\n";
    out += indent_code(code.empty() ? "nothing" : code);
    out += "        end\n";
    out += emit(marker.out_end);
    out += "        if !isnothing(__jjmcp_result)\n";
    out += "            show(stdout, MIME(\"text/plain\"), __jjmcp_result)\n";
    out += "            println()\n";
    out += "            flush(stdout)\n";
    out += "        end\n";
    out += emit(marker.val_end);
    out += "    catch e\n";
    out += emit(marker.error);
    out += "        showerror(stdout, e)\n";
    out += "        println()\n";
    out += "        flush(stdout)\n";
    out += emit(marker.bt);
    out += "        Base.show_backtrace(stdout, catch_backtrace())\n";
    out += "        println()\n";
    out += "        flush(stdout)\n";
    out += "    finally\n";
    out += emit(marker.end);
    out += "    end\n";
    out += "    nothing\n";
    out += "end\n";
    return out;
}

std::string make_activate_code(const std::string& path)
{
    return "using Pkg\nPkg.activate(" + julia_string_literal(path) + ")";
}

std::string make_revise_code()
{
    return "try\n"
           "    using Revise\n"
           "    Revise.revise()\n"
           "    println(\"Revise complete\")\n"
           "catch e\n"
           "    showerror(stdout, e, catch_backtrace())\n"
           "    println()\n"
           "end";
}

std::string make_test_code(const std::string& test_expr, const std::string& file, const std::string& test_item_pattern)
{
    if (!test_expr.empty()) {
        return test_expr;
    }
    if (!file.empty()) {
        return "include(" + julia_string_literal(file) + ")";
    }
    if (!test_item_pattern.empty()) {
        return "using TestItemRunner\n@run_package_tests filter=ti->occursin("
               + julia_string_literal(test_item_pattern) + ", ti.name)";
    }
    return "using Pkg\nPkg.test()";
}

std::vector<std::string> split_lines(const std::string& text)
{
    std::vector<std::string> lines;
    std::string current;
    for (const char c : text) {
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            lines.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty() || (!text.empty() && text.back() == '\n')) {
        lines.push_back(current);
    }
    return lines;
}

std::string trim_ascii(std::string text)
{
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' || text.back() == '\n')) {
        text.pop_back();
    }
    std::size_t first = 0;
    while (first < text.size() && (text[first] == ' ' || text[first] == '\t' || text[first] == '\r' || text[first] == '\n')) {
        ++first;
    }
    if (first > 0) {
        text.erase(0, first);
    }
    return text;
}

namespace {

std::vector<std::size_t> kmp_prefix_function(const std::string& s)
{
    std::vector<std::size_t> pi(s.size(), 0);
    for (std::size_t i = 1; i < s.size(); ++i) {
        std::size_t j = pi[i - 1];
        while (j > 0 && s[i] != s[j]) {
            j = pi[j - 1];
        }
        if (s[i] == s[j]) {
            ++j;
        }
        pi[i] = j;
    }
    return pi;
}

} // namespace

std::size_t compute_capture_overlap(const std::string& prior, const std::string& current)
{
    if (prior.empty() || current.empty()) {
        return 0;
    }
    // Case A: small pane has not reached the capture-pane window size, so consecutive captures
    // both return the FULL pane and prior is a strict prefix of current. The longest suffix of
    // prior that matches a prefix of current is short here, but the actual amount of "old"
    // content at the start of current is prior.size(). Without this fast path the accumulator
    // would duplicate everything in prior since the suffix-prefix KMP returns 0 for "abc...x"
    // vs "abc...xy" when x is not the same as the first char of current.
    if (current.size() >= prior.size()
        && std::memcmp(current.data(), prior.data(), prior.size()) == 0) {
        return prior.size();
    }
    // Case B: pane has scrolled. Compute the longest suffix-of-prior that equals a prefix-of-current
    // via KMP automaton walk over prior using current as the pattern. O(|prior| + |current|).
    const auto pi = kmp_prefix_function(current);
    std::size_t state = 0;
    for (const char c : prior) {
        if (state == current.size()) {
            state = pi[state - 1];
        }
        while (state > 0 && c != current[state]) {
            state = pi[state - 1];
        }
        if (c == current[state]) {
            ++state;
        }
    }
    return state;
}

ExtractedOutput extract_between_markers(const std::string& capture, const Marker& marker)
{
    ExtractedOutput result;
    const auto lines = split_lines(capture);
    bool inside = false;
    enum class Phase { Stdout, Value, Error, Backtrace };
    Phase phase = Phase::Stdout;
    std::vector<std::string> all_text;
    std::vector<std::string> stdout_lines;
    std::vector<std::string> value_lines;
    std::vector<std::string> error_lines;
    std::vector<std::string> backtrace_lines;

    for (const auto& line : lines) {
        const auto trimmed = trim_ascii(line);
        if (!inside) {
            if (trimmed == marker.begin) {
                result.found_begin = true;
                inside = true;
            }
            continue;
        }

        if (trimmed == marker.end) {
            result.found_end = true;
            break;
        }
        if (trimmed == marker.error) {
            result.julia_error = true;
            phase = Phase::Error;
            continue;
        }
        if (trimmed == marker.out_end) {
            phase = Phase::Value;
            continue;
        }
        if (trimmed == marker.val_end) {
            // No further structured content expected on the success path. Subsequent lines (if any
            // before END) are still recorded in the legacy text but not in any structured slice.
            phase = Phase::Stdout;
            continue;
        }
        if (trimmed == marker.bt) {
            phase = Phase::Backtrace;
            continue;
        }

        all_text.push_back(line);
        switch (phase) {
        case Phase::Stdout:
            stdout_lines.push_back(line);
            break;
        case Phase::Value:
            value_lines.push_back(line);
            break;
        case Phase::Error:
            error_lines.push_back(line);
            break;
        case Phase::Backtrace:
            backtrace_lines.push_back(line);
            break;
        }
    }

    result.text = join_lines(all_text, 0, all_text.size());
    result.stdout_text = join_lines(stdout_lines, 0, stdout_lines.size());
    result.value_repr = join_lines(value_lines, 0, value_lines.size());
    result.error_message = join_lines(error_lines, 0, error_lines.size());
    result.backtrace = join_lines(backtrace_lines, 0, backtrace_lines.size());
    return result;
}

std::string truncate_lines(const std::string& text, std::size_t max_lines)
{
    if (max_lines == 0) {
        return "[JJMCP truncated: output hidden because max line count is 0]";
    }

    auto lines = split_lines(text);
    if (!lines.empty() && lines.back().empty()) {
        lines.pop_back();
    }
    if (lines.size() <= max_lines) {
        return text;
    }

    const std::size_t omitted = lines.size() - max_lines;
    std::string out = join_lines(lines, 0, max_lines);
    if (!out.empty()) {
        out.push_back('\n');
    }
    out += "[JJMCP truncated: omitted " + std::to_string(omitted) + " line(s)]";
    return out;
}

std::string truncate_lines_tail(const std::string& text, std::size_t max_lines)
{
    if (max_lines == 0) {
        return "[JJMCP truncated: output hidden because max line count is 0]";
    }

    auto lines = split_lines(text);
    if (!lines.empty() && lines.back().empty()) {
        lines.pop_back();
    }
    if (lines.size() <= max_lines) {
        return text;
    }

    const std::size_t omitted = lines.size() - max_lines;
    std::string out = "[JJMCP truncated: omitted " + std::to_string(omitted) + " earlier line(s)]\n";
    out += join_lines(lines, omitted, lines.size());
    return out;
}

} // namespace jjmcp
