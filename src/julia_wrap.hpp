#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace jjmcp {

struct Marker {
    std::string id;
    std::string begin;
    std::string end;
    std::string error;
    std::string out_end;
    std::string val_end;
    std::string bt;
};

struct ExtractedOutput {
    bool found_begin = false;
    bool found_end = false;
    bool julia_error = false;
    // Legacy text: every line between BEGIN and END except marker lines themselves.
    // Preserved verbatim so existing clients that only read the text payload keep working.
    std::string text;
    // Structured slices populated by extract_between_markers using the four internal sentinels
    // (OUT_END, VAL_END, ERROR, BT). On success: stdout_text holds user println output,
    // value_repr holds the show(MIME"text/plain", result) output. On Julia error: error_message
    // holds the showerror message (no backtrace), backtrace holds Base.show_backtrace output.
    std::string stdout_text;
    std::string value_repr;
    std::string error_message;
    std::string backtrace;
};

Marker make_marker(std::string id);
std::string make_marker_id(unsigned long long sequence);
std::string julia_string_literal(const std::string& text);
std::string make_jjmcp_runtime_bootstrap_code(const Marker& marker);
std::string wrap_julia_code(const std::string& code, const Marker& marker, int timeout_ms);
std::string make_activate_code(const std::string& path);
std::string make_revise_code();
// Build the test-driver Julia expression. Precedence: test_expr > file > test_item_pattern > Pkg.test().
// test_item_pattern triggers `using TestItemRunner; @run_package_tests filter=ti->occursin(pattern, ti.name)`.
std::string make_test_code(const std::string& test_expr, const std::string& file, const std::string& test_item_pattern);

ExtractedOutput extract_between_markers(const std::string& capture, const Marker& marker);
std::string truncate_lines(const std::string& text, std::size_t max_lines);
std::string truncate_lines_tail(const std::string& text, std::size_t max_lines);
std::string truncate_bytes(const std::string& text, std::size_t max_bytes);
std::string truncate_bytes_tail(const std::string& text, std::size_t max_bytes);
std::vector<std::string> split_lines(const std::string& text);
std::string trim_ascii(std::string text);

// Longest k such that prior.suffix(k) == current.prefix(k). O(|prior| + |current|).
// Used by the eval_code accumulator to splice consecutive tmux capture-pane snapshots
// without losing the BEGIN marker when the visible pane buffer scrolls.
std::size_t compute_capture_overlap(const std::string& prior, const std::string& current);

} // namespace jjmcp
