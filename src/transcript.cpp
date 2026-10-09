#include "transcript.hpp"

#include <fstream>
#include <sstream>
#include <utility>

namespace jjmcp {

namespace {

std::string cap_output(const std::string& output)
{
    if (output.size() <= kMaxTranscriptOutputBytes) {
        return output;
    }
    const std::size_t omitted = output.size() - kMaxTranscriptOutputBytes;
    return "[jjmcp transcript: " + std::to_string(omitted) + " earlier bytes omitted]\n"
        + output.substr(omitted);
}

void append_block(std::ostringstream& out, const std::string& text)
{
    out << text;
    if (!text.empty() && text.back() != '\n') {
        out << '\n';
    }
}

} // namespace

TranscriptFormat transcript_format_from_string(const std::string& name)
{
    return name == "jsonl" ? TranscriptFormat::Jsonl : TranscriptFormat::Text;
}

std::string transcript_format_name(const TranscriptFormat format)
{
    return format == TranscriptFormat::Jsonl ? "jsonl" : "text";
}

std::string format_transcript_entry(const TranscriptEntry& entry, const TranscriptFormat format)
{
    const std::string output = cap_output(entry.output);
    if (format == TranscriptFormat::Jsonl) {
        nlohmann::json doc = {
            {"ts", entry.timestamp},
            {"seq", entry.sequence},
            {"tool", entry.tool},
            {"target", entry.target},
            {"ok", !entry.is_error},
            {"elapsed_ms", entry.elapsed_ms},
            {"output", output},
        };
        if (!entry.code.empty()) {
            doc["code"] = entry.code;
        } else {
            doc["arguments"] = entry.arguments.is_null() ? nlohmann::json::object() : entry.arguments;
        }
        if (!entry.transport.empty()) {
            doc["transport"] = entry.transport;
        }
        // Invalid UTF-8 from the pane must not lose the entry.
        return doc.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + '\n';
    }

    std::ostringstream out;
    out << "==== " << entry.timestamp << " #" << entry.sequence << ' ' << entry.tool;
    if (!entry.target.empty()) {
        out << " pane " << entry.target;
    }
    out << " ====\n";
    if (!entry.code.empty()) {
        out << ">>> code\n";
        append_block(out, entry.code);
    } else if (!entry.arguments.is_null() && !entry.arguments.empty()) {
        out << ">>> arguments "
            << entry.arguments.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) << '\n';
    }
    out << "<<< " << (entry.is_error ? "error" : "ok") << ", " << entry.elapsed_ms << " ms";
    if (!entry.transport.empty()) {
        out << ", " << entry.transport;
    }
    out << '\n';
    append_block(out, output);
    out << '\n';
    return out.str();
}

void Transcript::enable(std::filesystem::path path, const TranscriptFormat format)
{
    std::lock_guard<std::mutex> guard(mutex_);
    path_ = std::move(path);
    format_ = format;
}

void Transcript::disable()
{
    std::lock_guard<std::mutex> guard(mutex_);
    path_.clear();
}

bool Transcript::enabled() const
{
    std::lock_guard<std::mutex> guard(mutex_);
    return !path_.empty();
}

std::filesystem::path Transcript::path() const
{
    std::lock_guard<std::mutex> guard(mutex_);
    return path_;
}

TranscriptFormat Transcript::format() const
{
    std::lock_guard<std::mutex> guard(mutex_);
    return format_;
}

Result<void> Transcript::append(TranscriptEntry entry)
{
    std::lock_guard<std::mutex> guard(mutex_);
    if (path_.empty()) {
        return Result<void>::success();
    }
    entry.sequence = ++sequence_;
    std::error_code ec;
    if (path_.has_parent_path()) {
        std::filesystem::create_directories(path_.parent_path(), ec);
    }
    std::ofstream file(path_, std::ios::app | std::ios::binary);
    if (!file) {
        return Result<void>::failure("could not open transcript file: " + path_.string());
    }
    file << format_transcript_entry(entry, format_);
    file.flush();
    if (!file) {
        return Result<void>::failure("could not write transcript file: " + path_.string());
    }
    return Result<void>::success();
}

} // namespace jjmcp
