#pragma once
#include <atomic>
#include <exception>
#include <system_error>
#include <thread>
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
