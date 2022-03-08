#include "input_reader/fastq.hpp"

#include <absl/strings/str_join.h>
#include <gtest/gtest.h>

#include <boost/range/adaptor/transformed.hpp>
#include <boost/range/irange.hpp>
#include <boost/range/numeric.hpp>
#include <string_view>
#include <vector>

#include "input_reader_test_utils.hpp"

using boost::accumulate;
using boost::irange;
using boost::adaptors::transformed;

namespace kmercounter {
namespace input_reader {
namespace {
const char SMALL_SEQ[] = R"(@ERR024163.1 EAS51_210:1:1:1072:4554/1
AGGAGGTAA
+
EFFDEFFFF
)";
// A small sequence with 'N'
const char SMALL_SEQ_N[] = R"(@seq
AGGNNAGGTANA
+
EFFDEFFFFFFF
)";
const char FIVE_SEQS_N[] = R"(@seq0
AGGNNAGGTANA
+
EFFDEFFFFFFF
@seq1
ANANANANANAN
+
EFFDEFFFFFFF
@seq2
NNNNNNNNNNNN
+
EFFDEFFFFFFF
@seq3
AGGNNAGGTANA
+
EFFDEFFFFFFF
@seq4
NNNNNNNNNNNN
+
EFFDEFFFFFFF
)";
const char ONE_SEQ[] = R"(@ERR024163.1 EAS51_210:1:1:1072:4554/1
AGGAGGTAAATCTATCTTGAGCNAGTNAGNTNNNNNNNNAGGCATTATNNNANCTGACTTCAANATATATAACACAGCTATAGNAATCANNANANCNTNN
+
EFFDEFFFFFDAEDBDFD?B@@!@C/!77!7!!!!!!!!6961=7AA;!!!<!AAB>=B?>?@!CAAAACBD5CBC?AEAA?A!#####!!#!#!#!#!!
)";
const char FIVE_SEQS[] = R"(@ERR024163.1 EAS51_210:1:1:1072:4554/1
AGGAGGTAAATCTATCTTGAGCNAGTNAGNTNNNNNNNNAGGCATTATNNNANCTGACTTCAANATATATAACACAGCTATAGNAATCANNANANCNTNN
+
EFFDEFFFFFDAEDBDFD?B@@!@C/!77!7!!!!!!!!6961=7AA;!!!<!AAB>=B?>?@!CAAAACBD5CBC?AEAA?A!#####!!#!#!#!#!!
@ERR024163.2 EAS51_210:1:1:1072:12749/1
AGTGATTATTGGTACTAGTCACNAAGNGANGNNNNNNNNCAATCTTAANNNANATGATACTTTNAAAGATCAGCTCAAAACCTNCAATTNNANANANANN
+
EE?BEFFFFDDE?EEE=EEC==!?=7!<5!;!!!!!!!!7;:@<>??,!!!7!877::71;?;!;;<=?=-CEB?ABECA###!#####!!#!#!#!#!!
@ERR024163.3 EAS51_210:1:1:1072:8819/1
CTATGCAGCCATAAAAAAGGATNGGTNCANGNNNNNNNNAGGGACGTGNNNGNAGCTGGAAACNATCATTCTCAGAAAACTATNACAAGNNCNGNANANN
+
ADFBFE5DBDED:ED>>A>DB6!>65!7*!?!!!!!!!!A6668@<9>!!!/!:/.51*?958!;9=<B>:D:B:,@@95@@@!@####!!#!#!#!#!!
@ERR024163.4 EAS51_210:1:1:1073:14372/1
CTATGGGCAATGGGTACAAAGTNACANTTAANNNNNNNNAATCAGTTCNNNTNCCCTACTGTANAGTAAGGTAACTGTAATCANCAATANNANTNTATNN
+
EDEEDBEEE?FFFFDDBACE@C!?=1!?94;!!!!!!!!;7?::=7@;!!!9!8857:79>;>!9><;;=B4B??AA?CB66?!#####!!#!#!###!!
@ERR024163.5 EAS51_210:1:1:1073:1650/1
GATTAATCTTTGGACCACCACANCACNGCCANNNNNNNNTAGATAAAANNNANTTGGATTGAANAGGACTGAATTACTCACACNTATGGNNTNANCNTNN
+
A?:A>@D@DDC?C=A-?;9A;>!7>?!?###!!!!!!!!#########!!!#!##########!###################!#####!!#!#!#!#!!
)";

std::string build_seqs(int n) {
  std::string str;
  for (int i = 0; i < n; i++) {
    str += ONE_SEQ;
  }
  return str;
}

TEST(FastqReaderTest, SingleParition) {
  {
    std::unique_ptr<std::istream> input =
        std::make_unique<std::istringstream>(ONE_SEQ);
    auto reader = std::make_unique<FastqReader>(std::move(input));
    EXPECT_EQ(1, reader_size(std::move(reader)));
  }

  {
    std::unique_ptr<std::istream> input =
        std::make_unique<std::istringstream>(FIVE_SEQS);
    auto reader = std::make_unique<FastqReader>(std::move(input));
    EXPECT_EQ(5, reader_size(std::move(reader)));
  }
}

TEST(FastqReaderTest, MultiParition) {
  constexpr auto num_seqss =
      std::to_array({1, 2, 3, 4, 6, 9, 13, 17, 19, 21, 22, 24, 100, 1000});
  constexpr auto num_partss =
      std::to_array({1, 2, 3, 4, 5, 6, 9, 13, 17, 19, 64});

  for (const auto num_seqs : num_seqss) {
    std::string seqs = build_seqs(num_seqs);
    for (const auto num_parts : num_partss) {
      auto readers =
          irange(num_parts) | transformed([&seqs, num_parts](uint64_t part_id) {
            std::unique_ptr<std::istream> file =
                std::make_unique<std::istringstream>(seqs);
            auto reader = std::make_unique<FastqReader>(
                std::move(file), part_id, num_parts);
            return reader;
          });

      auto seqs_read = readers | transformed([](auto reader) {
                         const uint64_t lines_read =
                             reader_size(std::move(reader));
                         return lines_read;
                       });

      const uint64_t total_seqs_read = accumulate(seqs_read, 0ul);
      ASSERT_EQ(num_seqs, total_seqs_read)
          << "Incorrect number of seqs read for " << num_parts
          << " partitions.";
    }
  }

  std::unique_ptr<std::istream> input =
      std::make_unique<std::istringstream>(build_seqs(10));
  auto reader = std::make_unique<FastqReader>(std::move(input));
  EXPECT_EQ(10, reader_size(std::move(reader)));
}

TEST(FastqKmerReaderTest, SinglePartitionTest) {
  // 4mers
  {
    constexpr size_t K = 4;
    std::unique_ptr<std::istream> input =
          std::make_unique<std::istringstream>(SMALL_SEQ);
    auto reader = std::make_unique<FastqKMerReader<K>>(std::move(input));
    std::array<uint8_t, K> kmer;
    // AGGAGGTAA
    EXPECT_TRUE(reader->next(&kmer));
    EXPECT_EQ(std::to_array<uint8_t>({'A', 'G', 'G', 'A'}), kmer);
    EXPECT_TRUE(reader->next(&kmer));
    EXPECT_EQ(std::to_array<uint8_t>({'G', 'G', 'A', 'G'}), kmer);
    EXPECT_TRUE(reader->next(&kmer));
    EXPECT_EQ(std::to_array<uint8_t>({'G', 'A', 'G', 'G'}), kmer);
    EXPECT_TRUE(reader->next(&kmer));
    EXPECT_EQ(std::to_array<uint8_t>({'A', 'G', 'G', 'T'}), kmer);
    EXPECT_TRUE(reader->next(&kmer));
    EXPECT_EQ(std::to_array<uint8_t>({'G', 'G', 'T', 'A'}), kmer);
    EXPECT_TRUE(reader->next(&kmer));
    EXPECT_EQ(std::to_array<uint8_t>({'G', 'T', 'A', 'A'}), kmer);
    EXPECT_FALSE(reader->next(&kmer));
  }

  // 8mers
  {
    constexpr size_t K = 8;
    std::unique_ptr<std::istream> input =
          std::make_unique<std::istringstream>(SMALL_SEQ);
    auto reader = std::make_unique<FastqKMerReader<K>>(std::move(input));
    std::array<uint8_t, K> kmer;
    // AGGAGGTAA
    EXPECT_TRUE(reader->next(&kmer));
    EXPECT_EQ(std::to_array<uint8_t>({'A', 'G', 'G', 'A', 'G', 'G', 'T', 'A'}), kmer);
    EXPECT_TRUE(reader->next(&kmer));
    EXPECT_EQ(std::to_array<uint8_t>({'G', 'G', 'A', 'G', 'G', 'T', 'A', 'A'}), kmer);
    EXPECT_FALSE(reader->next(&kmer));
  }
}

TEST(FastqKMerPreloadReader, SinglePartitionTest) {
  // 4mers
  {
    constexpr size_t K = 4;
    std::unique_ptr<std::istream> input =
          std::make_unique<std::istringstream>(SMALL_SEQ);
    auto reader = std::make_unique<FastqKMerPreloadReader<K>>(std::move(input));
    std::array<uint8_t, K> kmer;
    // AGGAGGTAA
    EXPECT_TRUE(reader->next(&kmer));
    EXPECT_EQ(std::to_array<uint8_t>({'A', 'G', 'G', 'A'}), kmer);
    EXPECT_TRUE(reader->next(&kmer));
    EXPECT_EQ(std::to_array<uint8_t>({'G', 'G', 'A', 'G'}), kmer);
    EXPECT_TRUE(reader->next(&kmer));
    EXPECT_EQ(std::to_array<uint8_t>({'G', 'A', 'G', 'G'}), kmer);
    EXPECT_TRUE(reader->next(&kmer));
    EXPECT_EQ(std::to_array<uint8_t>({'A', 'G', 'G', 'T'}), kmer);
    EXPECT_TRUE(reader->next(&kmer));
    EXPECT_EQ(std::to_array<uint8_t>({'G', 'G', 'T', 'A'}), kmer);
    EXPECT_TRUE(reader->next(&kmer));
    EXPECT_EQ(std::to_array<uint8_t>({'G', 'T', 'A', 'A'}), kmer);
    EXPECT_FALSE(reader->next(&kmer));
  }

  // 8mers
  {
    constexpr size_t K = 8;
    std::unique_ptr<std::istream> input =
          std::make_unique<std::istringstream>(SMALL_SEQ);
    auto reader = std::make_unique<FastqKMerPreloadReader<K>>(std::move(input));
    std::array<uint8_t, K> kmer;
    // AGGAGGTAA
    EXPECT_TRUE(reader->next(&kmer));
    EXPECT_EQ(std::to_array<uint8_t>({'A', 'G', 'G', 'A', 'G', 'G', 'T', 'A'}), kmer);
    EXPECT_TRUE(reader->next(&kmer));
    EXPECT_EQ(std::to_array<uint8_t>({'G', 'G', 'A', 'G', 'G', 'T', 'A', 'A'}), kmer);
    EXPECT_FALSE(reader->next(&kmer));
  }
}

TEST(FastqKMerPreloadReader, ParseNTest) {
  // 4mers
  {
    constexpr size_t K = 4;
    std::unique_ptr<std::istream> input =
          std::make_unique<std::istringstream>(SMALL_SEQ_N);
    auto reader = std::make_unique<FastqKMerPreloadReader<K>>(std::move(input));
    std::array<uint8_t, K> kmer;
    // AGGNNAGGTANA
    EXPECT_TRUE(reader->next(&kmer));
    EXPECT_EQ(std::to_array<uint8_t>({'A', 'G', 'G', 'T'}), kmer);
    EXPECT_TRUE(reader->next(&kmer));
    EXPECT_EQ(std::to_array<uint8_t>({'G', 'G', 'T', 'A'}), kmer);
    EXPECT_FALSE(reader->next(&kmer));
  }

  // 8mers
  {
    constexpr size_t K = 8;
    std::unique_ptr<std::istream> input =
          std::make_unique<std::istringstream>(SMALL_SEQ);
    auto reader = std::make_unique<FastqKMerPreloadReader<K>>(std::move(input));
    std::array<uint8_t, K> kmer;
    // AGGNNAGGTANA
    EXPECT_FALSE(reader->next(&kmer));
  }
}

TEST(FastqKMerPreloadReader, MultiseqParseNTest) {
  // 4mers
  {
    constexpr size_t K = 4;
    std::unique_ptr<std::istream> input =
          std::make_unique<std::istringstream>(FIVE_SEQS_N);
    auto reader = std::make_unique<FastqKMerPreloadReader<K>>(std::move(input));
    std::array<uint8_t, K> kmer;
    ASSERT_TRUE(reader->next(&kmer));
    EXPECT_EQ(std::to_array<uint8_t>({'A', 'G', 'G', 'T'}), kmer);
    ASSERT_TRUE(reader->next(&kmer));
    EXPECT_EQ(std::to_array<uint8_t>({'G', 'G', 'T', 'A'}), kmer);
    ASSERT_TRUE(reader->next(&kmer));
    EXPECT_EQ(std::to_array<uint8_t>({'A', 'G', 'G', 'T'}), kmer);
    ASSERT_TRUE(reader->next(&kmer));
    EXPECT_EQ(std::to_array<uint8_t>({'G', 'G', 'T', 'A'}), kmer);
    ASSERT_FALSE(reader->next(&kmer));
  }

  // 8mers
  {
    constexpr size_t K = 8;
    std::unique_ptr<std::istream> input =
          std::make_unique<std::istringstream>(FIVE_SEQS_N);
    auto reader = std::make_unique<FastqKMerPreloadReader<K>>(std::move(input));
    std::array<uint8_t, K> kmer;
    EXPECT_FALSE(reader->next(&kmer));
  }
}

}  // namespace
}  // namespace input_reader
}  // namespace kmercounter