#pragma once

#include "result.hpp"

#include <cstddef>
#include <filesystem>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>

namespace jjmcp {

// One tool call as the transcript records it.
struct TranscriptEntry {
    std::string timestamp;
    unsigned long long sequence = 0;
    std::string tool;
    std::string target;        // bound pane, empty when none is bound
    std::string code;          // Julia code sent to the REPL; empty for tools that send none
    nlohmann::json arguments;  // the tool's arguments, recorded when no code was sent
    bool is_error = false;
    long long elapsed_ms = 0;
    std::string transport;     // "socket" or "tmux" for eval-style tools
    std::string output;        // the result, untruncated where the transport kept it
};

enum class TranscriptFormat { Text, Jsonl };

// "jsonl" selects JSON lines; anything else the text format.
TranscriptFormat transcript_format_from_string(const std::string& name);
std::string transcript_format_name(TranscriptFormat format);

// Largest output recorded per entry; longer output keeps its end with a truncation note.
constexpr std::size_t kMaxTranscriptOutputBytes = 8 * 1024 * 1024;

// One entry as written to the transcript file (no I/O).
std::string format_transcript_entry(const TranscriptEntry& entry, TranscriptFormat format);

// Append-only log of tool calls and their results. Each entry opens, appends to and closes the
// file, so the file can be read, truncated or rotated while the server runs. Disabled while the
// path is empty.
class Transcript {
public:
    void enable(std::filesystem::path path, TranscriptFormat format);
    void disable();
    [[nodiscard]] bool enabled() const;
    [[nodiscard]] std::filesystem::path path() const;
    [[nodiscard]] TranscriptFormat format() const;

    // Numbers the entry and writes it; a no-op when disabled.
    Result<void> append(TranscriptEntry entry);

private:
    mutable std::mutex mutex_;
    std::filesystem::path path_;
    TranscriptFormat format_ = TranscriptFormat::Text;
    unsigned long long sequence_ = 0;
};

} // namespace jjmcp
