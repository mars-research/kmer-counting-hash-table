#ifndef KVSTORE_FOLKLORE_HT_HEADER
#define KVSTORE_FOLKLORE_HT_HEADER

#include "constants.hpp"
#include "plog/Log.h"
#include "helper.hpp"
#include "hashtables/ht_helper.hpp"
#include "sync.h"
#include "hasher.hpp"

#include "data-structures/table_config.hpp"
#include "utils/default_hash.hpp"
#include "allocator/alignedallocator.hpp"

#include <stdexcept>

namespace kmercounter {

class FolkloreHashTable : public BaseHashTable {
 public:
  FolkloreHashTable(uint64_t capacity)
   : table(capacity) {}

  bool insert(const void *data) {
    return false; // TODO
  }

  // NEVER NEVER NEVER USE KEY OR ID 0
  // Your inserts will be ignored if you do (we use these as empty markers)
  void insert_batch(const InsertFindArguments &kp, collector_type* collector = nullptr) {
    // TODO
    // NEED FOR TEST
  }

  void insert_noprefetch(const void *data, collector_type* collector = nullptr) {
    // TODO
    // NEED FOR TEST
    const InsertFindArgument* kp = reinterpret_cast<const InsertFindArgument*>(data);
    
    FolkloreHashTable::handle_type ht = table.get_handle();
    ht.insert(kp->key, kp->value);

    throw std::invalid_argument("stop right there");
  }

  void flush_insert_queue(collector_type* collector = nullptr) {
    // TODO
    // NEED FOR TEST
  }

  // NEVER NEVER NEVER USE KEY OR ID 0
  // Your inserts will be ignored if you do (we use these as empty markers)
  void find_batch(const InsertFindArguments &kp, ValuePairs &vp, collector_type* collector = nullptr) {
    // TODO
    // NEED FOR TEST
  }

  void *find_noprefetch(const void *data, collector_type* collector = nullptr) {
    // NEED FOR TEST
    return nullptr; // TODO
  }

  void flush_find_queue(ValuePairs &vp, collector_type* collector = nullptr) {
    // TODO
    // NEED FOR TEST
  }

  void display() const {
    // TODO
  }

  size_t get_fill() const {
    return 0; // TODO
  }

  size_t get_capacity() const {
    return 0; // TODO
  }

  size_t get_max_count() const {
    return 0; // TODO
  }

  void print_to_file(std::string &outfile) const {
    // TODO
  }

  uint64_t read_hashtable_element(const void *data) {
    return 0; // TODO
  }

  void prefetch_queue(QueueType qtype) {
    // TODO
  }

  // uhh?
  // uint64_t num_reprobes = 0;
  // uint64_t num_soft_reprobes = 0;
  // uint64_t num_memcmps = 0;
  // uint64_t num_memcpys = 0;
  // uint64_t num_hashcmps = 0;
  // uint64_t num_queue_flushes = 0;
  // uint64_t sum_distance_from_bucket = 0;
  // uint64_t max_distance_from_bucket = 0;
  // uint64_t num_swaps = 0;

private:
  using table_config = typename growt::table_config<
    kmercounter::key_type, 
    kmercounter::value_type, 
    utils_tm::hash_tm::default_hash, 
    growt::AlignedAllocator<>
  >;
  using table_type = table_config::table_type;
  using handle_type = table_type::handle_type;

  alignas(64) table_type table;
};

}

#endif