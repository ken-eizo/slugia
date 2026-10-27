cmake_minimum_required(VERSION 3.24)

project(slugia VERSION 0.1.0 LANGUAGES C CXX)

include(cmake/SlugUI.cmake)

option(SLUGIA_BUILD_EXAMPLE "Build the kitchen-sink example" ${PROJECT_IS_TOP_LEVEL})
option(SLUGIA_BUILD_TESTS "Build unit tests" ${PROJECT_IS_TOP_LEVEL})
option(SLUGIA_BUILD_BENCHMARKS "Build CPU microbenchmarks" OFF)
option(SLUGIA_BUILD_GPU_TESTS "Build Vulkan readback tests (requires a working display/driver)" OFF)
option(SLUGIA_ENABLE_3D "Build the optional UI-oriented vector 3D extension" ON)
option(SLUGIA_ENABLE_SANITIZERS "Instrument slugia with address/undefined sanitizers (Clang/GCC)" OFF)
option(SLUGIA_ENABLE_VALIDATION "Enable Vulkan validation in debug builds" ON)
option(SLUGIA_STATIC_MSVC_RUNTIME "Link the standalone MSVC build against the static runtime"
       ${PROJECT_IS_TOP_LEVEL})
option(SLUGIA_INSTALL "Install the raw static library and public headers" ${PROJECT_IS_TOP_LEVEL})
option(SLUGIA_ENABLE_GLFW "Build the optional GLFW Window adapter" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(MSVC AND SLUGIA_STATIC_MSVC_RUNTIME)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# LunarG's per-user Windows installer can leave VULKAN_SDK unset even though a complete SDK
# is installed under %USERPROFILE%/VulkanSDK. Give FindVulkan explicit hints in that case so a
# clean clone configures without depending on an old CMake cache.
if(WIN32 AND "$ENV{VULKAN_SDK}" STREQUAL "")
  file(GLOB _slugia_vulkan_sdk_candidates LIST_DIRECTORIES true
    "$ENV{USERPROFILE}/VulkanSDK/*"
    "C:/VulkanSDK/*")
  list(SORT _slugia_vulkan_sdk_candidates COMPARE NATURAL ORDER DESCENDING)
  foreach(_slugia_vulkan_sdk IN LISTS _slugia_vulkan_sdk_candidates)
    if(EXISTS "${_slugia_vulkan_sdk}/Include/vulkan/vulkan.h" AND
       EXISTS "${_slugia_vulkan_sdk}/Lib/vulkan-1.lib")
      set(Vulkan_INCLUDE_DIR "${_slugia_vulkan_sdk}/Include" CACHE PATH "" FORCE)
      set(Vulkan_LIBRARY "${_slugia_vulkan_sdk}/Lib/vulkan-1.lib" CACHE FILEPATH "" FORCE)
      if(EXISTS "${_slugia_vulkan_sdk}/Bin/glslc.exe")
        set(Vulkan_GLSLC_EXECUTABLE "${_slugia_vulkan_sdk}/Bin/glslc.exe"
            CACHE FILEPATH "" FORCE)
      endif()
      message(STATUS "slugia: using Vulkan SDK ${_slugia_vulkan_sdk}")
      break()
    endif()
  endforeach()
  unset(_slugia_vulkan_sdk)
  unset(_slugia_vulkan_sdk_candidates)
endif()

find_package(Vulkan REQUIRED)

if(SLUGIA_ENABLE_GLFW)
  set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
  add_subdirectory(third_party/glfw)
endif()

set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
add_subdirectory(third_party/freetype)

# slugia keeps text shaping separate from outline extraction. HarfBuzz owns GSUB/GPOS,
# script/cluster shaping, while FreeType + Slughorn own exact outline decomposition and Slug
# coverage. Build only the HarfBuzz core; raster/vector/GPU/subset utilities are unrelated here.
set(HB_BUILD_UTILS OFF CACHE BOOL "" FORCE)
set(HB_BUILD_SUBSET OFF CACHE BOOL "" FORCE)
set(HB_BUILD_RASTER OFF CACHE BOOL "" FORCE)
set(HB_BUILD_VECTOR OFF CACHE BOOL "" FORCE)
set(HB_BUILD_GPU OFF CACHE BOOL "" FORCE)
set(HB_BUILD_GPU_DEMO OFF CACHE STRING "" FORCE)
set(HB_HAVE_FREETYPE OFF CACHE BOOL "" FORCE)
set(HB_HAVE_CAIRO OFF CACHE BOOL "" FORCE)
set(HB_HAVE_GLIB OFF CACHE BOOL "" FORCE)
set(HB_HAVE_ICU OFF CACHE BOOL "" FORCE)
set(HB_HAVE_GOBJECT OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/harfbuzz)
if(NOT TARGET Freetype::Freetype)
  add_library(Freetype::Freetype ALIAS freetype-interface)
endif()

# Build only the Slughorn core and FreeType bridge used by slugia.
add_library(slughorn STATIC third_party/slughorn/slughorn/slughorn.cpp)
target_include_directories(slughorn SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/third_party/slughorn")
target_compile_features(slughorn PUBLIC cxx_std_20)
target_compile_definitions(slughorn PRIVATE SLUGHORN_FREETYPE_IMPLEMENTATION PUBLIC SLUGHORN_HAS_FREETYPE)
target_link_libraries(slughorn PUBLIC Freetype::Freetype)
set_target_properties(slughorn PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
  target_compile_options(slughorn PRIVATE /W4 /wd4100)
else()
  target_compile_options(slughorn PRIVATE -Wall -Wextra -Wpedantic -Wno-unused-parameter)
endif()

set(SLUGIA_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(SLUGIA_SHADER_DIR "${SLUGIA_GENERATED_DIR}/shaders")
file(MAKE_DIRECTORY "${SLUGIA_SHADER_DIR}")

if(NOT Vulkan_GLSLC_EXECUTABLE)
  find_program(Vulkan_GLSLC_EXECUTABLE NAMES glslc HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
endif()
if(NOT Vulkan_GLSLC_EXECUTABLE)
  message(FATAL_ERROR "glslc was not found. Install the Vulkan SDK or set Vulkan_GLSLC_EXECUTABLE.")
endif()

function(slugia_compile_shader source)
  get_filename_component(name "${source}" NAME)
  set(output "${SLUGIA_SHADER_DIR}/${name}.spv")
  add_custom_command(
    OUTPUT "${output}"
    COMMAND "${Vulkan_GLSLC_EXECUTABLE}" --target-env=vulkan1.1 -O "${CMAKE_CURRENT_SOURCE_DIR}/${source}" -o "${output}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${source}"
    VERBATIM
  )
  set(SLUGIA_SHADER_OUTPUTS ${SLUGIA_SHADER_OUTPUTS} "${output}" PARENT_SCOPE)
endfunction()

slugia_compile_shader(src/shaders/vector.vert)
slugia_compile_shader(src/shaders/vector.frag)
slugia_compile_shader(src/shaders/primitive.vert)
slugia_compile_shader(src/shaders/primitive.frag)
set(SLUGIA_EMBEDDED_SHADER_HEADER "${SLUGIA_GENERATED_DIR}/slugia_embedded_shaders.hpp")
list(GET SLUGIA_SHADER_OUTPUTS 0 SLUGIA_VERTEX_SPV)
list(GET SLUGIA_SHADER_OUTPUTS 1 SLUGIA_FRAGMENT_SPV)
list(GET SLUGIA_SHADER_OUTPUTS 2 SLUGIA_PRIMITIVE_VERTEX_SPV)
list(GET SLUGIA_SHADER_OUTPUTS 3 SLUGIA_PRIMITIVE_FRAGMENT_SPV)
add_custom_command(
  OUTPUT "${SLUGIA_EMBEDDED_SHADER_HEADER}"
  COMMAND "${CMAKE_COMMAND}"
    -DVERTEX_SPV=${SLUGIA_VERTEX_SPV}
    -DFRAGMENT_SPV=${SLUGIA_FRAGMENT_SPV}
    -DPRIMITIVE_VERTEX_SPV=${SLUGIA_PRIMITIVE_VERTEX_SPV}
    -DPRIMITIVE_FRAGMENT_SPV=${SLUGIA_PRIMITIVE_FRAGMENT_SPV}
    -DOUTPUT=${SLUGIA_EMBEDDED_SHADER_HEADER}
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_spirv.cmake"
  DEPENDS ${SLUGIA_SHADER_OUTPUTS} "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_spirv.cmake"
  VERBATIM
)
add_custom_target(slugia_shaders DEPENDS "${SLUGIA_EMBEDDED_SHADER_HEADER}")

set(SLUGIA_SOURCES
  src/animation.cpp
  src/draw_list.cpp
  src/input.cpp
  src/render_device.cpp
  src/svg.cpp
  src/slugui.cpp
  src/ui.cpp
  src/vector_atlas.cpp
  src/vector_path.cpp
  src/vulkan_renderer.cpp
  "${SLUGIA_EMBEDDED_SHADER_HEADER}"
)
if(SLUGIA_ENABLE_3D)
  list(APPEND SLUGIA_SOURCES src/vector3d.cpp src/vector_scene3d.cpp src/curve3d.cpp src/import3d.cpp)
endif()
if(SLUGIA_ENABLE_GLFW)
  list(APPEND SLUGIA_SOURCES src/window.cpp)
endif()
if(WIN32)
  list(APPEND SLUGIA_SOURCES src/win32_surface.cpp)
endif()
add_library(slugia STATIC ${SLUGIA_SOURCES})
add_library(slugia::slugia ALIAS slugia)
add_dependencies(slugia slugia_shaders)
target_include_directories(slugia PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
                                  PRIVATE "${SLUGIA_GENERATED_DIR}")
target_include_directories(slugia SYSTEM PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nanosvg")
target_link_libraries(slugia PUBLIC slughorn Vulkan::Vulkan PRIVATE harfbuzz)
if(SLUGIA_ENABLE_GLFW)
  target_link_libraries(slugia PRIVATE glfw)
endif()
if(WIN32)
  target_link_libraries(slugia PRIVATE user32)
endif()
target_compile_definitions(slugia PUBLIC SLUGIA_ENABLE_GLFW=$<BOOL:${SLUGIA_ENABLE_GLFW}>)
target_compile_definitions(slugia PUBLIC SLUGIA_ENABLE_3D=$<BOOL:${SLUGIA_ENABLE_3D}>)
target_compile_definitions(slugia PRIVATE
  $<$<BOOL:${SLUGIA_ENABLE_VALIDATION}>:SLUGIA_ENABLE_VALIDATION=1>
)
if(SLUGIA_ENABLE_SANITIZERS)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT MSVC)
    target_compile_options(slugia PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(slugia PUBLIC -fsanitize=address,undefined)
  else()
    message(FATAL_ERROR "SLUGIA_ENABLE_SANITIZERS requires a GCC or Clang driver")
  endif()
endif()

if(MSVC)
  # /FS serializes PDB writes inside cl.exe. The example and generated Figma bridge are compiled
  # concurrently during low-latency imports, so without it MSBuild can fail with C1041.
  target_compile_options(slugia PRIVATE /W4 /permissive- /Zc:__cplusplus /FS /utf-8)
else()
  target_compile_options(slugia PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(SLUGIA_BUILD_EXAMPLE AND NOT SLUGIA_ENABLE_GLFW)
  message(FATAL_ERROR "SLUGIA_BUILD_EXAMPLE requires SLUGIA_ENABLE_GLFW=ON")
endif()

if(SLUGIA_BUILD_EXAMPLE)
  set(SLUGIA_GEIST_FONT "${CMAKE_CURRENT_SOURCE_DIR}/examples/assets/fonts/Geist[wght].ttf")
  set(SLUGIA_GEIST_HEADER "${SLUGIA_GENERATED_DIR}/geist_font.generated.hpp")
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_custom_command(
    OUTPUT "${SLUGIA_GEIST_HEADER}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_binary.py"
      "${SLUGIA_GEIST_FONT}" "${SLUGIA_GEIST_HEADER}"
      --namespace slugia::example::assets --symbol geistFont
    DEPENDS "${SLUGIA_GEIST_FONT}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_binary.py"
    VERBATIM)
  add_custom_target(slugia_geist_font DEPENDS "${SLUGIA_GEIST_HEADER}")
  set(SLUGIA_TIGER_HEADER "${SLUGIA_GENERATED_DIR}/tiger_svg.generated.hpp")
  add_custom_command(OUTPUT "${SLUGIA_TIGER_HEADER}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_binary.py"
      "${CMAKE_CURRENT_SOURCE_DIR}/examples/assets/Ghostscript_Tiger.svg" "${SLUGIA_TIGER_HEADER}"
      --namespace slugia::example::assets --symbol tigerSvg
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/examples/assets/Ghostscript_Tiger.svg"
      "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_binary.py" VERBATIM)
  add_custom_target(slugia_tiger_svg DEPENDS "${SLUGIA_TIGER_HEADER}")
  set(SLUGIA_SLUGUI_DEMO_HEADER "${SLUGIA_GENERATED_DIR}/slugui_demo.generated.hpp")
  set(SLUGIA_FIGMA_DEMO_HEADER "${SLUGIA_GENERATED_DIR}/figma_sample.generated.hpp")
  set(SLUGIA_SLUGUI_DEMO_STAMP "${SLUGIA_SLUGUI_DEMO_HEADER}.stamp")
  set(SLUGIA_FIGMA_DEMO_STAMP "${SLUGIA_FIGMA_DEMO_HEADER}.stamp")
  slugia_compile_slugui(
    STAMPED
    INPUT "${CMAKE_CURRENT_SOURCE_DIR}/examples/slugui_demo.slugui"
    OUTPUT "${SLUGIA_SLUGUI_DEMO_HEADER}"
    NAMESPACE slugia::example)
  slugia_compile_slugui(
    STAMPED
    STRIP_SOURCE_METADATA
    INPUT "${CMAKE_CURRENT_SOURCE_DIR}/examples/figma_sample.slugui"
    OUTPUT "${SLUGIA_FIGMA_DEMO_HEADER}"
    NAMESPACE slugia::example
    CLASS_NAME FigmaSampleGenerated)
  add_custom_target(slugia_slugui_example_codegen DEPENDS
    "${SLUGIA_SLUGUI_DEMO_STAMP}"
    "${SLUGIA_FIGMA_DEMO_STAMP}")
  add_executable(slugia_example MACOSX_BUNDLE
    examples/kitchen_sink.cpp
    examples/figma_import_bridge.cpp
    "${SLUGIA_GEIST_HEADER}"
    "${CMAKE_CURRENT_SOURCE_DIR}/examples/assets/fonts/OFL-Geist.txt")
  # Figma AOT output can contain thousands of construction statements. Keep that generated
  # code isolated in one tiny translation unit and compile that bridge without optimization.
  # The generator scopes every child subtree so moved temporaries do not accumulate in one huge
  # /Od stack frame; this gives fast iterative builds without the former stack-overflow hazard.
  if(MSVC)
    target_compile_options(slugia_example PRIVATE /FS)
    set_source_files_properties(examples/figma_import_bridge.cpp
      PROPERTIES COMPILE_OPTIONS "/Od;/Ob0;/bigobj")
  else()
    set_source_files_properties(examples/figma_import_bridge.cpp
      PROPERTIES COMPILE_OPTIONS "-O0")
  endif()
  target_link_libraries(slugia_example PRIVATE slugia::slugia)
  set_target_properties(slugia_example PROPERTIES OUTPUT_NAME slugia-example)
  target_include_directories(slugia_example PRIVATE "${SLUGIA_GENERATED_DIR}")
  # The generated Figma header is a BYPRODUCT of the stamp command. Make the bridge object's
  # dependency explicit so Visual Studio cannot reuse an object built against stale generated UI.
  set_property(SOURCE examples/figma_import_bridge.cpp APPEND PROPERTY
    OBJECT_DEPENDS "${SLUGIA_FIGMA_DEMO_HEADER}")
  target_compile_definitions(slugia_example PRIVATE
    SLUGIA_DEVELOPMENT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    SLUGIA_DEVELOPMENT_BUILD_DIR="${CMAKE_BINARY_DIR}"
    SLUGIA_DEVELOPMENT_CONFIG="$<CONFIG>"
    SLUGIA_CMAKE_COMMAND="${CMAKE_COMMAND}"
    SLUGIA_PYTHON_EXECUTABLE="${Python3_EXECUTABLE}"
    SLUGIA_FIGMA_GENERATED_HEADER="${SLUGIA_FIGMA_DEMO_HEADER}"
  )
  add_dependencies(slugia_example slugia_shaders slugia_slugui_example_codegen slugia_geist_font)
  add_executable(slugia_vector_demo examples/vector_benchmark.cpp "${SLUGIA_GEIST_HEADER}")
  target_link_libraries(slugia_vector_demo PRIVATE slugia::slugia)
  set_target_properties(slugia_vector_demo PROPERTIES OUTPUT_NAME slugia-vector-baseline)
  target_include_directories(slugia_vector_demo PRIVATE "${SLUGIA_GENERATED_DIR}")
  target_compile_definitions(slugia_vector_demo PRIVATE SLUGIA_VECTOR_EMBEDDED_FONT=1
    SLUGIA_VECTOR_TIGER="${CMAKE_CURRENT_SOURCE_DIR}/examples/assets/Ghostscript_Tiger.svg")
  add_dependencies(slugia_vector_demo slugia_geist_font slugia_tiger_svg)
  add_executable(slugia_vector_fast_demo examples/vector_benchmark.cpp "${SLUGIA_GEIST_HEADER}")
  target_link_libraries(slugia_vector_fast_demo PRIVATE slugia::slugia)
  set_target_properties(slugia_vector_fast_demo PROPERTIES OUTPUT_NAME slugia-vector)
  target_include_directories(slugia_vector_fast_demo PRIVATE "${SLUGIA_GENERATED_DIR}")
  target_compile_definitions(slugia_vector_fast_demo PRIVATE SLUGIA_VECTOR_EMBEDDED_FONT=1
    SLUGIA_VECTOR_FAST_PREVIEW=1
    SLUGIA_VECTOR_TIGER="${CMAKE_CURRENT_SOURCE_DIR}/examples/assets/Ghostscript_Tiger.svg")
  add_dependencies(slugia_vector_fast_demo slugia_geist_font slugia_tiger_svg)
  if(SLUGIA_ENABLE_3D)
    add_executable(slugia_vector3d_demo MACOSX_BUNDLE examples/vector3d_demo.cpp "${SLUGIA_GEIST_HEADER}")
    target_link_libraries(slugia_vector3d_demo PRIVATE slugia::slugia)
    set_target_properties(slugia_vector3d_demo PROPERTIES OUTPUT_NAME slugia-3d)
    target_include_directories(slugia_vector3d_demo PRIVATE "${SLUGIA_GENERATED_DIR}")
    add_dependencies(slugia_vector3d_demo slugia_geist_font)
  endif()
  set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/examples/assets/fonts/OFL-Geist.txt"
    PROPERTIES HEADER_FILE_ONLY TRUE MACOSX_PACKAGE_LOCATION Resources)
  set_target_properties(slugia_example PROPERTIES
    MACOSX_BUNDLE_BUNDLE_NAME "slugia Example"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.ken-eizo.slugia.example"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}")
endif()

if(SLUGIA_BUILD_TESTS)
  enable_testing()
  add_executable(slugia_tests tests/core_tests.cpp)
  target_link_libraries(slugia_tests PRIVATE slugia::slugia)
  add_test(NAME slugia_core COMMAND slugia_tests)
  add_executable(slugia_geometry_tests tests/geometry_tests.cpp)
  target_include_directories(slugia_geometry_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(slugia_geometry_tests PRIVATE slugia::slugia)
  add_test(NAME slugia_geometry COMMAND slugia_geometry_tests)
  add_executable(slugia_band_pack_tests tests/band_pack_tests.cpp)
  target_include_directories(slugia_band_pack_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_compile_features(slugia_band_pack_tests PRIVATE cxx_std_20)
  add_test(NAME slugia_band_pack COMMAND slugia_band_pack_tests)
  add_executable(slugia_svg_tests tests/svg_tests.cpp)
  target_include_directories(slugia_svg_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(slugia_svg_tests PRIVATE slugia::slugia)
  target_compile_definitions(slugia_svg_tests PRIVATE SLUGIA_TEST_TIGER="${CMAKE_CURRENT_SOURCE_DIR}/examples/assets/Ghostscript_Tiger.svg")
  add_test(NAME slugia_svg COMMAND slugia_svg_tests)
  set_tests_properties(slugia_svg PROPERTIES TIMEOUT 60)
  set_tests_properties(slugia_geometry PROPERTIES TIMEOUT 30)
  if(SLUGIA_ENABLE_3D)
    add_executable(slugia_vector3d_tests tests/vector3d_tests.cpp)
    target_link_libraries(slugia_vector3d_tests PRIVATE slugia::slugia)
    add_test(NAME slugia_vector3d COMMAND slugia_vector3d_tests)
    set_tests_properties(slugia_vector3d PROPERTIES TIMEOUT 60)
  endif()
  add_executable(slugia_stress_tests tests/stress_tests.cpp)
  target_link_libraries(slugia_stress_tests PRIVATE slugia::slugia)
  add_test(NAME slugui_stress COMMAND slugia_stress_tests)
  add_executable(slugia_fuzz_tests tests/fuzz_tests.cpp)
  target_link_libraries(slugia_fuzz_tests PRIVATE slugia::slugia)
  add_test(NAME slugia_fuzz COMMAND slugia_fuzz_tests)
  add_executable(slugia_golden_tests tests/golden_tests.cpp)
  target_link_libraries(slugia_golden_tests PRIVATE slugia::slugia)
  add_test(NAME slugui_golden COMMAND slugia_golden_tests)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_test(NAME slugui_compiler
    COMMAND "${Python3_EXECUTABLE}" -m unittest
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/slugui_compiler_test.py" -v)
  set_tests_properties(slugui_compiler PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  add_test(NAME slugia_campaign_reporting
    COMMAND "${Python3_EXECUTABLE}" -m unittest
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/vector_campaign_test.py"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/benchmark_protocol_test.py" -v)
  set_tests_properties(slugia_campaign_reporting PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 20)
  find_program(SLUGIA_NODE_EXECUTABLE NAMES node)
  if(SLUGIA_NODE_EXECUTABLE)
    add_test(NAME slugui_figma_exporter
      COMMAND "${SLUGIA_NODE_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/figma_plugin_test.js")
    set_tests_properties(slugui_figma_exporter PROPERTIES
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      ENVIRONMENT "SLUGUI_PYTHON=${Python3_EXECUTABLE}")
  else()
    message(WARNING "Node.js was not found; the Figma exporter integration test is disabled.")
  endif()
endif()

if(SLUGIA_BUILD_GPU_TESTS)
  if(NOT SLUGIA_BUILD_TESTS OR NOT SLUGIA_ENABLE_GLFW)
    message(FATAL_ERROR "SLUGIA_BUILD_GPU_TESTS requires SLUGIA_BUILD_TESTS and SLUGIA_ENABLE_GLFW")
  endif()
  add_executable(slugia_gpu_tests tests/gpu_tests.cpp)
  target_link_libraries(slugia_gpu_tests PRIVATE slugia::slugia)
  add_test(NAME slugia_gpu COMMAND slugia_gpu_tests)
  set_tests_properties(slugia_gpu PROPERTIES LABELS gpu TIMEOUT 120)
  add_executable(slugia_vector_quality_tests tests/vector_quality_tests.cpp)
  target_link_libraries(slugia_vector_quality_tests PRIVATE slugia::slugia)
  target_compile_definitions(slugia_vector_quality_tests PRIVATE
    SLUGIA_QUALITY_FONT="${CMAKE_CURRENT_SOURCE_DIR}/examples/assets/fonts/Geist[wght].ttf")
  add_test(NAME slugia_vector_quality COMMAND slugia_vector_quality_tests)
  set_tests_properties(slugia_vector_quality PROPERTIES LABELS gpu TIMEOUT 120)
  add_test(NAME slugia_vector_storage_quality COMMAND slugia_vector_quality_tests 40 --atlas-storage)
  add_test(NAME slugia_vector_storage_low_spec_quality COMMAND slugia_vector_quality_tests 40 --atlas-storage-low-spec)
  add_test(NAME slugia_vector_compact_quality COMMAND slugia_vector_quality_tests 40 --compact-curves)
  add_test(NAME slugia_gpu_compact COMMAND slugia_gpu_tests --compact-curves)
  add_test(NAME slugia_gpu_cpu_phases COMMAND slugia_gpu_tests --cpu-phases)
  set_tests_properties(slugia_gpu_cpu_phases PROPERTIES LABELS gpu TIMEOUT 120 SKIP_RETURN_CODE 77)
  add_test(NAME slugia_gpu_packed_bands COMMAND slugia_gpu_tests --packed-bands)
  add_test(NAME slugia_gpu_research_profile COMMAND slugia_gpu_tests --research-profile)
  add_test(NAME slugia_gpu_research_profile_low_spec COMMAND slugia_gpu_tests --research-profile-low-spec)
  add_test(NAME slugia_vector_research_profile_quality COMMAND slugia_vector_quality_tests 40 --research-profile)
  set_tests_properties(slugia_gpu_research_profile slugia_gpu_research_profile_low_spec slugia_vector_research_profile_quality
    PROPERTIES LABELS gpu TIMEOUT 120)
  add_test(NAME slugia_vector_packed_bands_quality COMMAND slugia_vector_quality_tests 40 --packed-bands)
  set_tests_properties(slugia_gpu_packed_bands slugia_vector_packed_bands_quality PROPERTIES LABELS gpu TIMEOUT 120)
  set_tests_properties(slugia_vector_compact_quality slugia_gpu_compact PROPERTIES LABELS gpu TIMEOUT 120)
  set_tests_properties(slugia_vector_storage_quality slugia_vector_storage_low_spec_quality
    PROPERTIES LABELS gpu TIMEOUT 120)
  add_test(NAME slugia_gpu_indexed COMMAND slugia_gpu_tests --indexed)
  add_test(NAME slugia_vector_indexed_quality COMMAND slugia_vector_quality_tests 40 --indexed)
  set_tests_properties(slugia_gpu_indexed slugia_vector_indexed_quality PROPERTIES LABELS gpu TIMEOUT 120)
  # Regression gate for the disabled tile prototype. This finite matrix does
  # not establish arbitrary SVG/zoom support or enable the optimization.
  add_executable(slugia_svg_tile_quality_tests tests/svg_tile_quality_tests.cpp)
  target_link_libraries(slugia_svg_tile_quality_tests PRIVATE slugia::slugia)
  target_compile_definitions(slugia_svg_tile_quality_tests PRIVATE
    SLUGIA_TEST_TIGER="${CMAKE_CURRENT_SOURCE_DIR}/examples/assets/Ghostscript_Tiger.svg")
  add_test(NAME slugia_svg_tile_quality COMMAND slugia_svg_tile_quality_tests --matrix)
  set_tests_properties(slugia_svg_tile_quality PROPERTIES LABELS gpu TIMEOUT 120)
  add_test(NAME slugia_svg_storage_quality COMMAND slugia_svg_tile_quality_tests --storage)
  set_tests_properties(slugia_svg_storage_quality PROPERTIES LABELS gpu TIMEOUT 120)
  add_test(NAME slugia_svg_band_quality COMMAND slugia_svg_tile_quality_tests --bands)
  set_tests_properties(slugia_svg_band_quality PROPERTIES LABELS gpu TIMEOUT 120)
  add_test(NAME slugia_svg_band_low_spec_quality COMMAND slugia_svg_tile_quality_tests --bands-low-spec)
  set_tests_properties(slugia_svg_band_low_spec_quality PROPERTIES LABELS gpu TIMEOUT 120)
  add_test(NAME slugia_svg_band256_quality COMMAND slugia_svg_tile_quality_tests --bands256)
  add_test(NAME slugia_svg_band256_low_spec_quality COMMAND slugia_svg_tile_quality_tests --bands256-low-spec)
  set_tests_properties(slugia_svg_band256_quality slugia_svg_band256_low_spec_quality
    PROPERTIES LABELS gpu TIMEOUT 120)
  add_test(NAME slugia_svg_band512_quality COMMAND slugia_svg_tile_quality_tests --bands512)
  add_test(NAME slugia_svg_band512_low_spec_quality COMMAND slugia_svg_tile_quality_tests --bands512-low-spec)
  add_test(NAME slugia_svg_band1024_quality COMMAND slugia_svg_tile_quality_tests --bands1024)
  add_test(NAME slugia_svg_band1024_low_spec_quality COMMAND slugia_svg_tile_quality_tests --bands1024-low-spec)
  add_test(NAME slugia_svg_compact_quality COMMAND slugia_svg_tile_quality_tests --compact-curves)
  add_test(NAME slugia_svg_packed_bands_quality COMMAND slugia_svg_tile_quality_tests --packed-bands)
  add_test(NAME slugia_svg_packed_bands_low_spec_quality COMMAND slugia_svg_tile_quality_tests --packed-bands-low-spec)
  add_test(NAME slugia_svg_packed_bands2048_quality COMMAND slugia_svg_tile_quality_tests --packed-bands2048)
  add_test(NAME slugia_svg_packed_bands2048_low_spec_quality COMMAND slugia_svg_tile_quality_tests --packed-bands2048-low-spec)
  add_test(NAME slugia_svg_research_profile_quality COMMAND slugia_svg_tile_quality_tests --research-profile)
  add_test(NAME slugia_svg_research_profile_low_spec_quality COMMAND slugia_svg_tile_quality_tests --research-profile-low-spec)
  set_tests_properties(slugia_svg_research_profile_quality slugia_svg_research_profile_low_spec_quality PROPERTIES LABELS gpu TIMEOUT 120)
  set_tests_properties(slugia_svg_packed_bands2048_quality slugia_svg_packed_bands2048_low_spec_quality PROPERTIES LABELS gpu TIMEOUT 120)
  set_tests_properties(slugia_svg_packed_bands_quality slugia_svg_packed_bands_low_spec_quality PROPERTIES LABELS gpu TIMEOUT 120)
  add_test(NAME slugia_svg_compact_low_spec_quality COMMAND slugia_svg_tile_quality_tests --compact-curves-low-spec)
  set_tests_properties(slugia_svg_compact_quality slugia_svg_compact_low_spec_quality PROPERTIES LABELS gpu TIMEOUT 120)
  set_tests_properties(slugia_svg_band1024_quality slugia_svg_band1024_low_spec_quality
    PROPERTIES LABELS gpu TIMEOUT 120)
  set_tests_properties(slugia_svg_band512_quality slugia_svg_band512_low_spec_quality
    PROPERTIES LABELS gpu TIMEOUT 120)
  add_test(NAME slugia_svg_indexed_quality COMMAND slugia_svg_tile_quality_tests --indexed)
  set_tests_properties(slugia_svg_indexed_quality PROPERTIES LABELS gpu TIMEOUT 120)
  if(TARGET slugia_vector_demo)
    add_test(NAME slugia_vector_smoke COMMAND slugia_vector_demo --smoke)
    set_tests_properties(slugia_vector_smoke PROPERTIES LABELS gpu TIMEOUT 120)
  endif()
  if(TARGET slugia_example)
    add_test(NAME slugia_example_smoke COMMAND slugia_example --smoke)
    set_tests_properties(slugia_example_smoke PROPERTIES LABELS gpu TIMEOUT 120)
  endif()
  if(TARGET slugia_vector_fast_demo)
    add_test(NAME slugia_vector_fast_smoke COMMAND slugia_vector_fast_demo --smoke)
    set_tests_properties(slugia_vector_fast_smoke PROPERTIES LABELS gpu TIMEOUT 120)
  endif()
  if(TARGET slugia_vector3d_demo)
    add_test(NAME slugia_vector3d_smoke COMMAND slugia_vector3d_demo --smoke)
    add_test(NAME slugia_vector3d_asset_smoke COMMAND slugia_vector3d_demo
      --smoke --asset "${CMAKE_CURRENT_SOURCE_DIR}/examples/vector3d_demo.slug3d")
    add_test(NAME slugia_vector3d_baseline_smoke COMMAND slugia_vector3d_demo --smoke --baseline)
    add_test(NAME slugia_vector3d_low_spec_smoke COMMAND slugia_vector3d_demo --smoke --low-spec)
    set_tests_properties(slugia_vector3d_baseline_smoke slugia_vector3d_low_spec_smoke
      PROPERTIES LABELS gpu TIMEOUT 120)
    set_tests_properties(slugia_vector3d_smoke slugia_vector3d_asset_smoke
      PROPERTIES LABELS gpu TIMEOUT 120)
  endif()
endif()

if(SLUGIA_BUILD_BENCHMARKS)
  add_executable(slugia_cpu_benchmarks tests/cpu_benchmarks.cpp)
  target_link_libraries(slugia_cpu_benchmarks PRIVATE slugia::slugia)
  if(SLUGIA_ENABLE_GLFW)
    add_executable(slugia_svg_benchmarks examples/vector_benchmark.cpp)
    target_link_libraries(slugia_svg_benchmarks PRIVATE slugia::slugia)
    target_compile_definitions(slugia_svg_benchmarks PRIVATE
      SLUGIA_VECTOR_TIGER="${CMAKE_CURRENT_SOURCE_DIR}/examples/assets/Ghostscript_Tiger.svg"
      SLUGIA_VECTOR_FONT="${CMAKE_CURRENT_SOURCE_DIR}/examples/assets/fonts/Geist[wght].ttf")
  endif()
endif()

if(SLUGIA_INSTALL)
  # slugia is a static archive, so its static implementation dependencies must travel with a raw
  # install as archives too. FreeType and HarfBuzz install themselves from their subdirectories;
  # Slughorn and the optional GLFW adapter are owned/configured here.
  install(TARGETS slugia slughorn
    ARCHIVE DESTINATION lib LIBRARY DESTINATION lib RUNTIME DESTINATION bin)
  if(SLUGIA_ENABLE_GLFW)
    install(TARGETS glfw
      ARCHIVE DESTINATION lib LIBRARY DESTINATION lib RUNTIME DESTINATION bin)
  endif()
  install(DIRECTORY include/ DESTINATION include)
  install(DIRECTORY third_party/slughorn/slughorn/ DESTINATION include/slughorn
    FILES_MATCHING PATTERN "*.hpp")
  install(PROGRAMS tools/slugui_compiler.py DESTINATION bin)
  install(FILES tools/svg_path.py DESTINATION bin)
  install(FILES cmake/SlugUI.cmake DESTINATION share/slugia/cmake)
  install(DIRECTORY tools/figma-slugui DESTINATION share/slugia)
endif()
