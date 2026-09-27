# Writes a C++ source defining a byte array with the contents of a file:
#   cmake -DINPUT=<file> -DOUTPUT=<file.cpp> -DNAME=<symbol> -P embed.cmake
# defines `extern const unsigned char NAME[]` and `extern const std::size_t NAME_size`.
file(READ "${INPUT}" hex HEX)
string(LENGTH "${hex}" length)
math(EXPR size "${length} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
file(WRITE "${OUTPUT}"
  "// Generated from ${INPUT} by cmake/embed.cmake.\n"
  "#include <cstddef>\n"
  "extern const unsigned char ${NAME}[] = {${bytes}};\n"
  "extern const std::size_t ${NAME}_size = ${size};\n")
