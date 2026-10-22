if(NOT DEFINED INPUT OR NOT DEFINED OUTPUT OR NOT DEFINED SYMBOL)
    message(FATAL_ERROR "embed_spirv.cmake requires INPUT, OUTPUT, and SYMBOL")
endif()

file(READ "${INPUT}" _hex HEX)
string(REGEX REPLACE "(..)" "0x\\1," _bytes "${_hex}")

file(WRITE "${OUTPUT}"
    "#include <cstddef>\n"
    "\n"
    "alignas(4) extern const unsigned char ${SYMBOL}[] = { ${_bytes} };\n"
    "extern const std::size_t ${SYMBOL}_size = sizeof(${SYMBOL});\n")
