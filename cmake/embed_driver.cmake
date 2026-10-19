# Writes OUT as a C array (same shape as `xxd -i driver43.bin`), so the build
# does not depend on xxd (absent from a stock MSYS2 install).
# Usage: cmake -DIN=<file> -DOUT=<file> -P embed_driver.cmake
file(READ "${IN}" hex HEX)
string(LENGTH "${hex}" hexlen)
math(EXPR len "${hexlen} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
file(WRITE "${OUT}" "unsigned char driver43_bin[] = {\n${bytes}\n};\nunsigned int driver43_bin_len = ${len};\n")
