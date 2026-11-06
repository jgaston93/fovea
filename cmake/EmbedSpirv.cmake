# cmake -DSPV=<in.spv> -DHPP=<out.hpp> -DNAME=<identifier> -P EmbedSpirv.cmake
# Writes the SPIR-V words (little-endian) as a constexpr std::array.
file(READ "${SPV}" hex HEX)
string(LENGTH "${hex}" length)
math(EXPR remainder "${length} % 8")
if(NOT remainder EQUAL 0)
    message(FATAL_ERROR "${SPV} is not a whole number of 32-bit words")
endif()
string(REGEX REPLACE "(..)(..)(..)(..)" "0x\\4\\3\\2\\1u," words "${hex}")
math(EXPR count "${length} / 8")
file(WRITE "${HPP}"
    "// Generated from ${SPV} by EmbedSpirv.cmake; do not edit.\n"
    "#pragma once\n#include <array>\n#include <cstdint>\n"
    "inline constexpr std::array<std::uint32_t, ${count}> ${NAME} = {${words}};\n")
