#pragma once
#include <string>
#include "sm-defs.h"
class sysconf {
public:
    static uint32_t _active_threads;
    static uint32_t worker_threads;
    static uint32_t threads;
    static int numa_nodes;
    static const uint32_t MAX_THREADS = 1024;
    static uint64_t prefault_gig;
    static int enable_gc;
    static int enable_ssi_read_only_opt;
    static std::string tmpfs_dir;
    static uint64_t ssn_read_opt_threshold;
    static const uint64_t SSN_READ_OPT_DISABLED = 0xffffffffffffffff;

    // XXX(tzwang): enabling safesnap for tpcc basically halves the performance.
    // perf says 30%+ of cycles are on oid_get_version, which makes me suspect
    // it's because enabling safesnap makes the reader has to go deeper in the
    // version chains to find the desired version. So perhaps don't enable this
    // for update-intensive workloads, like tpcc. TPC-E to test and verify.
    static int enable_safesnap;

    static void sanity_check();

    inline static uint32_t my_thread_id() {
        static __thread uint32_t __id = 0;
        if (__id == 0) {
            __id = __sync_fetch_and_add(&_active_threads, 1);
        }
        ASSERT(__id < threads);
        return __id;
    }
    
    static void init();

    static inline bool ssn_read_opt_enabled() {
        return ssn_read_opt_threshold < SSN_READ_OPT_DISABLED;
    }
};
