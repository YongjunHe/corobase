#ifndef _NDB_WRAPPER_H_
#define _NDB_WRAPPER_H_

#include "abstract_db.h"
#include "../txn_btree.h"
#include "../dbcore/sm-log.h"

namespace private_ {
  struct ndbtxn {
    abstract_db::TxnProfileHint hint;
    char buf[0];
  } PACKED;

  // XXX: doesn't check to make sure you are passing in an ndbtx
  // of the right hint
  template <template <typename> class Transaction, typename Traits>
  struct cast_base {
    typedef Transaction<Traits> type;
    inline ALWAYS_INLINE type *
    operator()(struct ndbtxn *p) const
    {
      return reinterpret_cast<type *>(&p->buf[0]);
    }
  };

  STATIC_COUNTER_DECL(scopedperf::tsc_ctr, ndb_get_probe0, ndb_get_probe0_cg)
  STATIC_COUNTER_DECL(scopedperf::tsc_ctr, ndb_put_probe0, ndb_put_probe0_cg)
  STATIC_COUNTER_DECL(scopedperf::tsc_ctr, ndb_insert_probe0, ndb_insert_probe0_cg)
  STATIC_COUNTER_DECL(scopedperf::tsc_ctr, ndb_scan_probe0, ndb_scan_probe0_cg)
  STATIC_COUNTER_DECL(scopedperf::tsc_ctr, ndb_remove_probe0, ndb_remove_probe0_cg)
  STATIC_COUNTER_DECL(scopedperf::tsc_ctr, ndb_dtor_probe0, ndb_dtor_probe0_cg)
}

template <template <typename> class Transaction>
class ndb_wrapper : public abstract_db {
protected:
  typedef private_::ndbtxn ndbtxn;
  template <typename Traits>
    using cast = private_::cast_base<Transaction, Traits>;

public:

  ndb_wrapper(const char *logdir, size_t segsize, size_t bufsize);

  virtual ssize_t txn_max_batch_size() const OVERRIDE { return 100; }

  virtual void
  do_txn_epoch_sync() const
  {
  }

  virtual void
  do_txn_finish() const
  {
  }

  virtual void
  thread_init(bool loader)
  {
  }

  virtual void
  thread_end()
  {
  }

  virtual size_t
  sizeof_txn_object(uint64_t txn_flags) const;

  virtual void *new_txn(
      uint64_t txn_flags,
      str_arena &arena,
      void *buf,
      TxnProfileHint hint);
  virtual rc_t commit_txn(void *txn);
  virtual void abort_txn(void *txn);
  virtual void print_txn_debug(void *txn) const;

  virtual abstract_ordered_index *
  open_index(const std::string &name,
             size_t value_size_hint,
             bool mostly_append);

  virtual void
  close_index(abstract_ordered_index *idx);

};

template <template <typename> class Transaction>
class ndb_ordered_index : public abstract_ordered_index {
protected:
  typedef private_::ndbtxn ndbtxn;
  template <typename Traits>
    using cast = private_::cast_base<Transaction, Traits>;

public:
  ndb_ordered_index(const std::string &name, size_t value_size_hint, bool mostly_append);
  virtual rc_t get(
      void *txn,
      const std::string &key,
      std::string &value, size_t max_bytes_read);
  virtual rc_t put(
      void *txn,
      const std::string &key,
      const std::string &value);
  virtual rc_t put(
      void *txn,
      std::string &&key,
      std::string &&value);
  virtual rc_t
  insert(void *txn,
         const std::string &key,
         const std::string &value);
  virtual rc_t
  insert(void *txn,
         std::string &&key,
         std::string &&value);
  virtual rc_t scan(
      void *txn,
      const std::string &start_key,
      const std::string *end_key,
      scan_callback &callback,
      str_arena *arena);
  virtual rc_t rscan(
      void *txn,
      const std::string &start_key,
      const std::string *end_key,
      scan_callback &callback,
      str_arena *arena);
  virtual rc_t remove(
      void *txn,
      const std::string &key);
  virtual rc_t remove(
      void *txn,
      std::string &&key);
  virtual size_t size() const;
  virtual std::map<std::string, uint64_t> clear();
private:
  std::string name;
  txn_btree<Transaction> btr;
};

#endif /* _NDB_WRAPPER_H_ */
