#ifndef TESTS_TESTS_HPP
#define TESTS_TESTS_HPP

#include "CacheMissTest.hpp"
#include "HashjoinTest.hpp"
#include "KmerTest.hpp"
#include "PrefetchTest.hpp"
#include "QueueTest.hpp"
#include "RWRatioTest.hpp"
#include "SynthTest.hpp"
#include "ZipfianTest.hpp"

namespace kmercounter {

class LynxQueue;
class BQueueAligned;
class SectionQueue;

class Tests {
 public:
  SynthTest st;
  PrefetchTest pt;
  // QueueTest<kmercounter::BQueueAligned> qt;
  //  QueueTest<kmercounter::LynxQueue> qt;
  QueueTest<kmercounter::SectionQueue> qt;
  CacheMissTest cmt;
  ZipfianTest zipf;
  KmerTest kmer;
  HashjoinTest hj;
  RWRatioTest rw;

  Tests() {}
};

}  // namespace kmercounter

#endif  // TESTS_TESTS_HPP
