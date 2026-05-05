#pragma once

#include <optional>
#include <string>
#include <utility>

namespace jjmcp {

template <typename T>
class Result {
public:
    static Result success(T value) { return Result(SuccessTag{}, std::move(value)); }
    static Result failure(std::string error) { return Result(FailureTag{}, std::move(error)); }

    [[nodiscard]] bool ok() const { return value_.has_value(); }
    explicit operator bool() const { return ok(); }

    T& value() { return *value_; }
    const T& value() const { return *value_; }

    const std::string& error() const { return error_; }

private:
    struct SuccessTag {};
    struct FailureTag {};

    Result(SuccessTag, T value) : value_(std::move(value)) {}
    Result(FailureTag, std::string error) : error_(std::move(error)) {}

    std::optional<T> value_;
    std::string error_;
};

template <>
class Result<void> {
public:
    static Result success() { return Result(true, {}); }
    static Result failure(std::string error) { return Result(false, std::move(error)); }

    [[nodiscard]] bool ok() const { return ok_; }
    explicit operator bool() const { return ok(); }

    const std::string& error() const { return error_; }

private:
    Result(bool ok, std::string error) : ok_(ok), error_(std::move(error)) {}

    bool ok_ = false;
    std::string error_;
};

} // namespace jjmcp
