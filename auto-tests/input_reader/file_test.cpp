#include <absl/strings/str_join.h>
#include <boost/range/adaptor/transformed.hpp>
#include <boost/range/irange.hpp>
#include <boost/range/numeric.hpp>
#include <gtest/gtest.h>
#include <sstream>

#include "input_reader/file.hpp"

using boost::accumulate;
using boost::irange;
using boost::adaptors::transformed;

namespace kmercounter {
namespace input_reader {

namespace {
/// Consumes the reader and return the size of it.
template<typename T>
size_t reader_size(InputReader<T> *reader) {
  size_t size = 0;
  while (reader->next()) {
    size++;
  }
  return size;
}

/// Generate comma seperated a CSV file.
std::string generate_csv(uint64_t num_rows, uint64_t num_cols=3) {
  std::string csv;
  for (uint64_t row = 0; row < num_rows; row++) {
    std::vector<uint64_t> fields;
    for (uint64_t col = 0 ; col < num_cols; col++) {
      fields.push_back(row + col * col);
    }
    csv += absl::StrJoin(fields, ",");
    csv += '\n';
  }
  return csv;
}

/// Make a std::array without providing a size.
/// CITE: https://stackoverflow.com/a/26351623
template <typename... T>
constexpr auto make_array(T&&... values) ->
    std::array<
       typename std::decay<
           typename std::common_type<T...>::type>::type,
       sizeof...(T)> {
    return std::array<
        typename std::decay<
            typename std::common_type<T...>::type>::type,
        sizeof...(T)>{std::forward<T>(values)...};
}

TEST(PartitionedFileTest, SimplePartitionTest) {
  const char* data = R"(line 1
    line 2
    line 3
    line 4
    line 5)";
  const size_t line_count = 5;

  // Test 1 partition.
  {
    std::unique_ptr<std::istream> file = std::make_unique<std::istringstream>(data);
    PartitionedFileReader reader(std::move(file), 0, 1);
    EXPECT_EQ(line_count, reader_size(&reader));
  }
}

TEST(PartitionedFileTest, PartitionTest) {
  constexpr auto num_liness = make_array(1, 2, 3, 4, 6, 9, 13, 17, 19, 21, 22, 24, 100, 1000);
  constexpr auto num_partss = make_array(1, 2, 3, 4, 5, 6, 9, 13, 17, 19, 64);

  for (const auto num_lines : num_liness) {
    std::string csv = generate_csv(num_lines);
    for (const auto num_parts : num_partss) {
      auto readers = irange(num_parts) | transformed([&csv, num_parts](uint64_t part_id) {
        std::unique_ptr<std::istream> file = std::make_unique<std::istringstream>(csv);
        PartitionedFileReader reader(std::move(file), part_id, num_parts);
        return reader;
      });

      auto lines_read = readers | transformed([](PartitionedFileReader reader) {
        const uint64_t lines_read = reader_size(&reader);
        PLOG_DEBUG << "Reader " << reader.part_id() << " read " << lines_read << " lines.";
        return lines_read;
      });

      const uint64_t total_lines_read = accumulate(lines_read, 0ul);
      ASSERT_EQ(num_lines, total_lines_read) << "Incorrect number of lines read for " << num_parts << " partitions.";
    }
  }
}

} // namespace
} // namespace input_reader
} // namespace kmercounter