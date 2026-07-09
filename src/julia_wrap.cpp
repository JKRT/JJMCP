#include "julia_wrap.hpp"

#include <chrono>
#include <cstring>
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

bool is_utf8_continuation(unsigned char c)
{
    return (c & 0xc0) == 0x80;
}

std::size_t utf8_safe_prefix_length(const std::string& text, std::size_t max_bytes)
{
    if (max_bytes >= text.size()) {
        return text.size();
    }
    std::size_t keep = max_bytes;
    while (keep > 0 && is_utf8_continuation(static_cast<unsigned char>(text[keep]))) {
        --keep;
    }
    return keep;
}

std::size_t utf8_safe_suffix_start(const std::string& text, std::size_t start)
{
    if (start >= text.size()) {
        return text.size();
    }
    while (start < text.size() && is_utf8_continuation(static_cast<unsigned char>(text[start]))) {
        ++start;
    }
    return start;
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
        case '$':
            out += "\\$";
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

std::string make_jjmcp_runtime_bootstrap_code(const Marker& marker)
{
    // Markers are emitted to stderr in dim grey via printstyled+flush so that they appear in the
    // pane scrollback distinct from the user's stdout output (which keeps default coloring).
    // tmux capture-pane -J -p captures both streams, so line-anchored extraction is unaffected.
    auto emit = [](const std::string& m) {
        return "            printstyled(stderr, " + julia_string_literal(m)
               + ", '\\n'; color=:light_black); flush(stderr)\n";
    };
    auto emit_inline = [](const std::string& m) {
        return "        printstyled(stderr, " + julia_string_literal(m)
               + ", '\\n'; color=:light_black); flush(stderr)\n";
    };

    std::string out;
    out += "begin\n";
    out += "    # JJMCP one-time Julia runtime bootstrap for clean @JJMCP_COMMAND evals.\n";
    out += emit_inline(marker.begin);
    out += "    try\n";
    out += "        if !isdefined(Main, :JJMCPRuntime)\n";
    out += "            Core.eval(Main, :(module JJMCPRuntime\n";
    out += "            end))\n";
    out += "        end\n";
    out += "        Core.eval(Main.JJMCPRuntime, quote\n";
    out += "            export @JJMCP_COMMAND\n";
    out += "            RUNTIME_VERSION = 1\n";
    out += "            _marker(id::AbstractString, kind::AbstractString) = \"__JJMCP_$(kind)_$(id)__\"\n";
    out += "            function _emit_marker(kind::AbstractString, id::AbstractString)\n";
    out += "                printstyled(stderr, _marker(id, kind), '\\n'; color = :light_black)\n";
    out += "                flush(stderr)\n";
    out += "            end\n";
    out += "            function _unwrap_load_error(e)\n";
    out += "                while e isa LoadError\n";
    out += "                    e = e.error\n";
    out += "                end\n";
    out += "                return e\n";
    out += "            end\n";
    out += "            function _eval_body(mod::Module, body)\n";
    out += "                if body isa Expr && body.head === :block\n";
    out += "                    value = nothing\n";
    out += "                    for stmt in body.args\n";
    out += "                        stmt isa LineNumberNode && continue\n";
    out += "                        value = Core.eval(mod, stmt)\n";
    out += "                    end\n";
    out += "                    return value\n";
    out += "                end\n";
    out += "                return Core.eval(mod, body)\n";
    out += "            end\n";
    out += "            function run_command(marker_id::AbstractString, timeout_ms::Integer, body, mod::Module)\n";
    out += "                result = nothing\n";
    out += "                _emit_marker(\"BEGIN\", marker_id)\n";
    out += "                try\n";
    out += "                    result = _eval_body(mod, body)\n";
    out += "                    _emit_marker(\"OUT_END\", marker_id)\n";
    out += "                    if !isnothing(result)\n";
    out += "                        show(stdout, MIME(\"text/plain\"), result)\n";
    out += "                        println()\n";
    out += "                        flush(stdout)\n";
    out += "                    end\n";
    out += "                    _emit_marker(\"VAL_END\", marker_id)\n";
    out += "                catch e\n";
    out += "                    _emit_marker(\"ERROR\", marker_id)\n";
    out += "                    err = _unwrap_load_error(e)\n";
    out += "                    showerror(stdout, err)\n";
    out += "                    println()\n";
    out += "                    flush(stdout)\n";
    out += "                    _emit_marker(\"BT\", marker_id)\n";
    out += "                    Base.show_backtrace(stdout, catch_backtrace())\n";
    out += "                    println()\n";
    out += "                    flush(stdout)\n";
    out += "                finally\n";
    out += "                    _emit_marker(\"END\", marker_id)\n";
    out += "                end\n";
    out += "                return nothing\n";
    out += "            end\n";
    out += "            macro JJMCP_COMMAND(marker_id, timeout_ms, body)\n";
    out += "                return :(Main.JJMCPRuntime.run_command($(esc(marker_id)), $(esc(timeout_ms)), $(QuoteNode(body)), $(QuoteNode(__module__))))\n";
    out += "            end\n";
    out += "        end)\n";
    out += "        Core.eval(Main, :(using .JJMCPRuntime: @JJMCP_COMMAND))\n";
    out += emit(marker.out_end);
    out += emit(marker.val_end);
    out += "    catch e\n";
    out += emit(marker.error);
    out += "        err = e\n";
    out += "        while err isa LoadError\n";
    out += "            err = err.error\n";
    out += "        end\n";
    out += "        showerror(stdout, err)\n";
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

std::string wrap_julia_code(const std::string& code, const Marker& marker, int timeout_ms)
{
    const std::string eval_source = code.empty() ? std::string("nothing") : code;

    std::string out;
    out += "@JJMCP_COMMAND " + julia_string_literal(marker.id) + " " + std::to_string(timeout_ms) + " begin\n";
    out += eval_source;
    if (!eval_source.empty() && eval_source.back() != '\n') {
        out.push_back('\n');
    }
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

std::string marker_segment_text(std::string segment)
{
    std::string out;
    out.reserve(segment.size());
    for (const char c : segment) {
        if (c != '\r') {
            out.push_back(c);
        }
    }
    if (!out.empty() && out.back() == '\n') {
        out.pop_back();
    }
    return out;
}

void append_segment(std::string& out, const std::string& segment)
{
    const std::string text = marker_segment_text(segment);
    if (text.empty()) {
        return;
    }
    if (!out.empty() && out.back() != '\n' && text.front() != '\n') {
        out.push_back('\n');
    }
    out += text;
}

std::size_t find_marker_line_payload_start(const std::string& capture, const std::string& marker)
{
    std::size_t line_start = 0;
    while (line_start <= capture.size()) {
        const std::size_t line_end = capture.find('\n', line_start);
        const std::size_t content_end = line_end == std::string::npos ? capture.size() : line_end;
        const std::string line = capture.substr(line_start, content_end - line_start);
        if (trim_ascii(line) == marker) {
            return line_end == std::string::npos ? content_end : line_end + 1;
        }
        if (line_end == std::string::npos) {
            break;
        }
        line_start = line_end + 1;
    }
    return std::string::npos;
}

struct FoundMarkerToken {
    enum class Kind { End, Error, OutEnd, ValEnd, Backtrace };

    std::size_t pos = std::string::npos;
    const std::string* marker = nullptr;
    Kind kind = Kind::End;
};

FoundMarkerToken find_next_marker_token(const std::string& capture, const Marker& marker,
                                        std::size_t start)
{
    FoundMarkerToken best;
    auto consider = [&](const std::string& text, FoundMarkerToken::Kind kind) {
        const std::size_t pos = capture.find(text, start);
        if (pos != std::string::npos && pos < best.pos) {
            best.pos = pos;
            best.marker = &text;
            best.kind = kind;
        }
    };

    consider(marker.end, FoundMarkerToken::Kind::End);
    consider(marker.error, FoundMarkerToken::Kind::Error);
    consider(marker.out_end, FoundMarkerToken::Kind::OutEnd);
    consider(marker.val_end, FoundMarkerToken::Kind::ValEnd);
    consider(marker.bt, FoundMarkerToken::Kind::Backtrace);
    return best;
}

std::size_t skip_marker_line_ending(const std::string& capture, std::size_t pos)
{
    if (pos < capture.size() && capture[pos] == '\r') {
        ++pos;
    }
    if (pos < capture.size() && capture[pos] == '\n') {
        ++pos;
    }
    return pos;
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
    enum class Phase { Stdout, Value, Error, Backtrace };
    Phase phase = Phase::Stdout;

    std::size_t pos = find_marker_line_payload_start(capture, marker.begin);
    if (pos == std::string::npos) {
        return result;
    }
    result.found_begin = true;

    while (pos < capture.size()) {
        const FoundMarkerToken token = find_next_marker_token(capture, marker, pos);
        if (token.marker == nullptr) {
            const std::string segment = capture.substr(pos);
            append_segment(result.text, segment);
            switch (phase) {
            case Phase::Stdout:
                append_segment(result.stdout_text, segment);
                break;
            case Phase::Value:
                append_segment(result.value_repr, segment);
                break;
            case Phase::Error:
                append_segment(result.error_message, segment);
                break;
            case Phase::Backtrace:
                append_segment(result.backtrace, segment);
                break;
            }
            break;
        }

        const std::string segment = capture.substr(pos, token.pos - pos);
        append_segment(result.text, segment);
        switch (phase) {
        case Phase::Stdout:
            append_segment(result.stdout_text, segment);
            break;
        case Phase::Value:
            append_segment(result.value_repr, segment);
            break;
        case Phase::Error:
            append_segment(result.error_message, segment);
            break;
        case Phase::Backtrace:
            append_segment(result.backtrace, segment);
            break;
        }

        if (token.kind == FoundMarkerToken::Kind::End) {
            result.found_end = true;
            break;
        }
        if (token.kind == FoundMarkerToken::Kind::Error) {
            result.julia_error = true;
            phase = Phase::Error;
        } else if (token.kind == FoundMarkerToken::Kind::OutEnd) {
            phase = Phase::Value;
        } else if (token.kind == FoundMarkerToken::Kind::ValEnd) {
            // No further structured content expected on the success path. Subsequent lines (if any
            // before END) are still recorded in the legacy text but not in any structured slice.
            phase = Phase::Stdout;
        } else if (token.kind == FoundMarkerToken::Kind::Backtrace) {
            phase = Phase::Backtrace;
        }

        pos = skip_marker_line_ending(capture, token.pos + token.marker->size());
    }
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

std::string truncate_bytes(const std::string& text, std::size_t max_bytes)
{
    if (max_bytes == 0) {
        return "[JJMCP truncated: output hidden because max byte count is 0]";
    }
    if (text.size() <= max_bytes) {
        return text;
    }

    const std::size_t keep = utf8_safe_prefix_length(text, max_bytes);
    std::string out = text.substr(0, keep);
    if (!out.empty() && out.back() != '\n') {
        out.push_back('\n');
    }
    out += "[JJMCP truncated: omitted " + std::to_string(text.size() - keep) + " byte(s)]";
    return out;
}

std::string truncate_bytes_tail(const std::string& text, std::size_t max_bytes)
{
    if (max_bytes == 0) {
        return "[JJMCP truncated: output hidden because max byte count is 0]";
    }
    if (text.size() <= max_bytes) {
        return text;
    }

    const std::size_t start = utf8_safe_suffix_start(text, text.size() - max_bytes);
    return "[JJMCP truncated: omitted " + std::to_string(start) + " earlier byte(s)]\n"
        + text.substr(start);
}

} // namespace jjmcp
