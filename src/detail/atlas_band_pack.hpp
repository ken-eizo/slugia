#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace slugia::detail {
inline constexpr std::uint32_t packedAtlasBandFlag = 0x02000000U;

// Headers keep 32-bit counts and word offsets. Only the curve ID lists use
// two uint16 IDs per word, independently aligned at each band's beginning.
// Leave the input intact if validation fails or packing does not save space.
inline bool packAtlasBandIndices(std::vector<std::uint32_t>& words,
                                 std::size_t querySize,
                                 std::size_t headerCount,
                                 std::size_t curveCount) {
  if (querySize > 4096 || headerCount > 8192 || curveCount > 65536 ||
      words.size() > std::numeric_limits<std::uint32_t>::max())
    return false;
  const auto prefix = 2 * querySize + 2 * headerCount;
  if (prefix > words.size()) return false;
  std::size_t required = prefix;
  for (std::size_t i = 0; i < headerCount; ++i) {
    const auto count = words[2 * querySize + 2 * i];
    const auto offset = words[2 * querySize + 2 * i + 1];
    if (offset < prefix || offset > words.size() || count > words.size() - offset)
      return false;
    const auto added = std::size_t(count) / 2 + (count & 1U);
    if (added > words.size() - required) return false;
    required += added;
    for (std::size_t j = 0; j < count; ++j)
      if (words[offset + j] >= curveCount) return false;
  }
  if (required >= words.size()) return false;
  std::vector<std::uint32_t> result(words.begin(), words.begin() + prefix);
  result.reserve(required);
  for (std::size_t i = 0; i < headerCount; ++i) {
    const auto header = 2 * querySize + 2 * i;
    const auto count = words[header], offset = words[header + 1];
    result[header + 1] = static_cast<std::uint32_t>(result.size());
    for (std::size_t j = 0; j < count; j += 2) {
      const auto low = words[offset + j];
      const auto high = j + 1 < count ? words[offset + j + 1] : 0U;
      result.push_back(low | (high << 16U));
    }
  }
  words.swap(result);
  return true;
}

inline std::uint32_t atlasBandCurveIndex(const std::uint32_t* words,
                                        std::size_t offset,
                                        std::size_t entry, bool packed) {
  const auto word = words[offset + (packed ? entry / 2 : entry)];
  return packed ? (word >> ((entry & 1U) * 16U)) & 65535U : word;
}
} // namespace slugia::detail
