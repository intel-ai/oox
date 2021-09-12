#include <benchmark/benchmark.h>
#include <oox/oox.h>

constexpr int FibN=28;

namespace Serial {
    int Fib(volatile int n) {
        if(n < 2) return n;
        return Fib(n-1) + Fib(n-2);
    }
}
static void Fib_Serial(benchmark::State& state) {
  for (auto _ : state)
    Serial::Fib(FibN);
}
// Register the function as a benchmark
BENCHMARK(Fib_Serial);


namespace OOX {
    oox_var<int> Fib(int n) {
        if(n < 2) return n;
        return oox_run(std::plus<int>(), oox_run(Fib, n-1), Fib(n-2));
    }
}
// Define another benchmark
static void Fib_OOX(benchmark::State& state) {
  for (auto _ : state)
    oox_wait_and_get(OOX::Fib(FibN));
}
BENCHMARK(Fib_OOX);

#if HAVE_TBB

#include <tbb/tbb.h>
namespace TBB {
    int Fib(int n) {                  // TBB: High-level blocking style
        if(n < 2) return n;
        int left, right;
        tbb::parallel_invoke(
            [&] { left = Fib(n-1); },
            [&] { right = Fib(n-2); }
        );
        return left + right;
    }
}

static void Fib_TBB(benchmark::State& state) {
  for (auto _ : state)
    TBB::Fib(FibN);
}
BENCHMARK(Fib_TBB);

#endif //HAVE_TBB

#if HAVE_TF

#include <taskflow/taskflow.hpp>

namespace TF {
    int spawn(int n, tf::Subflow& sbf) {
        if (n < 2) return n;
        int res1, res2;

        // compute f(n-1)
        sbf.emplace([&res1, n] (tf::Subflow& sbf) { res1 = spawn(n - 1, sbf); } );
            //.name(std::to_string(n-1));

        // compute f(n-2)
        sbf.emplace([&res2, n] (tf::Subflow& sbf) { res2 = spawn(n - 2, sbf); } );
            //.name(std::to_string(n-2));

        sbf.join();
        return res1 + res2;
    }

    void Fib(int N) {
        tf::Executor executor;
        tf::Taskflow taskflow("fibonacci");
        int res;  // result
        taskflow.emplace([&res, N] (tf::Subflow& sbf) { 
            res = spawn(N, sbf);  
        }); //.name(std::to_string(N));

        executor.run(taskflow).wait();        
    }
}
static void Fib_TF(benchmark::State& state) {
  for (auto _ : state)
    TF::Fib(FibN);
}
BENCHMARK(Fib_TF);
#endif //HAVE_TF

BENCHMARK_MAIN();
