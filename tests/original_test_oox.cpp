// Copyright (C) 2014 Intel Corporation
//
// SPDX-License-Identifier: Apache-2.0

#include <utility>
#include <functional>
#include <type_traits>
#include <limits>
#include <iostream>

#define TBB_PREVIEW_WAITING_FOR_WORKERS 1
#define TBB_USE_ASSERT 1
#include <tbb/tbb.h>
#include "harness.h"
#undef  TRACE
#define TRACE !Verbose ? (void)0 : Harness::internal::tracer.set_trace_info(Harness::internal::Tracer::need_lf, HARNESS_TRACE_ORIG_INFO)->trace

#define OOX_SERIAL 0

#define __TBB_AUTO_TYPE_FUNC(expr) ->decltype(expr) { return expr; }

#if OOX_SERIAL == 1 ////////////////////// Immediate execution //////////////////////////////////
class oox_node : tbb::internal::no_assign {};

template<typename T>
struct oox_var : public oox_node {
    static_assert(std::is_same<T, typename std::decay<T>::type>::value,
                  "Specialize oox_var only by plain types."
                  "For references, use reference_wrapper,"
                  "for const types use shared_ptr<T>.");
    T my_value;
    oox_var() : my_value() {}
    oox_var(T t) : my_value(t) {}
};

template<>
struct oox_var<void> : public oox_node {
    oox_var() {}
};

template< typename T > // create temporary copies to simulate parallel implementation
typename std::decay<T>::type unoox(T&& t) { return typename std::decay<T>::type(std::forward<T>(t)); }

template< typename T >
const T& unoox(const oox_var<T>& t) { return t.my_value; }

template< typename T >
T& unoox(oox_var<T>& t) { return t.my_value; }

template< typename T >
T&& unoox(oox_var<T>&& t) { return std::move(t.my_value); }

template< typename T >
struct gen_oox {
    typedef oox_var<typename std::decay<T>::type> type;
    template< typename F, typename... Args >
    static type run(F&& f, Args&&... args) { return type(std::forward<F>(f)(std::forward<Args>(args)...)); }
};
template< typename VT >
struct gen_oox<oox_var<VT> > {
    typedef oox_var<VT> type;
    template< typename F, typename... Args >
    static type run(F&& f, Args&&... args) { return std::forward<F>(f)(std::forward<Args>(args)...); }
};
template<>
struct gen_oox<void> {
    typedef oox_node type;
    template< typename F, typename... Args >
    static type run(F&& f, Args&&... args) { std::forward<F>(f)(std::forward<Args>(args)...); return oox_node(); }
};
template< typename T> using oox_type = typename gen_oox<T>::type;

template< typename F, typename... Args >
auto oox_run(F&& f, Args&&... args)->oox_type<decltype(f(unoox(std::forward<Args>(args))...))>
{
    return gen_oox<decltype(f(unoox(std::forward<Args>(args))...))>
    ::run(std::forward<F>(f), unoox(std::forward<Args>(args))...);
}

template<typename T>
T oox_wait_and_get(const oox_var<T> &ov) { return ov.my_value; }

void oox_wait_for_all(oox_node &) {}

#else ///////////////////////////////// TBB execution ///////////////////////////////////

namespace internal {

struct task_node;
struct oox_node_base;

struct output_node {
    // 0 if next writer is not known yet.
    // 1 if next writer is not known yet, but value is available and countdown includes extra one
    // 3 if next writer is end without var ownership
    // ptr|1 if next writer is end with var ownership, ptr points to var storage.
    // Otherwise points to next node that overwrites the value written by this node.
    tbb::atomic<task_node*> next_writer;
    tbb::atomic<int> countdown;
    output_node() {
        next_writer = NULL;
        countdown   = 1;
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
    tbb::atomic<arc*> head;
    // Add i to arc_list.
    // Return true if success, false otherwise.
    bool add_arc( arc* i );
    arc_list() { head = NULL; }
};

struct task_node : public tbb::task, arc_list {
    // Prerequisites to start the task TODO: use task_prefix::ref_count
    tbb::atomic<int> start_count;
    // Pointers to this structure and live output nodes
    tbb::atomic<int> life_count;
    // TODO: exception storage here?

    task_node() { set_ref_count(2); } // prepare the task for waiting on it directly

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
    int  forward_successors( int output_slots, int *counters, oox_node_base& );
    // Account for completion of n prerequisites
    void remove_prerequisite( int n=1 );
    // Process next writer notification
    int  notify_next_writer( task_node* d );
    // Account for removal of a back_arc
    int  remove_back_arc( int output_port, int n=1 );
    // Set new output dependence
    void set_next_writer( int output_port, task_node* n );
    // decrement lifetime references
    void release( int n=1 );
    // Call base notify successors
    template<int slots>
    tbb::task* notify_successors();
    // Call base forward successors
    template<int slots>
    tbb::task* forward_successors( oox_node_base& );

    // It is called when producer is done and notifies consumers to copy the value
    virtual void on_ready(int) { __TBB_ASSERT(false, "not implemented"); }
};

bool arc_list::add_arc( arc* i ) {
    __TBB_ASSERT( uintptr_t(i->node)>2, NULL );
    for(;;) {
        arc* j = head;
        if( j==(arc*)uintptr_t(1) )
            return false;
        i->next = j;
        if( head.compare_and_swap( i, j )==j )
            return true;
    }
}

int task_node::assign_prerequisite( task_node *n, int req_port ) {
    arc* j = new arc( this, req_port ); // TODO: embed into the task
    if( n->add_arc(j) ) {
        TRACE("%p assign_prerequisite: assigned to %p, %d",this,n,req_port);
        return 1; // Prerequisite n will decrement start_count when it produces a value
    } else {
        // Prerequisite n already produced a value. Add this as a consumer of n.
        int k = ++n->out(req_port).countdown;
        TRACE("%p assign_prerequisite: preventing %p, port %d, count %d",this,n,req_port,k);
        __TBB_ASSERT_EX(k>1,"risk that a prerequisite might be prematurely destroyed");
        j->node = n;
        j->kind = arc::back_only;
        bool success = add_arc(j); //TODO: add_arc_unsafe?
        __TBB_ASSERT_EX(success,NULL);
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
            TRACE("%p notify: %p->remove_back_arc(%d)",this,n,j->port);
            if( int k = n->remove_back_arc( j->port ) )
                n->release( k );
            delete j;
        } else {
            if( j->kind == arc::flow_back ) {
                // "n" is task that consumes value that this task produced.
                // Add back arc so that "n" can notify this when it is done consuming the value.
                j->node = this;
                j->kind = arc::back_only;
                if( out(j->port).next_writer != (task_node*)uintptr_t(3) ) {
                    bool b = n->add_arc( j );
                    __TBB_ASSERT_EX(b, "corrupted?");
                    --count[j->port];
                } else delete j; // very unlikely?
            } else if( j->kind == arc::flow_copy )
                n->on_ready( j->port );
            else if( j->kind == arc::forward_copy )
                __TBB_ASSERT(false, "incorrect forwarding"); // has to be processed by forward_successors only
            // Let "n" know that prerequisite "this" is ready.
            TRACE("%p notify: %p->remove_prequisite()",this,n);
            n->remove_prerequisite();
        }
    } while( r );
}

int task_node::do_notify_out( int port, int count ) {
    if( out(port).next_writer==NULL
        && out(port).next_writer.compare_and_swap( (task_node*)uintptr_t(1), NULL )==NULL ) {
        // The thread that installs the non-NULL "next_writer" will see the 1 and do the decrement.
        --count;
        TRACE("%p notify out %d: next_writer went from 0 to 1",this,port);
    } else if( !(uintptr_t((void*)out(port).next_writer)&1) ) {
#if OOX_AFFINITY
        task_node* d = out(port).next_writer;
        d->affinity = a;
#endif /* OOX_AFFINITY */
        TRACE("%p notify out %d: next_writer is %p\n",this,port,out(port).next_writer);
    } else {
        TRACE("%p notify out %d: next_writer is final: %p\n",this,port,out(port).next_writer);
    }
    return remove_back_arc( port, count );
}

int task_node::notify_successors( int output_slots, int *count ) {
    for( int i = 0; i <  output_slots; i++ ) {
        // it should be safe to assign countdowns here because no successors were notified yet
        out(i).countdown = count[i] = std::numeric_limits<int>::max()/2;
    }
    TRACE("%p notify successors",this);
    // Grab list of successors and mark as competed.
    // Note that countdowns can change asynchronously after this point
    if( arc* r = head.fetch_and_store( (arc*)uintptr_t(1) ) )
        do_notify_arcs( r, count );
    int refs = 0;
    for( int i = 0; i <  output_slots; i++ )
        refs += do_notify_out( i, count[i] );
    __TBB_ASSERT(refs>=0, NULL);
    return refs;
}

void task_node::remove_prerequisite( int n ) {
    int k = start_count-=n;
    __TBB_ASSERT(k>=0,"invalid start_count detected while removing prerequisite");
    if( k==0 ) {
#if OOX_AFFINITY
        if( affinity )
            t.set_affinity(affinity);
#endif /* OOX_AFFINITY */
        TRACE("%p remove_prerequisite: spawning",this);
        tbb::task::spawn( *this );
    }
}

int task_node::notify_next_writer( task_node* d ) {
    uintptr_t i = (uintptr_t)d;
    if( i&1 ) {
        if( i == 3 )
            return 1;
        __TBB_ASSERT( i!=1, "remove_back_arc called on output node with next_writer=1" );
        d = (task_node*)(i&~1);
        if( d == this )
            return 2;
        d->release();
    } else {
        __TBB_ASSERT( d!=NULL, "remove_back_arc called on output node with next_writer==0" );
        d->remove_prerequisite();
    }
    return 1; // the last, release the node
}

int task_node::remove_back_arc( int output_port, int n ) {
    int k = out(output_port).countdown -= n;
    __TBB_ASSERT(k>=0,"invalid countdown detected while removing back_arc");
    TRACE("%p remove_back_arc port %d: %d (next_writer is %p)",this,output_port,k,out(output_port).next_writer);
    if( k==0 ) {
        // Next writer was waiting on all consumers of me to finish.
        return notify_next_writer( out(output_port).next_writer );
    }
    return 0;
}

void task_node::set_next_writer( int output_port, task_node* d ) {
    __TBB_ASSERT( uintptr_t(d)!=1, NULL );
    task_node* o = out(output_port).next_writer.fetch_and_store(d);
    TRACE("%p set_next_writer(%d, %p): next_writer was %p",this,output_port,d,o);
    if( o ) {
        if( uintptr_t(o)==1 ) {
            // this has value and conceptual back_arc from its owning oox that was removed.
            if( int k = remove_back_arc( output_port ) ) // TODO: optimize it for set_next_writer without contention
                release( k );
        } else {
            __TBB_ASSERT( uintptr_t(o)==3, NULL );
            __TBB_ASSERT( uintptr_t(d)==3, "TODO forward_successors" ); // TODO
        }
    }
}

void task_node::release( int n ) {
    int k = life_count-=n;
    TRACE("%p release: %d",this,k);
    __TBB_ASSERT(k>=0,"invalid life_count detected while removing prerequisite");
    if( k==0 ) {
#if TBB_USE_ASSERT
        __TBB_ASSERT(state()==task::allocated, NULL);
        tbb::task *t = new( tbb::task::allocate_root(*this->context())) tbb::empty_task;
        t->set_ref_count(2);
        set_parent(t);                // current implementation has incorrect assert for NULL
        tbb::task::destroy(*this);
        tbb::task::destroy(*t);
#else
        set_parent(NULL);             // prevent assert in tbb_debug library
        tbb::task::destroy(*this);
#endif
    }
}

template<int slots>
tbb::task* task_node::notify_successors() {
    int counters[slots];
    int k, n = notify_successors( slots, counters );
    if( life_count != n && (k = (life_count -= n)) > 0 ) {
        __TBB_ASSERT(k>=0,"invalid life_count detected while removing prerequisite");
        recycle_as_safe_continuation(); // do not destroy the task after execution and decrement parent().ref_count()
        set_parent(this);   // and decrement this->ref_count() after completion to enable direct waits on this task
    } else set_ref_count(0);
    return NULL;
}

template<int slots>
struct task_node_slots : task_node {
    output_node output_nodes[slots];
};

output_node& task_node::out(int n) const {
    typedef task_node_slots<1024> self_t;
    self_t* self = (self_t*)this;
    return self->output_nodes[n];
}

template<int slots, typename T>
struct storage_task : task_node_slots<slots> {
    T my_precious;
    tbb::task* execute() { __TBB_ASSERT(false, "not runnable"); return NULL; }
    storage_task() = default;
    storage_task(T&& t) : my_precious(std::move(t)) {}
    storage_task(const T& t) : my_precious(t) {}
};

struct oox_node_base : tbb::internal::no_assign {
    //TODO: make it a class with private members
    template< typename T > friend struct gen_oox;
    task_node*  current_task = NULL;
    void*       storage_ptr;
    int         storage_offset; // task_node* original = ptr - offset
    short int   current_port = 0; // the problem can arise from concurrent accesses to oox_var, TODO: check
    bool        is_forward = false;  // indicate if it refers to another oox_var recursively

    void set_next_writer( int output_port, task_node* d ) {
        __TBB_ASSERT(current_task, "empty oox_var");
        current_task->set_next_writer( current_port, d );
        current_task = d, current_port = output_port;
    }
    void bind_to( task_node * t, void* ptr, int lifetime, bool fwd = false ) {
        current_task = t, current_port = 0, storage_ptr = ptr, is_forward = fwd;
        storage_offset = uintptr_t(storage_ptr) - uintptr_t(current_task);
        t->life_count = lifetime;
        TRACE("%p bind: store=%p life=%d fwd=%d",t,ptr,lifetime,fwd);
    }
    void wait() {
        __TBB_ASSERT(current_task, "wait for empty oox_var");
        if( current_task->ref_count() > 1 )
            current_task->wait_for_all();
    }
    void release() {
        if( current_task ) {
            current_task->set_next_writer( current_port, (task_node*)uintptr_t(
                    storage_offset? ((uintptr_t(storage_ptr)-storage_offset)|1) : 3/*var<void>*/) );
            current_task = nullptr;
        }
    }
    ~oox_node_base() { release(); }
};

#if 0
int task_node::forward_successors( int output_slots, int *count, oox_node_base& n ) {
    for( int i = 0; i <  output_slots; i++ ) {
        // it is safe to assign countdowns here because no successors were notified yet
        out(i).countdown = count[i] = std::numeric_limits<int>::max()/2;
    }
    arc* r = head.fetch_and_store( (arc*)uintptr_t(1) ); // mark it completed
    task_node* d = out(0).next_writer.fetch_and_store( (internal::task_node*)uintptr_t(3) ); // finish this node
    int refs = 1;
    TRACE("%p forward_successors(%p, %d): arcs=%p next_writer=%p",this,n.current_task,n.current_port,r,d);
    if( r ) {
        arc* l = n.current_task->head.fetch_and_store( r ); // forward dependencies
        if( l ) {
            TRACE("%p forward_successors(%p, %d): notify arcs myself %p",this,n.current_task,n.current_port,l);
            __TBB_ASSERT( uintptr_t(l)==1, "arc lists merge is not implemented" ); // TODO
            __TBB_ASSERT(!n.is_forward, "not implemented"); // TODO
            do_notify_arcs( r, count );
        }
    }
    if( d ) { // TODO: can be converted as another arc type instead of working with outputs?
        task_node* o = n.current_task->out(n.current_port).next_writer.fetch_and_store( d );
        if( o ) { // next node is ready already
            TRACE("%p forward_successors(%p, %d): removing back arc myself %p",this,n.current_task,n.current_port,o);
            __TBB_ASSERT( uintptr_t(o)==1, NULL );
            __TBB_ASSERT(!n.is_forward, "not implemented"); // TODO
            __TBB_ASSERT(out(0).countdown == count[0], "not implemented"); // TODO?
            notify_next_writer( d );
        }
    }
    //n.current_task = NULL;
    // now we have next writer to be processed here
    for( int i = 1; i <  output_slots; i++ )
        refs += do_notify_out( i, count[i] );
    __TBB_ASSERT(refs>=0, NULL);
    return refs;
}

template<int slots>
tbb::task* task_node::forward_successors( oox_node_base& m ) {
    int counters[slots];
    int k, n = forward_successors( slots, counters, m );
    if( life_count != n && (k = (life_count -= n)) > 0 ) {
        __TBB_ASSERT(k>=0,"invalid life_count detected while forwarding prerequisites");
        recycle_as_safe_continuation(); // do not destroy the task after execution and decrement parent().ref_count()
        set_parent(this);   // and decrement this->ref_count() after completion to enable direct waits on this task
    } else set_ref_count(0);
    return NULL;
}
#endif

template< typename T > struct gen_oox;

} // namespace internal


template< typename T >
class oox_var : public internal::oox_node_base {
    static_assert(std::is_same<T, typename std::decay<T>::type>::value,
                  "Specialize oox_var only by plain types and pointers."
                  "For references, use reference_wrapper,"
                  "for const types use shared_ptr<T>.");

    void* allocate_new() noexcept {
        auto *v = new( tbb::task::allocate_root() )
                internal::storage_task<1, tbb::aligned_space<T> >();
        TRACE("%p oox_var",v);
        v->out(0).next_writer = (internal::task_node*)uintptr_t(1);
        v->head = (internal::arc*)uintptr_t(1);
        v->set_ref_count(0); // nobody wait on this task
        this->bind_to( v, v->my_precious.begin(), 2 );
        return storage_ptr;
    }

public:
    oox_var() {}
    oox_var(const T& t) noexcept { new(allocate_new()) T( t ); } // TODO: add exception-safe
    oox_var(T&& t)      noexcept { new(allocate_new()) T( std::move(t) ); }
    oox_var(oox_var<T>&& t) : internal::oox_node_base(std::move(t)) { t.current_task = nullptr; }
    oox_var& operator=(oox_var<T>&& t) {
        release();
        new(this) internal::oox_node_base(std::move(t));
        t.current_task = nullptr;
        return *this;
    }
};

template<>
class oox_var<void> : public internal::oox_node_base {
    template< typename T > friend struct gen_oox;
public:
    oox_var() {}
    template<typename D>
    oox_var(oox_var<D>&& src) : internal::oox_node_base(src) {
        (internal::task_node*)(uintptr_t(src.storage_ptr)-src.storage_offset)->release();
        src.current_task = nullptr;
    }
};

typedef oox_var<void> oox_node;

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
// Args is variadic list of oox_run argument types
template< typename Types, typename... Args > struct oox_args;
// User functor might have default arguments which are not specified thus ignoring them
template< typename IgnoredTypes > struct oox_args<IgnoredTypes> {
    static constexpr int write_nodes_count = 1; // for resulting node
    int setup(int, internal::task_node *) { return 0 /* resulting node is ready initially*/; }
};

template< typename T, typename... Types, typename A, typename... Args >
struct oox_args<types<T, Types...>, A, Args...> : oox_args<types<Types...>, Args...> {
    typedef oox_args<types<Types...>, Args...> base_type;

    typename std::decay<A>::type my_value;

    oox_args( A&& a, Args&&... args ) : base_type( std::forward<Args>(args)... ), my_value(std::forward<A>(a)) {}
    typename std::decay<A>::type&& consume() { return std::move(my_value); }
    static constexpr int write_nodes_count = base_type::write_nodes_count;
    int setup( int port, internal::task_node *self, A&& a, Args&&... args ) {
        //__TBB_ASSERT(my_value == a, NULL);
        return base_type::setup( port, self, std::forward<Args>(args)...);
    }
};

template< typename Types, typename... Args > struct oox_var_args;
template< typename T, typename... Types, typename C, typename... Args >
struct oox_var_args<types<T, Types...>, C, Args...> : oox_args<types<Types...>, Args...> {
    typedef oox_args<types<Types...>, Args...> base_type;
    typedef typename  std::decay<C>::type      ooxed_type;
    typedef oox_var<ooxed_type>                oox_type;

    uintptr_t my_ptr;
    // TODO: copy-based optimizations
    oox_var_args( const oox_type& cov, Args&&... args ) : base_type( std::forward<Args>(args)... ) {}
    static constexpr int is_writer = (std::is_rvalue_reference<C>::value
        || (std::is_lvalue_reference<T>::value && !std::is_const<typename std::remove_reference<T>::type>::value))? 1 : 0;
    static constexpr int write_nodes_count = base_type::write_nodes_count + is_writer;
    int setup( int port, internal::task_node *self, const oox_type& cov, Args&&... args ) {
        int count = is_writer;
        if( Verbose )
            std::cout << self << " arg: " << get_type<C>("oox_var<A>") << "=" << cov.current_task
                << " as " << get_type<T>("T") << ": is_writer=" << count << std::endl;
        if( count ) {
            auto &ov = const_cast<oox_type&>(cov); // actual type is non-const due to is_writer
            if( !cov.current_task ) {
                new( &ov ) oox_type(ooxed_type()); // allocate oox container with default value
            }
            ov.set_next_writer( port, self );// TODO: add 'count =' because no need in sync here
        } else
            count = self->assign_prerequisite( cov.current_task, cov.current_port );
        if( cov.is_forward ) {
            oox_node_base& next = *(oox_node_base*)cov.storage_ptr;
            my_ptr = 1|(uintptr_t)&next.storage_ptr;
        } else
            my_ptr = (uintptr_t)cov.storage_ptr;
        //TODO: broken? if( !std::is_lvalue_reference<C>::value ) // consume oox_var
        //    ov.~oox_var(); // TODO: no need in sync for not yet published task
        return count + base_type::setup( port+is_writer, self, std::forward<Args>(args)...);
    }
    C&& consume() {
        if( my_ptr&1 ) // is forwarded?
             return static_cast<C&&>(**(ooxed_type**)(my_ptr^1));
        else return static_cast<C&&>(*(ooxed_type*)my_ptr);
    }
};
template< typename T, typename... Types, typename A, typename... Args >
struct oox_args<types<T, Types...>, oox_var<A>&, Args...> : oox_var_args<types<T, Types...>, A&, Args...> {
    using oox_var_args<types<T, Types...>, A&, Args...>::oox_var_args;
};
template< typename T, typename... Types, typename A, typename... Args >
struct oox_args<types<T, Types...>, const oox_var<A>&, Args...> : oox_var_args<types<T, Types...>, const A&, Args...> {
    using oox_var_args<types<T, Types...>, const A&, Args...>::oox_var_args;
};
template< typename T, typename... Types, typename A, typename... Args >
struct oox_args<types<T, Types...>, oox_var<A>&&, Args...> : oox_var_args<types<T, Types...>, A&&, Args...> {
    using oox_var_args<types<T, Types...>, A&&, Args...>::oox_var_args;
};

template< typename F, typename... Preceding, typename Args >
auto apply_args( F&& f, Args&& pack, Preceding&&... params )
__TBB_AUTO_TYPE_FUNC((
    apply_args( std::forward<F>(f), std::forward<typename Args::base_type>(pack),
                std::forward<Preceding>(params)..., pack.consume() )
))

template< typename F, typename... Preceding, typename Last >
auto apply_args( F&& f, oox_args<Last>&&/*pack*/, Preceding&&... params )
__TBB_AUTO_TYPE_FUNC((
    std::forward<F>( f )( std::forward<Preceding>(params)... )
))

template< typename F, typename Args >
struct oox_bind {
    F my_func;
    Args my_args;
    oox_bind(F&& f, Args&& a) : my_func(std::forward<F>(f)), my_args(std::move(a)) {}
    auto operator()() __TBB_AUTO_TYPE_FUNC(( apply_args(std::move(my_func), std::move(my_args))  ))
};

template<int slots, typename F, typename R>
struct functional_task : storage_task<slots, F> {
    using storage_task<slots, F>::storage_task;
    tbb::aligned_space<R> my_result;
    tbb::task* execute() override {
        TRACE("%p do_run: start",this);
        new(my_result.begin()) R( this->my_precious() );
        return task_node::notify_successors<slots>();
    }
    ~functional_task() {
        my_result.begin()->~R(); // TODO: what if it was canceled?
    }
};

template<int slots, typename F>
struct functional_task<slots, F, void> : storage_task<slots, F> {
    using storage_task<slots, F>::storage_task;
    tbb::task* execute() override {
        TRACE("%p do_run: start",this);
        this->my_precious();
        return task_node::notify_successors<slots>();
    }
};

template<int slots, typename F, typename VT> // forwarding task
struct functional_task<slots, F, oox_var<VT> > : storage_task<slots, F> {
    // TODO: NRVO optimized forwarding
    using storage_task<slots, F>::storage_task;
    tbb::aligned_space< oox_var<VT> > my_result;
    bool is_executed = false;
    tbb::task* execute() override {
#if 0
        TRACE("%p do_run: start forward",this);
        new(my_result.begin()) oox_var<VT>( this->my_precious() );
        return task_node::forward_successors<slots>( *my_result.begin() );
#else
        if( !is_executed ) {
            TRACE("%p do_run: start forward",this);
            new(my_result.begin()) oox_var<VT>( this->my_precious() );
            is_executed = true;
            this->start_count = 1;
            arc* j = new arc( this, 0, arc::flow_only ); // TODO: embed into the task
            if( my_result.begin()->current_task->add_arc(j) ) {
                this->recycle_as_continuation(); // TODO: avoid race in TBB scheduler
                return NULL;
            }
            else delete j;
        }
        TRACE("%p do_run: notify forward",this);
        return task_node::notify_successors<slots>();
#endif
    }
    ~functional_task() {
        my_result.begin()->~oox_var<VT>(); // current_task is finished in forward_successors
    }
};

template< typename T >
struct gen_oox {
    typedef oox_var<T> type;
    template< int slots, typename F >
    static type bind_to(internal::functional_task<slots, F, T> * t) {
        type oox; oox.bind_to( t, t->my_result.begin(), slots+1 ); return oox;
    }
};
template<>
struct gen_oox<void> {
    typedef oox_var<void> type;
    template< int slots, typename F >
    static type bind_to(internal::functional_task<slots, F, void> * t) {
        type oox; oox.bind_to( t, t, slots ); return oox;
    }
};
template< typename VT >
struct gen_oox<oox_var<VT> > {
    typedef oox_var<VT> type;
    template< int slots, typename F >
    static type bind_to(internal::functional_task<slots, F, oox_var<VT> > * t) {
        type oox; oox.bind_to( t, t->my_result.begin(), slots+1, true ); return oox;
    }
};
template< typename T>
using oox_type = typename gen_oox<T>::type;

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
auto get_functor_info(F&&) __TBB_AUTO_TYPE_FUNC(( get_functor_info( &std::remove_reference<F>::type::operator() ) ))
template< typename F >
using result_type_of = typename decltype( get_functor_info(std::declval<F>()) )::result_type;
template< typename F >
using args_list_of = typename decltype( get_functor_info(std::declval<F>()) )::args_list_type;

} //namespace internal

template< typename F, typename... Args > // ->...decltype(f(internal::unoox(args)...))
auto oox_run(F&& f, Args&&... args)->internal::oox_type<internal::result_type_of<F> >
{
    typedef internal::result_type_of<F>                      r_type;
    typedef internal::args_list_of<F>                call_args_type;
    typedef internal::oox_args<call_args_type, Args&&...> args_type;
    typedef internal::oox_bind<F, args_type>           functor_type;
    typedef internal::functional_task<args_type::write_nodes_count, functor_type, r_type> task_type;

    task_type *t = new( tbb::task::allocate_root() )
            task_type( functor_type(std::forward<F>(f), args_type(std::forward<Args>(args)...)) );
    TRACE("%p oox_run: write ports %d",t,args_type::write_nodes_count);
    int protect_count = t->start_count = std::numeric_limits<int>::max();
    // process functor types
    protect_count -= t->my_precious.my_args.setup( 1, t, std::forward<Args>(args)...);
    auto r = internal::gen_oox<r_type>::bind_to( t );
    t->remove_prerequisite( protect_count ); // publish it
    return r;
}

void oox_wait_for_all(internal::oox_node_base& on ) {
    on.wait();
}

template<typename T>
T oox_wait_and_get(oox_var<T> &&ov) { oox_wait_for_all(ov); return *(T*)ov.storage_ptr; }
template<typename T>
T oox_wait_and_get(oox_var<T> &ov) { oox_wait_for_all(ov); return *(T*)ov.storage_ptr; }
template<typename T>
T oox_wait_and_get(const oox_var<T> &ov) { oox_wait_for_all(const_cast<oox_var<T>&>(ov)); return *(T*)ov.storage_ptr; }

#endif

/////////////////////////////////////// EXAMPLES    ////////////////////////////////////////
#include <numeric>
#include <vector>
#include <tbb/concurrent_vector.h>
namespace ArchSample {
    // example from original OOX
    typedef int T;
    T f() { return 1; }
    T g() { return 2; }
    T h() { return 3; }
    T test() {
        oox_var<T> a, b, c;
        oox_run([](T&A){A=f();}, a);
        oox_run([](T&B){B=g();}, b);
        oox_run([](T&C){C=h();}, c);
        oox_run([](T&A, T B){A+=B;}, a, b);
        oox_run([](T&B, T C){B+=C;}, b, c);
        oox_wait_for_all(b);
        return oox_wait_and_get(a);
    }
}
namespace Fib0 {
    // Concise demonstration
    int Fib(int n) {
        if(n < 2) return n;
        return Fib(n-1) + Fib(n-2);
    }
}
namespace Fib1 {
    // Concise demonstration
    oox_var<int> Fib(int n) {
        if(n < 2) return n;
        return oox_run(std::plus<int>(), oox_run(Fib, n-1), oox_run(Fib, n-2) );
    }
}
namespace Fib2 {
    // Optimized number and order of tasks
    oox_var<int> Fib(int n) {                              // OOX: High-level continuation style
        if(n < 2) return n;
        auto right = oox_run(Fib, n-2);                    // spawn right child
        return oox_run(std::plus<int>(), Fib(n-1), right); // assign continuation
    }
}

namespace Filesystem {
struct INode {
    int v;
    bool is_file() { return v>7; }
    size_t size()  { return v; }
    INode* begin() { return this+1; }
    INode* end()   { return this+1+v; }
} tree[]{4,2,3,8,9,10};

namespace Serial {
size_t disk_usage(INode& node) {
    if (node.is_file())
        return node.size();
    size_t sum = 0;
    for (auto &subnode : node)
        sum += disk_usage(subnode);
    return sum;
}
}
namespace Simple {
oox_var<size_t> disk_usage(INode& node)
{
    if (node.is_file())
        return node.size();
    oox_var<size_t> sum = 0;
    for (auto &subnode : node)
        oox_run([](size_t &sm, // serialized on write operation
                 size_t sz){ sm += sz; }, sum, oox_run(disk_usage, std::ref(subnode))); // parallel recursive leaves
    return sum;
}
}//namespace Filesystem::Simple
namespace AntiDependence {
oox_var<size_t> disk_usage(INode& node)
{
    if (node.is_file())
        return node.size();
    typedef tbb::concurrent_vector<size_t> cv_t;
    typedef cv_t *cv_ptr_t;
    oox_var<cv_ptr_t> resv = new cv_t;
    for (auto &subnode : node)
        oox_run([](const cv_ptr_t &v, // make it read-only (but not copyable) and thus parallel
                            size_t s){ v->push_back(s); }, resv, oox_run(disk_usage, std::ref(subnode)));
    return oox_run([](cv_ptr_t &v) { // make it read-write to induce anti-dependence from the above read-only tasks
        size_t res = std::accumulate(v->begin(), v->end(), 0);
        delete v; v = nullptr;      // release memory, reset the pointer
        return res;
    }, resv);
}
}//namespace Filesystem::AntiDependence
#if 0
namespace Extended {
oox_var<size_t> disk_usage(INode &node)
{
    if (node.is_file())
        return node.size();
    typedef tbb::concurrent_vector<size_t> cv_t;
    cv_t *v = new cv_t;
    std::vector<oox_node> tasks;
    for (auto &subnode : *node)
        tasks.push_back(oox_run( [v](size_t s){ v->push_back(s); }, oox_run(disk_usage, std::ref(subnode))) );
    auto join_node = oox_join( tasks.begin(), tasks.end() );
    return oox_run( join_node,              // use explicit flow-dependence
        [v]{
            size_t res = std::accumulate(v->begin(), v->end(), 0);
            delete v;                       // release memory
            return res;
        });
}
}//namespace Filesystem::Extended
#endif
}//namespace Filesystem

namespace Wavefront_LCS {
static const int MAX_LEN = 1000;
const char input1[] = "has 10000 line input, and each line has ave 3000 chars (include space). It took me  50 mins to find lcs. the requirement is not exceed 5 mins.";
const char input2[] = "has 100 line input, and each line has ave 60k chars. It never finish running";

namespace Serial {
int LCS( const char* x, size_t xlen, const char* y, size_t ylen )
{
    int F[MAX_LEN+1][MAX_LEN+1];

    for( size_t i=1; i<=xlen; ++i )
        for( size_t j=1; j<=ylen; ++j )
            F[i][j] = x[i-1]==y[j-1] ? F[i-1][j-1]+1 : max(F[i][j-1],F[i-1][j]);
    return F[xlen][ylen];
}}

namespace Straight {
int LCS( const char* x, size_t xlen, const char* y, size_t ylen )
{
    oox_var<int> F[MAX_LEN+1][MAX_LEN+1];
    oox_var<int> zero(0);
    auto f = [x,y,xlen,ylen](int i, int j, int F11, int F01, int F10) {
        return x[i-1]==y[j-1] ? F11+1 : max(F01, F10);
    };

    for( size_t i=1; i<=xlen; ++i )
        for( size_t j=1; j<=ylen; ++j )
            F[i][j] = oox_run(f, i, j, i+j==0?zero:F[i-1][j-1], j==0?zero:F[i][j-1], i==0?zero:F[i-1][j]);

    return oox_wait_and_get(F[xlen][ylen]);
}}

struct Range2D : tbb::blocked_range2d<int,int> {
    using tbb::blocked_range2d<int,int>::blocked_range2d;
    bool is_divisible4() {
        return rows().is_divisible() && cols().is_divisible();
    }
    Range2D quadrant(bool bottom, bool right) {
        int rmin = rows().begin(), rmax = rows().end(), rmid = (rmin+rmax+1)/2;
        int cmin = cols().begin(), cmax = cols().end(), cmid = (cmin+cmax+1)/2;
        return Range2D(bottom?rmid:rmin, bottom?rmax:rmid, rows().grainsize()
                       ,right?cmid:cmin, right?cmax:cmid, cols().grainsize());
    }
};
#if 0
namespace SimpleRecursive {
struct RecursionBody {
    int (*F)[MAX_LEN+1];
    const char *X, *Y;
    RecursionBody(int f[][MAX_LEN+1], const char* x, const char* y) : F(f), X(x), Y(y) {}
    oox_node operator()(Range2D r) {
        if( !r.is_divisible4() ) {
            for( size_t i=r.rows().begin(), ie=r.rows().end(); i<ie; ++i )
                for( size_t j=r.cols().begin(), je=r.cols().end(); j<je; ++j )
                    F[i][j] = X[i-1]==Y[j-1] ? F[i-1][j-1]+1 : max(F[i][j-1], F[i-1][j]);
            return oox_node();
        }
        oox_node node00 = oox_run( *this, r.quadrant(0,0) );
        oox_node node10 = oox_run( *this, r.quadrant(1,0), node00 );
        oox_node node01 = oox_run( *this, r.quadrant(0,1), node00 );
        return oox_run( *this, r.quadrant(1,1), node10, node01 );
    }
};
void LCS( const char* x, size_t xlen, const char* y, size_t ylen ) {
    int F[MAX_LEN+1][MAX_LEN+1];
    RecursionBody rf(F, x, y);

    oox_wait_and_get( rf(Range2D(0,xlen,0,ylen)) );
}}
#endif

void test() {
    int lcs0 = Serial::LCS(input1, sizeof(input1), input2, sizeof(input2));
    //int lcs1 = Straight::LCS(input1, sizeof(input1), input2, sizeof(input2));
    //REMARK("LCS:\t%d %d\n", lcs0, lcs1);
}
}//namespace Wavefront_LCS

int plus(int a, int b) { return a+b; }

int TestMain() {
    tbb::task_scheduler_init blocking( tbb::task_scheduler_init::default_num_threads(), 0, true);

    if(1) {
        const oox_var<int> a = oox_run(plus, 2, 3);
        oox_var<int> b = oox_run(plus, 1, a);
        REMARK("Simple:\t%d %d\n", oox_wait_and_get(a), oox_wait_and_get(b));
    }
    if(1) {
        int arch = ArchSample::test();
        REMARK("Sample:\t%d\n", arch);
    }
    if(1) {
        int x = 30;
        int fib0 = Fib0::Fib(x);
        int fib1 = oox_wait_and_get(Fib1::Fib(x));
        int fib2 = oox_wait_and_get(Fib2::Fib(x));
        TRACE("Fib:\t%d %d %d\n", fib0, fib1, fib2);
        ASSERT(fib0 == fib1 && fib1 == fib2, "");
    }
    if(1) {
        int files0 = Filesystem::Serial::disk_usage(Filesystem::tree[0]);
        int files1 = oox_wait_and_get(Filesystem::Simple::disk_usage(Filesystem::tree[0]));
        int files2 = oox_wait_and_get(Filesystem::AntiDependence::disk_usage(Filesystem::tree[0]));
        REMARK("Files:\t%d %d %d\n", files0, files1, files2);
        ASSERT(files0 == files1 && files1 == files2, "");
    }
    if(1)
        Wavefront_LCS::test();

    return Harness::Done;
}
