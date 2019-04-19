/*
 * A YCSB implementation based off of Silo's and equivalent to FOEDUS's.
 */
#include <iostream>
#include <sstream>
#include <vector>
#include <utility>
#include <string>
#include <set>

#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>
#include <numa.h>

#include "bench.h"
#include "ycsb.h"

extern uint64_t local_key_counter[ermia::config::MAX_THREADS];
extern uint g_reps_per_tx;
extern uint g_rmw_additional_reads;
extern char g_workload;
extern uint g_initial_table_size;
extern int g_sort_load_keys;

// { insert, read, update, scan, rmw }
extern YcsbWorkload YcsbWorkloadA;
extern YcsbWorkload YcsbWorkloadB;
extern YcsbWorkload YcsbWorkloadC;
extern YcsbWorkload YcsbWorkloadD;
extern YcsbWorkload YcsbWorkloadE;

// Combine reps_per_tx and rmw_additional_reads to have "10R+10RMW" style
// transactions.
extern YcsbWorkload YcsbWorkloadF;

// Extra workloads (not in spec)
extern YcsbWorkload YcsbWorkloadG;
extern YcsbWorkload YcsbWorkloadH;

extern YcsbWorkload ycsb_workload;

class ycsb_dia_worker : public bench_worker {
 public:
  ycsb_dia_worker(unsigned int worker_id, unsigned long seed, ermia::Engine *db,
              const std::map<std::string, ermia::OrderedIndex *> &open_tables,
              spin_barrier *barrier_a, spin_barrier *barrier_b)
      : bench_worker(worker_id, true, seed, db, open_tables, barrier_a, barrier_b),
        tbl((ermia::DecoupledMasstreeIndex*)open_tables.at("USERTABLE")),
        rnd_record_select(477377 + worker_id) {}

  virtual cmdlog_redo_workload_desc_vec get_cmdlog_redo_workload() const {
    LOG(FATAL) << "Not applicable";
  }

  virtual workload_desc_vec get_workload() const {
    workload_desc_vec w;
    if (ycsb_workload.insert_percent())
      w.push_back(workload_desc(
          "Insert", double(ycsb_workload.insert_percent()) / 100.0, TxnInsert));
    if (ycsb_workload.read_percent())
      w.push_back(workload_desc(
          "Read", double(ycsb_workload.read_percent()) / 100.0, TxnRead));
    if (ycsb_workload.update_percent())
      w.push_back(workload_desc(
          "Update", double(ycsb_workload.update_percent()) / 100.0, TxnUpdate));
    if (ycsb_workload.scan_percent())
      w.push_back(workload_desc(
          "Scan", double(ycsb_workload.scan_percent()) / 100.0, TxnScan));
    if (ycsb_workload.rmw_percent())
      w.push_back(workload_desc(
          "RMW", double(ycsb_workload.rmw_percent()) / 100.0, TxnRMW));
    return w;
  }

  static rc_t TxnInsert(bench_worker *w) {
    MARK_REFERENCED(w);
    return {RC_TRUE};
  }

  static rc_t TxnRead(bench_worker *w) {
    return static_cast<ycsb_dia_worker *>(w)->txn_read();
  }

  static rc_t TxnUpdate(bench_worker *w) {
    MARK_REFERENCED(w);
    return {RC_TRUE};
  }

  static rc_t TxnScan(bench_worker *w) {
    MARK_REFERENCED(w);
    return {RC_TRUE};
  }

  static rc_t TxnRMW(bench_worker *w) {
    return static_cast<ycsb_dia_worker *>(w)->txn_rmw();
  }

  static uint32_t routing(const ermia::varstr *key) {
    uint32_t worker_id = (uint32_t)(*(uint64_t *)key->data() >> 32);
    return worker_id % (ermia::config::dia_logical_index_threads +
                        ermia::config::dia_physical_index_threads);
  }

  // Request result placeholders
  void PrepareForDIA(rc_t **out_rcs, ermia::OID **out_oids) {
    thread_local rc_t *rcs = nullptr;
    thread_local ermia::OID *oids = nullptr;
    uint32_t n = std::max<uint32_t>(g_reps_per_tx, g_rmw_additional_reads);
    if (!rcs) {
      rcs = (rc_t *)malloc(sizeof(rc_t) * n);
      oids = (ermia::OID *)malloc(sizeof(ermia::OID) * n);
    }
    for (uint32_t i = 0; i < n; ++i) {
      rcs[i] = rc_t{RC_INVALID};
      oids[i] = 0;
    }
    *out_rcs = rcs;
    *out_oids = oids;
  }

  rc_t txn_read() {
    ermia::transaction *txn = db->NewTransaction(0, arena, txn_buf());
    arena.reset();

    thread_local std::vector<ermia::varstr *> values;
    values.clear();

    thread_local std::vector<ermia::varstr *> keys;
    keys.clear();

    rc_t *rcs = nullptr;
    ermia::OID *oids = nullptr;
    PrepareForDIA(&rcs, &oids);

    for (uint i = 0; i < g_reps_per_tx; ++i) {
      auto &k = BuildKey(worker_id);
      keys.push_back(&k);
      ermia::varstr &v = str((ermia::config::index_probe_only) ? 0 : sizeof(YcsbRecord));
      values.push_back(&v);

      // TODO(tzwang): add read/write_all_fields knobs
      // FIXME(tzwang): DIA may need to copy the key?
      tbl->SendGet(txn, rcs[i], *keys[i], &oids[i],
                   worker_id)); // Send out async Get request
    }

    for (uint32_t i = 0; i < g_reps_per_tx; ++i) {
      ermia::varstr *r = values[i];
      tbl->RecvGet(txn, rcs[i], oids[i], *r);
      // TODO(tzwang): if we abort here (e.g. because the return value rc says
      // so), it might be beneficial to rescind the subsequent requests that
      // have been sent.
#if !defined(SSI) && !defined(SSN) && !defined(MVOCC)
      // Under SI this must succeed
      ALWAYS_ASSERT(rcs[i]._val == RC_TRUE);
      ASSERT(*(char *)values[i]->data() == 'a');
#endif
      if (!ermia::config::index_probe_only)
        memcpy((char *)values[i] + sizeof(ermia::varstr),
               (char *)values[i]->data(), sizeof(YcsbRecord));
    }

#if defined(SSI) || defined(SSN) || defined(MVOCC)
    for (uint32_t i = 0; i < g_reps_per_tx; ++i) {
      TryCatch(rcs[i]); // Might abort if we use SSI/SSN/MVOCC
    }
#endif

    TryCatch(db->Commit(txn));
    return {RC_TRUE};
  }

  rc_t txn_rmw() {
    ermia::transaction *txn = db->NewTransaction(0, arena, txn_buf());
    arena.reset();

    thread_local std::vector<ermia::varstr *> values;
    values.clear();

    thread_local std::vector<ermia::varstr *> keys;
    keys.clear();

    rc_t *rcs = nullptr;
    ermia::OID *oids = nullptr;
    PrepareForDIA(&rcs, &oids);

    // Issue all reads
    for (uint i = 0; i < g_reps_per_tx; ++i) {
      auto &k = BuildKey(worker_id);
      keys.push_back(&k);
      values.push_back(&str(sizeof(YcsbRecord)));
      // TODO(tzwang): add read/write_all_fields knobs
      tbl->SendGet(txn, rcs[i], *keys[i], &oids[i], routing(keys[i]));
    }

    for (uint32_t i = 0; i < g_reps_per_tx; ++i) {
      tbl->RecvGet(txn, rcs[i], oids[i], *values[i]);
#if !defined(SSI) && !defined(SSN) && !defined(MVOCC)
      // Under SI this must succeed
      ASSERT(rcs[i]._val == RC_TRUE);
      ASSERT(*(char *)values[i]->data() == 'a');
#endif
      if (!ermia::config::index_probe_only)
        memcpy((char *)values[i] + sizeof(ermia::varstr),
               (char *)values[i]->data(), sizeof(YcsbRecord));
    }

#if defined(SSI) || defined(SSN) || defined(MVOCC)
    for (uint32_t i = 0; i < g_reps_per_tx; ++i) {
      TryCatch(rcs[i]); // Might abort if we use SSI/SSN/MVOCC
    }
#endif

    for (uint32_t i = 0; i < g_reps_per_tx; ++i) {
      // Reset the return value placeholders
      rcs[i]._val = RC_INVALID;
      oids[i] = 0;

      // Copy to user space and do the write.
      // Re-set the data area here as the previous read (DoTupleRead) has r->p
      // pointing to the object's data area
      ermia::varstr *r = values[i];
      new (r)
          ermia::varstr((char *)r + sizeof(ermia::varstr), sizeof(YcsbRecord));
      memset((char *)r->data(), 'a', sizeof(YcsbRecord));
      tbl->SendPut(txn, rcs[i], *keys[i], &oids[i],
                   routing(keys[i])); // Modify-write

      // TODO(tzwang): similar to read-only case, see if we can rescind
      // subsequent requests for better performance. Note: before we have this
      // feature, we have to first RecvGet all RCs and then do TryCatch.
      // Otherwise because TryCatch will return as long as it sees a 'false' in
      // any RC, we might risk the indexing threads that are still working on
      // previous RecvGets overwriting rc results for the new requests that are
      // reusing RCs.
    }

    // Wait for writes to finish
    for (uint32_t i = 0; i < g_reps_per_tx; ++i) {
      tbl->RecvPut(txn, rcs[i], oids[i], *keys[i], *values[i]);
    }

    for (uint32_t i = 0; i < g_reps_per_tx; ++i) {
      TryCatch(rcs[i]);
    }

    PrepareForDIA(&rcs, &oids);
    keys.clear();
    values.clear();
    for (uint i = 0; i < g_rmw_additional_reads; ++i) {
      auto &k = BuildKey(worker_id);
      keys.push_back(&k);
      if (ermia::config::index_probe_only)
        values.push_back(&str(0));
      else
        values.push_back(&str(sizeof(YcsbRecord)));
      tbl->SendGet(txn, rcs[i], *keys[i], &oids[i], routing(keys[i]));
    }

    for (uint i = 0; i < g_rmw_additional_reads; ++i) {
      tbl->RecvGet(txn, rcs[i], oids[i], *values[i]);
#if !defined(SSI) && !defined(SSN) && !defined(MVOCC)
      // Under SI this must succeed
      ASSERT(rcs[i]._val == RC_TRUE);
      ASSERT(*(char *)values[i]->data() == 'a');
#endif
      if (!ermia::config::index_probe_only)
        memcpy((char *)values[i] + sizeof(ermia::varstr),
               (char *)values[i]->data(), values[i]->size());
    }

#if defined(SSI) || defined(SSN) || defined(MVOCC)
    for (uint i = 0; i < g_rmw_additional_reads; ++i) {
      TryCatch(rcs[i]); // Might abort if we use SSI/SSN/MVOCC
    }
#endif

    TryCatch(db->Commit(txn));
    return {RC_TRUE};
  }

protected:
  ALWAYS_INLINE ermia::varstr &str(uint64_t size) { return *arena.next(size); }


  ermia::varstr &BuildKey(int worker_id) {
    uint32_t r = rnd_record_select.next();
    uint64_t hi = r % ermia::config::worker_threads;
    ASSERT(local_key_counter[worker_id] > 0);
    uint64_t lo = r % local_key_counter[worker_id];
    ermia::varstr &k = str(sizeof(uint64_t));  // 8-byte key
    new (&k) ermia::varstr((char *)&k + sizeof(ermia::varstr), sizeof(uint64_t));
    ::BuildKey(hi, lo, k);
    return k;
  }

private:
  ermia::DecoupledMasstreeIndex *tbl;
  util::fast_random rnd_record_select;
};

class ycsb_dia_usertable_loader : public bench_loader {
 public:
  ycsb_dia_usertable_loader(unsigned long seed, ermia::Engine *db,
                            const std::map<std::string, ermia::OrderedIndex *> &open_tables,
                            uint32_t loader_id)
      : bench_loader(seed, db, open_tables), loader_id(loader_id) {}

 private:
  uint32_t loader_id;

protected:
  // XXX(tzwang): for now this is serial
  void load() {
    local_key_counter[loader_id] = 0;
    int64_t total_inserts = g_initial_table_size / ermia::config::worker_threads;
    int64_t remaining_inserts = total_inserts;
    uint32_t high = loader_id, low = 0;
    std::vector<ermia::varstr*> keys;
    while (remaining_inserts-- > 0) {
      ermia::varstr *key = (ermia::varstr *)malloc(sizeof(ermia::varstr) + sizeof(uint64_t));  // 8-byte key
      new (key) ermia::varstr((char *)key + sizeof(ermia::varstr), sizeof(uint64_t));
      BuildKey(high, low++, *key);
      keys.push_back(key);
      local_key_counter[loader_id]++;
    }

    ALWAYS_ASSERT(keys.size());
    if (g_sort_load_keys) {
      std::sort(keys.begin(), keys.end());
    }

    ermia::DecoupledMasstreeIndex *tbl = (ermia::DecoupledMasstreeIndex*)open_tables.at("USERTABLE");
    // start a transaction and insert all the records
    for (auto &key : keys) {
      arena.reset();
      ermia::varstr &v = str(sizeof(uint64_t));
      new (&v) ermia::varstr((char *)&v + sizeof(ermia::varstr), sizeof(YcsbRecord));
      *(char *)v.p = 'a';
      ermia::transaction *txn = db->NewTransaction(0, arena, txn_buf());

      rc_t rc = rc_t{RC_INVALID};
      ermia::OID oid = 0;
      ermia::dbtuple *tuple = nullptr;
      tbl->SendInsert(txn, rc, *key, v, &oid, &tuple,
                      ycsb_dia_worker::routing(key));
      ASSERT(tuple);
      tbl->RecvInsert(txn, rc, oid, *key, v, tuple);
      TryVerifyStrict(rc);

      TryVerifyStrict(db->Commit(txn));
    }

    // Verify inserted values
    for (auto &key : keys) {
      ermia::transaction *txn = db->NewTransaction(0, arena, txn_buf());
      arena.reset();
      rc_t rc = rc_t{RC_INVALID};
      ermia::OID oid = 0;
      ermia::varstr &v = str(0);
      tbl->Get(txn, rc, *key, v, &oid);
      ALWAYS_ASSERT(*(char*)v.data() == 'a');
      TryVerifyStrict(rc);
      TryVerifyStrict(db->Commit(txn));
      free(key);
    }

    if (ermia::config::verbose) {
      std::cerr << "[INFO] loader " << loader_id <<  " loaded "
                << total_inserts << " kyes in USERTABLE" << std::endl;
    }
  }
};

class ycsb_dia_bench_runner : public bench_runner {
public:
  ycsb_dia_bench_runner(ermia::Engine *db) : bench_runner(db) {
    db->CreateMasstreeTable("USERTABLE", true /* decoupled index access */);
  }

  virtual void prepare(char *) {
    open_tables["USERTABLE"] = ermia::IndexDescriptor::GetIndex("USERTABLE");
  }

protected:
  virtual std::vector<bench_loader *> make_loaders() {
    uint64_t requested = g_initial_table_size;
    uint64_t records_per_thread = std::max<uint64_t>(1, g_initial_table_size / ermia::config::worker_threads);
    g_initial_table_size = records_per_thread * ermia::config::worker_threads;

    if (ermia::config::verbose) {
      std::cerr << "[INFO] requested for " << requested
           << " records, will load "
           << records_per_thread *ermia::config::worker_threads << std::endl;
    }

    std::vector<bench_loader *> ret;
    for (uint32_t i = 0; i < ermia::config::worker_threads; ++i) {
      ret.push_back(new ycsb_dia_usertable_loader(0, db, open_tables, i));
    }
    return ret;
  }

  virtual std::vector<bench_worker *> make_cmdlog_redoers() {
    // Not implemented
    std::vector<bench_worker *> ret;
    return ret;
  }
  virtual std::vector<bench_worker *> make_workers() {
    util::fast_random r(8544290);
    std::vector<bench_worker *> ret;
    for (size_t i = 0; i < ermia::config::worker_threads; i++) {
      ret.push_back(new ycsb_dia_worker(i, r.next(), db, open_tables,
                                        &barrier_a, &barrier_b));
    }
    return ret;
  }
};

void ycsb_dia_do_test(ermia::Engine *db, int argc, char **argv) {
  // parse options
  optind = 1;
  while (1) {
    static struct option long_options[] = {
        {"reps-per-tx", required_argument, 0, 'r'},
        {"rmw-additional-reads", required_argument, 0, 'a'},
        {"workload", required_argument, 0, 'w'},
        {"initial-table-size", required_argument, 0, 's'},
        {"sort-load-keys", no_argument, &g_sort_load_keys, 1},
        {0, 0, 0, 0}};

    int option_index = 0;
    int c = getopt_long(argc, argv, "r:a:w:s:", long_options, &option_index);
    if (c == -1)
      break;
    switch (c) {
    case 0:
      if (long_options[option_index].flag != 0)
        break;

    case 'r':
      g_reps_per_tx = strtoul(optarg, NULL, 10);
      break;

    case 'a':
      g_rmw_additional_reads = strtoul(optarg, NULL, 10);
      break;

    case 's':
      g_initial_table_size = strtoul(optarg, NULL, 10);
      break;

    case 'w':
      g_workload = optarg[0];
      if (g_workload == 'A')
        ycsb_workload = YcsbWorkloadA;
      else if (g_workload == 'B')
        ycsb_workload = YcsbWorkloadB;
      else if (g_workload == 'C')
        ycsb_workload = YcsbWorkloadC;
      else if (g_workload == 'D')
        ycsb_workload = YcsbWorkloadD;
      else if (g_workload == 'E')
        ycsb_workload = YcsbWorkloadE;
      else if (g_workload == 'F')
        ycsb_workload = YcsbWorkloadF;
      else if (g_workload == 'G')
        ycsb_workload = YcsbWorkloadG;
      else if (g_workload == 'H')
        ycsb_workload = YcsbWorkloadH;
      else {
        std::cerr << "Wrong workload type: " << g_workload << std::endl;
        abort();
      }
      break;

    case '?':
      /* getopt_long already printed an error message. */
      exit(1);

    default:
      abort();
    }
  }

  ALWAYS_ASSERT(g_initial_table_size);

  if (ermia::config::verbose) {
    std::cerr << "ycsb settings:" << std::endl
              << "  workload:                   " << g_workload << std::endl
              << "  initial user table size:    " << g_initial_table_size
              << std::endl
              << "  operations per transaction: " << g_reps_per_tx << std::endl
              << "  additional reads after RMW: " << g_rmw_additional_reads
              << std::endl
              << "  sort load keys:             " << g_sort_load_keys
              << std::endl;
  }

  ycsb_dia_bench_runner r(db);
  r.run();
}
