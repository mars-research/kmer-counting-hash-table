#ifndef __ZIPFIAN_TEST_HPP__
#define __ZIPFIAN_TEST_HPP__

#include "base_kht.hpp"
#include "types.hpp"

namespace kmercounter {

class ZipfianTest {
 public:
  void run(Shard *sh, BaseHashTable *kmer_ht);
};

}  // namespace kmercounter

#endif  // __SYNTH_TEST_HPP__