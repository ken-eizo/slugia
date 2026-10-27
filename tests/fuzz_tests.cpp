#include "slugia/input.hpp"
#include "slugia/slugui.hpp"
#include "slugia/vector_atlas.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>

using namespace slugia;

namespace {
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
}

int main(int argc, char** argv) try {
  std::uint64_t scale = 1;
  if (argc != 1) {
    require(argc == 3 && std::string_view(argv[1]) == "--scale",
            "Expected --scale followed by a power of two");
    std::size_t consumed = 0;
    scale = std::stoull(argv[2], &consumed);
    require(consumed == std::string_view(argv[2]).size() && scale >= 1 &&
                scale <= (1U << 20U) && (scale & (scale - 1)) == 0,
            "Fuzz scale must be a power of two in 1..1048576");
  }
  std::mt19937 rng(0x51A6B00Bu);
  std::uniform_int_distribution<int> byteValue(0, 255);
  std::uniform_int_distribution<int> lengthValue(0, 48);
  for (std::uint64_t iteration = 0; iteration < 20000 * scale; ++iteration) {
    std::string bytes(static_cast<std::size_t>(lengthValue(rng)), '\0');
    for (char& value : bytes) value = static_cast<char>(byteValue(rng));
    const auto decoded = decodeUtf8(bytes);
    for (const auto codepoint : decoded)
      require(codepoint <= 0x10ffffU && !(codepoint >= 0xd800U && codepoint <= 0xdfffU),
              "UTF-8 decoder emitted an invalid Unicode scalar");
  }
  InputState input;
  InputWriter writer(input);
  std::uniform_int_distribution<std::size_t> selection(0, 64);
  for (std::uint64_t iteration = 0; iteration < 5000 * scale; ++iteration) {
    writer.beginFrame();
    const std::u32string text = U"composition";
    writer.composition(text, selection(rng), selection(rng));
    const auto& state = input.composition();
    require(state.selectionStart <= state.text.size(), "composition start was not clamped");
    require(state.selectionStart + state.selectionLength <= state.text.size(),
            "composition length was not clamped");
    if ((iteration & 1) == 0) writer.cancelComposition();
    else writer.commitComposition(U"x");
  }

  namespace sui = slugui;
  sui::PropertyStore properties;
  const auto source = properties.define<float>("source", 0.0f);
  const auto derived = properties.define<float>("derived", 1.0f);
  properties.bind<float>(derived, {source.id}, [source](const sui::PropertyStore& store) {
    return store.get(source) + 1.0f;
  });
  std::uniform_real_distribution<float> scalar(-10000.0f, 10000.0f);
  for (std::uint64_t iteration = 0; iteration < 10000 * scale; ++iteration) {
    const float value = scalar(rng);
    properties.set(source, value);
    properties.evaluateBindings();
    require(std::abs(properties.get(derived) - (value + 1.0f)) < 0.001f,
            "dirty binding propagation produced an incorrect value");
  }

  std::cout << "Deterministic fuzz tests passed; scale=" << scale
            << " utf8=" << 20000 * scale << " ime=" << 5000 * scale
            << " bindings=" << 10000 * scale << '\n';
  return 0;
} catch (const std::exception& error) {
  std::cerr << "Fuzz failure: " << error.what() << '\n';
  return 1;
}
