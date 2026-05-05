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
};

struct ExtractedOutput {
    bool found_begin = false;
    bool found_end = false;
    bool julia_error = false;
    std::string text;
};

Marker make_marker(std::string id);
std::string make_marker_id(unsigned long long sequence);
std::string julia_string_literal(const std::string& text);
std::string wrap_julia_code(const std::string& code, const Marker& marker);
std::string make_activate_code(const std::string& path);
std::string make_revise_code();
std::string make_test_code(const std::string& test_expr, const std::string& file);

ExtractedOutput extract_between_markers(const std::string& capture, const Marker& marker);
std::string truncate_lines(const std::string& text, std::size_t max_lines);
std::string truncate_lines_tail(const std::string& text, std::size_t max_lines);
std::vector<std::string> split_lines(const std::string& text);
std::string trim_ascii(std::string text);

} // namespace jjmcp
