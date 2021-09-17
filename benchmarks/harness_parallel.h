
// values for GRAIN macro
#define GS_STATIC  -1
#define GS_2CHUNKS -2
#define GS_3CHUNKS -3
#define GS_4CHUNKS -4
#define GS_5CHUNKS -5
#define GS_6CHUNKS -6
#define GS_7CHUNKS -7
#define GS_8CHUNKS -8
#define GS_16CHUNKS -16
#define GS_32CHUNKS -32
#define GS_64CHUNKS -64
#define GS_OPENCL  -16
#ifndef GS_BEST
#define GS_BEST 1
#endif

// values for PARALLEL macro
#define OMP_STATIC  1
#define OMP_DYNAMIC 2
#define OMP_GUIDED  3
#define OMP_RUNTIME 4
#define OMP_S_STEAL 5
#define OMP_STATIC_STEAL 5
#ifndef OMP_BEST
#define OMP_BEST OMP_STATIC
#endif

#define TBB_SIMPLE 10
#define TBB_AUTO    11
#define TBB_AFFINITY 12
#define TBB_C_AFF 14
#define TBB_CONST_AFFINITY 14
#define TBB_STATIC    15
#define TBB_RAPID   17
#ifndef TBB_BEST
#define TBB_BEST TBB_STATIC
#endif

#define TF_FOR_EACH 30

#ifndef PARALLEL
#define PARALLEL TBB_SIMPLE
#endif

#if PARALLEL < TBB_SIMPLE
#define __USE_OPENMP__ 1
#elif PARALLEL < TF_FOR_EACH
#define __USE_TBB__ 1
#else
#define __USE_TF__ 1
#endif

#include <algorithm>
#include <stdio.h>
#include <atomic>

#include <tbb/info.h>
#if __USE_OPENMP__
#include <omp.h>
#elif __USE_TBB__
#if PARALLEL == TBB_RAPID
    #include "rapid_start.h"
#endif
#include <tbb/parallel_for.h>
#include <tbb/blocked_range.h>
#include <tbb/tbb_allocator.h>
#include <tbb/scalable_allocator.h>
#include <tbb/global_control.h>
#include <tbb/version.h>
//#include "harness_pinner.h"

#include <tbb/task_arena.h>
#ifndef __USE_TASK_ARENA__
#define __USE_TASK_ARENA__ 1
#endif
#ifndef __USE_OBSERVER__
#define __USE_OBSERVER__ 0 // I see no positive changes from using observer now
#endif

#elif __USE_TF__
#include <taskflow/taskflow.hpp>
#endif

#include <sys/syscall.h>

#ifndef __INTEL_COMPILER
#define __forceinline inline
#endif

#define MACRO_STRING_AUX(...) #__VA_ARGS__
#define MACRO_STRING(x) MACRO_STRING_AUX( x )

namespace Harness {

static int nThreads;
#if __USE_TBB__
#if __USE_OBSERVER__
struct LeavingObserver : public tbb::task_scheduler_observer
{
    LeavingObserver() : tbb::task_scheduler_observer(TBB_INTERFACE_VERSION < 7003) {
        printf("Using observer\n"); fflush(0);
        observe(true);
    }
    /*override*/ void on_scheduler_entry(bool isWorker) {
#ifdef LOG_INFO
        if(isWorker) printf("+");
#endif
    }
    /*override*/ void on_scheduler_exit(bool isWorker) {
#ifdef LOG_INFO
        if(isWorker) printf("-");
#endif
    }
};
static __thread LeavingObserver * g_observer;
#endif //__USE_OBSERVER__
static std::atomic<int> g_globalRefCounter;
static __thread int g_localRefCounter = 0;
static __thread tbb::global_control * g_tbbConfig = NULL;
#if PARALLEL == TBB_RAPID
static Harness::RapidStart g_rs;
#undef __USE_TASK_ARENA__
#elif __USE_TASK_ARENA__
static tbb::task_arena* g_globalArena = NULL;
#endif
#endif //__USE_TBB__

#if __USE_TF__
tf::Executor executor;
tf::Taskflow taskflow;
#endif // __USE_TF__

static int InitParallel(int n = 0)
{
    nThreads = n? n : ::tbb::info::default_concurrency(); //tbb::this_task_arena::max_concurrency();
#if __USE_TBB__
    if(TBB_INTERFACE_VERSION != TBB_runtime_interface_version()) {
        fprintf(stderr, "ERROR: Compiled with TBB interface version " __TBB_STRING(TBB_INTERFACE_VERSION) " while runtime provides %d\n",
            TBB_runtime_interface_version());
        fflush(stderr);
        exit(-2);
    }
    if(tbb::tbb_allocator<int>::allocator_type() != tbb::tbb_allocator<int>::scalable) {
        fprintf(stderr, "ERROR: Scalable allocator library must be loaded.\n");
        fflush(stderr);
        exit(-2);
    }
#ifdef LOG_INFO
    setenv("TBB_VERSION", "1", 1);
#endif
    setenv("TBB_MALLOC_USE_HUGE_PAGES", "1", 1);
    scalable_allocation_mode(USE_HUGE_PAGES, 1);

    printf("Setting %d threads for TBB\n", nThreads); fflush(0);

    if(!g_localRefCounter++) {
        __TBB_ASSERT(!g_tbbConfig,0);
        //Harness::LimitNumberOfThreads(n, (n+MIC_CORES-1)/MIC_CORES);
        g_tbbConfig = new tbb::global_control(tbb::global_control::max_allowed_parallelism, nThreads);
        if(!g_globalRefCounter++) {
#if PARALLEL == TBB_RAPID
            //Harness::PinTbbThreads( nThreads );
            g_rs.init(nThreads);
#elif __USE_TASK_ARENA__
            __TBB_ASSERT(!g_globalArena,0);
    #ifdef LOG_INFO
            printf("Using TASK_ARENA(explicit) with %d threads\n", nThreads); fflush(0);
    #endif
            g_globalArena = new tbb::task_arena(nThreads, 1);
            g_globalArena->execute( [&]{
                //Harness::PinTbbThreads( nThreads );
#if __USE_OBSERVER__
                g_observer = new LeavingObserver;
#endif
            });
#else
            Harness::PinTbbThreads( nThreads );
#if __USE_OBSERVER__
            g_observer = new LeavingObserver;
#endif
#endif
        }
    }

#elif __USE_OPENMP__ // OpenMP

#if PARALLEL == OMP_S_STEAL
    setenv("OMP_SCHEDULE", "static_steal", 1);
#endif
#ifdef KMP_AFFINITY
#ifdef LOG_INFO
    puts( "KMP_AFFINITY=" MACRO_STRING(KMP_AFFINITY) );
#endif
    setenv("KMP_AFFINITY", MACRO_STRING(KMP_AFFINITY), 1);
#else
    setenv("KMP_AFFINITY", "granularity=fine,balanced", 1);
#endif
#ifdef KMP_BLOCKTIME
#ifdef LOG_INFO
    puts( "KMP_BLOCKTIME=" MACRO_STRING(KMP_BLOCKTIME) );
#endif
    setenv("KMP_BLOCKTIME", MACRO_STRING(KMP_BLOCKTIME), 1);
#else
    setenv("KMP_BLOCKTIME", "infinite", 1); // no sleeps
#endif
//    setenv("KMP_LIBRARY", "turnaround", 1); // disables yields
#ifdef LOG_INFO
    setenv("KMP_VERSION", "1", 1);
//    setenv("KMP_D_DEBUG", "7", 1);
#endif
    //nThreads = n? n : omp_get_max_threads();

    // configure OMP environment
    printf("Setting %d threads for OMP\n", nThreads); fflush(0);
    omp_set_num_threads(nThreads);

    // Warm up OMP workers
    #pragma omp parallel for
    for(int j=0; j<nThreads; ++j)
    {
#if 0
        cpu_set_t target_mask;
        CPU_ZERO(&target_mask);
        sched_getaffinity(0, sizeof(target_mask), &target_mask);
        char temp[1024];
        for(int i=0; i<248/8; ++i)
        {
            sprintf(temp+2*i, "%02X",(int)(((char*)&target_mask)[(248/8)-i-1])&0xFF);
        }
        printf("Pipeline thread, worker = %d, tid=%x, %s\n", j, (int)syscall(SYS_gettid), temp);
        fflush(0);
#endif
    }
#elif __USE_TF__
    //nThreads = n? n : std::thread::hardware_concurrency();
    
    printf("Setting %d threads for TaskFlow\n", nThreads); fflush(0);
    taskflow.for_each_index(0, nThreads, 1, [](int _){});
    executor.run(taskflow).get();
#endif
    return nThreads;
}

static void DestroyParallel() {

#ifdef __USE_TBB__
    if ( !--g_localRefCounter )
    {
#if __USE_OBSERVER__
        g_observer->observe(false);
        delete g_observer;
#endif
        //__TBB_ASSERT(g_tbbConfig, 0);
        // destroy all TBB threads
        delete g_tbbConfig;
        g_tbbConfig = 0;
        if( !--g_globalRefCounter ) {
#if __USE_TASK_ARENA__
            delete g_globalArena;
#endif
#ifdef LOG_INFO
            printf("Shutting down TBB global scheduler\n");
            fflush(0);
#endif
        }
    }
#endif
}

template<typename Body>
struct executive_range_body {
    executive_range_body(const Body &b) : my_func(b) {}
    const Body &my_func;
#if __USE_TBB__
    template<typename Iter>
    __forceinline void operator()(const tbb::blocked_range<Iter> &r) const {
        operator()( r.begin(), r.end(), r.grainsize() );
    }
#endif
    template<typename Iter>
    __forceinline void operator()(Iter s, Iter e, int info=-1) const {
#if __INTEL_COMPILER
        #pragma ivdep
#endif
        for( Iter i = s; i < e; i++)
            operator()( i );
    }
    template<typename Iter>
    __forceinline void operator()(Iter i) const {
#ifdef __KERNEL_FORCEINLINE
            #pragma forceinline
#elif __INTEL_COMPILER
            #pragma noinline
#endif
        my_func( i );
    }
};

#ifdef GRAIN
    #if GRAIN > 0 // GRAIN is specified as absolute grain-size
        #define GS(n) std::max(g, Iter(GRAIN))
    #elif GRAIN < 0 // GRAIN by modulo is number of chunks per thread. Variables are defined in the function below
        #define GS(n) std::max(g, ((e-s)/(n*(- GRAIN ))) )
    #endif
#else
    #define GS(n) g
#endif

template <typename Iter, typename Body>
void parallel_for( Iter s, Iter e, Iter g, const Body &b) {
#ifdef LOG_INFO
    if( sizeof(Body) >= 256 ) {
        static bool printed = false;
        if( !printed ) {
            printf("The task Body size is too big: %lu\n", sizeof(Body)); fflush(0);
            printed = true;
        }
    }
#endif
#if GRAIN < 0
    const int per_thread = - GRAIN;
#elif GRAIN > 0
    const int per_thread = std::min(32, int(e-s)/GRAIN );
#else
    const int per_thread = std::min(32, int((e-s)/g) );
#endif
    executive_range_body<Body> executive_range(b);
#ifdef LOG_RANGES
    printf("Parallel for [%d,%d):%d, thread id = %x\n", int(s), int(e), int(GS(nThreads)),  (int)syscall(SYS_gettid));
#endif
#if __USE_OPENMP__

    g = GS(nThreads);
    #pragma omp parallel
    {
#if PARALLEL==OMP_STATIC
        #pragma omp for nowait schedule(static)
#elif PARALLEL==OMP_DYNAMIC
        #pragma omp for nowait schedule(dynamic, g)
#elif PARALLEL==OMP_GUIDED
        #pragma omp for nowait schedule(guided, g)
#elif PARALLEL==OMP_RUNTIME || PARALLEL == OMP_S_STEAL
        #pragma omp for nowait schedule(runtime)
#else
        #error Wrong PARALLEL mode
#endif
#if __INTEL_COMPILER
        #pragma ivdep
#endif
        for(int i = s; i < e; i++)
            executive_range( i );
    }
#elif PARALLEL == CILK_SIMPLE

#if __INTEL_COMPILER
    #pragma ivdep
#endif
    cilk_for(Iter i = s; i < e; i++)
        executive_range( i );

#elif PARALLEL == TBB_RAPID
    g_rs.parallel_ranges(s, e, executive_range);

#elif PARALLEL == TF_FOR_EACH
    taskflow.for_each_index(s, e, 1, executive_range);
    executor.run(taskflow).get();

#else // other TBB parallel_fors
//  implied:  static tbb::task_group_context context(tbb::task_group_context::isolated, tbb::task_group_context::default_traits);
    static tbb::task_group_context context(tbb::task_group_context::bound, tbb::task_group_context::default_traits | tbb::task_group_context::concurrent_wait);

#if __USE_TASK_ARENA__
    g_globalArena->execute( [&]{
#endif

#if PARALLEL == TBB_STATIC
    Harness::static_parallel_ranges(s, e, executive_range, nThreads);

#elif PARALLEL == TBB_NESTED
    Harness::static_parallel_ranges(s, e, [per_thread,executive_range](Iter s, Iter e, int p)
        {
            tscg_task_start( out );
            tscg_task_data( data, "%d + %d #%d", int(s), int(e-s), p );
            Iter m = s, per = std::min(e-s, Iter(per_thread));
            if( per_thread > 1 ) {
                m += (e-s)/per--;
                executive_range(s, m, p);
            }
            tbb::parallel_for(tbb::blocked_range<Iter>(m, e, (e-m)/per*2-1 ), executive_range, tbb::simple_partitioner(), context);
            tscg_task_stop( out, data );
        }, nThreads);

#else // regular partitioners

#if PARALLEL==TBB_SIMPLE
    const tbb::simple_partitioner part;
#elif PARALLEL==TBB_AUTO
    const tbb::auto_partitioner part;
#elif PARALLEL==TBB_AFFINITY
    static tbb::affinity_partitioner part;
#elif PARALLEL==TBB_CONST_AFFINITY
    tbb::affinity_partitioner part;
#elif PARALLEL==TBB_OPENCL || PARALLEL==TBB_UNEVEN
    const tbb::opencl_partitioner part;
#else
    #error Wrong PARALLEL mode
#endif
    tbb::parallel_for(tbb::blocked_range<Iter>(s,e,GS(nThreads)*2-1), executive_range, part, context);
#endif /* partitioners */

#if __USE_TASK_ARENA__
    });
#endif

#endif /*outermost*/
}

}
