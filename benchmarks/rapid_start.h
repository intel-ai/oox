
#pragma once
#include <algorithm>
#include <atomic>
#if __has_include(<zmmintrin.h>)
#include <zmmintrin.h>
#define _clevict(a, b) _mm_clevict(a, b)
#else
#define _clevict(a, b)
#endif

#include <tbb/task_group.h>

typedef uintptr_t mask_t;

namespace Harness {
const size_t MAX_THREADS = sizeof(mask_t)*8;
using tbb_task = tbb::detail::d1::task;

class distribution_base {
public:
    virtual void execute(int part, int parts) const = 0;
    virtual ~distribution_base() {}
    void run(int slot, mask_t mask) {
        int part=0, parts=1;
        for(int i = 1; i < int(sizeof(mask)*8); i++)
            if(mask&(mask_t(1)<<i)) {
                if(i==slot) part = parts;
                ++parts;
            }
        execute(part, parts);
    }
};
template<typename F>
class distribution_function : public distribution_base {
    int my_start, my_end;
    F &my_func;
    /*override*/ void execute(int part, int parts) const {
        const int range = my_end - my_start;
        const int step = range / parts;
        const int remainder = range % parts;
        const int start = my_start + part*step + std::min(remainder, part);
        part++;
        const int end = my_start + part*step + std::min(remainder, part);
        #pragma forceinline
        my_func( start, end, part );
    }
public:
    distribution_function ( int s, int e, F& f ) : my_start(s), my_end(e), my_func(f) {}
};
struct mask1 {
    std::atomic<mask_t> start_mask;
};
struct mask2 {
    std::atomic<mask_t> finish_mask;
};
struct work_ {
    std::atomic<mask_t> run_mask;
    distribution_base *func_ptr;
    std::atomic<uintptr_t> epoch;
    volatile int mode; // 0 - stopping, 1 - rebalance, 2 - trapped
};
struct __attribute__((aligned(64))) RapidStart : tbb::detail::padded<mask1>, tbb::detail::padded<mask2>, tbb::detail::padded<work_> {
    std::atomic<uintptr_t> n_tasks;
    tbb::task_group tg;

    friend class TrapperTask;

    void spread_work(distribution_base *f) {
        uintptr_t e = epoch;
        run_mask.store(0U, std::memory_order_relaxed);
        func_ptr = f;
        epoch.store(e+1, std::memory_order_release);
        //tbb::atomic_fence();
        __asm__ __volatile__("lock; addl $0,(%%rsp)":::"memory");
        mask_t mask_snapshot = start_mask.load(std::memory_order_acquire);
        finish_mask.store(0, std::memory_order_relaxed);
        run_mask.store(mask_snapshot|1, std::memory_order_release);
        _clevict(&finish_mask, _MM_HINT_T0);

        f->run(0, mask_snapshot);
        tbb::detail::spin_wait_until_eq(finish_mask, mask_snapshot);
    }

    struct TrapperTask {
        void operator() () const {
            __TBB_ASSERT(slot,0);
            const mask_t bit = mask_t(1)<<slot;
            if(global.mode) {
                global.start_mask.fetch_add(bit);
                uintptr_t e = global.epoch.load(std::memory_order_relaxed);
                mask_t r = global.run_mask.load(std::memory_order_acquire);
                do {
                    if(r&bit) {
                        global.func_ptr->run(slot, r);
                        global.finish_mask.fetch_or(bit);
                        _clevict(&global.finish_mask, _MM_HINT_T1);
                    }
                    tbb::detail::spin_wait_while_eq(global.epoch, e);
                    e = global.epoch;
                    _mm_prefetch((const char*)global.func_ptr, _MM_HINT_T0);
                    tbb::detail::spin_wait_while_eq(global.run_mask, 0U);
                    r = global.run_mask;
                } while( (r&bit)==bit || global.mode == 2);
                //printf("#%d exited r=%lu mode=%d\n", slot, r, global.mode );
                global.start_mask.fetch_add(-bit);
                // are we were late to leave the group
                if( e != global.epoch.load(std::memory_order_relaxed) ) {
                    tbb::detail::spin_wait_while_eq(global.run_mask, 0U);
                    r = global.run_mask;
                    if(r&bit) {
                        global.func_ptr->run(slot, r);
                        global.finish_mask.fetch_or(bit);
                        _clevict(&global.finish_mask, _MM_HINT_T1);
                    }
                }
            }
            if(global.mode == 0)
                global.n_tasks--;
            else
                global.tg.run( TrapperTask(slot, global) );
        }
        RapidStart &global;
        const int slot;
    public:
        TrapperTask(int s, RapidStart &g) : global(g), slot(s) { }
    };

public:
    RapidStart () {
        start_mask = run_mask = finish_mask = 0;
        mode = 2;
    }
    void init ( int maxThreads = MAX_THREADS ) {
        if( maxThreads > MAX_THREADS ) maxThreads = MAX_THREADS;
        n_tasks = maxThreads;
#if 1
        for ( int i = 1; i < maxThreads; ++i )
            tg.run( TrapperTask(i, *this) );
#else
        tbb::task_list  tl;
        for ( int i = 1; i < n_tasks; ++i ) {
            tbb::task &t = *new( tbb::task::allocate_root() ) TrapperTask(i, *this);
            t.set_affinity( tbb::task::affinity_id( (i*maxThreads/n_tasks)+1) );
            tl.push_back( t );
        }
        n_tasks--;
        tbb::task::spawn(tl);
#endif
    }
    ~RapidStart() {
        mode = 0;
        run_mask = 1;
        epoch++;
        //tbb::detail::spin_wait_until_eq(n_tasks, 0U);
        tg.wait();
    }

    template <typename Body>
    void parallel_ranges( int start, int end, const Body &b) {
        distribution_function<const Body> F(start, end, b);
        spread_work(&F);
    }
}; //

}
