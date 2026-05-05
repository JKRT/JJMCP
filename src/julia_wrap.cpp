#include "julia_wrap.hpp"

#include <chrono>
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
    std::string out;
    out += "let\n";
    out += "    local __jjmcp_result\n";
    out += "    println(" + julia_string_literal(marker.begin) + ")\n";
    out += "    try\n";
    out += "        __jjmcp_result = begin\n";
    out += indent_code(code.empty() ? "nothing" : code);
    out += "        end\n";
    out += "        if !isnothing(__jjmcp_result)\n";
    out += "            show(stdout, MIME(\"text/plain\"), __jjmcp_result)\n";
    out += "            println()\n";
    out += "        end\n";
    out += "    catch e\n";
    out += "        println(" + julia_string_literal(marker.error) + ")\n";
    out += "        showerror(stdout, e, catch_backtrace())\n";
    out += "        println()\n";
    out += "    finally\n";
    out += "        println(" + julia_string_literal(marker.end) + ")\n";
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

std::string make_test_code(const std::string& test_expr, const std::string& file)
{
    if (!test_expr.empty()) {
        return test_expr;
    }
    if (!file.empty()) {
        return "include(" + julia_string_literal(file) + ")";
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

ExtractedOutput extract_between_markers(const std::string& capture, const Marker& marker)
{
    ExtractedOutput result;
    const auto lines = split_lines(capture);
    bool inside = false;
    std::vector<std::string> output;

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
            continue;
        }
        output.push_back(line);
    }

    result.text = join_lines(output, 0, output.size());
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
