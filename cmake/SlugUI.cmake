include(CMakeParseArguments)

function(slugia_compile_slugui)
  cmake_parse_arguments(ARG "STAMPED;STRIP_SOURCE_METADATA" "INPUT;OUTPUT;NAMESPACE;CLASS_NAME;COMPILER" "" ${ARGN})
  if(NOT ARG_INPUT OR NOT ARG_OUTPUT)
    message(FATAL_ERROR "slugia_compile_slugui requires INPUT and OUTPUT")
  endif()
  if(NOT ARG_NAMESPACE)
    set(ARG_NAMESPACE "slugia::generated")
  endif()
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  if(ARG_COMPILER)
    set(_compiler "${ARG_COMPILER}")
  else()
    get_filename_component(_source_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
    set(_compiler "${_source_root}/tools/slugui_compiler.py")
    if(NOT EXISTS "${_compiler}")
      get_filename_component(_install_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../../.." ABSOLUTE)
      set(_compiler "${_install_root}/bin/slugui_compiler.py")
    endif()
  endif()
  get_filename_component(_compiler_dir "${_compiler}" DIRECTORY)
  set(_command
    "${Python3_EXECUTABLE}" "${_compiler}"
    "${ARG_INPUT}" --output "${ARG_OUTPUT}" --namespace "${ARG_NAMESPACE}")
  if(ARG_CLASS_NAME)
    list(APPEND _command --class-name "${ARG_CLASS_NAME}")
  endif()
  if(ARG_STRIP_SOURCE_METADATA)
    list(APPEND _command --strip-source-metadata)
  endif()
  if(ARG_STAMPED)
    # Keep generated C++ content-addressed by bytes: slugui_compiler.py intentionally preserves
    # an unchanged header's mtime so downstream translation units do not rebuild. The separate
    # stamp records that this input/compiler revision has nevertheless been checked.
    set(_stamp "${ARG_OUTPUT}.stamp")
    add_custom_command(
      OUTPUT "${_stamp}"
      BYPRODUCTS "${ARG_OUTPUT}"
      COMMAND ${_command}
      COMMAND "${CMAKE_COMMAND}" -E touch "${_stamp}"
      DEPENDS
        "${_compiler}"
        "${_compiler_dir}/svg_path.py"
        "${ARG_INPUT}"
      VERBATIM)
    set_source_files_properties("${ARG_OUTPUT}" PROPERTIES GENERATED TRUE)
  else()
    add_custom_command(
      OUTPUT "${ARG_OUTPUT}"
      COMMAND ${_command}
      DEPENDS
        "${_compiler}"
        "${_compiler_dir}/svg_path.py"
        "${ARG_INPUT}"
      VERBATIM)
  endif()
endfunction()
