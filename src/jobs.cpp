#include "jobs.hpp"

#include "log.hpp"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace jjmcp {
namespace {

constexpr std::size_t kMaxEvalAccumulatorBytes = 1024 * 1024;
constexpr std::size_t kEvalAccumulatorTailLines = 2000;
constexpr std::size_t kJobLiveTailBytes = 64 * 1024;
constexpr std::size_t kMaxJobsInMemory = 32;
constexpr std::size_t kMaxPersistedJobs = 32;
constexpr int kMaxCaptureFailures = 5;
constexpr std::chrono::milliseconds kBackoffMin{50};
constexpr std::chrono::milliseconds kBackoffMax{1000};
constexpr std::size_t kProgressMinDelta = 4096;
constexpr std::size_t kProgressMaxTail = 4096;

void append_line(std::string& out, const std::string& line)
{
    out += line;
    out.push_back('\n');
}

void append_block(std::string& out, const std::string& text)
{
    if (text.empty()) {
        return;
    }
    out += text;
    if (out.back() != '\n') {
        out.push_back('\n');
    }
}

std::string compact_marker_accumulator(const ExtractedOutput& extracted, const Marker& marker,
                                       bool saw_out_end, bool saw_val_end, bool saw_error,
                                       bool saw_backtrace, bool saw_end)
{
    std::string out;
    append_line(out, marker.begin);
    append_block(out, truncate_tool_tail(extracted.stdout_text, kEvalAccumulatorTailLines));

    if (saw_error) {
        append_line(out, marker.error);
        append_block(out, truncate_tool_tail(extracted.error_message, kEvalAccumulatorTailLines));
        if (saw_backtrace) {
            append_line(out, marker.bt);
            append_block(out, truncate_tool_tail(extracted.backtrace, 200, kMaxMcpBacktraceBytes));
        }
    } else if (saw_out_end) {
        append_line(out, marker.out_end);
        append_block(out, truncate_tool_tail(extracted.value_repr, kEvalAccumulatorTailLines));
        if (saw_val_end) {
            append_line(out, marker.val_end);
        }
    }

    if (saw_end) {
        append_line(out, marker.end);
    }
    return out;
}

// The REPL rejects the pasted wrapper with an UndefVarError when Main.JJMCPRuntime is gone, which
// happens when the Julia process in the pane exited and a new one took its place. Scan only after
// the echo of this eval's marker id, so an older failure in the scrollback cannot trigger a rerun.
bool jjmcp_runtime_missing(const std::string& capture, const std::string& marker_id)
{
    const auto echo = capture.rfind(marker_id);
    if (echo == std::string::npos) {
        return false;
    }
    const auto error_pos = capture.find("UndefVarError", echo);
    return error_pos != std::string::npos
        && capture.find("JJMCP_COMMAND", error_pos) != std::string::npos;
}

long long unix_ms_now()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool is_safe_job_id(const std::string& id)
{
    if (id.empty() || id.size() > 128) {
        return false;
    }
    return id.find_first_not_of("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_-")
        == std::string::npos;
}

// Fields of /proc/<pid>/stat after the parenthesised comm, which may itself contain spaces.
std::vector<std::string> proc_stat_fields(long long pid)
{
    std::ifstream stat_file("/proc/" + std::to_string(pid) + "/stat");
    if (!stat_file) {
        return {};
    }
    std::string line;
    std::getline(stat_file, line);
    const auto comm_end = line.rfind(')');
    if (comm_end == std::string::npos) {
        return {};
    }
    std::istringstream rest(line.substr(comm_end + 1));
    std::vector<std::string> fields;
    std::string field;
    while (rest >> field) {
        fields.push_back(field);
    }
    return fields;
}

long long to_ll(const std::string& text)
{
    try {
        return std::stoll(text);
    } catch (const std::exception&) {
        return 0;
    }
}

} // namespace

std::string truncate_tool_tail(const std::string& text, std::size_t max_lines, std::size_t max_bytes)
{
    return truncate_bytes_tail(truncate_lines_tail(text, max_lines), max_bytes);
}

Result<void> AdvisoryLock::acquire(const std::string& marker_id)
{
    std::error_code ec;
    std::filesystem::create_directories(path_.parent_path(), ec);
    for (int attempt = 0; attempt < 2; ++attempt) {
        const int fd = ::open(path_.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0644);
        if (fd >= 0) {
            fd_ = fd;
            const std::string content = std::to_string(static_cast<long long>(::getpid())) + " "
                                        + marker_id + "\n";
            const auto written = ::write(fd, content.data(), content.size());
            (void)written;  // best-effort; the lock is in place even if write fails
            return Result<void>::success();
        }
        if (errno != EEXIST) {
            return Result<void>::failure(std::string("could not open lock file: ")
                                         + std::strerror(errno));
        }
        // Lock exists; check if it is stale (process gone).
        std::ifstream f(path_);
        long long pid = 0;
        f >> pid;
        if (pid > 0 && ::kill(static_cast<pid_t>(pid), 0) == -1 && errno == ESRCH) {
            ::unlink(path_.c_str());
            continue;
        }
        return Result<void>::failure("an evaluation is already in progress (lock at "
                                     + path_.string() + ")");
    }
    return Result<void>::failure("could not acquire advisory lock after retry");
}

void AdvisoryLock::release()
{
    if (fd_ >= 0) {
        ::close(fd_);
        ::unlink(path_.c_str());
        fd_ = -1;
    }
}

long long pane_foreground_pid(const std::string& pane_pid)
{
    if (pane_pid.empty() || pane_pid.find_first_not_of("0123456789") != std::string::npos) {
        return 0;
    }
    const auto fields = proc_stat_fields(to_ll(pane_pid));
    if (fields.size() < 6) {
        return 0;
    }
    const long long tpgid = to_ll(fields[5]);  // field 8 of /proc/<pid>/stat
    return tpgid > 0 ? tpgid : 0;
}

ProcStats read_proc_stats(long long pid)
{
    ProcStats stats;
    if (pid <= 0) {
        return stats;
    }
    const auto fields = proc_stat_fields(pid);
    if (fields.size() < 22) {
        return stats;
    }
    const long ticks = ::sysconf(_SC_CLK_TCK);
    const long page = ::sysconf(_SC_PAGESIZE);
    stats.available = true;
    stats.pid = pid;
    stats.state = fields[0];                                              // field 3
    stats.threads = to_ll(fields[17]);                                    // field 20
    stats.rss_bytes = to_ll(fields[21]) * (page > 0 ? page : 4096);       // field 24, in pages
    const long long jiffies = to_ll(fields[11]) + to_ll(fields[12]);      // fields 14 and 15
    stats.cpu_seconds = ticks > 0 ? static_cast<double>(jiffies) / static_cast<double>(ticks) : 0.0;
    return stats;
}

MarkerPoller::MarkerPoller(const Tmux& tmux, std::string target, Marker marker, int poll_capture_lines)
    : tmux_(tmux), target_(std::move(target)), marker_(std::move(marker)),
      poll_capture_lines_(poll_capture_lines)
{
}

PollOutcome MarkerPoller::poll_until(const std::chrono::steady_clock::time_point deadline,
                                     const PollOptions& options)
{
    int capture_failures = 0;

    for (;;) {
        if (options.cancel != nullptr && options.cancel->load()) {
            return {PollStop::Cancelled, {}};
        }

        const auto captured = tmux_.capture_pane(target_, poll_capture_lines_);
        if (!captured) {
            // A capture can fail transiently while the tmux server is under load. Losing a poll is
            // harmless, so keep waiting and give up only on a persistent failure.
            if (++capture_failures > kMaxCaptureFailures) {
                return {PollStop::Fatal, captured.error()};
            }
            log::warn("capture_pane_failed",
                      {{"error", captured.error()}, {"consecutive", capture_failures}});
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                return {PollStop::Pending, {}};
            }
            const auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
            std::this_thread::sleep_for(std::min(kBackoffMax, wait));
            continue;
        }
        capture_failures = 0;
        const std::string& cap = captured.value();

        // Each capture-pane returns the last poll_capture_lines_ of the visible pane. Splice
        // consecutive captures by their longest suffix-prefix overlap so output that has scrolled
        // off the pane is still preserved here. Without this, a long compile log pushes BEGIN out
        // of the capture window and an eval that actually succeeded looks unfinished.
        bool grew = false;
        if (accumulator_.empty()) {
            if (!cap.empty()) {
                accumulator_ = cap;
                grew = true;
            }
        } else if (cap != last_capture_) {
            const std::size_t overlap = compute_capture_overlap(last_capture_, cap);
            if (overlap < cap.size()) {
                accumulator_.append(cap, overlap, cap.size() - overlap);
                grew = true;
            }
        }
        last_capture_ = cap;

        // Prefer the live capture: it is authoritative for the common not-yet-scrolled case and is
        // immune to any imprecision in the accumulator splice. Fall back to the accumulator once
        // BEGIN has scrolled out of the live capture.
        last_extract_ = extract_between_markers(cap, marker_);
        if (!last_extract_.found_begin) {
            last_extract_ = extract_between_markers(accumulator_, marker_);
        }

        if (options.detect_missing_runtime && !last_extract_.found_begin
            && jjmcp_runtime_missing(cap, marker_.id)) {
            return {PollStop::RuntimeMissing, {}};
        }

        // Once BEGIN is located, drop all pre-BEGIN pane history: later extraction only scans from
        // BEGIN onward, so the working set stays bounded by output size, not by pane history.
        if (!begin_trimmed_ && last_extract_.found_begin) {
            const auto begin_pos = accumulator_.find(marker_.begin);
            if (begin_pos != std::string::npos) {
                const auto line_start = accumulator_.rfind('\n', begin_pos);
                const std::size_t trim_to = (line_start == std::string::npos) ? 0 : line_start + 1;
                if (trim_to > 0) {
                    accumulator_.erase(0, trim_to);
                }
            }
            begin_trimmed_ = true;
        }

        if (options.progress != nullptr && options.progress->active() && last_extract_.found_begin) {
            const std::size_t cur = last_extract_.text.size();
            if (cur >= last_progress_size_ + kProgressMinDelta) {
                const std::string tail =
                    truncate_bytes_tail(last_extract_.text.substr(last_progress_size_), kProgressMaxTail);
                ++progress_count_;
                options.progress->emit(static_cast<double>(progress_count_), tail);
                last_progress_size_ = cur;
            }
        }

        if (!last_extract_.found_end && accumulator_.size() > kMaxEvalAccumulatorBytes) {
            if (last_extract_.found_begin) {
                const bool saw_out_end = accumulator_.find(marker_.out_end) != std::string::npos;
                const bool saw_val_end = accumulator_.find(marker_.val_end) != std::string::npos;
                const bool saw_error = accumulator_.find(marker_.error) != std::string::npos;
                const bool saw_backtrace = accumulator_.find(marker_.bt) != std::string::npos;
                const bool saw_end = accumulator_.find(marker_.end) != std::string::npos;
                accumulator_ = compact_marker_accumulator(last_extract_, marker_, saw_out_end,
                                                          saw_val_end, saw_error, saw_backtrace,
                                                          saw_end);
                last_extract_ = extract_between_markers(accumulator_, marker_);
                last_progress_size_ = 0;
            } else {
                // BEGIN is in no capture we have seen, so nothing in the head can be recovered.
                const std::size_t drop = accumulator_.size() - kMaxEvalAccumulatorBytes / 2;
                const auto line_end = accumulator_.find('\n', drop);
                accumulator_.erase(0, line_end == std::string::npos ? drop : line_end + 1);
            }
        }

        if (options.on_tick) {
            options.on_tick(grew);
        }

        if (last_extract_.found_end) {
            return {PollStop::End, {}};
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return {PollStop::Pending, {}};
        }

        // Adaptive polling: start at 50 ms, double when no growth, reset on growth, cap at 1000 ms.
        backoff_ = grew ? kBackoffMin : std::min(backoff_ * 2, kBackoffMax);
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        std::this_thread::sleep_for(std::min(backoff_, remaining));
    }
}

std::string job_state_name(const JobState state)
{
    switch (state) {
    case JobState::Running:
        return "running";
    case JobState::Completed:
        return "completed";
    case JobState::Failed:
        return "failed";
    case JobState::TimedOut:
        return "timed_out";
    }
    return "unknown";
}

bool EvalJob::finished() const
{
    std::lock_guard<std::mutex> guard(mu);
    return state != JobState::Running;
}

void EvalJob::publish_live()
{
    if (poller == nullptr) {
        return;
    }
    const auto& extracted = poller->extracted();
    std::string tail = truncate_bytes_tail(extracted.text, kJobLiveTailBytes);
    std::lock_guard<std::mutex> guard(mu);
    live_tail = std::move(tail);
    live_found_begin = extracted.found_begin;
    output_bytes = extracted.text.size();
    last_output_unix_ms = unix_ms_now();
}

void EvalJob::finish(const JobState final_state, std::string failure_text)
{
    JobResult final_result;
    if (poller != nullptr) {
        const auto& extracted = poller->extracted();
        final_result.found_begin = extracted.found_begin;
        final_result.found_end = extracted.found_end;
        final_result.julia_error = extracted.julia_error;
        final_result.text = extracted.text;
        final_result.stdout_text = extracted.stdout_text;
        final_result.value_repr = extracted.value_repr;
        final_result.error_message = extracted.error_message;
        final_result.backtrace = extracted.backtrace;
    }
    final_result.timed_out = final_state == JobState::TimedOut;
    final_result.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - started)
                                  .count();

    {
        std::lock_guard<std::mutex> guard(mu);
        if (state != JobState::Running) {
            return;
        }
        state = final_state;
        failure = std::move(failure_text);
        result = std::move(final_result);
        elapsed_ms = result.elapsed_ms;
        output_bytes = result.text.size();
        live_tail.clear();
    }
    // The accumulator can hold a megabyte per job and the result no longer needs it.
    poller.reset();
    // The pane is free again the moment the marker resolved, so drop the lock before waking the
    // waiters that may want to submit the next job.
    lock.reset();
    cv.notify_all();
}

nlohmann::json EvalJob::snapshot(const int capture_lines) const
{
    const auto cap_lines = static_cast<std::size_t>(capture_lines);
    std::lock_guard<std::mutex> guard(mu);
    const bool running = state == JobState::Running;
    const long long live_elapsed =
        running ? std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - started)
                      .count()
                : elapsed_ms;

    nlohmann::json out = {
        {"job_id", id},
        {"marker_id", marker.id},
        {"state", job_state_name(state)},
        {"target", target},
        {"submitted_at", submitted_at},
        {"detached", detached},
        {"timeout_ms", timeout_ms},
        {"elapsed_ms", live_elapsed},
        {"output_bytes", output_bytes},
        {"timed_out", state == JobState::TimedOut},
        {"found_begin", running ? live_found_begin : result.found_begin},
        {"found_end", result.found_end},
        {"julia_error", result.julia_error},
        {"stdout", truncate_tool_tail(result.stdout_text, cap_lines)},
        {"value_repr", truncate_tool_tail(result.value_repr, cap_lines)},
        {"error_message", truncate_tool_tail(result.error_message, cap_lines)},
        {"backtrace", truncate_tool_tail(result.backtrace, 200, kMaxMcpBacktraceBytes)},
        {"transport", "tmux"},
    };
    if (!failure.empty()) {
        out["failure"] = failure;
    }
    if (last_output_unix_ms > 0) {
        out["last_output_unix_ms"] = last_output_unix_ms;
        out["last_output_age_ms"] = unix_ms_now() - last_output_unix_ms;
    }
    if (running) {
        out["live_tail"] = truncate_tool_tail(live_tail, cap_lines);
    }
    return out;
}

JobStore::JobStore(std::filesystem::path dir) : dir_(std::move(dir)) {}

JobStore::~JobStore()
{
    shutdown();
}

void JobStore::set_dir(std::filesystem::path dir)
{
    std::lock_guard<std::mutex> guard(mu_);
    dir_ = std::move(dir);
}

Result<void> JobStore::register_running(const std::shared_ptr<EvalJob>& job)
{
    std::lock_guard<std::mutex> guard(mu_);
    if (shutting_down_) {
        return Result<void>::failure("jjmcp is shutting down");
    }
    if (const auto it = running_by_target_.find(job->target); it != running_by_target_.end()) {
        const auto existing = jobs_.find(it->second);
        if (existing != jobs_.end() && !existing->second->finished()) {
            return Result<void>::failure("job " + it->second + " is still running in " + job->target
                                         + "; wait for it or call jjmcp_interrupt");
        }
        running_by_target_.erase(it);
    }
    jobs_[job->id] = job;
    order_.push_back(job->id);
    running_by_target_[job->target] = job->id;
    trim_locked();
    return Result<void>::success();
}

void JobStore::trim_locked()
{
    for (std::size_t rotations = 0; order_.size() > kMaxJobsInMemory && rotations < order_.size();) {
        const std::string id = order_.front();
        order_.pop_front();
        const auto it = jobs_.find(id);
        if (it != jobs_.end() && !it->second->finished()) {
            order_.push_back(id);  // never evict a job that is still running
            ++rotations;
            continue;
        }
        jobs_.erase(id);
    }
}

std::vector<std::thread> JobStore::take_joinable_locked()
{
    std::vector<std::thread> done;
    for (auto it = threads_.begin(); it != threads_.end();) {
        if (it->first->finished()) {
            done.push_back(std::move(it->second));
            it = threads_.erase(it);
            continue;
        }
        ++it;
    }
    return done;
}

void JobStore::detach(std::shared_ptr<EvalJob> job)
{
    std::vector<std::thread> reaped;
    {
        std::lock_guard<std::mutex> guard(mu_);
        reaped = take_joinable_locked();
    }
    for (auto& thread : reaped) {
        if (thread.joinable()) {
            thread.join();
        }
    }

    {
        std::lock_guard<std::mutex> guard(job->mu);
        job->detached = true;
    }

    std::thread runner([this, job]() {
        const auto deadline = job->started + std::chrono::milliseconds(job->timeout_ms);
        PollOptions options;
        options.detect_missing_runtime = true;
        options.cancel = &job->cancel;
        options.on_tick = [&job](const bool grew) {
            if (grew) {
                job->publish_live();
            }
        };
        PollOutcome outcome;
        try {
            outcome = job->poller->poll_until(deadline, options);
        } catch (const std::exception& e) {
            outcome = {PollStop::Fatal, std::string("poller failed: ") + e.what()};
        } catch (...) {
            outcome = {PollStop::Fatal, "poller failed with an unknown exception"};
        }
        finish_from_outcome(job, outcome);
        retire(job);
    });

    std::lock_guard<std::mutex> guard(mu_);
    threads_.emplace_back(std::move(job), std::move(runner));
}

void JobStore::retire(const std::shared_ptr<EvalJob>& job)
{
    {
        std::lock_guard<std::mutex> guard(mu_);
        if (const auto it = running_by_target_.find(job->target);
            it != running_by_target_.end() && it->second == job->id) {
            running_by_target_.erase(it);
        }
    }
    persist(job);
}

void JobStore::abandon(const std::shared_ptr<EvalJob>& job)
{
    std::lock_guard<std::mutex> guard(mu_);
    if (const auto it = running_by_target_.find(job->target);
        it != running_by_target_.end() && it->second == job->id) {
        running_by_target_.erase(it);
    }
    jobs_.erase(job->id);
    order_.erase(std::remove(order_.begin(), order_.end(), job->id), order_.end());
}

std::shared_ptr<EvalJob> JobStore::find(const std::string& id) const
{
    std::lock_guard<std::mutex> guard(mu_);
    const auto it = jobs_.find(id);
    return it == jobs_.end() ? nullptr : it->second;
}

std::vector<std::shared_ptr<EvalJob>> JobStore::list() const
{
    std::lock_guard<std::mutex> guard(mu_);
    std::vector<std::shared_ptr<EvalJob>> out;
    out.reserve(order_.size());
    for (const auto& id : order_) {
        if (const auto it = jobs_.find(id); it != jobs_.end()) {
            out.push_back(it->second);
        }
    }
    return out;
}

std::shared_ptr<EvalJob> JobStore::running_in(const std::string& target) const
{
    std::lock_guard<std::mutex> guard(mu_);
    const auto it = running_by_target_.find(target);
    if (it == running_by_target_.end()) {
        return nullptr;
    }
    const auto job = jobs_.find(it->second);
    if (job == jobs_.end() || job->second->finished()) {
        return nullptr;
    }
    return job->second;
}

std::shared_ptr<EvalJob> JobStore::most_recent() const
{
    std::lock_guard<std::mutex> guard(mu_);
    for (auto it = order_.rbegin(); it != order_.rend(); ++it) {
        if (const auto job = jobs_.find(*it); job != jobs_.end()) {
            return job->second;
        }
    }
    return nullptr;
}

void JobStore::persist(const std::shared_ptr<EvalJob>& job) const
{
    if (dir_.empty() || !is_safe_job_id(job->id)) {
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
    if (ec) {
        log::warn("job_persist_failed", {{"job_id", job->id}, {"error", ec.message()}});
        return;
    }
    nlohmann::json payload = job->snapshot(kEvalAccumulatorTailLines);
    payload["code"] = truncate_bytes_tail(job->code, 8192);
    payload["text"] = [&]() {
        std::lock_guard<std::mutex> guard(job->mu);
        return truncate_tool_tail(job->result.text, kEvalAccumulatorTailLines);
    }();

    const auto final_path = dir_ / (job->id + ".json");
    const auto temp_path = dir_ / (job->id + ".json.tmp");
    {
        std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
        if (!out) {
            log::warn("job_persist_failed", {{"job_id", job->id}, {"error", "could not open temp file"}});
            return;
        }
        out << payload.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) << '\n';
    }
    std::filesystem::rename(temp_path, final_path, ec);
    if (ec) {
        std::filesystem::remove(temp_path, ec);
        log::warn("job_persist_failed", {{"job_id", job->id}, {"error", ec.message()}});
        return;
    }
    prune_persisted();
}

void JobStore::prune_persisted() const
{
    std::error_code ec;
    const std::filesystem::directory_iterator listing(dir_, ec);
    if (ec) {
        return;
    }
    std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> entries;
    for (const auto& entry : listing) {
        if (!entry.is_regular_file() || entry.path().extension() != ".json") {
            continue;
        }
        const auto written = entry.last_write_time(ec);
        if (ec) {
            ec.clear();
            continue;
        }
        entries.emplace_back(written, entry.path());
    }
    if (entries.size() <= kMaxPersistedJobs) {
        return;
    }
    std::sort(entries.begin(), entries.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    for (std::size_t i = 0; i + kMaxPersistedJobs < entries.size(); ++i) {
        std::filesystem::remove(entries[i].second, ec);
    }
}

std::optional<nlohmann::json> JobStore::load_persisted(const std::string& id) const
{
    if (dir_.empty() || !is_safe_job_id(id)) {
        return std::nullopt;
    }
    std::ifstream in(dir_ / (id + ".json"), std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    try {
        nlohmann::json parsed;
        in >> parsed;
        if (!parsed.is_object()) {
            return std::nullopt;
        }
        return parsed;
    } catch (const std::exception& e) {
        log::warn("job_load_failed", {{"job_id", id}, {"error", e.what()}});
        return std::nullopt;
    }
}

void JobStore::shutdown()
{
    std::vector<std::pair<std::shared_ptr<EvalJob>, std::thread>> threads;
    {
        std::lock_guard<std::mutex> guard(mu_);
        if (shutting_down_) {
            return;
        }
        shutting_down_ = true;
        threads.swap(threads_);
    }
    for (auto& entry : threads) {
        entry.first->cancel.store(true);
    }
    for (auto& entry : threads) {
        if (entry.second.joinable()) {
            entry.second.join();
        }
    }
}

void finish_from_outcome(const std::shared_ptr<EvalJob>& job, const PollOutcome& outcome)
{
    switch (outcome.stop) {
    case PollStop::End:
        job->finish(JobState::Completed, {});
        return;
    case PollStop::Pending:
        job->finish(JobState::TimedOut, {});
        return;
    case PollStop::RuntimeMissing:
        job->finish(JobState::Failed,
                    "the Julia REPL in " + job->target
                        + " no longer defines @JJMCP_COMMAND; the process was replaced after this "
                          "job was sent");
        return;
    case PollStop::Cancelled:
        job->finish(JobState::Failed, "job was cancelled");
        return;
    case PollStop::Fatal:
        job->finish(JobState::Failed, outcome.error);
        return;
    }
    job->finish(JobState::Failed, "unknown poller outcome");
}

} // namespace jjmcp
