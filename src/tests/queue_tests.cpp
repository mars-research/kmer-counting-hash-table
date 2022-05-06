#include <algorithm>
#include <cassert>
#include <cinttypes>
#include <tuple>

#include "tests/QueueTest.hpp"
#include "fastrange.h"
#include "hasher.hpp"
#include "hashtables/simple_kht.hpp"
#include "helper.hpp"
#include "misc_lib.h"
#include "print_stats.h"
#include "sync.h"
#include "queues/lynxq.hpp"
#include "queues/section_queues.hpp"
#include "queues/bqueue_aligned.hpp"
#include "xorwow.hpp"

#if defined(BQ_TESTS_INSERT_ZIPFIAN)
#include "hashtables/ht_helper.hpp"
#include "zipf.h"
#include "zipf_distribution.hpp"
#endif

#include "utils/vtune.hpp"

#define PGROUNDDOWN(x)  (x & ~(PAGESIZE - 1))

namespace kmercounter {

using namespace std;

const uint64_t CACHELINE_SIZE = sysconf(_SC_LEVEL1_DCACHE_LINESIZE);
const uint64_t CACHELINE_MASK = CACHELINE_SIZE - 1;
const uint64_t PAGESIZE = sysconf(_SC_PAGESIZE);

void setup_signal_handler(void);

extern uint64_t HT_TESTS_HT_SIZE;
extern uint64_t HT_TESTS_NUM_INSERTS;

// Test Variables
[[maybe_unused]] static uint64_t transactions = 100000000;

const unsigned BQ_TESTS_DEQUEUE_ARR_LENGTH = HT_TESTS_BATCH_LENGTH;
const unsigned BQ_TESTS_BATCH_LENGTH_PROD = 1;
const unsigned BQ_TESTS_BATCH_LENGTH_CONS = BQ_TESTS_DEQUEUE_ARR_LENGTH;

// for synchronization of threads
static uint64_t ready = 0;
static uint64_t ready_threads = 0;

extern BaseHashTable *init_ht(uint64_t, uint8_t);
extern void get_ht_stats(Shard *, BaseHashTable *);

struct bq_kmer {
  char data[KMER_DATA_LENGTH];
} __attribute__((aligned(64)));

static struct bq_kmer bq_kmers[BQ_TESTS_DEQUEUE_ARR_LENGTH];

// thread-local since we have multiple consumers
static __thread int data_idx = 0;
static __thread uint64_t keys[BQ_TESTS_DEQUEUE_ARR_LENGTH];
__attribute__((
    aligned(64))) static __thread Keys _items[BQ_TESTS_DEQUEUE_ARR_LENGTH] = {0};

inline std::tuple<double, uint64_t, uint64_t> get_params(uint32_t n_prod,
                                                         uint32_t n_cons,
                                                         uint32_t tid) {
  // In a non-bqueue setting, num-threads = t, then we will have
  // t hashtables of HT_TESTS_HT_SIZE buckets each.
  // However, for bqueue setting where we have different number of
  // producers and consumers, we cannot create hashtable with the same
  // N buckets as it won't be comparable. Why? Because, some of the
  // threads are assigned as producers and they won't have any hash tables.
  // In the end, we want to compare M insertions in total for both bqueue and
  // non-bqueue setting.
  auto ratio = static_cast<double>(n_prod) / n_cons + 1;

  auto num_messages = HT_TESTS_NUM_INSERTS / n_prod;
  if (tid == (n_prod - 1)) {
    num_messages += HT_TESTS_NUM_INSERTS % n_prod;
  }
  // our HT has a notion of empty keys which is 0. So, no '0' key for now!
  uint64_t key_start =
      std::max(static_cast<uint64_t>(num_messages) * tid, (uint64_t)1);
  return std::make_tuple(ratio, num_messages, key_start);
}

static auto hash_to_cpu(std::uint32_t hash, unsigned int count) {
  return fastrange32(_mm_crc32_u32(0xffffffff, hash), count);
};

static auto get_current_node() { return numa_node_of_cpu(sched_getcpu()); };

static auto mbind_buffer_local(void *buf, ssize_t sz) {
  unsigned long nodemask[4096] = { 0 };
  ssize_t page_size = sysconf(_SC_PAGESIZE);
  nodemask[0] = 1 << get_current_node();
  PLOGV.printf("nodemask %x", nodemask[0]);
  long ret =
      mbind(buf, std::max(sz, page_size), MPOL_BIND, nodemask, 4096, MPOL_MF_MOVE | MPOL_MF_STRICT);
  if (ret < 0) {
    PLOGE.printf("mbind failed! ret %d (errno %d)", ret, errno);
  }
  return ret;
}


template <typename T>
void QueueTest<T>::producer_thread(const uint32_t tid, const uint32_t n_prod,
                                 const uint32_t n_cons, const bool main_thread,
                                 const double skew) {
  // Get shard pointer from the shards array
  Shard *sh = &this->shards[tid];

  // Allocate memory for stats
  sh->stats = (thread_stats *)calloc(1, sizeof(thread_stats));
  alignas(64) uint64_t k = 0;

  uint8_t this_prod_id = sh->shard_idx;
  uint32_t cons_id = 0;
  uint64_t transaction_id;
  typename T::prod_queue_t *pqueues[n_cons];
  typename T::cons_queue_t *cqueues[n_cons];

  vtune::set_threadname("producer_thread" + std::to_string(tid));

  auto [ratio, num_messages, key_start] = get_params(n_prod, n_cons, tid);
  Hasher hasher;
#define CONFIG_NUMA_AFFINITY

  for (auto i = 0u; i < n_cons; i++) {
    pqueues[i] = &this->queues->all_pqueues[this_prod_id][i];
#ifdef CONFIG_NUMA_AFFINITY
    mbind_buffer_local((void*)PGROUNDDOWN((uint64_t)pqueues[i]), sizeof(typename T::prod_queue_t));
    //mbind_buffer_local(pqueues[i]->data, this->queues->queue_size);
#endif
   }

  struct xorwow_state _xw_state, init_state;
  auto key_start_orig = key_start;

#if defined(BQ_TESTS_INSERT_ZIPFIAN)
  constexpr auto keyrange_width = (1ull << 31);  // 192 * (1 << 20);
#ifdef ZIPF_FAST
  zipf_distribution_apache distribution(keyrange_width, skew);
#else
  zipf_distribution distribution{skew, num_messages, this_prod_id + 1};
#endif

  std::vector<std::uint64_t> values(num_messages);
  for (auto &value : values) {
#ifdef ZIPF_FAST
    value = distribution.sample();
#else
    value = distribution();
#endif
  }
#endif

#if defined(XORWOW)
  xorwow_init(&_xw_state);
  init_state = _xw_state;
#endif
  static auto event = -1;
  if (main_thread) {
    // Wait for threads to be ready for test
    while (ready_consumers < n_cons) fipc_test_pause();
    // main thread is a producer, but won't increment!
    while (ready_producers < (n_prod - 1)) fipc_test_pause();

    fipc_test_mfence();

    // Signal begin
    test_ready = 1;
    fipc_test_mfence();
    event = vtune::event_start("message_enq");
  } else {
    fipc_test_FAI(ready_producers);
    while (!test_ready) fipc_test_pause();
    fipc_test_mfence();
  }

  PLOG_DEBUG.printf(
      "[prod:%u] started! Sending %lu messages to %d consumers | "
      "key_start %lu",
      this_prod_id, num_messages, n_cons, key_start);

  auto get_next_cons = [&](auto inc) {
    auto next_cons_id = cons_id + inc;
    if (next_cons_id >= n_cons) next_cons_id = 0;
    return next_cons_id;
  };

  auto t_start = RDTSC_START();

  for (auto j = 0u; j < config.insert_factor; j++) {
    key_start = key_start_orig;
#if defined(XORWOW)
    _xw_state = init_state;
#endif
    for (transaction_id = 0u; transaction_id < num_messages;) {
#if defined(XORWOW)
#warning "Xorwow rand kmer insert"
      const auto value = xorwow(&_xw_state);
      k = value;
#elif defined(BQ_TESTS_INSERT_ZIPFIAN)
#warning "Zipfian insertion"
      if (transaction_id & 7 == 0 && transaction_id + 16 < values.size())
        prefetch_object<false>(&values.at(transaction_id + 16), 64);

      k = values.at(transaction_id);
#else
      k = key_start++;
#endif
      // XXX: if we are testing without insertions, make sure to pick CRC as
      // the hashing mechanism to have reduced overhead
      uint64_t hash_val = hasher(&k, sizeof(k));
      cons_id = hash_to_cpu(hash_val, n_cons);
      // k has the computed hash in upper 32 bits
      // and the actual key value in lower 32 bits
      k |= (hash_val << 32);

      //if (++cons_id >= n_cons) cons_id = 0;

      auto pq = pqueues[cons_id];
      this->queues->enqueue_new(pq, this_prod_id, cons_id, (data_t)k);

      auto npq = pqueues[get_next_cons(1)];
      if (1) {
        if (((uint64_t)npq->enqPtr & 0x3f) == 0) {
          __builtin_prefetch(npq->enqPtr + 8, 1, 3);
        }
      }

      transaction_id++;
    }
  }

  // enqueue halt messages and the consumer automatically knows
  // when to stop
  for (cons_id = 0; cons_id < n_cons; cons_id++) {
    this->queues->push_done(this_prod_id, cons_id);

    PLOG_DEBUG.printf(
        "Prod %d Sending END message to cons %d (transaction %u)",
        this_prod_id, cons_id, transaction_id);
    transaction_id++;
  }

  auto t_end = RDTSCP();

  if (main_thread) {
    vtune::event_end(event);
  }

  sh->stats->enqueue_cycles = (t_end - t_start);
  sh->stats->num_enqueues = transaction_id;

  PLOG_DEBUG.printf("Producer %d -> Sending end messages to all consumers",
                    this_prod_id);
  // main thread will also increment this
  fipc_test_FAI(completed_producers);
}

template <typename T>
void QueueTest<T>::consumer_thread(const uint32_t tid, const uint32_t n_prod,
                                 const uint32_t n_cons,
                                 const uint32_t num_nops) {
  // Get shard pointer from the shards array
  Shard *sh = &this->shards[tid];

  // Allocate memory for stats
  sh->stats =
      //(thread_stats *)std::aligned_alloc(CACHE_LINE_SIZE,
      // sizeof(thread_stats));
      (thread_stats *)calloc(1, sizeof(thread_stats));

  std::uint64_t count{};
  BaseHashTable *kmer_ht = NULL;
  uint8_t finished_producers = 0;
  // alignas(64)
  uint64_t k = 0;
  uint64_t transaction_id = 0;
  uint32_t prod_id = 0;

  uint8_t this_cons_id = sh->shard_idx - n_prod;
  uint64_t inserted = 0u;
  typename T::cons_queue_t *cqueues[n_prod];

  // initialize the local queues array from queue_map
  for (auto i = 0u; i < n_prod; i++) {
    cqueues[i] = &this->queues->all_cqueues[this_cons_id][i];
#ifdef CONFIG_NUMA_AFFINITY
    mbind_buffer_local((void*)PGROUNDDOWN((uint64_t)cqueues[i]), sizeof(typename T::cons_queue_t));
    //mbind_buffer_local(cqueues[i]->data, this->queues->queue_size);
#endif
  }
  vtune::set_threadname("consumer_thread" + std::to_string(tid));

  auto ht_size = config.ht_size / n_cons;

  if constexpr (bq_load == BQUEUE_LOAD::HtInsert) {
    PLOG_INFO.printf("[cons:%u] init_ht id:%d size:%u", this_cons_id,
                   sh->shard_idx, ht_size);
    kmer_ht = init_ht(ht_size, sh->shard_idx);
    (*this->ht_vec)[tid] = kmer_ht;
  }

  fipc_test_FAI(ready_consumers);
  while (!test_ready) fipc_test_pause();
  fipc_test_mfence();

  PLOG_DEBUG.printf("[cons:%u] starting", this_cons_id);

  unsigned int cpu, node;
  getcpu(&cpu, &node);

  PLOGI.printf("[cons:%u] starting tid %u | cpu %u\n", this_cons_id, gettid(), cpu);

  static auto event = -1;
  if (tid == n_prod)
    event = vtune::event_start("message_deq");

  auto t_start = RDTSC_START();

  // Round-robin between 0..n_prod
  prod_id = 0;

  auto active_qmask = 0;

  for (auto i = 0u; i < n_prod; i++) {
    active_qmask |= (1 << i);
  }

  while (finished_producers < n_prod) {
    auto submit_batch = [&](auto num_elements) {
      KeyPairs kp = std::make_pair(num_elements, &_items[0]);

      kmer_ht->insert_batch(kp);
      inserted += kp.first;

      data_idx = 0;
    };

    auto get_next_prod = [&](auto inc) {
      auto next_prod_id = prod_id + inc;
      if (next_prod_id >= n_prod) next_prod_id = 0;
      return next_prod_id;
    };

    if constexpr (bq_load == BQUEUE_LOAD::HtInsert) {
      kmer_ht->prefetch_queue(QueueType::insert_queue);
    }
    auto cq = cqueues[prod_id];

    {
      auto np1 = get_next_prod(1);
      typename T::cons_queue_t *cq1 = cqueues[np1];
      __builtin_prefetch(cq1->deqPtr + 0, 1, 3);
      __builtin_prefetch(cq1->deqPtr + 8, 1, 3);

      auto np2 = get_next_prod(np1 + 1);
      typename T::cons_queue_t *cq2 = cqueues[np2];
      __builtin_prefetch(cq2, 1, 3);
    }

    if (!(active_qmask & (1 << prod_id))) {
      goto pick_next_msg;
    }

    for (auto i = 0u; i < 1 * BQ_TESTS_BATCH_LENGTH_CONS; i++) {
      // dequeue one message
      auto ret = this->queues->dequeue_new(cq, prod_id, this_cons_id, (data_t *)&k);
      if (ret == RETRY) {
        if constexpr (bq_load == BQUEUE_LOAD::HtInsert) {
          if (!config.no_prefetch) {
            if (data_idx > 0) {
              submit_batch(data_idx);
            }
          }
        }
        goto pick_next_msg;
      }
      /*
      IF_PLOG(plog::verbose) {
        PLOG_VERBOSE.printf("dequeing from q[%d][%d] value %llu",
                    prod_id, this_cons_id, k & ((1 << 31) - 1));
      }*/
      ++count;

      // STOP condition. On receiving this magic message, the consumers stop
      // dequeuing from the queues
      if ((data_t)k == QueueTest::BQ_MAGIC_64BIT) [[unlikely]] {
        fipc_test_FAI(finished_producers);
        //printf("Got MAGIC bit. stopping consumer\n");
        this->queues->pop_done(prod_id, this_cons_id);
        active_qmask &= ~(1 << prod_id);
        /*PLOG_DEBUG.printf(
            "Consumer %u, received HALT from prod_id %u. "
            "finished_producers :%u",
            this_cons_id, prod_id, finished_producers);
         */

#ifdef CALC_STATS
        PLOG_DEBUG.printf("Consumer experienced %" PRIu64 " reprobes, %" PRIu64
                          " soft",
                          kmer_ht->num_reprobes, kmer_ht->num_soft_reprobes);
#endif

        PLOG_DEBUG.printf("Consumer received %" PRIu64, count);
      }

      if constexpr (bq_load == BQUEUE_LOAD::HtInsert) {
        _items[data_idx].key = _items[data_idx].id = k;
        _items[data_idx].value = k & 0xffffffff;

        // for (auto i = 0u; i < num_nops; i++) asm volatile("nop");

        if (config.no_prefetch) {
          kmer_ht->insert_noprefetch(&k);
          inserted++;
        } else {
          if (++data_idx == BQ_TESTS_DEQUEUE_ARR_LENGTH) {
            submit_batch(BQ_TESTS_DEQUEUE_ARR_LENGTH);
          }
        }
      }

      transaction_id++;
#ifdef CALC_STATS
      /*if (transaction_id % (HT_TESTS_NUM_INSERTS * n_cons / 10) == 0) {
        PLOG_INFO.printf("[cons:%u] transaction_id %lu deq_failures %lu",
                         this_cons_id, transaction_id, q->num_deq_failures);
      }*/
#endif
    }
  pick_next_msg:
    // reset the value of k
    // incase if the next dequeue fails, we will have a stale value of k
    k = 0;
    if (++prod_id >= n_prod) {
      prod_id = 0;
    }
  }

  auto t_end = RDTSCP();

  if (tid == n_prod)
    vtune::event_end(event);

  sh->stats->insertion_cycles = (t_end - t_start);
  sh->stats->num_inserts = transaction_id;

  if constexpr (bq_load == BQUEUE_LOAD::HtInsert) {
    get_ht_stats(sh, kmer_ht);
  }

  for (auto i = 0u; i < n_prod; ++i) {
    this->queues->dump_stats(i, this_cons_id);
  }
#ifdef CONFIG_ALIGN_BQUEUE_METADATA
  for (auto i = 0u; i < n_prod; ++i) {
    auto *q = cqueues[i];
    PLOG_INFO.printf("[cons:%u] q[%d] deq_failures %u", this_cons_id, i,
                     q->num_deq_failures);
  }
#endif

  PLOG_INFO.printf("cons_id %d | inserted %lu elements", this_cons_id,
                   inserted);
  PLOG_INFO.printf(
      "Quick Stats: Consumer %u finished, receiving %lu messages "
      "(cycles per message %lu) prod_count %u | finished %u",
      this_cons_id, transaction_id, (t_end - t_start) / transaction_id, n_prod,
      finished_producers);

  // Write to file
  if (!this->cfg->ht_file.empty()) {
    std::string outfile = this->cfg->ht_file + std::to_string(sh->shard_idx);
    PLOG_INFO.printf("Shard %u: Printing to file: %s", sh->shard_idx,
                     outfile.c_str());
    kmer_ht->print_to_file(outfile);
  }

  // End test
  fipc_test_FAI(completed_consumers);
}

template <typename T>
void QueueTest<T>::find_thread(int tid, int n_prod, int n_cons,
                             bool main_thread) {
  Shard *sh = &this->shards[tid];
  uint64_t found = 0, not_found = 0;
  uint64_t count = std::max(HT_TESTS_NUM_INSERTS * tid, (uint64_t)1);
  BaseHashTable *ktable;
  Hasher hasher;

  struct xorwow_state _xw_state, init_state;

  xorwow_init(&_xw_state);
  init_state = _xw_state;

  alignas(64) uint64_t k = 0;

  vtune::set_threadname("find_thread" + std::to_string(tid));

  ktable = this->ht_vec->at(tid);

  if (ktable == nullptr) {
    // Both producer and consumer threads participate in find. However, the
    // producer threads do not have any <k,v> pairs to find. So, they queue the
    // find request to the actual partitions which hosts these keys.
    // Nevertheless, they need this ktable object to queue the find requests to
    // other partitions. So, just create a HT with 100 buckets.

    auto ht_size = config.ht_size / n_cons;
    PLOG_INFO.printf("[find%u] init_ht ht_size: %u | id: %d", tid, ht_size,
                     sh->shard_idx);
    ktable = init_ht(ht_size, sh->shard_idx);
    this->ht_vec->at(tid) = ktable;
  }

  Values *values;
  values = new Values[HT_TESTS_FIND_BATCH_LENGTH];

  if (main_thread) {
    // Wait for threads to be ready for test
    while (ready_threads < (uint64_t)(n_prod + n_cons - 1)) fipc_test_pause();

    // Signal begin
    ready = 1;
    fipc_test_mfence();
  } else {
    fipc_test_FAI(ready_threads);
    while (!ready) fipc_test_pause();
    fipc_test_mfence();
  }

  auto num_messages = HT_TESTS_NUM_INSERTS / (n_prod + n_cons);

  // our HT has a notion of empty keys which is 0. So, no '0' key for now!
  uint64_t key_start =
    std::max(static_cast<uint64_t>(num_messages) * tid, (uint64_t)1);

  __attribute__((aligned(64))) Keys items[HT_TESTS_FIND_BATCH_LENGTH] = {0};

  ValuePairs vp = std::make_pair(0, values);

  PLOG_INFO.printf("Finder %u starting. key_start %lu | num_messages %lu", tid,
                   key_start, num_messages);

  int partition;
  int j = 0;

  static const auto event = vtune::event_start("find_batch");

  auto t_start = RDTSC_START();

  for (auto m = 0u; m < config.insert_factor; m++) {
    key_start =
      std::max(static_cast<uint64_t>(num_messages) * tid, (uint64_t)1);
    _xw_state = init_state;
    for (auto i = 0u; i < num_messages; i++) {
#if defined(XORWOW)
      k = xorwow(&_xw_state);
#else
      k = key_start++;
#endif
      uint64_t hash_val = hasher(&k, sizeof(k));

      partition = hash_to_cpu(hash_val, n_cons);
      // PLOGI.printf("partition %d", partition);
      // k has the computed hash in upper 32 bits
      // and the actual key value in lower 32 bits
      k |= (hash_val << 32);

      items[j].key = k;
      items[j].id = count;
      items[j].part_id = partition + n_prod;
      count++;

      if (j == 0) {
        ktable->prefetch_queue(QueueType::find_queue);
      }
      if (++j == HT_TESTS_FIND_BATCH_LENGTH) {
        KeyPairs kp = std::make_pair(HT_TESTS_FIND_BATCH_LENGTH, &items[0]);
        // PLOGI.printf("calling find_batch i = %d", i);
        // ktable->find_batch((Keys *)items, HT_TESTS_FIND_BATCH_LENGTH);
        ktable->find_batch(kp, vp);
        found += vp.first;
        j = 0;
        not_found += HT_TESTS_FIND_BATCH_LENGTH - vp.first;
        vp.first = 0;
        PLOGD.printf("tid %lu count %lu | found -> %lu | not_found -> %lu", tid,
            count, found, not_found);
      }

#ifdef CALC_STATS
      if (i % (num_messages / 10) == 0) {
        PLOG_INFO.printf(
            "Finder %u, transaction_id %lu | (found %lu, not_found %lu)", tid, i,
            found, not_found);
      }
#endif
    }
  }
  auto t_end = RDTSCP();

  vtune::event_end(event);

  sh->stats->find_cycles = (t_end - t_start);
  sh->stats->num_finds = found;

  if (found >= 0) {
    PLOG_INFO.printf(
        "thread %u | num_finds %lu (not_found %lu) | cycles per get: %lu",
        sh->shard_idx, found, not_found,
        found > 0 ? (t_end - t_start) / found : 0);
  }

  get_ht_stats(sh, ktable);
}

template <typename T>
void QueueTest<T>::init_queues(uint32_t nprod, uint32_t ncons) {
  PLOG_DEBUG.printf("Initializing queues");
  if (std::is_same<T, kmercounter::LynxQueue>::value) {
    this->QUEUE_SIZE = QueueTest::LYNX_QUEUE_SIZE;
  } else if(std::is_same<T, kmercounter::BQueueAligned>::value) {
    this->QUEUE_SIZE = QueueTest::BQ_QUEUE_SIZE;
  } else if (std::is_same<T, kmercounter::SectionQueue>::value) {
    this->QUEUE_SIZE = 4;
  }
  this->queues = new T(nprod, ncons, this->QUEUE_SIZE, this->npq);
}

template <typename T>
void QueueTest<T>::run_test(Configuration *cfg, Numa *n, NumaPolicyQueues *npq) {
  this->ht_vec =
      new std::vector<BaseHashTable *>(cfg->n_prod + cfg->n_cons, nullptr);
  // 1) Insert using bqueues
  this->insert_with_queues(cfg, n, npq);

  // 2) spawn n_prod + n_cons threads for find
  this->run_find_test(cfg, n, npq);
}

template <typename T>
void QueueTest<T>::run_find_test(Configuration *cfg, Numa *n,
                               NumaPolicyQueues *npq) {
  uint32_t i = 0, j = 0;
  cpu_set_t cpuset;
  // Spawn threads that will perform find operation
  for (uint32_t assigned_cpu : this->npq->get_assigned_cpu_list_producers()) {
    // skip the first CPU, we'll launch it later
    if (assigned_cpu == 0) continue;
    Shard *sh = &this->shards[i];
    sh->shard_idx = i;
    auto _thread = std::thread(&QueueTest::find_thread, this, i, cfg->n_prod,
                               cfg->n_cons, false);
    CPU_ZERO(&cpuset);
    CPU_SET(assigned_cpu, &cpuset);
    pthread_setaffinity_np(_thread.native_handle(), sizeof(cpu_set_t), &cpuset);
    this->prod_threads.push_back(std::move(_thread));
    PLOG_INFO.printf("Thread find_thread: %u, affinity: %u", i, assigned_cpu);
    i += 1;
  }

  PLOG_INFO.printf("creating cons threads i %d ", i);
  Shard *main_sh = &this->shards[i];
  main_sh->shard_idx = i;
  CPU_ZERO(&cpuset);
  uint32_t last_cpu = 0;
  CPU_SET(last_cpu, &cpuset);
  sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
  PLOG_INFO.printf("Thread 'controller': affinity: %u", last_cpu);

  // Spawn find threads
  i = cfg->n_prod;
  for (auto assigned_cpu : this->npq->get_assigned_cpu_list_consumers()) {
    PLOG_INFO.printf("i %d assigned cpu %d", i, assigned_cpu);

    Shard *sh = &this->shards[i];
    sh->shard_idx = i;

    auto _thread = std::thread(&QueueTest::find_thread, this, i, cfg->n_prod,
                               cfg->n_cons, false);

    CPU_ZERO(&cpuset);
    CPU_SET(assigned_cpu, &cpuset);

    pthread_setaffinity_np(_thread.native_handle(), sizeof(cpu_set_t), &cpuset);

    PLOG_INFO.printf("Thread find_thread: %u, affinity: %u", i, assigned_cpu);
    PLOG_INFO.printf("[%d] sh->insertion_cycles %lu", sh->shard_idx,
                     sh->stats->insertion_cycles);

    this->cons_threads.push_back(std::move(_thread));
    i += 1;
    j += 1;
  }

  {
    PLOG_VERBOSE.printf("Running master thread with id %d", main_sh->shard_idx);
    this->find_thread(main_sh->shard_idx, cfg->n_prod, cfg->n_cons, true);
  }

  for (auto &th : this->prod_threads) {
    th.join();
  }

  for (auto &th : this->cons_threads) {
    th.join();
  }
  PLOG_INFO.printf("Find done!");
  print_stats(this->shards, *cfg);
}

template <class T>
void QueueTest<T>::insert_with_queues(Configuration *cfg, Numa *n,
                                     NumaPolicyQueues *npq) {
  cpu_set_t cpuset;
  uint32_t i = 0, j = 0;

  this->n = n;
  this->nodes = this->n->get_node_config();
  this->npq = npq;
  this->cfg = cfg;

  uint32_t num_nodes = static_cast<uint32_t>(this->n->get_num_nodes());
  uint32_t num_cpus = static_cast<uint32_t>(this->n->get_num_total_cpus());

  // Calculate total threads (Prod + cons)
  cfg->num_threads = cfg->n_prod + cfg->n_cons;

  // bail out if n_prod + n_cons > num_cpus
  if (this->cfg->n_prod + this->cfg->n_cons > num_cpus) {
    PLOG_ERROR.printf(
        "producers (%u) + consumers (%u) exceeded number of "
        "available CPUs (%u)",
        this->cfg->n_prod, this->cfg->n_cons, num_cpus);
    exit(-1);
  }

  PLOG_DEBUG.printf(
      "Controller starting ... nprod: %u, ncons: %u (num_threads %u)",
      cfg->n_prod, cfg->n_cons, cfg->num_threads);

  // alloc shards array
  this->shards = (Shard *)calloc(sizeof(Shard), cfg->num_threads);

  // Init queues
  this->init_queues(cfg->n_prod, cfg->n_cons);

  fipc_test_mfence();

  // Spawn producer threads
  for (uint32_t assigned_cpu : this->npq->get_assigned_cpu_list_producers()) {
    // skip the first CPU, we'll launch producer on this
    if (assigned_cpu == 0) continue;
    Shard *sh = &this->shards[i];
    sh->shard_idx = i;
    auto _thread = std::thread(&QueueTest<T>::producer_thread, this, i,
                               cfg->n_prod, cfg->n_cons, false, cfg->skew);
    CPU_ZERO(&cpuset);
    CPU_SET(assigned_cpu, &cpuset);
    pthread_setaffinity_np(_thread.native_handle(), sizeof(cpu_set_t), &cpuset);
    this->prod_threads.push_back(std::move(_thread));
    PLOG_INFO.printf("Thread producer_thread: %u, affinity: %u", i,
                      assigned_cpu);
    i += 1;
  }

  PLOG_DEBUG.printf("creating cons threads i %d ", i);
  Shard *main_sh = &this->shards[i];
  main_sh->shard_idx = i;
  CPU_ZERO(&cpuset);
  uint32_t last_cpu = 0;
  CPU_SET(last_cpu, &cpuset);
  sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
  PLOG_DEBUG.printf("Thread 'controller': affinity: %u", last_cpu);

  // Spawn consumer threads
  i = cfg->n_prod;
  for (auto assigned_cpu : this->npq->get_assigned_cpu_list_consumers()) {
    Shard *sh = &this->shards[i];
    sh->shard_idx = i;

    PLOG_DEBUG.printf("tid %d assigned cpu %d", i, assigned_cpu);

    auto _thread = std::thread(&QueueTest<T>::consumer_thread, this, i,
                               cfg->n_prod, cfg->n_cons, cfg->num_nops);

    CPU_ZERO(&cpuset);
    CPU_SET(assigned_cpu, &cpuset);

    pthread_setaffinity_np(_thread.native_handle(), sizeof(cpu_set_t), &cpuset);

    PLOG_INFO.printf("Thread consumer_thread: %u, affinity: %u", i,
                      assigned_cpu);

    this->cons_threads.push_back(std::move(_thread));
    i += 1;
    j += 1;
  }

  {
    PLOG_DEBUG.printf("Running master thread with id %d", main_sh->shard_idx);
    this->producer_thread(main_sh->shard_idx, cfg->n_prod, cfg->n_cons, true,
                          cfg->skew);
  }

  // Wait for producers to complete
  while (completed_producers < cfg->n_prod) fipc_test_pause();

  fipc_test_mfence();

  // Wait for consumers to complete
  while (completed_consumers < cfg->n_cons) fipc_test_pause();

  fipc_test_mfence();

  for (auto &th : this->prod_threads) {
    th.join();
  }

  for (auto &th : this->cons_threads) {
    th.join();
  }

  this->prod_threads.clear();
  this->cons_threads.clear();

  // TODO free everything
  // TODO: Move this stats to find after testing find
  //print_stats(this->shards, *cfg);
}

template class QueueTest<SectionQueue>;
/*template class QueueTest<LynxQueue>;
template class QueueTest<BQueueAligned>;
*/
}  // namespace kmercounter
