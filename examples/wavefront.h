// Copyright (C) 2021 Intel Corporation
//
// SPDX-License-Identifier: Apache-2.0

#include <oox/oox.h>

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
    auto F = new oox::var<int>[MAX_LEN+1][MAX_LEN+1];
    oox::var<int> zero(0);
    auto f = [x,y,xlen,ylen](int i, int j, int F11, int F01, int F10) {
        return x[i-1]==y[j-1] ? F11+1 : std::max(F01, F10);
    };

    for( size_t i=1; i<=xlen; ++i )
        for( size_t j=1; j<=ylen; ++j )
            F[i][j] = std::move(oox::run(f, i, j, i+j==0?zero:F[i-1][j-1], j==0?zero:F[i][j-1], i==0?zero:F[i-1][j]));

    auto r = oox::wait_and_get(F[xlen][ylen]);
    delete [] F;
    return r;
}

}

#if 0 // TODO: HAVE_TBB
} //namespace Wavefront_LCS
#include <tbb/blocked_range2d.h>
#include <algorithm>
namespace Wavefront_LCS {

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
    oox::node operator()(Range2D r) {
        if( !r.is_divisible4() ) {
            for( size_t i=r.rows().begin(), ie=r.rows().end(); i<ie; ++i )
                for( size_t j=r.cols().begin(), je=r.cols().end(); j<je; ++j )
                    F[i][j] = X[i-1]==Y[j-1] ? F[i-1][j-1]+1 : std::max(F[i][j-1], F[i-1][j]);
            return oox::node();
        }
        oox::node node00 = oox::run( *this, r.quadrant(0,0) );
        oox::node node10 = oox::run( *this, r.quadrant(1,0), node00 ); // TODO: implement additional deps for oox_run
        oox::node node01 = oox::run( *this, r.quadrant(0,1), node00 );
        return oox::run( *this, r.quadrant(1,1), node10, node01 );
    }
};

void LCS( const char* x, size_t xlen, const char* y, size_t ylen ) {
    int F[MAX_LEN+1][MAX_LEN+1];
    RecursionBody rf(F, x, y);

    oox::wait_and_get( rf(Range2D(0,xlen,0,ylen)) );
}

}
#endif
}//namespace Wavefront_LCS
