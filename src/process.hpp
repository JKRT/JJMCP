#pragma once

#include "result.hpp"

#include <chrono>
#include <string>
#include <vector>

namespace jjmcp {

struct RunSpec {
    std::vector<std::string> argv;
    std::string stdin_data;
    std::chrono::milliseconds timeout{5000};
};

struct ProcessResult {
    int exit_code = -1;
    bool signaled = false;
    int signal_number = 0;
    bool timed_out = false;
    std::string stdout_text;
    std::string stderr_text;
};

class ProcessRunner {
public:
    Result<ProcessResult> run(const RunSpec& spec) const;
};

std::string describe_process_result(const ProcessResult& result);

} // namespace jjmcp
