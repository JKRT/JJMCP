#pragma once

#include "julia_wrap.hpp"
#include "progress.hpp"
#include "result.hpp"
#include "tmux.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace jjmcp {

inline constexpr std::size_t kMaxMcpTextBytes = 256 * 1024;
inline constexpr std::size_t kMaxMcpBacktraceBytes = 128 * 1024;

std::string truncate_tool_tail(const std::string& text, std::size_t max_lines,
                               std::size_t max_bytes = kMaxMcpTextBytes);

// Exclusive marker for one in-flight evaluation. Held for the life of a job, which outlives the
// tool call that started it, so a detached job still blocks a second paste into the same REPL.
class AdvisoryLock {
public:
    explicit AdvisoryLock(std::filesystem::path path) : path_(std::move(path)) {}
    ~AdvisoryLock() { release(); }
    AdvisoryLock(const AdvisoryLock&) = delete;
    AdvisoryLock& operator=(const AdvisoryLock&) = delete;

    Result<void> acquire(const std::string& marker_id);
    void release();

private:
    std::filesystem::path path_;
    int fd_ = -1;
};

// Live process counters for the REPL behind a pane. Read from /proc, so they stay available while
// Julia itself is busy and cannot answer.
struct ProcStats {
    bool available = false;
    long long pid = 0;
    std::string state;
    double cpu_seconds = 0.0;
    long long rss_bytes = 0;
    long long threads = 0;
};

// Foreground process group of a pane terminal, which is the REPL currently running there.
// Zero when it cannot be read.
long long pane_foreground_pid(const std::string& pane_pid);
ProcStats read_proc_stats(long long pid);

enum class PollStop {
    Pending,         // deadline reached with the job still running
    End,             // END marker seen
    RuntimeMissing,  // the REPL rejected @JJMCP_COMMAND
    Cancelled,
    Fatal,
};

struct PollOutcome {
    PollStop stop = PollStop::Pending;
    std::string error;
};

struct PollOptions {
    bool detect_missing_runtime = false;
    ProgressEmitter* progress = nullptr;
    const std::atomic<bool>* cancel = nullptr;
    // Invoked after every capture, on the polling thread. `grew` is true when the capture added
    // bytes. Used to republish the live view of a background job.
    std::function<void(bool grew)> on_tick;
};

// Resumable poller for one marker. It owns the capture accumulator, so output that has already
// scrolled out of the visible pane stays recoverable, and a foreground wait can hand the same
// accumulator to a background thread without losing anything at the handoff.
class MarkerPoller {
public:
    MarkerPoller(const Tmux& tmux, std::string target, Marker marker, int poll_capture_lines);

    PollOutcome poll_until(std::chrono::steady_clock::time_point deadline, const PollOptions& options);

    const Marker& marker() const { return marker_; }
    const ExtractedOutput& extracted() const { return last_extract_; }
    const std::string& last_capture() const { return last_capture_; }
    std::size_t output_bytes() const { return last_extract_.text.size(); }

private:
    const Tmux& tmux_;
    std::string target_;
    Marker marker_;
    int poll_capture_lines_;

    std::string accumulator_;
    std::string last_capture_;
    ExtractedOutput last_extract_;
    bool begin_trimmed_ = false;
    std::size_t last_progress_size_ = 0;
    int progress_count_ = 0;
    std::chrono::milliseconds backoff_{50};
};

enum class JobState { Running, Completed, Failed, TimedOut };

std::string job_state_name(JobState state);

struct JobResult {
    bool found_begin = false;
    bool found_end = false;
    bool julia_error = false;
    bool timed_out = false;
    std::string text;
    std::string stdout_text;
    std::string value_repr;
    std::string error_message;
    std::string backtrace;
    long long elapsed_ms = 0;
};

// One evaluation, tracked independently of the tool call that started it. Identity fields are set
// before the job is registered and never change; everything under `mu` is republished by whichever
// thread is currently polling.
struct EvalJob {
    std::string id;
    Marker marker;
    std::string target;
    std::string pane_pid;
    std::string code;
    int timeout_ms = 0;
    int capture_lines = 0;
    std::string submitted_at;
    std::chrono::steady_clock::time_point started{};

    std::unique_ptr<MarkerPoller> poller;
    std::unique_ptr<AdvisoryLock> lock;
    std::atomic<bool> cancel{false};

    mutable std::mutex mu;
    std::condition_variable cv;
    JobState state = JobState::Running;
    bool detached = false;
    std::string failure;
    JobResult result;
    std::string live_tail;
    bool live_found_begin = false;
    std::size_t output_bytes = 0;
    long long elapsed_ms = 0;
    long long last_output_unix_ms = 0;

    [[nodiscard]] bool finished() const;
    // Republish the live view from the poller. Call only from the thread that owns the poller.
    void publish_live();
    // Record the terminal result, release the pane lock and wake every waiter.
    void finish(JobState final_state, std::string failure_text);
    [[nodiscard]] nlohmann::json snapshot(int capture_lines) const;
};

// Map a poller outcome onto the job's terminal state. Shared by the foreground wait in eval_code
// and by the background poller thread.
void finish_from_outcome(const std::shared_ptr<EvalJob>& job, const PollOutcome& outcome);

// Bounded registry of jobs. Completed results are also written under `dir` so that a client
// reconnect, an eviction, or a jjmcp restart cannot lose a result that tmux scrollback no longer
// holds either.
class JobStore {
public:
    explicit JobStore(std::filesystem::path dir);
    ~JobStore();
    // Follows the bound project, so results land beside the config that selected the pane.
    void set_dir(std::filesystem::path dir);
    JobStore(const JobStore&) = delete;
    JobStore& operator=(const JobStore&) = delete;

    // Registers a job as running. Fails when another job is still running in the same pane.
    Result<void> register_running(const std::shared_ptr<EvalJob>& job);
    // Moves polling of an already-registered job onto a background thread.
    void detach(std::shared_ptr<EvalJob> job);
    // Terminal bookkeeping: clears the pane slot and persists the result.
    void retire(const std::shared_ptr<EvalJob>& job);
    // Drops a job that never reached the REPL, leaving no record behind.
    void abandon(const std::shared_ptr<EvalJob>& job);

    [[nodiscard]] std::shared_ptr<EvalJob> find(const std::string& id) const;
    [[nodiscard]] std::vector<std::shared_ptr<EvalJob>> list() const;
    [[nodiscard]] std::shared_ptr<EvalJob> running_in(const std::string& target) const;
    [[nodiscard]] std::shared_ptr<EvalJob> most_recent() const;
    [[nodiscard]] std::optional<nlohmann::json> load_persisted(const std::string& id) const;

    void shutdown();

private:
    void persist(const std::shared_ptr<EvalJob>& job) const;
    void prune_persisted() const;
    void trim_locked();
    // Takes the threads of jobs that already finished. Join them without holding mu_: a poller
    // thread calls back into retire() after it publishes the result.
    std::vector<std::thread> take_joinable_locked();

    std::filesystem::path dir_;
    mutable std::mutex mu_;
    std::unordered_map<std::string, std::shared_ptr<EvalJob>> jobs_;
    std::deque<std::string> order_;
    std::unordered_map<std::string, std::string> running_by_target_;
    std::vector<std::pair<std::shared_ptr<EvalJob>, std::thread>> threads_;
    bool shutting_down_ = false;
};

} // namespace jjmcp
