#pragma once
#include <atomic>
#include <vector>
#include "LineOfSight.h"
#include "Threads.h"

// Line of sight for many observers against many targets, on every core. Each pair is its own
// profile and its own ComputeLineOfSight, written to its own place in the answer: nothing is
// summed, sorted or shared between pairs, so which thread answers a pair, and in what order,
// can't change a bit of it. The answer at any thread count is the answer
// ComputeBatchLineOfSight gives for the same pairs on one.

// One end of a sight line: where it is and how high.
struct SightEnd
{
    GeoPoint point;
    DatumHeight height;
};

struct LineOfSightPairs
{
    int observerCount = 0;
    int targetCount = 0;

    // Observer by observer: the answer for observer o and target t is results[o * targetCount + t],
    // exactly what ComputeLineOfSight gives along that pair's own profile. A pair whose path
    // can't be sampled is refused on its own (status InvalidInput) and the rest answered, as
    // ComputeBatchLineOfSight does.
    std::vector<LineOfSightResult> results;

    // How many threads answered: the number asked for (ThreadsFor), or fewer if the system
    // wouldn't start them all.
    int threadsUsed = 0;

    const LineOfSightResult& At(int observer, int target) const { return results[(size_t)observer * targetCount + target]; }
};

// === BATCH PATH, ON EVERY CORE ===
// Answers every observer against every target, at spacingDeg, on ThreadsFor(threadCount)
// threads -- the calling thread among them; 0, the default, is one per hardware thread.
// Threads take pairs in blocks of 16 from a shared counter, so one that draws short paths
// takes more of them; each keeps its own profile buffer. The sampler is read from every
// thread at once and must allow it, as every sampler in this library does once constructed.
//
// Complexity: O(observers * targets * samples per path) work, spread across the threads.
// Memory: one result per pair, and one profile buffer per thread.
// Thread-safety: safe to call from any thread; it reads the sampler from several at once.
// Exceptions: running out of memory on any thread is rethrown on the calling one, after
// every thread has stopped.
inline LineOfSightPairs ComputeLineOfSightPairs(const std::vector<SightEnd>& observers, const std::vector<SightEnd>& targets, double spacingDeg, IElevationSampler& sampler, double k = 4.0 / 3.0, int threadCount = 0)
{
    LineOfSightPairs pairs;
    pairs.observerCount = (int)observers.size();
    pairs.targetCount = (int)targets.size();
    pairs.results.resize(observers.size() * targets.size());
    if (pairs.results.empty())
    {
        pairs.threadsUsed = 1;
        return pairs;
    }

    const size_t pairCount = pairs.results.size();
    const size_t block = 16;
    std::atomic<size_t> next{ 0 };
    pairs.threadsUsed = RunOnThreads(ThreadsFor(threadCount), [&](int, const std::atomic<bool>& failed) {
        std::vector<ProfileSample> profile;
        while (!failed.load(std::memory_order_relaxed))
        {
            size_t first = next.fetch_add(block, std::memory_order_relaxed);
            if (first >= pairCount) return;
            size_t last = (std::min)(first + block, pairCount);
            for (size_t p = first; p < last; p++)
            {
                const SightEnd& observer = observers[p / targets.size()];
                const SightEnd& target = targets[p % targets.size()];
                InputProblem pathProblem = CheckProfileRequest(observer.point, target.point, spacingDeg);
                if (pathProblem != InputProblem::None)
                {
                    LineOfSightResult refused;
                    refused.status = ComputationStatus::InvalidInput;
                    refused.inputProblem = pathProblem;
                    pairs.results[p] = refused;
                    continue;
                }
                GetTerrainProfile(observer.point, target.point, spacingDeg, sampler, profile);
                pairs.results[p] = ComputeLineOfSight(profile, observer.height, target.height, k);
            }
        }
    });
    return pairs;
}
