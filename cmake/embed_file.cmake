# Embeds a binary file as a C array: cmake -DIN=<file> -DOUT=<inc> -DNAME=<identifier> -P embed_file.cmake
# writes `static const unsigned char NAME[] = {...};` (rvk terrain.cpp: the cliffs' rock).
file(READ "${IN}" hex HEX)
string(LENGTH "${hex}" len)
math(EXPR bytes "${len} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," body "${hex}")
string(REGEX REPLACE "(0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,)" "\\1\n" body "${body}")
file(WRITE "${OUT}" "// Generated from ${IN} (cmake/embed_file.cmake)\nstatic const unsigned char ${NAME}[${bytes}] = {\n${body}\n};\n")
