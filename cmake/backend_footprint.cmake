# SPDX-License-Identifier: MPL-2.0
file(READ "${MAP}" map)
string(REGEX MATCH "0x([0-9a-fA-F]+)[ \t]+_rom_end = \\." end "${map}")

if(NOT end)
    message(FATAL_ERROR "Cannot find linked ROM extent in ${MAP}")
endif()

math(EXPR rom_size "0x${CMAKE_MATCH_1}")
file(SIZE "${MODULE}" module_size)
set(report "Backend: ${BACKEND}\nLinked ROM extent: ${rom_size} bytes\nHUNK file including relocations: ${module_size} bytes\n")
file(WRITE "${REPORT}" "${report}")
message(STATUS "${BACKEND} graphics.library: ROM ${rom_size} bytes; HUNK ${module_size} bytes")
