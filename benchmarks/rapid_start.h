/*
    Copyright 2005-2017 Intel Corporation.  All Rights Reserved.

    The source code contained or described herein and all documents related
    to the source code ("Material") are owned by Intel Corporation or its
    suppliers or licensors.  Title to the Material remains with Intel
    Corporation or its suppliers and licensors.  The Material is protected
    by worldwide copyright laws and treaty provisions.  No part of the
    Material may be used, copied, reproduced, modified, published, uploaded,
    posted, transmitted, distributed, or disclosed in any way without
    Intel's prior express written permission.

    No license under any patent, copyright, trade secret or other
    intellectual property right is granted to or conferred upon you by
    disclosure or delivery of the Materials, either expressly, by
    implication, inducement, estoppel or otherwise.  Any license under such
    intellectual property rights must be express and approved by Intel in
    writing.
*/

#pragma once
#include <algorithm>
#include <atomic>
#include <tbb/task.h>

typedef uintptr_t mask_t;

namespace Harness {
const size_t MAX_THREADS = sizeof(mask_t)*8;

class distribution_base : tbb::internal::no_assign {
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
        #pragma ivdep
        #pragma forceinline
        my_func( start, end, part );
    }
public:
    distribution_function ( int s, int e, F& f ) : my_start(s), my_end(e), my_func(f) {}
};
struct mask1 {
    tbb::atomic<mask_t> start_mask;
};
struct mask2 {
    tbb::atomic<mask_t> finish_mask;
};
struct work_ {
    tbb::atomic<mask_t> run_mask;
    distribution_base *func_ptr;
    tbb::atomic<uintptr_t> epoch;
    volatile int mode; // 0 - stopping, 1 - rebalance, 2 - trapped
};
struct __attribute__((aligned(64))) RapidStart : tbb::internal::padded<mask1>, tbb::internal::padded<mask2>, tbb::internal::padded<work_> {
    tbb::atomic<uintptr_t> n_tasks;

    friend class TrapperTask;

    void spread_work(distribution_base *f) {
        uintptr_t e = epoch;
        run_mask.store<tbb::relaxed>(0U);
        func_ptr = f;
        epoch.store<tbb::release>(e+1);
        //tbb::atomic_fence();
        __asm__ __volatile__("lock; addl $0,(%%rsp)":::"memory");
        mask_t mask_snapshot = start_mask.load<tbb::acquire>();
        finish_mask.store<tbb::relaxed>(0);
        run_mask.store<tbb::release>(mask_snapshot|1);
        _mm_clevict(&finish_mask, _MM_HINT_T0);
        f->run(0, mask_snapshot);
        tbb::internal::spin_wait_until_eq(finish_mask, mask_snapshot);
    }

    class TrapperTask : public tbb::task {
        tbb::task* execute () {
            __TBB_ASSERT(slot,0);
            const mask_t bit = mask_t(1)<<slot;
            if(global.mode) {
                global.start_mask.fetch_and_add(bit);
                uintptr_t e = global.epoch.load<tbb::relaxed>();
                mask_t r = global.run_mask.load<tbb::acquire>();
                do {
                    if(r&bit) {
                        global.func_ptr->run(slot, r);
                        __TBB_AtomicOR(&global.finish_mask, bit);
                        _mm_clevict(&global.finish_mask, _MM_HINT_T1);
                    }
                    tbb::internal::spin_wait_while_eq(global.epoch, e);
                    e = global.epoch;
                    _mm_prefetch((const char*)global.func_ptr, _MM_HINT_T0);
                    tbb::internal::spin_wait_while_eq(global.run_mask, 0U);
                    r = global.run_mask;
                } while( (r&bit)==bit || global.mode == 2);
                //printf("#%d exited r=%lu mode=%d\n", slot, r, global.mode );
                global.start_mask.fetch_and_add(-bit);
                // are we were late to leave the group
                if( e != global.epoch.load<tbb::relaxed>() ) {
                    tbb::internal::spin_wait_while_eq(global.run_mask, 0U);
                    r = global.run_mask;
                    if(r&bit) {
                        global.func_ptr->run(slot, r);
                        __TBB_AtomicOR(&global.finish_mask, bit);
                        _mm_clevict(&global.finish_mask, _MM_HINT_T1);
                    }
                }
            }
            if(global.mode == 0)
                global.n_tasks--;
            else
                tbb::task::enqueue( *new(allocate_root()) TrapperTask(slot, global) );
            return NULL;
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
        n_tasks = maxThreads;
        if( n_tasks > 60/*MAX_THREADS*/ ) n_tasks = 60/*MAX_THREADS*/;
#if 0
        for ( int i = 1; i < maxThreads; ++i )
            tbb::task::enqueue( *new(tbb::task::allocate_root()) TrapperTask(i, *this) );
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
        tbb::internal::spin_wait_until_eq(n_tasks, 0U);
    }

    template <typename Body>
    void parallel_ranges( int start, int end, const Body &b) {
        distribution_function<const Body> F(start, end, b);
        spread_work(&F);
    }
}; //

}
