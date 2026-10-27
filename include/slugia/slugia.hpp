#pragma once

#include "slugia/animation.hpp"
#include "slugia/draw_list.hpp"
#include "slugia/input.hpp"
#include "slugia/layout.hpp"
#include "slugia/platform_surface.hpp"
#include "slugia/render_device.hpp"
#include "slugia/slugui.hpp"
#include "slugia/svg.hpp"
#include "slugia/text_edit.hpp"
#include "slugia/types.hpp"
#include "slugia/ui.hpp"
#include "slugia/vector_atlas.hpp"
#include "slugia/vector_path.hpp"
#if defined(SLUGIA_ENABLE_3D) && SLUGIA_ENABLE_3D
#include "slugia/vector3d.hpp"
#endif
#include "slugia/vulkan_renderer.hpp"
#if defined(_WIN32)
#include "slugia/win32_surface.hpp"
#endif
#if defined(SLUGIA_ENABLE_GLFW) && SLUGIA_ENABLE_GLFW
#include "slugia/window.hpp"
#endif
