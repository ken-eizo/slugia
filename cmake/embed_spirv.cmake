if(NOT DEFINED VERTEX_SPV OR NOT DEFINED FRAGMENT_SPV OR
   NOT DEFINED PRIMITIVE_VERTEX_SPV OR NOT DEFINED PRIMITIVE_FRAGMENT_SPV OR
   NOT DEFINED OUTPUT)
  message(FATAL_ERROR "All shader SPIR-V inputs and OUTPUT are required")
endif()

function(write_shader_array input symbol append)
  file(READ "${input}" shader_hex HEX)
  string(LENGTH "${shader_hex}" hex_length)
  math(EXPR byte_count "${hex_length} / 2")
  string(REGEX REPLACE "([0-9a-fA-F][0-9a-fA-F])" "0x\\1," shader_bytes "${shader_hex}")
  if(append)
    file(APPEND "${OUTPUT}"
      "alignas(4) inline constexpr unsigned char ${symbol}[] = {${shader_bytes}};\n"
      "inline constexpr unsigned long long ${symbol}Size = ${byte_count}ULL;\n")
  else()
    file(WRITE "${OUTPUT}"
      "#pragma once\n\nnamespace slugia::embedded {\n"
      "alignas(4) inline constexpr unsigned char ${symbol}[] = {${shader_bytes}};\n"
      "inline constexpr unsigned long long ${symbol}Size = ${byte_count}ULL;\n")
  endif()
endfunction()

write_shader_array("${VERTEX_SPV}" vectorVert false)
write_shader_array("${FRAGMENT_SPV}" vectorFrag true)
write_shader_array("${PRIMITIVE_VERTEX_SPV}" primitiveVert true)
write_shader_array("${PRIMITIVE_FRAGMENT_SPV}" primitiveFrag true)
file(APPEND "${OUTPUT}" "} // namespace slugia::embedded\n")
