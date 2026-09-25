#pragma once
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>
#include "VerticalDatum.h"

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

// A height as the command line takes it, in any datum the engine can put on the terrain:
//   2               2 m above the ground, as every command has always read a bare number
//   2:agl           the same, said outright
//   1937:msl        1937 m above mean sea level (orthometric)
//   1915.4:hae:-21.6  1915.4 m above the WGS84 ellipsoid, where the geoid lies 21.6 m below it
// An ellipsoidal height needs the geoid undulation at its point to be put on the terrain's
// mean sea level, and is refused without one rather than read as if it were zero.
enum class HeightArgProblem
{
    None,
    NotANumber,                   // the metres, or the undulation, isn't a finite number
    UnknownDatum,                 // a suffix other than agl, msl or hae:<undulation>
    EllipsoidWithoutUndulation    // hae with no undulation after it
};

struct HeightArg
{
    DatumHeight height;
    HeightArgProblem problem = HeightArgProblem::None;
};

// Complexity: O(length of text). Thread-safety: as ParseFiniteDouble.
inline HeightArg ParseHeight(const char* text)
{
    HeightArg result;
    if (text == nullptr) { result.problem = HeightArgProblem::NotANumber; return result; }

    std::string whole(text);
    std::vector<std::string> parts;
    for (size_t start = 0;;)
    {
        size_t colon = whole.find(':', start);
        parts.push_back(whole.substr(start, colon == std::string::npos ? std::string::npos : colon - start));
        if (colon == std::string::npos) break;
        start = colon + 1;
    }

    std::optional<double> metres = ParseFiniteDouble(parts[0].c_str());
    if (!metres.has_value()) { result.problem = HeightArgProblem::NotANumber; return result; }
    result.height.valueM = *metres;

    if (parts.size() == 1 || (parts.size() == 2 && parts[1] == "agl"))
    {
        result.height.datum = VerticalDatum::HeightAboveGround;
    }
    else if (parts.size() == 2 && parts[1] == "msl")
    {
        result.height.datum = VerticalDatum::OrthometricMsl;
    }
    else if (parts[1] == "hae" && parts.size() <= 3)
    {
        result.height.datum = VerticalDatum::EllipsoidalHae;
        if (parts.size() == 2) { result.problem = HeightArgProblem::EllipsoidWithoutUndulation; return result; }
        std::optional<double> undulation = ParseFiniteDouble(parts[2].c_str());
        if (!undulation.has_value()) { result.problem = HeightArgProblem::NotANumber; return result; }
        result.height.geoidUndulationM = *undulation;
    }
    else
    {
        result.problem = HeightArgProblem::UnknownDatum;
    }
    return result;
}
