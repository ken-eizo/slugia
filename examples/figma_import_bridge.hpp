#pragma once

#include "slugia/slugui.hpp"

#include <optional>

namespace slugia {
class VectorAtlas;
}

namespace slugia::example {

// Keep the generated Figma translation unit out of kitchen_sink.cpp.
// This lets a .slugui change rebuild only the small bridge instead of the whole demo.
std::optional<slugui::Component> buildFigmaImport(VectorAtlas& atlas, bool enabled);

} // namespace slugia::example
