#ifndef _NDB_TUPLE_H_
#define _NDB_TUPLE_H_

#include <atomic>
#include <vector>
#include <string>
#include <utility>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <ostream>
#include <thread>

#include "amd64.h"
#include "core.h"
#include "counter.h"
#include "macros.h"
#include "varkey.h"
#include "util.h"
#include "allocator.h"
#include "thread.h"
#include "spinlock.h"
#include "small_unordered_map.h"
#include "prefetch.h"
#include "ownership_checker.h"

#include "object.h"
#include "dbcore/xid.h"
#include "dbcore/sm-alloc.h"

using namespace TXN;

template <template <typename> class Protocol, typename Traits>
  class transaction; // forward decl

// XXX: hack
extern std::string (*g_proto_version_str)(uint64_t v);

/**
 * A dbtuple is the type of value which we stick
 * into underlying (non-transactional) data structures- it
 * also contains the memory of the value
 */
struct dbtuple {
public:
  typedef uint32_t size_type;
  typedef varstr string_type;

  fat_ptr clsn;     // version creation stamp
#if defined(USE_PARALLEL_SSN) || defined(USE_PARALLEL_SSI)
  typedef unsigned int rl_bitmap_t;  // _builtin_ctz needs it to be uint
  rl_bitmap_t rl_bitmap;   // bitmap of in-flight readers
  uint64_t sstamp;         // successor (overwriter) stamp (\pi), updated when writer commits
  uint64_t xstamp;         // access (reader) stamp (\eta), updated when reader commits
#endif
#ifdef USE_PARALLEL_SSI
  uint64_t s2;  // smallest successor stamp of all reads performed by the tx
                // that clobbered this version
                // Consider a transaction T which clobbers this version, upon
                // commit, T writes its cstamp in sstamp, and the smallest
                // sstamp among all its reads in s2 of this version. This
                // basically means T has clobbered this version, and meantime,
                // some other transaction C clobbered T's read.
                // So [X] r:w T r:w C. If anyone reads this version again,
                // it will become the X in the dangerous structure above
                // and must abort.
#endif
  size_type size; // actual size of record
  uint8_t value_start[0];   // must be last field

private:
  static inline ALWAYS_INLINE size_type
  CheckBounds(size_type s)
  {
    INVARIANT(s <= std::numeric_limits<size_type>::max());
    return s;
  }

  dbtuple(size_type size)
    :
      clsn(NULL_PTR)
#if defined(USE_PARALLEL_SSN) || defined(USE_PARALLEL_SSI)
      , rl_bitmap(rl_bitmap_t(0))
      , sstamp(0)
      , xstamp(0)
#endif
#ifdef USE_PARALLEL_SSI
      , s2(0)
#endif
      , size(CheckBounds(size))
  {
#ifdef USE_PARALLEL_SSN
    // FIXME: seems this assumes some 8-byte alignment, which isn't the
    // case when dbtuple is without those ssn-related fields.
    INVARIANT(((char *)this) + sizeof(*this) == (char *) &value_start[0]);
#endif
    ++g_evt_dbtuple_creates;
  }

  ~dbtuple();

  static event_avg_counter g_evt_avg_dbtuple_read_retries;

public:
  enum ReadStatus {
    READ_FAILED,
    READ_EMPTY,
    READ_RECORD,
  };

  inline void
  prefetch() const
  {
#ifdef TUPLE_PREFETCH
    prefetch_bytes(this, sizeof(*this) + size);
#endif
  }

  inline ALWAYS_INLINE uint8_t *
  get_value_start()
  {
    return &value_start[0];
  }

  inline ALWAYS_INLINE const uint8_t *
  get_value_start() const
  {
    return &value_start[0];
  }

private:

#ifdef ENABLE_EVENT_COUNTERS
  struct scoped_recorder {
    scoped_recorder(unsigned long &n) : n(&n) {}
    ~scoped_recorder()
    {
      g_evt_avg_dbtuple_read_retries.offer(*n);
    }
  private:
    unsigned long *n;
  };
#endif

  static event_counter g_evt_dbtuple_creates;
  static event_counter g_evt_dbtuple_bytes_allocated;
  static event_counter g_evt_dbtuple_bytes_freed;

public:
  template <typename Reader, typename StringAllocator>
  inline ALWAYS_INLINE ReadStatus
  stable_read(Reader &reader, StringAllocator &sa) const
  {
    if (unlikely(size && !reader(get_value_start(), size, sa)))
      return READ_FAILED;
    return size ? READ_RECORD : READ_EMPTY;
  }

  // XXX: kind of hacky, but we do this to avoid virtual
  // functions / passing multiple function pointers around
  enum TupleWriterMode {
    TUPLE_WRITER_NEEDS_OLD_VALUE, // all three args ignored
    TUPLE_WRITER_COMPUTE_NEEDED,
    TUPLE_WRITER_COMPUTE_DELTA_NEEDED, // last two args ignored
    TUPLE_WRITER_DO_WRITE,
    TUPLE_WRITER_DO_DELTA_WRITE,
  };
  typedef size_t (*tuple_writer_t)(TupleWriterMode, const void *, uint8_t *, size_t);

  static inline dbtuple *
  init(char*p ,size_type sz)
  {
    return new (p) dbtuple(sz);
  }
}
#if !defined(CHECK_INVARIANTS)
//PACKED
#endif
;

#endif /* _NDB_TUPLE_H_ */
