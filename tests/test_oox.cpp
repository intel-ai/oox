#include <gtest/gtest.h>

#include <iostream>
#include <numeric>
#include <vector>
//#include <tbb/concurrent_vector.h>

bool g_oox_verbose = false;
#define println(s, ...) printf(s "\n",  __VA_ARGS__)
#define __OOX_TRACE if (g_oox_verbose) println
//#define __OOX_ASSERT(b, m) ASSERT_TRUE(b) << (m)
//#define __OOX_ASSERT_EX(b, m) ASSERT_TRUE(b) << (m)

#include <oox/oox.h>

#define REMARK println
#define ASSERT(b, m) ASSERT_TRUE(b) << (m)

/////////////////////////////////////// EXAMPLES ////////////////////////////////////////

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
    // Serial
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
#if 0
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
#endif
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
static const int MAX_LEN = 200;
const char input1[] = "has 10000 line input, and each line has ave 3000 chars (include space). It took me  50 mins to find lcs. the requirement is not exceed 5 mins.";
const char input2[] = "has 100 line input, and each line has ave 60k chars. It never finish running";

namespace Serial {
int LCS( const char* x, size_t xlen, const char* y, size_t ylen )
{
    int F[MAX_LEN+1][MAX_LEN+1];

    for( size_t i=1; i<=xlen; ++i )
        for( size_t j=1; j<=ylen; ++j )
            F[i][j] = x[i-1]==y[j-1] ? F[i-1][j-1]+1 : std::max(F[i][j-1],F[i-1][j]);
    return F[xlen][ylen];
}}

namespace Straight {
int LCS( const char* x, size_t xlen, const char* y, size_t ylen )
{
    auto F = new oox_var<int>[MAX_LEN+1][MAX_LEN+1];
    oox_var<int> zero(0);
    auto f = [x,y,xlen,ylen](int i, int j, int F11, int F01, int F10) {
        return x[i-1]==y[j-1] ? F11+1 : std::max(F01, F10);
    };

    for( size_t i=1; i<=xlen; ++i )
        for( size_t j=1; j<=ylen; ++j )
            F[i][j] = oox_run(f, i, j, i+j==0?zero:F[i-1][j-1], j==0?zero:F[i][j-1], i==0?zero:F[i-1][j]);

    auto r = oox_wait_and_get(F[xlen][ylen]);
    delete [] F;
    return r;
}}

#if 0
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
    int lcs1 = Straight::LCS(input1, sizeof(input1), input2, sizeof(input2));
    REMARK("LCS:\t%d %d\n", lcs0, lcs1);
}
}//namespace Wavefront_LCS

int plus(int a, int b) { return a+b; }


TEST(OOX, Simple) {
    const oox_var<int> a = oox_run(plus, 2, 3);
    oox_var<int> b = oox_run(plus, 1, a);
    REMARK("Simple:\t%d %d\n", oox_wait_and_get(a), oox_wait_and_get(b));
}
TEST(OOX, DISABLED_Empty) { // TODO!
    oox_var<int> a;
    oox_var<int> b = oox_run(plus, 1, a);
    //a = 2;
    // optional, future, ref?
    REMARK("Simple:\t%d %d\n", oox_wait_and_get(a), oox_wait_and_get(b));
}
TEST(OOX, Arch) {
    int arch = ArchSample::test();
    REMARK("Sample:\t%d\n", arch);
}
TEST(OOX, Fib) {
    int x = 20;
    int fib0 = Fib0::Fib(x);
    int fib1 = oox_wait_and_get(Fib1::Fib(x));
    int fib2 = oox_wait_and_get(Fib2::Fib(x));
    REMARK("Fib:\t%d %d %d\n", fib0, fib1, fib2);
    ASSERT(fib0 == fib1 && fib1 == fib2, "");
}
TEST(OOX, FS) {
    int files0 = Filesystem::Serial::disk_usage(Filesystem::tree[0]);
    int files1 = oox_wait_and_get(Filesystem::Simple::disk_usage(Filesystem::tree[0]));
    //int files2 = oox_wait_and_get(Filesystem::AntiDependence::disk_usage(Filesystem::tree[0]));
    REMARK("Files:\t%d %d\n", files0, files1);
    //REMARK("Files:\t%d %d %d\n", files0, files1, files2);
    //ASSERT(files0 == files1 && files1 == files2, "");
}
TEST(OOX, Wavefront) {
    Wavefront_LCS::test();
}


int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);

    int err{0};
    try {
        err = RUN_ALL_TESTS();
    } catch (const std::exception& e) {
        REMARK("Error: %s", e.what());
    }
    return err;
}
