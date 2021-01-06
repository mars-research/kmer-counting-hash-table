#ifndef _SKHT_H
#define _SKHT_H

#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <array>
#include <cassert>
#include <fstream>
#include <iostream>

#include "dbg.hpp"
#include "helper.hpp"
#include "ht_helper.hpp"
#include "sync.h"

#if defined(BRANCHLESS) && defined(BRACNHLESS_NO_SIMD)
#error \
    "BRACHLESS and BRANCHLESS_NO_SIMD options selected in CFLAGS, remove one to fix this error"
#endif

#if defined(BRANCHLESS)
#include <immintrin.h>
#include <x86intrin.h> // _bit_scan_forward
#endif

namespace kmercounter {

// TODO use char and bit manipulation instead of bit fields in Kmer_KV:
// https://stackoverflow.com/questions/1283221/algorithm-for-copying-n-bits-at-arbitrary-position-from-one-int-to-another
// TODO how long should be the count variable?
// TODO should we pack the struct?

template <typename KV, typename KVQ>
class alignas(64) PartitionedHashStore : public BaseHashTable {
 public:
  KV *hashtable;
  int fd;
  int id;
  size_t data_length, key_length;

  // https://www.bfilipek.com/2019/08/newnew-align.html
  void *operator new(std::size_t size, std::align_val_t align) {
    auto ptr = aligned_alloc(static_cast<std::size_t>(align), size);

    if (!ptr) throw std::bad_alloc{};

    std::cout << "[INFO] "
              << "new: " << size
              << ", align: " << static_cast<std::size_t>(align)
              << ", ptr: " << ptr << '\n';

    return ptr;
  }

  void operator delete(void *ptr, std::size_t size,
                       std::align_val_t align) noexcept {
    std::cout << "delete: " << size
              << ", align: " << static_cast<std::size_t>(align)
              << ", ptr : " << ptr << '\n';
    free(ptr);
  }

  void prefetch(uint64_t i) {
#if defined(PREFETCH_WITH_PREFETCH_INSTR)
    prefetch_object(&this->hashtable[i & (this->capacity - 1)],
                    sizeof(this->hashtable[i & (this->capacity - 1)]));
#endif

#if defined(PREFETCH_WITH_WRITE)
    prefetch_with_write(&this->hashtable[i & (this->capacity - 1)]);
#endif
  };

  inline uint8_t touch(uint64_t i) {
#if defined(TOUCH_DEPENDENCY)
    if (this->hashtable[i & (this->capacity - 1)].kb.count == 0) {
      this->hashtable[i & (this->capacity - 1)].kb.count = 1;
    } else {
      this->hashtable[i & (this->capacity - 1)].kb.count = 1;
    };
#else
    this->hashtable[i & (this->capacity - 1)].kb.count = 1;
#endif
    return 0;
  };

  PartitionedHashStore(uint64_t c, uint8_t id)
      : fd(-1), id(id), queue_idx(0), find_idx(0) {
    this->capacity = kmercounter::next_pow2(c);
    this->hashtable = calloc_ht<KV>(capacity, this->id, &this->fd);
    this->empty_item = this->empty_item.get_empty_key();
    this->key_length = empty_item.key_length();
    this->data_length = empty_item.data_length();

    cout << "Empty item: " << this->empty_item << endl;
    this->queue = (KVQ *)(aligned_alloc(64, PREFETCH_QUEUE_SIZE * sizeof(KVQ)));
    this->find_queue =
        (KVQ *)(aligned_alloc(64, PREFETCH_FIND_QUEUE_SIZE * sizeof(KVQ)));

    dbg("id: %d this->queue %p | find_queue %p\n", id, this->queue,
        this->find_queue);
    printf("[INFO] Hashtable size: %lu\n", this->capacity);
    printf("%s, data_length %lu\n", __func__, this->data_length);
  }

  ~PartitionedHashStore() {
    free(queue);
    free_mem<KV>(this->hashtable, this->capacity, this->id, this->fd);
  }

#if INSERT_BATCH

  insert_one() {
    occupied = this->hashtable[pidx].kb.occupied;

    /* Compare with empty kmer to check if bucket is empty, and insert.*/
    if (!occupied) {
#ifdef CALC_STATS
      this->num_memcpys++;
#endif
      memcpy(&this->hashtable[pidx].kb.kmer.data, q->kmer_p, this->data_length);
      this->hashtable[pidx].kb.count++;
      this->hashtable[pidx].kb.occupied = true;
      this->hashtable[pidx].key_hash = q->key_hash;
      return;
    }

#ifdef CALC_STATS
    this->num_hashcmps++;
#endif

    if (this->hashtable[pidx].kmer_hash == q->kmer_hash) {
#ifdef CALC_STATS
      this->num_memcmps++;
#endif

      if (memcmp(&this->hashtable[pidx].kb.kmer.data, q->kmer_p,
                 this->data_length) == 0) {
        this->hashtable[pidx].kb.count++;
        return;
      }
    }

    {
      /* insert back into queue, and prefetch next bucket.
      next bucket will be probed in the next run
      */
      pidx++;
      pidx = pidx & (this->capacity - 1);  // modulo
      prefetch(pidx);
      q->kmer_idx = pidx;

      this->queue[this->queue_idx] = *q;
      // this->queue[this->queue_idx].data = q->data;
      // this->queue[this->queue_idx].idx = q->idx;
      this->queue_idx++;

#ifdef CALC_STATS
      this->num_reprobes++;
#endif
    }

    void insert_batch(kmer_data_t * karray[4]) {
      uint64_t hash_new_0 = this->hash((const char *)karray[0]);
      size_t __kmer_idx_1 = hash & (this->capacity - 1);  // modulo

      uint64_t hash_new_0 = this->hash((const char *)karray[0]);
      size_t __kmer_idx_1 = hash & (this->capacity - 1);  // modulo

      uint64_t hash_new_0 = this->hash((const char *)karray[0]);
      size_t __kmer_idx_1 = hash & (this->capacity - 1);  // modulo

      uint64_t hash_new_0 = this->hash((const char *)karray[0]);
      size_t __kmer_idx_1 = hash & (this->capacity - 1);  // modulo

      return;
    }
  };

#endif

  void insert_noprefetch(void *data) {
    cout << "Not implemented!" << endl;
    assert(false);
  }

  /* insert and increment if exists */
  bool insert(const void *data) override {
    assert(this->queue_idx < PREFETCH_QUEUE_SIZE);
    __insert_into_queue(data);

    /* if queue is full, actually insert */
    // now queue_idx = 20
    if (this->queue_idx >= PREFETCH_QUEUE_SIZE) {
      this->__insert_from_queue();

#if 0
      for (auto i = 0u; i < PREFETCH_QUEUE_SIZE / 2; i += 4)
        __builtin_prefetch(&this->queue[this->queue_idx + i], 1, 3);
#endif
    }

    // XXX: This is to ensure correctness of the hashtable. No matter what the
    // queue size is, this has to be enabled!
    if (this->queue_idx == PREFETCH_QUEUE_SIZE) {
      this->flush_queue();
    }

    // XXX: Most likely the HT is full here. We should panic here!
    assert(this->queue_idx < PREFETCH_QUEUE_SIZE);

    return true;
  }

  void flush_queue() override {
    size_t curr_queue_sz = this->queue_idx;
    while (curr_queue_sz != 0) {
      __flush_from_queue(curr_queue_sz);
      curr_queue_sz = this->queue_idx;
    }
#ifdef CALC_STATS
    this->num_queue_flushes++;
#endif
  }

  uint8_t flush_find_queue() override {
    size_t curr_queue_sz = this->find_idx;
    uint8_t found = 0;
    while (curr_queue_sz != 0) {
      found += __flush_find_queue(curr_queue_sz);
      curr_queue_sz = this->find_idx;
    }
    return found;
  }

  uint8_t find_batch(uint64_t *__keys, uint32_t batch_len) override {
    auto found = 0;
    KV *keys = reinterpret_cast<KV *>(__keys);
    for (auto k = 0u; k < batch_len; k++) {
      void *data = reinterpret_cast<void *>(&keys[k]);

      add_to_find_queue(data);

      if (this->find_idx >= PREFETCH_FIND_QUEUE_SIZE) {
        found += this->find_prefetched_batch();
      }

      found += this->flush_find_queue();
    }
    printf("%s, found %d keys\n", __func__, found);
    return found;
  }

  void *find(const void *data) override {
#ifdef CALC_STATS
    uint64_t distance_from_bucket = 0;
#endif
    uint64_t hash = this->hash((const char *)data);
    size_t idx = hash;
    KV *curr = NULL;
    bool found = false;

    for (auto i = 0u; i < this->capacity; i++) {
      idx = idx & (this->capacity - 1);
      curr = &this->hashtable[idx];

      if (curr->is_empty()) {
        found = false;
        goto exit;
      } else if (curr->compare_key(data)) {
        found = true;
        break;
      }

#ifdef CALC_STATS
      distance_from_bucket++;
#endif
      idx++;
    }
#ifdef CALC_STATS
    if (distance_from_bucket > this->max_distance_from_bucket) {
      this->max_distance_from_bucket = distance_from_bucket;
    }
    this->sum_distance_from_bucket += distance_from_bucket;
#endif
    // return empty_element if nothing is found
    if (!found) {
      curr = &this->empty_item;
    }
  exit:
    return curr;
  }

  void display() const override {
    for (size_t i = 0; i < this->capacity; i++) {
      if (!this->hashtable[i].is_empty()) {
        cout << this->hashtable[i] << endl;
      }
    }
  }

  size_t get_fill() const override {
    size_t count = 0;
    for (size_t i = 0; i < this->capacity; i++) {
      if (!this->hashtable[i].is_empty()) {
        count++;
      }
    }
    return count;
  }

  size_t get_capacity() const override { return this->capacity; }

  size_t get_max_count() const override {
    size_t count = 0;
    for (size_t i = 0; i < this->capacity; i++) {
      if (this->hashtable[i].get_value() > count) {
        count = this->hashtable[i].get_value();
      }
    }
    return count;
  }

  void print_to_file(std::string &outfile) const override {
    std::ofstream f(outfile);
    if (!f) {
      dbg("Could not open outfile %s\n", outfile.c_str());
      return;
    }
    for (size_t i = 0; i < this->get_capacity(); i++) {
      if (!this->hashtable[i].is_empty()) {
        f << this->hashtable[i] << std::endl;
      }
    }
  }

 private:
  uint64_t capacity;
  KV empty_item; /* for comparison for empty slot */
  KVQ *queue;    // TODO prefetch this?
  KVQ *find_queue;
  uint32_t queue_idx;
  uint32_t find_idx;

  uint64_t hash(const void *k) {
    uint64_t hash_val;
#if defined(CITY_HASH)
    hash_val = CityHash64((const char *)k, this->key_length);
#elif defined(FNV_HASH)
    hash_val = hval = fnv_32a_buf(k, this->key_length, hval);
#elif defined(XX_HASH)
    hash_val = XXH64(k, this->key_length, 0);
#elif defined(XX_HASH_3)
    hash_val = XXH3_64bits(k, this->key_length);
#endif
    return hash_val;
  }

  /* Insert using prefetch: using a dynamic prefetch queue.
          If bucket is occupied, add to queue again to reprobe.
  */
  void __insert_with_soft_reprobe(KVQ *q) {
    /* hashtable location at which data is to be inserted */
    size_t pidx = q->idx;
    KV *curr = &this->hashtable[pidx];
    // printf("%s trying to insert %d\n", __func__, *(uint64_t*)q->data);
    // cout << "element at pidx: " << pidx << " => " << this->hashtable[pidx] <<
    // " occupied: " << occupied << endl;
  try_insert:
    // Compare with empty element
    if (curr->is_empty()) {
#ifdef CALC_STATS
      this->num_memcpys++;
#endif
      curr->update_item(q->data);
      // this->hashtable[pidx].kb.count++;
      // this->hashtable[pidx].set_count(this->hashtable[pidx].count() + 1);
      // printf("inserting %d | ",
      // *(uint64_t*)&this->hashtable[pidx].kb.kmer.data printf("%lu: %d |
      // %d\n", pidx, hashtable[pidx].kb.count, no_ins++);
      // cout << "Inserting " << this->hashtable[pidx] << endl;
#ifdef COMPARE_HASH
      this->hashtable[pidx].key_hash = q->key_hash;
#endif
      return;
    }

#ifdef CALC_STATS
    this->num_hashcmps++;
#endif

#ifdef COMPARE_HASH
    if (this->hashtable[pidx].key_hash == q->key_hash)
#endif
    {
#ifdef CALC_STATS
      this->num_memcmps++;
#endif

      if (curr->compare_key(q->data)) {
        // update value
        curr->update_value(q->data, 0);
        return;
      }
    }

    {
      /* insert back into queue, and prefetch next bucket.
      next bucket will be probed in the next run
      */
      pidx++;
      pidx = pidx & (this->capacity - 1);  // modulo

      //   | cacheline |
      //   | i | i + 1 |
      //   In the case where two elements fit in a cacheline, a single prefetch
      //   would bring in both the elements. We need not issue a second
      //   prefetch.
      if ((pidx & 0x1) || (pidx & 0x2)) {
        // if (unlikely(pidx & 0x1)) {
#ifdef CALC_STATS
        this->num_soft_reprobes++;
#endif
        goto try_insert;
      }
      prefetch(pidx);
      // q->idx = pidx;

      // this->queue[this->queue_idx] = *q;
      this->queue[this->queue_idx].data = q->data;
      this->queue[this->queue_idx].idx = pidx;
      this->queue_idx++;
      // printf("reprobe pidx %d\n", pidx);

#ifdef CALC_STATS
      this->num_reprobes++;
#endif
      return;
    }
  }

  // TODO: Move this to makefile
  //#define BRANCHLESS_FIND

  auto __find_one(KVQ *q) {
    // hashtable idx where the data should be found
    size_t idx = q->idx;
    uint64_t found = 0;
#ifdef BRANCHLESS_FIND
    KV *curr = &this->hashtable[idx];
    uint64_t retry;
    if (*(uint64_t *)q->data == 11180) {
      // printf("corrupt value\n");
      // assert(false);
    }
    found = curr->find_key_brless(q->data, &retry);

    // if (found)
    // printf("key = %lu , value %lu | retry = %lu | found = %lu\n",
    // *(uint64_t*) q->data,
    //                              *((uint64_t*)q->data + 1), retry, found);

    // insert back into queue, and prefetch next bucket.
    // next bucket will be probed in the next run
    idx++;
    idx = idx & (this->capacity - 1);  // modulo

    prefetch(idx);

    this->find_queue[this->find_idx].data = q->data;
    this->find_queue[this->find_idx].idx = idx;

    // this->find_idx should not be incremented if either
    // the desired key is empty or it is found.
    uint64_t inc{1};
    inc = (retry == 0x1) ? inc : 0;
    this->find_idx += inc;

    return found;
#else
  try_find:
    KV *curr = &this->hashtable[idx];

    // Compare with empty element
    if (curr->is_empty()) {
      goto exit;
    } else if (curr->compare_key(q->data)) {
      KV *kv = const_cast<KV *>(reinterpret_cast<const KV *>(q->data));
      kv->set_value(curr);
      found = 1;
      goto exit;
    }

    // insert back into queue, and prefetch next bucket.
    // next bucket will be probed in the next run
    idx++;
    idx = idx & (this->capacity - 1);  // modulo

    // |    4 elements |
    // | 0 | 1 | 2 | 3 | 4 | 5 ....
    if ((idx & 0x3) != 0) {
      goto try_find;
    }

    prefetch(idx);

    this->find_queue[this->find_idx].data = q->data;
    this->find_queue[this->find_idx].idx = idx;
    this->find_idx++;
  exit:
    // printf("%s, key = %lu | found = %lu\n", __func__, *(uint64_t*) q->data,
    // found);
    return found;
#endif
  }

  void __insert(KVQ *q) {
    // hashtable idx at which data is to be inserted
    size_t idx = q->idx;
  try_insert:
    KV *curr = &this->hashtable[idx];

#ifdef BRANCHLESS_NO_SIMD
    // 0xFF: insert or update was successfull
    uint16_t cmp = curr->insert_or_update(q->data);

    /* prepare for (possible) soft reprobe */
    idx++;
    idx = idx & (this->capacity - 1);  // modulo

    prefetch(idx);
    this->queue[this->queue_idx].data = q->data;
    this->queue[this->queue_idx].idx = idx;

    // this->queue_idx should not be incremented if either
    // of the try_inserts succeeded
    int inc{1};
    inc = (cmp == 0xFF) ? 0 : inc;
    this->queue_idx += inc;

    return;

#elif defined(BRANCHLESS)
    static_assert(CACHE_LINE_SIZE == 64);
    static_assert(sizeof(KV) == 16);
    constexpr size_t KV_PER_CACHE_LINE = CACHE_LINE_SIZE / sizeof (KV);

    // masks for AVX512 instructions
    constexpr __mmask8 KEY0 = 0b00000001;
    constexpr __mmask8 KEY1 = 0b00000100;
    constexpr __mmask8 KEY2 = 0b00010000;
    constexpr __mmask8 KEY3 = 0b01000000;
    constexpr __mmask8 VAL0 = 0b00000010;
    constexpr __mmask8 VAL1 = 0b00001000;
    constexpr __mmask8 VAL2 = 0b00100000;
    constexpr __mmask8 VAL3 = 0b10000000;
    constexpr __mmask8 KVP0 = KEY0 | VAL0;
    constexpr __mmask8 KVP1 = KEY1 | VAL1;
    constexpr __mmask8 KVP2 = KEY2 | VAL2;
    constexpr __mmask8 KVP3 = KEY3 | VAL3;

    // a vector of 1s that is used for incrementing a value
    constexpr __m512i INCREMENT_VECTOR = {
      0ULL, 1ULL, // KVP0
      0ULL, 1ULL, // KVP1
      0ULL, 1ULL, // KVP2
      0ULL, 1ULL, // KVP3
    };

    // cacheline_masks is indexed by q->idx % KV_PER_CACHE_LINE
    constexpr std::array<__mmask8, KV_PER_CACHE_LINE> cacheline_masks = {
      KVP3|KVP2|KVP1|KVP0, // load all KV pairs in the cacheline
      KVP3|KVP2|KVP1,      // skip the first KV pair
      KVP3|KVP2,           // skip the first two KV pairs
      KVP3,                // load only the last KV pair
    };
    // key_cmp_masks are indexed by cidx, the index of an entry in a cacheline
    // the masks are used to mask irrelevant bits of the result of 4-way SIMD
    // key comparisons
    constexpr std::array<__mmask8, KV_PER_CACHE_LINE> key_cmp_masks = {
      KEY3|KEY2|KEY1|KEY0, // cidx: 0; all key comparisons valid
      KEY3|KEY2|KEY1,      // cidx: 1; only last three comparisons valid
      KEY3|KEY2,           // cidx: 2; only last two comparisons valid
      KEY3,                // cidx: 3; only last comparison valid
    };
    // key_copy_masks are indexed by the return value of the BSF instruction,
    // executed on the mask returned by a search for empty keys in a
    // cacheline
    //       <-------------------- cacheline --------------------->
    //       || key | val || key | val || key | val || key | val ||
    // bits: ||  0     1      2     3      4     5      6     7  ||
    // the only possible indices are: 0, 2, 4, 6
    constexpr std::array<__mmask8, 8> key_copy_masks = {
      KEY0, // bit 0 set; choose first key
      0,
      KEY1, // bit 2 set; choose second
      0,
      KEY2, // bit 4 set; choose third
      0,
      KEY3, // bit 6 set; choose last
      0,
    };

    auto load_key_vector = [q]() {
      // we want to load only the keys into a ZMM register, as two 32-bit
      // integers. 0b0011 matches the first 64 bits of a KV pair -- the key
      __mmask16 mask{0b0011001100110011};
      __m128i   kv = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(q->data));
      return _mm512_maskz_broadcast_i32x2(mask, kv);
    };

    auto load_cacheline = [this, idx, &cacheline_masks](size_t cidx) {
      const KV *cptr = &this->hashtable[idx & ~(KV_PER_CACHE_LINE-1)];
      return _mm512_maskz_load_epi64(cacheline_masks[cidx], cptr);
    };

    auto store_cacheline = [this, idx](__m512i cacheline, __mmask8 kv_mask) {
      KV *cptr = &this->hashtable[idx & ~(KV_PER_CACHE_LINE-1)];
      _mm512_mask_store_epi64(cptr, kv_mask, cacheline);
    };

    auto key_cmp = [&key_cmp_masks](__m512i cacheline, __m512i key_vector, size_t cidx) {
      __mmask8 cmp = _mm512_cmpeq_epu64_mask(cacheline, key_vector);
      // zmm registers are compared as 8 uint64_t
      // mask irrelevant results before returning
      return cmp & key_cmp_masks[cidx];
    };

    auto empty_cmp = [&key_cmp_masks](__m512i cacheline, size_t cidx) {
      const __m512i empty_key_vector = _mm512_setzero_si512();
      __mmask8 cmp = _mm512_cmpeq_epu64_mask(cacheline, empty_key_vector);
      // zmm registers are compared as 8 uint64_t
      // mask irrelevant results before returning
      return cmp & key_cmp_masks[cidx];
    };

    auto key_copy_mask = [&empty_cmp, &key_copy_masks](__m512i cacheline, __mmask8 eq_cmp, size_t cidx) {
      __mmask8 locations = empty_cmp(cacheline, cidx);
      __mmask8 copy_mask = key_copy_masks[_bit_scan_forward(locations)];
      // we do not need to copy the key if it is already present
      // if (eq_cmp) copy_mask = 0;
      asm (
          "xorw %%cx, %%cx\n\t"
          "test %[eq_cmp], %[eq_cmp]\n\t"
          "cmovew %%cx, %[copy_mask]\n\t"
          : [copy_mask]"+r"(copy_mask)
          : [eq_cmp]"r"(eq_cmp)
          : "rcx"
      );
      return copy_mask;
    };

    auto copy_key = [](__m512i &cacheline, __m512i key_vector, __mmask8 copy_mask) {
      cacheline = _mm512_mask_blend_epi64(copy_mask, cacheline, key_vector);
    };

    auto increment_count = [INCREMENT_VECTOR](__m512i &cacheline, __mmask8 val_mask) {
      cacheline = _mm512_mask_add_epi64(cacheline, val_mask, cacheline, INCREMENT_VECTOR);
    };

    // compute index within the cacheline
    const size_t cidx = idx & (KV_PER_CACHE_LINE-1);

    // depending on the value of idx, between 1 and 4 KV pairs in a cacheline
    // can be probed for an insert operation. load the cacheline.
    __m512i cacheline = load_cacheline(cidx);
    // load a vector of the key in all 4 positions
    __m512i key_vector = load_key_vector();

    __mmask8 eq_cmp = key_cmp(cacheline, key_vector, cidx);
    // compute a mask for copying the key into an empty slot
    __mmask8 copy_mask = key_copy_mask(cacheline, eq_cmp, cidx);
    copy_key(cacheline, key_vector, copy_mask);

    // between eq_cmp and copy_mask, at most one bit will be set
    // if we shift-left eq_cmp|copy_mask by 1 bit, the bit will correspond
    // to the value of the KV-pair we are interested in
    __mmask8 key_mask = eq_cmp | copy_mask;
    __mmask8 val_mask = key_mask << 1;
    // at this point, increment the count
    increment_count(cacheline, val_mask);

    // write the cacheline back; just the KV pair that was modified
    __mmask8 kv_mask = key_mask | val_mask;
    store_cacheline(cacheline, kv_mask);

    // prepare for possible reprobe
    // point idx to the start of the cacheline
    idx = idx + KV_PER_CACHE_LINE - cidx;
    idx = idx & (this->capacity - 1);  // modulo
    this->queue[this->queue_idx].data = q->data;
    this->queue[this->queue_idx].idx = idx;
    auto queue_idx_inc = 1;
    // if kv_mask != 0, insert succeeded; reprobe unnecessary
    asm (
        "xorq %%rcx, %%rcx\n\t"
        "movq $4,    %%rbx\n\t"
        "test %[kv_mask], %[kv_mask]\n\t"
        // if (kv_mask) queue_idx_inc = 0;
        "cmovnzq %%rcx, %[inc]\n\t"
        // if (kv_mask) idx -= 4;
        "cmovnzq %%rbx, %%rcx\n\t"
        "sub %%rcx, %[idx]\n\t"
        : [idx]"+r"(idx), [inc]"+r"(queue_idx_inc)
        : [kv_mask]"r"(kv_mask)
        : "rbx", "rcx"
    );

    // issue prefetch
    prefetch(idx);
    this->queue_idx += queue_idx_inc;

    return;
#else  // !BRANCHLESS
    assert(
        !const_cast<KV *>(reinterpret_cast<const KV *>(q->data))->is_empty());
    // Compare with empty element
    if (curr->is_empty()) {
#ifdef CALC_STATS
      this->num_memcpys++;
#endif
      curr->insert_item(q->data, 0);
#ifdef COMPARE_HASH
      this->hashtable[pidx].key_hash = q->key_hash;
#endif
      return;
    }

#ifdef CALC_STATS
    this->num_hashcmps++;
#endif

#ifdef COMPARE_HASH
    if (this->hashtable[pidx].key_hash == q->key_hash)
#endif
    {
#ifdef CALC_STATS
      this->num_memcmps++;
#endif

      if (curr->compare_key(q->data)) {
        curr->update_value(q->data, 0);
        return;
      }
    }

    // insert back into queue, and prefetch next bucket.
    // next bucket will be probed in the next run
    idx++;
    idx = idx & (this->capacity - 1);  // modulo

    // |    4 elements |
    // | 0 | 1 | 2 | 3 | 4 | 5 ....
    if ((idx & 0x3) != 0) {
      goto try_insert;
    }

    prefetch(idx);

    this->queue[this->queue_idx].data = q->data;
    this->queue[this->queue_idx].idx = idx;
    this->queue_idx++;

#ifdef CALC_STATS
    this->num_reprobes++;
#endif
    return;
#endif  // BRANCHLESS
  }

  /* Insert items from queue into hash table, interpreting "queue"
  as an array of size queue_sz*/
  void __insert_from_queue() {
    this->queue_idx = 0;  // start again
    for (size_t i = 0; i < PREFETCH_QUEUE_SIZE; i++) {
      __insert(&this->queue[i]);
    }
  }

  void __flush_from_queue(size_t qsize) {
    this->queue_idx = 0;  // start again
    for (size_t i = 0; i < qsize; i++) {
      __insert(&this->queue[i]);
    }
  }

  void __insert_into_queue(const void *data) {
    uint64_t hash = this->hash((const char *)data);
    size_t idx = hash & (this->capacity - 1);  // modulo
    // size_t __kmer_idx2 = (hash + 3) & (this->capacity - 1);  // modulo
    // size_t __kmer_idx = cityhash_new % (this->capacity);

    /* prefetch buckets and store data pointers in queue */
    // TODO how much to prefetch?
    // TODO if we do prefetch: what to return? API breaks
    this->prefetch(idx);
    // this->prefetch(__kmer_idx2);

    // printf("inserting into queue at %u\n", this->queue_idx);
    // for (auto i = 0; i < 10; i++)
    //  asm volatile("nop");
    this->queue[this->queue_idx].data = data;
    this->queue[this->queue_idx].idx = idx;
#ifdef COMPARE_HASH
    this->queue[this->queue_idx].key_hash = hash;
#endif
    this->queue_idx++;
  }

  void add_to_find_queue(void *data) {
    uint64_t hash = this->hash((const char *)data);
    size_t idx = hash & (this->capacity - 1);  // modulo

    this->prefetch(idx);
    this->find_queue[this->find_idx].idx = idx;
    this->find_queue[this->find_idx].data = data;

#ifdef COMPARE_HASH
    this->queue[this->find_idx].key_hash = hash;
#endif
    this->find_idx++;
  }

  // fetch items from queue and call find
  auto find_prefetched_batch() {
    uint8_t found = 0;
    this->find_idx = 0;  // start again
    for (size_t i = 0; i < PREFETCH_FIND_QUEUE_SIZE; i++) {
      found += __find_one(&this->find_queue[i]);
    }
    return found;
  }

  auto __flush_find_queue(size_t qsize) {
    uint8_t found = 0;
    this->find_idx = 0;  // start again
    for (size_t i = 0; i < qsize; i++) {
      found += __find_one(&this->find_queue[i]);
    }
    return found;
  }
};

// TODO bloom filters for high frequency kmers?

}  // namespace kmercounter
#endif /* _SKHT_H_ */
