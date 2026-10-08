#pragma once

// Internal helpers for turning errno into C++ errors.

#include <cerrno>
#include <system_error>

namespace http::detail {

// The error from the last failed system call, as a std::error_code that
// compares equal to the matching std::errc value.
inline std::error_code lastError() noexcept
{
    return {errno, std::generic_category()};
}

// Throws the error from the last failed system call. `call` names the failing
// function so the message says what went wrong, e.g. "bind: Address in use".
[[noreturn]] inline void throwSystemError(const char* call)
{
    throw std::system_error{lastError(), call};
}

} // namespace http::detail
