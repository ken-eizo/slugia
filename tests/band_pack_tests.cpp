#include "detail/atlas_band_pack.hpp"
#include <algorithm>
#include <iostream>
#include <random>
#include <stdexcept>

using namespace slugia::detail;
void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

int main() try {
  std::mt19937 rng(0xBADD16U);
  std::size_t entries = 0;
  for (int trial = 0; trial < 4096; ++trial) {
    const std::size_t headers = 2 + rng() % 64;
    const std::size_t curves = trial == 0 ? 65536 : 1 + rng() % 65536;
    std::vector<std::uint32_t> original(64 + 2 * headers, 0);
    for (std::size_t i = 0; i < 64; ++i) original[i] = rng() % (headers / 2);
    for (std::size_t i = 0; i < headers; ++i) {
      const auto count = 2 + rng() % 65;
      original[64 + 2 * i] = count;
      original[65 + 2 * i] = static_cast<std::uint32_t>(original.size());
      for (std::size_t j = 0; j < count; ++j)
        original.push_back(trial == 0 && j == 0 ? 65535U : rng() % curves);
      entries += count;
    }
    auto packed = original;
    require(packAtlasBandIndices(packed, 32, headers, curves), "packing failed");
    require(packed.size() < original.size(), "packing expanded its input");
    require(std::equal(packed.begin(), packed.begin() + 64, original.begin()), "indirection changed");
    for (std::size_t i = 0; i < headers; ++i) {
      require(packed[64 + 2 * i] == original[64 + 2 * i], "count changed");
      const auto count = original[64 + 2 * i];
      for (std::size_t j = 0; j < count; ++j)
        require(atlasBandCurveIndex(packed.data(), packed[65 + 2 * i], j, true) ==
                original[original[65 + 2 * i] + j], "order or index changed");
    }
    for (int corruption = 0; corruption < 4; ++corruption) {
      auto bad = original;
      if (corruption == 0) bad[65] = 0;
      if (corruption == 1) bad[65] = 0xffffffffU;
      if (corruption == 2) bad[64] = 0xffffffffU;
      if (corruption == 3) bad[bad[65]] = static_cast<std::uint32_t>(curves);
      const auto saved = bad;
      require(!packAtlasBandIndices(bad, 32, headers, curves), "corruption accepted");
      require(bad == saved, "failure mutated input");
    }
    auto oversized = original;
    require(!packAtlasBandIndices(oversized, 32, headers, 65537), "large IDs accepted");
    require(oversized == original, "fallback changed input");
  }
  std::vector<std::uint32_t> empty(68, 0);
  empty[65] = empty[67] = 68;
  const auto saved = empty;
  require(!packAtlasBandIndices(empty, 32, 2, 0) && empty == saved, "empty-list fallback failed");
  std::cout << "Band packing: 4096 seeded cases, " << entries
            << " ordered entries, invalid input rollback passed\n";
  return 0;
} catch (const std::exception& e) {
  std::cerr << e.what() << '\n';
  return 1;
}
