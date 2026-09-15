#pragma once
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <optional>

// Number parsing for the command-line driver. std::stod and std::stoi throw on text
// that isn't a number, and an uncaught exception ends the process without a word
// about which argument was wrong. These return nullopt instead. The number must be
// the whole argument -- "12abc" is rejected, not read as 12 -- and finite.

// Complexity: O(length of text). Thread-safety: uses errno, which is thread-local;
// safe to call concurrently.
inline std::optional<double> ParseFiniteDouble(const char* text)
{
    if (text == nullptr || *text == '\0') return std::nullopt;

    char* end = nullptr;
    errno = 0;
    double value = std::strtod(text, &end);
    if (end == text || *end != '\0' || errno == ERANGE || !std::isfinite(value)) return std::nullopt;

    return value;
}

// Complexity: O(length of text). Thread-safety: uses errno, which is thread-local;
// safe to call concurrently.
inline std::optional<int> ParseInt(const char* text)
{
    if (text == nullptr || *text == '\0') return std::nullopt;

    char* end = nullptr;
    errno = 0;
    long long value = std::strtoll(text, &end, 10);
    if (end == text || *end != '\0' || errno == ERANGE || value < INT_MIN || value > INT_MAX) return std::nullopt;

    return (int)value;
}
