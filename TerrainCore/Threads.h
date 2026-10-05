#pragma once
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <exception>
#include <functional>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

// Spreading work that needs no answer from any other part of it across threads. Everything
// here is only the running of threads: what they compute, and that nothing they compute
// depends on which thread computes it or in what order, is the caller's to keep.

// The threads a request for threadCount runs on: that many, or with 0 or less, one per
// hardware thread (at least one).
//
// Complexity: O(1). Thread-safety: safe to call concurrently.
inline int ThreadsFor(int threadCount)
{
    if (threadCount > 0) return threadCount;
    unsigned hardware = std::thread::hardware_concurrency();
    return hardware == 0 ? 1 : (int)hardware;
}

// Runs work(thread, failed) on up to `threads` threads at once, the calling thread being
// thread 0, and returns how many ran -- fewer when the system won't start more, in which case
// the ones that did start do all the work. work should stop taking more once `failed` is set,
// which happens when any thread throws; that exception is rethrown here once every thread has
// stopped. Nothing is left running when this returns or throws.
//
// Complexity: O(1) beyond starting the threads and the work itself.
// Thread-safety: work runs on several threads at once and must be safe to.
template <class Work>
int RunOnThreads(int threads, Work&& work)
{
    std::vector<std::exception_ptr> errors(threads < 1 ? 1 : threads);
    std::atomic<bool> failed{ false };
    auto guarded = [&](int thread) {
        try
        {
            work(thread, failed);
        }
        catch (...)
        {
            errors[thread] = std::current_exception();
            failed = true;
        }
    };

    std::vector<std::thread> helpers;
    helpers.reserve(errors.size() - 1);
    for (int t = 1; t < (int)errors.size(); t++)
    {
        try
        {
            helpers.emplace_back(guarded, t);
        }
        catch (const std::system_error&)
        {
            break;
        }
    }
    guarded(0);
    for (auto& helper : helpers) helper.join();

    for (const auto& error : errors)
    {
        if (error) std::rethrow_exception(error);
    }
    return (int)helpers.size() + 1;
}

// A report of how many items have been taken so far; false asks the work to stop.
using BlockProgress = std::function<bool(size_t itemsTaken)>;

// What ForEachBlockOnThreads did: whether every block was done, and on how many threads.
struct BlocksRun
{
    bool finished = true;
    int threads = 1;
};

// Runs work(first, last, thread) once for every block of the items [0, count) -- [0, block),
// [block, 2 * block) and so on, the last cut short at count -- on up to `threads` threads at once
// (RunOnThreads), each taking the next block not yet taken from a shared counter. first() runs on
// the calling thread before any other thread starts, for what every block needs done first.
//
// Progress is reported on the calling thread only, with the items taken so far: 0 before first()
// and before any thread starts, then before each later block the calling thread takes. So a
// callback never runs on a thread its caller didn't start, and the first report is always of no
// work done. A report that returns false stops every thread before its next block, and the run
// comes back unfinished. With no items there is nothing to report.
//
// Complexity: O(count / block) blocks taken, beyond the work itself.
// Thread-safety: work runs on several threads at once; it must write only its own items.
// Exceptions: as RunOnThreads -- any thread's is rethrown here once every thread has stopped.
template <class First, class Work>
BlocksRun ForEachBlockOnThreads(size_t count, size_t block, int threads, const BlockProgress& progress, First&& first, Work&& work)
{
    BlocksRun run;
    if (count > 0 && progress && !progress(0))
    {
        run.finished = false;
        return run;
    }
    first();

    std::atomic<size_t> next{ 0 };
    std::atomic<bool> stopped{ false };
    run.threads = RunOnThreads(threads, [&](int thread, const std::atomic<bool>& failed) {
        while (!stopped.load(std::memory_order_relaxed) && !failed.load(std::memory_order_relaxed))
        {
            size_t start = next.fetch_add(block, std::memory_order_relaxed);
            if (start >= count) return;
            if (thread == 0 && start > 0 && progress && !progress(start))
            {
                stopped = true;
                return;
            }
            work(start, (std::min)(start + block, count), thread);
        }
    });
    run.finished = !stopped;
    return run;
}

// The same, with nothing to do first.
template <class Work>
BlocksRun ForEachBlockOnThreads(size_t count, size_t block, int threads, const BlockProgress& progress, Work&& work)
{
    return ForEachBlockOnThreads(count, block, threads, progress, [] {}, std::forward<Work>(work));
}
