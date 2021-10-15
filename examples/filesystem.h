// Copyright (C) 2021 Intel Corporation
//
// SPDX-License-Identifier: Apache-2.0

#include <oox/oox.h>

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

oox::var<size_t> disk_usage(INode& node)
{
    if (node.is_file())
        return node.size();
    oox::var<size_t> sum = 0;
    for (auto &subnode : node)
        oox::run([](size_t &sm, // serialized on write operation
                 size_t sz){ sm += sz; }, sum, oox::run(disk_usage, std::ref(subnode))); // parallel recursive leaves
    return sum;
}

}//namespace Filesystem::Simple
#if HAVE_TBB
}//namespace Filesystem

#include <tbb/concurrent_vector.h>
#include <functional>
#include <numeric>
#include <vector>

namespace Filesystem {
namespace AntiDependence {

oox::var<size_t> disk_usage(INode& node)
{
    if (node.is_file())
        return node.size();
    typedef tbb::concurrent_vector<size_t> cv_t;
    typedef cv_t *cv_ptr_t;
    oox::var<cv_ptr_t> resv = new cv_t;
    for (auto &subnode : node)
        oox::run([](const cv_ptr_t &v, // make it read-only (but not copyable) and thus parallel
                            size_t s){ v->push_back(s); }, resv, oox::run(disk_usage, std::ref(subnode)));
    return oox::run([](cv_ptr_t &v) { // make it read-write to induce anti-dependence from the above read-only tasks
        size_t res = std::accumulate(v->begin(), v->end(), 0);
        delete v; v = nullptr;      // release memory, reset the pointer
        return res;
    }, resv);
}

}//namespace Filesystem::AntiDependence
namespace Extended {
#if 0
oox::var<size_t> disk_usage(INode &node)
{
    if (node.is_file())
        return node.size();
    typedef tbb::concurrent_vector<size_t> cv_t;
    cv_t *v = new cv_t;
    std::vector<oox::node> tasks;
    for (auto &subnode : *node)
        tasks.push_back(oox::run( [v](size_t s){ v->push_back(s); }, oox::run(disk_usage, std::ref(subnode))) );
    auto join_node = oox::join( tasks.begin(), tasks.end() );
    return oox::run( join_node,              // use explicit flow-dependence
        [v]{
            size_t res = std::accumulate(v->begin(), v->end(), 0);
            delete v;                       // release memory
            return res;
        });
}
#endif
}//namespace Filesystem::Extended
#endif // HAVE_TBB
}//namespace Filesystem
