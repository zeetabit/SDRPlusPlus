#pragma once
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

// Parallel seed execution for the benchmark harnesses.
//
// Seeds are independent decodes over independent Channel instances, and the
// decode path holds no mutable global state: Channel owns no ToneScanner, so no
// FFTW planner (which is not thread-safe) is reachable from it, and every
// `static` under src/cw/ is either `static constexpr` or a function-local
// `static const` table.
//
// Results must be written to a slot indexed by the loop variable, never
// appended, so output does not depend on thread count or scheduling. Set
// CW_TEST_THREADS=1 to compare against serial execution.
namespace cw_test {

    inline int testThreads() {
        if (const char* env = std::getenv("CW_TEST_THREADS")) {
            const int n = std::atoi(env);
            if (n > 0) { return n; }
        }
        const unsigned hc = std::thread::hardware_concurrency();
        return hc > 0 ? (int)hc : 1;
    }

    // fn(i) for i in [0,n). fn must not use Catch2 macros or printf — both are
    // thread-unsafe, and a Catch2 assertion throws, which would terminate the
    // worker. An escaping exception is captured and rethrown on this thread.
    inline void parallelFor(int n, const std::function<void(int)>& fn) {
        const int threads = std::min(testThreads(), n);
        if (threads <= 1) {
            for (int i = 0; i < n; i++) { fn(i); }
            return;
        }

        std::atomic<int> next{0};
        std::mutex errMutex;
        std::exception_ptr firstErr;

        std::vector<std::thread> pool;
        pool.reserve(threads);
        for (int t = 0; t < threads; t++) {
            pool.emplace_back([&] {
                // Atomic claim rather than static chunking: per-seed decode
                // time varies several-fold across profiles.
                for (int i = next++; i < n; i = next++) {
                    try { fn(i); }
                    catch (...) {
                        std::lock_guard<std::mutex> lock(errMutex);
                        if (!firstErr) { firstErr = std::current_exception(); }
                        return;
                    }
                }
            });
        }
        for (auto& th : pool) { th.join(); }
        if (firstErr) { std::rethrow_exception(firstErr); }
    }
}
