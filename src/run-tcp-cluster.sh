#!/bin/bash 
# Note: adjust segment size to 16GB - TCP backup currently doesn't
# support creating new segments like the RDMA variant does

primary="192.168.1.106"
declare -a backups=(192.168.1.101 192.168.1.104 192.168.1.107 192.168.1.102 192.168.1.105 192.168.1.100 192.168.1.103)

function cleanup {
  killall -9 ermia_SI 2> /dev/null
  for b in "${backups[@]}"; do
    echo "Kill $b"
    ssh $b "killall -9 ermia_SI 2> /dev/null"
  done
}

trap cleanup EXIT

run() {
  num_backups=$1
  t=$2
  policy=$3
  full=$4
  redoers=$5
  delay=$6
  nvram=$7
  persist_nvram_on_replay=$8

  logbuf_mb=16 #$9
  group_commit_size_mb=4 #${10}

  unset GLOG_logtostderr

  echo "----------"
  echo backups:$num_backups thread:$t $policy full_redo=$full redoers=$redoers delay=$delay nvram_log_buffer=$nvram group_commit_size_mb=$group_commit_size_mb
  echo "----------"
  ./run-cluster.sh SI $t 10 $t $logbuf_mb tpcc_org tpccr \
    "-group_commit -group_commit_size_mb=$group_commit_size_mb -chkpt_interval=1000000 -node_memory_gb=19 -log_ship_by_rdma=0 -fake_log_write -wait_for_backups -num_backups=$num_backups -persist_policy=sync" \
    "-primary_host=$primary -node_memory_gb=20 -log_ship_by_rdma=0 -nvram_log_buffer=$nvram -quick_bench_start -wait_for_primary -replay_policy=$policy -full_replay=$full -replay_threads=$redoers -nvram_delay_type=$delay -persist_nvram_on_replay=$persist_nvram_on_replay" \
    "${backups[@]:0:$num_backups}"
  echo
}

# Absolute the baseline - sync shipping, sync replay, single replay thread, full replay, no NVRAM
all_sync() {
  for num_backups in 1 2 3 4 5 6 7; do
    run $num_backups 16 sync 1 1 none 0 0
  done
  for t in 1 2 4 8; do
    run 1 $t sync 1 1 none 0 0
  done
}

# Still baseline, but with parallel replay
parallel_replay() {
  for num_backups in 1 2 3 4 5 6 7; do
    for nredoers in 1 2 4 8 16; do
      run $num_backups 16 sync 1 $nredoers none 0 0
    done
  done

  nredoers=1
  for t in 1 2 4 8; do
    run 1 $t sync 1 $nredoers none 0 0
  done

  nredoers=2
  for t in 2 4 8; do
    run 1 $t sync 1 $nredoers none 0 0
  done

  nredoers=4
  for t in 4 8; do
    run 1 $t sync 1 $nredoers none 0 0
  done

  nredoers=8
  t=8
  run 1 $t sync 1 $nredoers none 0 0
}

for r in 1 2 3; do
  echo "Running all_sync r$r"
  all_sync
done

for r in 1 2 3; do
  echo "Running parallel_replay r$r"
  parallel_replay
done

