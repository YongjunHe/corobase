#include <benchmark/benchmark.h>

#include <dbcore/sm-alloc.h>
#include <dbcore/sm-config.h>
#include <dbcore/sm-coroutine.h>

void ermia_init() {
    ermia::config::node_memory_gb = 10;
    ermia::config::num_backups = 0;

    // FIXME: hack to cover all the numa nodes
    ermia::config::numa_spread = true;
    ermia::config::physical_workers_only = false;
    ermia::config::threads = 4;

    // Enable profiling
    ermia::config::enable_perf = true;
    ermia::config::perf_record_event = "cycles";

    ermia::config::init();
    ermia::MM::prepare_node_memory();
}

int main(int argc, char** argv) {
    ::benchmark::Initialize(&argc, argv);

    ermia_init();
    // FIXME:
    ermia::dia::coro_task_private::memory_pool memory_pool;
    ::benchmark::RunSpecifiedBenchmarks();
}

