#ifndef __OOX_H__
#define __OOX_H__

#include <utility>
#include <functional>
#include <type_traits>
#include <limits>
#include <atomic>

#if HAVE_TBB
#define TBB_USE_ASSERT 0
#include <oneapi/tbb/detail/_task.h>
#include <oneapi/tbb/task_group.h>
#elif HAVE_TF
#include <taskflow/taskflow.hpp>
#else
#include <future>
#endif

#ifndef __OOX_TRACE
#define __OOX_TRACE(...)
#endif
#ifndef __OOX_ASSERT
#include <cassert>
#define __OOX_ASSERT(a, b) assert(a), b
#define __OOX_ASSERT_EX(a, b) __OOX_ASSERT(a, b)
#endif

#define __OOX_AUTO_TYPE_FUNC(expr) ->decltype(expr) { return expr; }

namespace oox {

#if OOX_SERIAL_DEBUG //////////////////// Immediate execution //////////////////////////////////
class oox_node {
    oox_node &operator=(const oox_node &) = delete;
};

template<typename T>
struct var : public oox_node {
    static_assert(std::is_same<T, typename std::decay<T>::type>::value,
                  "Specialize oox::var only by plain types."
                  "For references, use reference_wrapper,"
                  "for const types use shared_ptr<T>.");
    T my_value;
    var() : my_value() {}
    var(const T& t) noexcept : my_value( t ) {}
    var(T&& t)      noexcept : my_value( std::move(t) ) { }
    var(var<T>&& t) : my_value( std::move(t.my_value) ) { }
    var& operator=(var<T>&& t) { my_value = std::move(t.my_value); return *this; }
};

template<>
struct var<void> : public oox_node {
    var() {}
};

template< typename T > // create temporary copies to simulate parallel implementation
typename std::decay<T>::type unoox(T&& t) { return typename std::decay<T>::type(std::forward<T>(t)); }

template< typename T >
const T& unoox(const var<T>& t) { return t.my_value; }

template< typename T >
T& unoox(var<T>& t) { return t.my_value; }

template< typename T >
T&& unoox(var<T>&& t) { return std::move(t.my_value); }

template< typename T >
struct gen_oox {
    typedef var<typename std::decay<T>::type> type;
    template< typename F, typename... Args >
    static type run(F&& f, Args&&... args) { return type(std::forward<F>(f)(std::forward<Args>(args)...)); }
};
template< typename VT >
struct gen_oox<var<VT> > {
    typedef var<VT> type;
    template< typename F, typename... Args >
    static type run(F&& f, Args&&... args) { return std::forward<F>(f)(std::forward<Args>(args)...); }
};
template<>
struct gen_oox<void> {
    typedef oox_node type;
    template< typename F, typename... Args >
    static type run(F&& f, Args&&... args) { std::forward<F>(f)(std::forward<Args>(args)...); return oox_node(); }
};
template< typename T> using var_type = typename gen_oox<T>::type;

template< typename F, typename... Args >
auto run(F&& f, Args&&... args)->var_type<decltype(f(unoox(std::forward<Args>(args))...))>
{
    return gen_oox<decltype(f(unoox(std::forward<Args>(args))...))>
    ::run(std::forward<F>(f), unoox(std::forward<Args>(args))...);
}

template<typename T>
T wait_and_get(const var<T> &ov) { return ov.my_value; }

void wait_for_all(oox_node &) {}

#else ///////////////////////////////// Parallel execution  ///////////////////////////////////

namespace internal {

struct task_life {
    // Pointers to this structure and live output nodes
    std::atomic<int> life_count;
    virtual ~task_life() {}

    void life_set_count(int lifetime) {
        life_count.store(lifetime, std::memory_order_release);
    }

    int  life_get_count() {
        return life_count.load(std::memory_order_acquire);
    }

    bool life_release( int n ) {
        if(life_count.load(std::memory_order_acquire) == n) {
            __OOX_TRACE("%p release all: %d", this, n);
            return true;
        }
        else {
            int k = life_count-=n;
            __OOX_TRACE("%p release: %d", this, k);
            __OOX_ASSERT(k >= 0, "invalid life_count detected while removing prerequisite");
            return (k == 0);          // double-check after atomic
        }
    }
};

#if HAVE_TBB ///////////////////////// TBB ////////////////////////////////////////
#define OOX_USING_TBB
using tbb::detail::d1::execution_data;
using tbb_task = tbb::detail::d1::task;
using tbb::detail::d1::small_object_allocator;
static tbb::task_group_context tbb_context;
#define TASK_EXECUTE_METHOD tbb_task* execute(execution_data&) override

struct task : public tbb_task, task_life {
    tbb::detail::d1::wait_context waiter{1};
#ifndef OOX_USE_STDMALLOC
    small_object_allocator alloc{};
#endif
#if TBB_USE_ASSERT
    std::atomic<bool> is_spawned{false};
    virtual ~task() {
        if(!is_spawned.load(std::memory_order_acquire);)
            waiter.release();
    }
#else
    virtual ~task() {}
#endif

    TASK_EXECUTE_METHOD {
        __OOX_ASSERT(false, "");
        return nullptr;
    }
    virtual tbb_task* cancel(execution_data& ed) override {
        __OOX_ASSERT(false, "");
        return nullptr;
    }
    void release( int n = 1 ) {
        if(life_release(n)) {
#if OOX_USE_STDMALLOC
            delete this;
#else
            this->~task();
            alloc.deallocate(this);
#endif
        }
    }
    template<typename T, typename... Args>
    static T* allocate(Args && ... args) {
#if OOX_USE_STDMALLOC
        return new T(std::forward<Args>(args)...);
#else
        small_object_allocator a{};
        auto *t = a.new_object<T>(std::forward<Args>(args)...);
        t->alloc = a; // store deallocation info
        return t;
#endif
    }
    void spawn() {
#if TBB_USE_ASSERT
        is_spawned.store(true, std::memory_order_release);
#endif
        tbb::detail::d1::spawn(*this, tbb_context);
    }
    void wait() {
        __OOX_ASSERT(life_get_count(), "");
        tbb::detail::d1::wait(waiter, tbb_context);
    }
    void wakeup() {
        waiter.release();
    }
};
#elif HAVE_TF /////////////////////// Taskflow ///////////////////////////////////////
#define OOX_USING_TF
#define TASK_EXECUTE_METHOD void* execute() override

tf::Executor tf_pool; // TODO :)

struct task : task_life {
    
    std::promise<void> waiter;

    virtual ~task() {}
    virtual void* execute() = 0;

    void release( int n = 1 ) {
        if(life_release(n))
            delete this;
    }
    template<typename T, typename... Args>
    static T* allocate(Args && ... args) {
        return new T(std::forward<Args>(args)...);
    }
    void spawn() {
        tf_pool.silent_async([this]{this->execute();});
    }
    void wait() {
        waiter.get_future().wait();
    }
    void wakeup() {
        waiter.set_value();
    }
};

#else /////////////////////////////// plain STD impl /////////////////////////////////
#define OOX_USING_STD
#define TASK_EXECUTE_METHOD void* execute() override

struct task : task_life {
    std::promise<void> waiter;

    virtual ~task() {}
    virtual void* execute() = 0;

    void release( int n = 1 ) {
        if(life_release(n))
            delete this;
    }
    template<typename T, typename... Args>
    static T* allocate(Args && ... args) {
        return new T(std::forward<Args>(args)...);
    }
    void spawn() {
        std::async(std::launch::async, &task::execute, this);
    }
    void wait() {
        waiter.get_future().wait();
    }
    void wakeup() {
        waiter.set_value();
    }
};
#endif // HAVE_TBB,TF /////////////////////////////////////////////////////////////////////

struct task_node;
struct oox_var_base;

struct output_node {
    // 0 if next writer is not known yet.
    // 1 if next writer is not known yet, but value is available and countdown includes extra one
    // 3 if next writer is end without var ownership
    // ptr|1 if next writer is end with var ownership, ptr points to var storage.
    // Otherwise points to next node that overwrites the value written by this node.
    std::atomic<task_node*> next_writer;
    std::atomic<int> countdown;
    output_node() {
        next_writer.store(nullptr, std::memory_order_relaxed);
        countdown.store(1, std::memory_order_relaxed);
    }
};

struct arc {
    // types of task relations beside output dependence
    enum kinds : char {
        flow_only,    //< notify consumer when producer is completed
        back_only,    //< notify producer when consumer is completed TODO: unnecessary when stored in task directly
        flow_back,    //< flow_only then back_only
        flow_copy,    //< call consumer to copy its value when producer is completed
        forward_copy  //< copy a pointer to the var storage found by producer to consumer
    };
    typedef short int port_int;
    arc*       next;
    task_node* node;
    port_int   port;
    kinds      kind;
    arc( task_node* n, int p, kinds k = flow_back ) : node(n), port(port_int(p)), kind(k) {}
};

struct arc_list {
    // Root of list of nodes that are waiting for this node's value to be produced.
    // A node can be waiting for *this to produce a value OR waiting for *this to consume its value.
    // Special value 1 means no need to wait (e.g. value has been produced).
    std::atomic<arc*> head;
    // Add i to arc_list.
    // Return true if success, false otherwise.
    bool add_arc( arc* i );
    arc_list() { head.store(nullptr, std::memory_order_relaxed); }
};

struct task_node : public task, arc_list {
    // Prerequisites to start the task
    std::atomic<int> start_count;
    // TODO: exception storage here?

    task_node() { } // prepare the task for waiting on it directly
    virtual ~task_node() {}

    // Result output node
    inline output_node& out(int n) const;
    // Add a prerequisite
    int  assign_prerequisite( task_node *n, int req_port );
    // Process flow- and anti-dependence arcs
    void do_notify_arcs( arc* r, int *count );
    // Process output dependence
    int  do_notify_out( int port, int count );
    // Process flow and output arcs. Returns number of finished output nodes
    int  notify_successors( int output_slots, int *counters );
    // Process flow- and anti-dependence arcs. Returns number of finished output nodes
    int  forward_successors( int output_slots, int *counters, oox_var_base& );
    // Account for completion of n prerequisites
    void remove_prerequisite( int n=1 );
    // Process next writer notification
    int  notify_next_writer( task_node* d );
    // Account for removal of a back_arc
    int  remove_back_arc( int output_port, int n=1 );
    // Set new output dependence
    void set_next_writer( int output_port, task_node* n );
    // Call base notify successors
    template<int slots>
    void notify_successors();
    // Call base forward successors
    template<int slots>
    void forward_successors( oox_var_base& );

    // It is called when producer is done and notifies consumers to copy the value
    virtual void on_ready(int) { __OOX_ASSERT(false, "not implemented"); }
};

bool arc_list::add_arc( arc* i ) {
    __OOX_ASSERT( uintptr_t(i->node)>2, "" );
    for(;;) {
        arc* j = head.load(std::memory_order_acquire);
        if( j==(arc*)uintptr_t(1) )
            return false;
        i->next = j;
        if( head.compare_exchange_weak( j, i ) ) // TODO: weak or strong? what's perf?
            return true;
    }
}

int task_node::assign_prerequisite( task_node *n, int req_port ) {
    arc* j = new arc( this, req_port ); // TODO: embed into the task
    __OOX_ASSERT_EX(j && n, "");
    if( n->add_arc(j) ) {
        __OOX_TRACE("%p assign_prerequisite: assigned to %p, %d",this,n,req_port);
        return 1; // Prerequisite n will decrement start_count when it produces a value
    } else {
        // Prerequisite n already produced a value. Add this as a consumer of n.
        int k = ++n->out(req_port).countdown;
        __OOX_TRACE("%p assign_prerequisite: preventing %p, port %d, count %d",this,n,req_port,k);
        __OOX_ASSERT_EX(k>1,"risk that a prerequisite might be prematurely destroyed");
        j->node = n;
        j->kind = arc::back_only;
        bool success = add_arc(j); //TODO: add_arc_unsafe?
        __OOX_ASSERT_EX(success, "");
    }
    return 0;
}

void task_node::do_notify_arcs( arc* r, int *count ) {
    // Notify successors that value is available
    do {
        arc* j = r;
        r = j->next;
        task_node* n = j->node;
        if( j->kind == arc::back_only ) {
            // Notify producer that this task has finished consuming its value
            __OOX_TRACE("%p notify: %p->remove_back_arc(%d)",this,n,j->port);
            if( int k = n->remove_back_arc( j->port ) )
                n->release( k );
            delete j;
        } else {
            if( j->kind == arc::flow_back ) {
                // "n" is task that consumes value that this task produced.
                // Add back arc so that "n" can notify this when it is done consuming the value.
                j->node = this;
                j->kind = arc::back_only;
                if( out(j->port).next_writer.load(std::memory_order_acquire) != (task_node*)uintptr_t(3) ) {
                    bool b = n->add_arc( j );
                    __OOX_ASSERT_EX(b, "corrupted?");
                    --count[j->port];
                } else delete j; // very unlikely?
            } else if( j->kind == arc::flow_copy )
                n->on_ready( j->port );
            else if( j->kind == arc::forward_copy )
                __OOX_ASSERT(false, "incorrect forwarding"); // has to be processed by forward_successors only
            // Let "n" know that prerequisite "this" is ready.
            __OOX_TRACE("%p notify: %p->remove_prequisite()",this,n);
            n->remove_prerequisite();
        }
    } while( r );
}

int task_node::do_notify_out( int port, int count ) {
    task_node* null = nullptr;
    if( out(port).next_writer.load(std::memory_order_acquire)==nullptr
        && out(port).next_writer.compare_exchange_strong( null, (task_node*)uintptr_t(1)) ) {
        // The thread that installs the non-nullptr "next_writer" will see the 1 and do the decrement.
        --count;
        __OOX_TRACE("%p notify out %d: next_writer went from 0 to 1",this,port);
    } else if( !(uintptr_t((void*)out(port).next_writer.load(std::memory_order_acquire))&1) ) {
#if OOX_AFFINITY
        task_node* d = out(port).next_writer;
        d->affinity = a;
#endif /* OOX_AFFINITY */
        __OOX_TRACE("%p notify out %d: next_writer is %p\n",this,port,out(port).next_writer.load(std::memory_order_acquire));
    } else {
        __OOX_TRACE("%p notify out %d: next_writer is final: %p\n",this,port,out(port).next_writer.load(std::memory_order_acquire));
    }
    return remove_back_arc( port, count );
}

int task_node::notify_successors( int output_slots, int *count ) {
    for( int i = 0; i <  output_slots; i++ ) {
        // it should be safe to assign countdowns here because no successors were notified yet
        out(i).countdown.store( count[i] = std::numeric_limits<int>::max()/2, std::memory_order_release );
    }
    __OOX_TRACE("%p notify successors",this);
    // Grab list of successors and mark as competed.
    // Note that countdowns can change asynchronously after this point
    if( arc* r = head.exchange( (arc*)uintptr_t(1) ) )
        do_notify_arcs( r, count );
    int refs = 0;
    for( int i = 0; i <  output_slots; i++ )
        refs += do_notify_out( i, count[i] );
    __OOX_ASSERT(refs>=0, "");
    return refs;
}

void task_node::remove_prerequisite( int n ) {
    int k = start_count-=n;
    __OOX_ASSERT(k>=0,"invalid start_count detected while removing prerequisite");
    if( k==0 ) {
        __OOX_TRACE("%p remove_prerequisite: spawning",this);
        spawn();
    }
}

int task_node::notify_next_writer( task_node* d ) {
    uintptr_t i = (uintptr_t)d;
    if( i&1 ) {
        if( i == 3 )
            return 1;
        __OOX_ASSERT( i!=1, "remove_back_arc called on output node with next_writer=1" );
        d = (task_node*)(i&~1);
        if( d == this )
            return 2;
        d->release();
    } else {
        __OOX_ASSERT( d!=nullptr, "remove_back_arc called on output node with next_writer==0" );
        d->remove_prerequisite();
    }
    return 1; // the last, release the node
}

int task_node::remove_back_arc( int output_port, int n ) {
    int k = out(output_port).countdown -= n;
    __OOX_ASSERT(k>=0,"invalid countdown detected while removing back_arc");
    __OOX_TRACE("%p remove_back_arc port %d: %d (next_writer is %p)",this,output_port,k,out(output_port).next_writer.load(std::memory_order_acquire));
    if( k==0 ) {
        // Next writer was waiting on all consumers of me to finish.
        return notify_next_writer( out(output_port).next_writer.load(std::memory_order_acquire) );
    }
    return 0;
}

void task_node::set_next_writer( int output_port, task_node* d ) {
    __OOX_ASSERT( uintptr_t(d)!=1, "" );
    task_node* o = out(output_port).next_writer.exchange(d);
    __OOX_TRACE("%p set_next_writer(%d, %p): next_writer was %p",this,output_port,d,o);
    if( o ) {
        if( uintptr_t(o)==1 ) {
            // this has value and conceptual back_arc from its owning oox that was removed.
            if( int k = remove_back_arc( output_port ) ) // TODO: optimize it for set_next_writer without contention
                release( k );
        } else {
            __OOX_ASSERT( uintptr_t(o)==3, "" );
            __OOX_ASSERT( uintptr_t(d)==3, "TODO forward_successors" ); // TODO
        }
    }
}

template<int slots>
void task_node::notify_successors() {
    int counters[slots];
    int n = notify_successors( slots, counters );
    wakeup();
    release(n);
}

template<int slots>
struct task_node_slots : task_node {
    output_node output_nodes[slots];
};

#if defined(__clang__)
__attribute__((no_sanitize("undefined")))
#endif
output_node& task_node::out(int n) const {
    typedef task_node_slots<1024> self_t;
    auto self = const_cast<self_t*>(reinterpret_cast<const self_t*>(this));
    return self->output_nodes[n];
}

template<int slots, typename T>
struct alignas(64) storage_task : task_node_slots<slots> {
    T my_precious;
    TASK_EXECUTE_METHOD { __OOX_ASSERT(false, "not runnable"); return nullptr; }
    storage_task() = default;
    storage_task(T&& t) : my_precious(std::move(t)) {}
    storage_task(const T& t) : my_precious(t) {}
};

struct oox_var_base {
    //TODO: make it a class with private members
    oox_var_base &operator=(const oox_var_base &) = delete;

    template< typename T > friend struct gen_oox;
    task_node*  current_task = nullptr;
    void*       storage_ptr;
    int         storage_offset; // task_node* original = ptr - offset
    short int   current_port = 0; // the problem can arise from concurrent accesses to oox_var, TODO: check
    bool        is_forward = false;  // indicate if it refers to another oox::var recursively

    void set_next_writer( int output_port, task_node* d ) {
        __OOX_ASSERT(current_task, "empty oox::var");
        current_task->set_next_writer( current_port, d );
        current_task = d, current_port = output_port;
    }
    void bind_to( task_node * t, void* ptr, int lifetime, bool fwd = false ) {
        current_task = t, current_port = 0, storage_ptr = ptr, is_forward = fwd;
        storage_offset = uintptr_t(storage_ptr) - uintptr_t(current_task);
        t->life_set_count(lifetime);
        __OOX_TRACE("%p bind: store=%p life=%d fwd=%d",t,ptr,lifetime,fwd);
    }
    void wait() {
        __OOX_ASSERT_EX(current_task, "wait for empty oox::var");
        current_task->wait();
    }
    void release() {
        if( current_task ) {
            current_task->set_next_writer( current_port, (task_node*)uintptr_t(
                    storage_offset? ((uintptr_t(storage_ptr)-storage_offset)|1) : 3/*var<void>*/) );
            current_task = nullptr;
        }
    }
    ~oox_var_base() { release(); }
};

#if 0
int task_node::forward_successors( int output_slots, int *count, oox_var_base& n ) {
    for( int i = 0; i <  output_slots; i++ ) {
        // it is safe to assign countdowns here because no successors were notified yet
        out(i).countdown.store(count[i] = std::numeric_limits<int>::max()/2, std::memory_order_release);
    }
    arc* r = head.exchange( (arc*)uintptr_t(1) ); // mark it completed
    task_node* d = out(0).next_writer.exchange( (internal::task_node*)uintptr_t(3) ); // finish this node
    int refs = 1;
    __OOX_TRACE("%p forward_successors(%p, %d): arcs=%p next_writer=%p",this,n.current_task,n.current_port,r,d);
    if( r ) {
        arc* l = n.current_task->head.exchange( r ); // forward dependencies
        if( l ) {
            __OOX_TRACE("%p forward_successors(%p, %d): notify arcs myself %p",this,n.current_task,n.current_port,l);
            __OOX_ASSERT( uintptr_t(l)==1, "arc lists merge is not implemented" ); // TODO
            __OOX_ASSERT(!n.is_forward, "not implemented"); // TODO
            do_notify_arcs( r, count );
        }
    }
    if( d ) { // TODO: can be converted as another arc type instead of working with outputs?
        task_node* o = n.current_task->out(n.current_port).next_writer.exchange( d );
        if( o ) { // next node is ready already
            __OOX_TRACE("%p forward_successors(%p, %d): removing back arc myself %p",this,n.current_task,n.current_port,o);
            __OOX_ASSERT( uintptr_t(o)==1, "" );
            __OOX_ASSERT(!n.is_forward, "not implemented"); // TODO
            __OOX_ASSERT(out(0).countdown == count[0], "not implemented"); // TODO?
            notify_next_writer( d );
        }
    }
    //n.current_task = nullptr;
    // now we have next writer to be processed here
    for( int i = 1; i <  output_slots; i++ )
        refs += do_notify_out( i, count[i] );
    __OOX_ASSERT(refs>=0, "");
    return refs;
}

template<int slots>
tbb::task* task_node::forward_successors( oox_var_base& m ) {
    int counters[slots];
    int k, n = forward_successors( slots, counters, m );
    if( life_count.load(std::memory_order_aquire) != n && (k = (life_count -= n)) > 0 ) {
        __OOX_ASSERT(k>=0,"invalid life_count detected while forwarding prerequisites");
        recycle_as_safe_continuation(); // do not destroy the task after execution and decrement parent().ref_count()
        set_parent(this);   // and decrement this->ref_count() after completion to enable direct waits on this task
    } else set_ref_count(0);
    return nullptr;
}
#endif

template< typename T > struct gen_oox;

} // namespace internal


template< typename T >
class var : public internal::oox_var_base {
    static_assert(std::is_same<T, typename std::decay<T>::type>::value,
                  "Specialize oox::var only by plain types and pointers."
                  "For references, use reference_wrapper,"
                  "for const types use shared_ptr<T>.");

    void* allocate_new() noexcept {
        auto *v = internal::task::allocate<internal::storage_task<1, typename std::aligned_storage<sizeof(T), alignof(T)>::type >>();
        __OOX_TRACE("%p oox::var",v);
        v->out(0).next_writer.store((internal::task_node*)uintptr_t(1), std::memory_order_release);
        v->head.store((internal::arc*)uintptr_t(1), std::memory_order_release);
        // nobody wait on this task
        this->bind_to( v, &v->my_precious, 2 );
        return storage_ptr;
    }

public:
    var()                    { } // allocates default value lazily for sake of optimization
    var(const T& t) noexcept { new(allocate_new()) T( t ); } // TODO: add exception-safe
    var(T&& t)      noexcept { new(allocate_new()) T( std::move(t) ); }
    var(var<T>&& t) : internal::oox_var_base(std::move(t)) { t.current_task = nullptr; }
    var& operator=(var<T>&& t) {
        release();
        new(this) internal::oox_var_base(std::move(t));
        __OOX_ASSERT_EX(current_task, "");
        t.current_task = nullptr;
        return *this;
    }
    ~var() { release(); }
};

template<>
class var<void> : public internal::oox_var_base {
    template< typename T > friend struct gen_oox;
public:
    var() {}
    template<typename D>
    var(var<D>&& src) : internal::oox_var_base(src) {
        ((internal::task_node*)(uintptr_t(src.storage_ptr)-src.storage_offset))->release();
        src.current_task = nullptr;
    }
};

typedef var<void> oox_node;

namespace internal {
template< typename T >
std::string get_type(const char *m = "T") {
    std::string s = (std::is_const<typename std::remove_reference<T>::type>::value
            || std::is_const<T>::value)? "const " : "";
    s.append( m );
    if(std::is_lvalue_reference<T>::value) s.append( "&" );
    if(std::is_rvalue_reference<T>::value) s.append( "&&" );
    return s;
}

template< typename... Args > struct types {};

// Types is types<list> of user functor argument types
// Args is variadic list of run argument types
template< typename Types, typename... Args > struct base_args;
// User functor might have default arguments which are not specified thus ignoring them
template< typename IgnoredTypes > struct base_args<IgnoredTypes> {
    static constexpr int write_nodes_count = 1; // for resulting node
    int setup(int, internal::task_node *) { return 0 /* resulting node is ready initially*/; }
};

template< typename T, typename... Types, typename A, typename... Args >
struct base_args<types<T, Types...>, A, Args...> : base_args<types<Types...>, Args...> {
    typedef base_args<types<Types...>, Args...> base_type;

    typename std::decay<A>::type my_value;

    base_args( A&& a, Args&&... args ) : base_type( std::forward<Args>(args)... ), my_value(std::forward<A>(a)) {}
    typename std::decay<A>::type&& consume() { return std::move(my_value); }
    static constexpr int write_nodes_count = base_type::write_nodes_count;
    int setup( int port, internal::task_node *self, A&& a, Args&&... args ) {
        //__OOX_ASSERT(my_value == a, "");
        return base_type::setup( port, self, std::forward<Args>(args)...);
    }
};

template< typename Types, typename... Args > struct oox_var_args;
template< typename T, typename... Types, typename C, typename... Args >
struct oox_var_args<types<T, Types...>, C, Args...> : base_args<types<Types...>, Args...> {
    typedef base_args<types<Types...>, Args...> base_type;
    typedef typename  std::decay<C>::type      ooxed_type;
    typedef var<ooxed_type>                var_type;

    uintptr_t my_ptr;
    // TODO: copy-based optimizations
    oox_var_args( const var_type& cov, Args&&... args ) : base_type( std::forward<Args>(args)... ) {}
    static constexpr int is_writer = (std::is_rvalue_reference<C>::value
        || (std::is_lvalue_reference<T>::value && !std::is_const<typename std::remove_reference<T>::type>::value))? 1 : 0;
    static constexpr int write_nodes_count = base_type::write_nodes_count + is_writer;

    int setup( int port, internal::task_node *self, const var_type& cov, Args&&... args ) {
        int count = is_writer;
        __OOX_TRACE("%p arg: %s=%p as %s: is_writer=%d", self, get_type<C>("oox::var<A>").c_str(), cov.current_task, get_type<T>("T").c_str(), count);
        if( !cov.current_task )
            new( &const_cast<var_type&>(cov) ) var_type(ooxed_type()); // allocate oox container with default value
        if( count ) {
            auto &ov = const_cast<var_type&>(cov); // actual type is non-const due to is_writer
            ov.set_next_writer( port, self );// TODO: add 'count =' because no need in sync here
        } else
            count = self->assign_prerequisite( cov.current_task, cov.current_port );
        if( cov.is_forward ) {
            oox_var_base& next = *(oox_var_base*)cov.storage_ptr;
            my_ptr = 1|(uintptr_t)&next.storage_ptr;
        } else
            my_ptr = (uintptr_t)cov.storage_ptr;
        //TODO: broken? if( !std::is_lvalue_reference<C>::value ) // consume oox::var
        //    ov.~var(); // TODO: no need in sync for not yet published task
        return count + base_type::setup( port+is_writer, self, std::forward<Args>(args)...);
    }
    C&& consume() {
        if( my_ptr&1 ) // is forwarded?
             return static_cast<C&&>(**(ooxed_type**)(my_ptr^1));
        else return static_cast<C&&>(*(ooxed_type*)my_ptr);
    }
};
template< typename T, typename... Types, typename A, typename... Args >
struct base_args<types<T, Types...>, var<A>&, Args...> : oox_var_args<types<T, Types...>, A&, Args...> {
    using oox_var_args<types<T, Types...>, A&, Args...>::oox_var_args;
};
template< typename T, typename... Types, typename A, typename... Args >
struct base_args<types<T, Types...>, const var<A>&, Args...> : oox_var_args<types<T, Types...>, const A&, Args...> {
    using oox_var_args<types<T, Types...>, const A&, Args...>::oox_var_args;
};
template< typename T, typename... Types, typename A, typename... Args >
struct base_args<types<T, Types...>, var<A>&&, Args...> : oox_var_args<types<T, Types...>, A&&, Args...> {
    using oox_var_args<types<T, Types...>, A&&, Args...>::oox_var_args;
};

template< typename F, typename... Preceding, typename Args >
auto apply_args( F&& f, Args&& pack, Preceding&&... params )
__OOX_AUTO_TYPE_FUNC((
    apply_args( std::forward<F>(f), std::forward<typename Args::base_type>(pack),
                std::forward<Preceding>(params)..., pack.consume() )
))

template< typename F, typename... Preceding, typename Last >
auto apply_args( F&& f, base_args<Last>&&/*pack*/, Preceding&&... params )
__OOX_AUTO_TYPE_FUNC((
    std::forward<F>( f )( std::forward<Preceding>(params)... )
))

template< typename F, typename Args >
struct oox_bind {
    F my_func;
    Args my_args;
    oox_bind(F&& f, Args&& a) : my_func(std::forward<F>(f)), my_args(std::move(a)) {}
    auto operator()() __OOX_AUTO_TYPE_FUNC(( apply_args(std::move(my_func), std::move(my_args))  ))
};

template<int slots, typename F, typename R>
struct alignas(64) functional_task : storage_task<slots, F> {
    using storage_task<slots, F>::storage_task;
    typename std::aligned_storage<sizeof(R), alignof(R)>::type my_result;
    TASK_EXECUTE_METHOD {
        __OOX_TRACE("%p do_run: start",this);
        new(&my_result) R( this->my_precious() );
        task_node::notify_successors<slots>();
        return nullptr;
    }
    ~functional_task() {
        reinterpret_cast<R*>(&my_result)->~R(); // TODO: what if it was canceled?
    }
};

template<int slots, typename F>
struct functional_task<slots, F, void> : storage_task<slots, F> {
    using storage_task<slots, F>::storage_task;
    TASK_EXECUTE_METHOD {
        __OOX_TRACE("%p do_run: start",this);
        this->my_precious();
        task_node::notify_successors<slots>();
        return nullptr;
    }
};

template<int slots, typename F, typename VT> // forwarding task
struct functional_task<slots, F, var<VT> > : storage_task<slots, F> {
    // TODO: NRVO optimized forwarding
    using storage_task<slots, F>::storage_task;
    typename std::aligned_storage< sizeof(var<VT>), alignof(var<VT>) >::type my_result;
    bool is_executed = false;
    TASK_EXECUTE_METHOD {
#if 0
        __OOX_TRACE("%p do_run: start forward",this);
        new(my_result.begin()) var<VT>( this->my_precious() );
        return task_node::forward_successors<slots>( *my_result.begin() );
#else
        if( !is_executed ) {
            __OOX_TRACE("%p do_run: start forward",this);
            new(&my_result) var<VT>( this->my_precious() );
            is_executed = true;
            this->start_count.store(1, std::memory_order_release);
            arc* j = new arc( this, 0, arc::flow_only ); // TODO: embed into the task
            if( reinterpret_cast<var<VT>*>(&my_result)->current_task->add_arc(j) ) {
                __OOX_TRACE("%p do_run: add_arc", this); // recycle_as_continuation was here
                return nullptr;
            }
            else delete j;
        }
        __OOX_TRACE("%p do_run: notify forward",this);
        task_node::notify_successors<slots>();
        return nullptr;
#endif
    }
    ~functional_task() {
        reinterpret_cast<var<VT>*>(&my_result)->~var<VT>(); // current_task is finished in forward_successors
    }
};

template< typename T >
struct gen_oox {
    typedef var<T> type;
    template< int slots, typename F >
    static type bind_to(internal::functional_task<slots, F, T> * t) {
        type oox; oox.bind_to( t, &t->my_result, slots+1 ); return oox;
    }
};
template<>
struct gen_oox<void> {
    typedef var<void> type;
    template< int slots, typename F >
    static type bind_to(internal::functional_task<slots, F, void> * t) {
        type oox; oox.bind_to( t, t, slots ); return oox;
    }
};
template< typename VT >
struct gen_oox<var<VT> > {
    typedef var<VT> type;
    template< int slots, typename F >
    static type bind_to(internal::functional_task<slots, F, var<VT> > * t) {
        type oox; oox.bind_to( t, &t->my_result, slots+1, true ); return oox;
    }
};
template< typename T>
using var_type = typename gen_oox<T>::type;

template< typename R, typename... Types >
struct functor_info {
    typedef R result_type;
    typedef types<Types...> args_list_type;
};
template< typename R, typename... Args >
functor_info<R, Args...> get_functor_info(R (&)(Args...)) { return functor_info<R, Args...>(); }
template< typename R, typename C, typename... Args >
functor_info<R, Args...> get_functor_info(R (C::*)(Args...)) { return functor_info<R, Args...>(); }
template< typename R, typename C, typename... Args >
functor_info<R, Args...> get_functor_info(R (C::*)(Args...) const) { return functor_info<R, Args...>(); }
template< typename F >
auto get_functor_info(F&&) __OOX_AUTO_TYPE_FUNC(( get_functor_info( &std::remove_reference<F>::type::operator() ) ))
template< typename F >
using result_type_of = typename decltype( get_functor_info(std::declval<F>()) )::result_type;
template< typename F >
using args_list_of = typename decltype( get_functor_info(std::declval<F>()) )::args_list_type;

} //namespace internal

template< typename F, typename... Args > // ->...decltype(f(internal::unoox(args)...))
auto run(F&& f, Args&&... args)->internal::var_type<internal::result_type_of<F> >
{
    typedef internal::result_type_of<F>                      r_type;
    typedef internal::args_list_of<F>                call_args_type;
    typedef internal::base_args<call_args_type, Args&&...> args_type;
    typedef internal::oox_bind<F, args_type>           functor_type;
    typedef internal::functional_task<args_type::write_nodes_count, functor_type, r_type> task_type;

    task_type *t = internal::task::allocate<task_type>( functor_type(std::forward<F>(f), args_type(std::forward<Args>(args)...)) );
    __OOX_TRACE("%p oox::run: write ports %d",t,args_type::write_nodes_count);
    int protect_count = std::numeric_limits<int>::max();
    t->start_count.store(protect_count, std::memory_order_release);
    // process functor types
    protect_count -= t->my_precious.my_args.setup( 1, t, std::forward<Args>(args)...);
    auto r = internal::gen_oox<r_type>::bind_to( t );
    t->remove_prerequisite( protect_count ); // publish it
    return r;
}

void wait_for_all(internal::oox_var_base& on ) {
    on.wait();
}

template<typename T>
T wait_and_get(var<T> &&ov) { wait_for_all(ov); return *(T*)ov.storage_ptr; }
template<typename T>
T wait_and_get(var<T> &ov) { wait_for_all(ov); return *(T*)ov.storage_ptr; }
template<typename T>
T wait_and_get(const var<T> &ov) { wait_for_all(const_cast<var<T>&>(ov)); return *(T*)ov.storage_ptr; }

#undef TASK_EXECUTE_METHOD

#endif // !OOX_SERIAL

} // namespace oox
#endif // __OOX_H__
