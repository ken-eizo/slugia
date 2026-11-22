if(NOT DEFINED BUILD_DIR OR NOT DEFINED SOURCE_DIR OR NOT DEFINED CONFIG OR
   NOT DEFINED EXECUTABLE_PATH)
  message(FATAL_ERROR "BUILD_DIR, SOURCE_DIR, CONFIG, and EXECUTABLE_PATH are required")
endif()

# stageFigmaSource() stamps changed staged input with the current time. Do not delete the
# generated header here: when an identical .slugui is loaded in a fresh preview session, keeping
# the header lets CMake/MSBuild skip both AOT generation and the dependent C++ compile.
set(_build_environment VSLANG=1033)
if(WIN32 AND (NOT DEFINED ENV{TMP} OR "$ENV{TMP}" STREQUAL "") AND
   DEFINED ENV{TEMP} AND NOT "$ENV{TEMP}" STREQUAL "")
  # MSBuild 17.14 can emit MSB8029 for every project when TMP is missing even though TEMP and the
  # actual CMake build tree are valid. Normalize the conventional pair for the detached rebuild.
  list(APPEND _build_environment "TMP=$ENV{TEMP}")
endif()
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env ${_build_environment}
          "${CMAKE_COMMAND}" --build "${BUILD_DIR}" --config "${CONFIG}"
          --target slugia_example --parallel
  RESULT_VARIABLE build_result
  OUTPUT_VARIABLE build_output
  ERROR_VARIABLE build_error
  ENCODING UTF-8
)
file(WRITE "${BUILD_DIR}/slugui_drop_rebuild.log" "${build_output}\n${build_error}")

# The launcher may be a COPIED distribution EXE (dist/bin). Building
# BUILD_DIR/Release/slugia-example.exe does not replace the copied EXE, and
# relaunching EXECUTABLE_PATH would show the old frame while falsely claiming
# "Drop import succeeded". Always launch the executable that CMake JUST built.
if(WIN32)
  set(_fresh_executable "${BUILD_DIR}/${CONFIG}/slugia-example.exe")
  if(NOT EXISTS "${_fresh_executable}")
    set(_fresh_executable "${BUILD_DIR}/slugia-example.exe")
  endif()
elseif(APPLE)
  set(_fresh_executable "${BUILD_DIR}/${CONFIG}/slugia-example.app")
  if(NOT EXISTS "${_fresh_executable}")
    set(_fresh_executable "${BUILD_DIR}/slugia-example.app")
  endif()
else()
  set(_fresh_executable "${BUILD_DIR}/slugia-example")
endif()
if(build_result EQUAL 0 AND NOT EXISTS "${_fresh_executable}")
  string(APPEND build_error "\nCompiled example not found: ${_fresh_executable}\n")
  set(build_result 1)
endif()

if(build_result EQUAL 0)
  file(REMOVE "${SOURCE_DIR}/examples/figma_sample.slugui.drop-backup")
  set(import_result --import-success)
  file(APPEND "${BUILD_DIR}/slugui_drop_rebuild.log"
    "\nVerified relaunch executable: ${_fresh_executable}\n")
else()
  if(EXISTS "${SOURCE_DIR}/examples/figma_sample.slugui.drop-backup")
    file(COPY_FILE
      "${SOURCE_DIR}/examples/figma_sample.slugui.drop-backup"
      "${SOURCE_DIR}/examples/figma_sample.slugui"
      ONLY_IF_DIFFERENT)
    file(REMOVE "${SOURCE_DIR}/examples/figma_sample.slugui.drop-backup")
  endif()
  # The generated header may describe the rejected source. Dropping only the stamp forces the
  # next build to regenerate it from the restored fixture while retaining the atomic header file.
  file(REMOVE "${BUILD_DIR}/generated/figma_sample.generated.hpp.stamp")
  set(import_result --import-failed)
endif()

if(NO_RELAUNCH)
  return()
endif()

if(WIN32)
  if(build_result EQUAL 0)
    set(_relaunch_path "${_fresh_executable}")
  else()
    set(_relaunch_path "${EXECUTABLE_PATH}")
  endif()
  # CMake builds in BUILD_DIR. EXECUTABLE_PATH is merely the original launcher,
  # possibly a detached stale dist/bin EXE. Never relaunch it on success.
  execute_process(
    COMMAND cmd.exe /D /C start "" "${_relaunch_path}" --figma ${import_result}
  )
elseif(APPLE)
  if(build_result EQUAL 0)
    set(_relaunch_path "${_fresh_executable}")
  else()
    set(_relaunch_path "${EXECUTABLE_PATH}")
  endif()
  execute_process(
    COMMAND open -n "${_relaunch_path}" --args --figma ${import_result}
  )
else()
  message(WARNING "Automatic relaunch is only implemented for Windows and macOS")
endif()
