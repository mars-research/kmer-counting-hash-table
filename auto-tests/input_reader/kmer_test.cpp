#include "input_reader/kmer.hpp"

#include <vector>
#include <array>

#include <gtest/gtest.h>

#include "input_reader/file.hpp"
#include "input_reader_test_utils.hpp"

namespace kmercounter {
namespace input_reader {
namespace {
TEST(KmerTest, SimpleTest) {
  const char* data = R"(ABCD
POG
)";

  std::unique_ptr<std::istream> file =
      std::make_unique<std::istringstream>(data);
  auto file_reader =
      std::make_unique<FileReader>(std::move(file), 0, 1);
  KMerReader<2, std::string_view> kmer_reader(std::move(file_reader));
  std::array<uint8_t, 2> kmer;
  EXPECT_TRUE(kmer_reader.next(&kmer));
  EXPECT_EQ(std::to_array<uint8_t>({'A', 'B'}), kmer);
  EXPECT_TRUE(kmer_reader.next(&kmer));
  EXPECT_EQ(std::to_array<uint8_t>({'B', 'C'}), kmer);
  EXPECT_TRUE(kmer_reader.next(&kmer));
  EXPECT_EQ(std::to_array<uint8_t>({'C', 'D'}), kmer);
  EXPECT_TRUE(kmer_reader.next(&kmer));
  EXPECT_EQ(std::to_array<uint8_t>({'P', 'O'}), kmer);
  EXPECT_TRUE(kmer_reader.next(&kmer));
  EXPECT_EQ(std::to_array<uint8_t>({'O', 'G'}), kmer);
  EXPECT_FALSE(kmer_reader.next(&kmer));
}

} // namespace
} // namespace input_reader
} // namespace kmercounter