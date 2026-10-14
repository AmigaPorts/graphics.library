# SPDX-License-Identifier: MPL-2.0
cmake_minimum_required(VERSION 3.14.0)
project(graphics.library LANGUAGES C VERSION 47.10)

set(CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

include(CPM-init)

get_verstring(VERSTRING)

set(GRAPHICS_CPU "68040" CACHE STRING "Minimum target CPU")
set_property(CACHE GRAPHICS_CPU PROPERTY STRINGS 68000 68020 68030 68040 68060)

if(NOT GRAPHICS_CPU MATCHES "^(68000|68020|68030|68040|68060)$")
    message(FATAL_ERROR "Unknown GRAPHICS_CPU: ${GRAPHICS_CPU}")
endif()

set(GRAPHICS_BACKEND "COMBINED" CACHE STRING "Display backend: FRAMEBUFFER, CHIPSET or COMBINED")
set_property(CACHE GRAPHICS_BACKEND PROPERTY STRINGS FRAMEBUFFER CHIPSET COMBINED)

if(NOT GRAPHICS_BACKEND MATCHES "^(FRAMEBUFFER|CHIPSET|COMBINED)$")
    message(FATAL_ERROR "Unknown GRAPHICS_BACKEND: ${GRAPHICS_BACKEND}")
endif()

if(GRAPHICS_CPU STREQUAL "68000" AND NOT GRAPHICS_BACKEND STREQUAL "CHIPSET")
    message(FATAL_ERROR "The framebuffer backend requires a 68020 or later CPU")
endif()

set(GRAPHICS_FRAMEBUFFER ON)
set(GRAPHICS_CHIPSET ON)

if(GRAPHICS_BACKEND STREQUAL "CHIPSET")
    set(GRAPHICS_FRAMEBUFFER OFF)
elseif(GRAPHICS_BACKEND STREQUAL "FRAMEBUFFER")
    set(GRAPHICS_CHIPSET OFF)
endif()

if(GRAPHICS_FRAMEBUFFER AND NOT TARGET devicetree)
	CPMGetPackage(devicetree.resource)
endif()

# Set default install prefix to ./install subdirectory
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
	set(CMAKE_INSTALL_PREFIX "${CMAKE_SOURCE_DIR}/install" CACHE PATH "Install path" FORCE)
endif()

sfdc(sfd/graphics_lib.sfd)
target_include_directories(graphics INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_executable(graphics.library
		src/main.c
		src/init.c
		src/vectors.c
		src/bitmap.c
		src/scroll.c
		src/flood.c
		src/sprite.c
		src/frame.c
		src/blitter.c
		src/native.c
		src/backend.c
		src/framebuffer_render.c
		src/framebuffer_palette.c
		src/line.c
		src/pixel.c
		src/ellipse.c
		src/area.c
		src/state.c
		src/initialize.c
		src/rastport.c
		src/font.c
		src/topaz.c
		src/topaz9.c
		src/text.c
		src/memory.c
		src/layer.c
		src/color.c
		src/videocontrol.c
		src/node.c
		src/monitor.c
		src/view.c
		src/display.c
		src/region.c
		src/copper.c
		src/gels.c
		src/gel_draw.c
		src/gel_list.c
		src/end.c
)

target_compile_definitions(graphics.library PRIVATE VERSION_STRING="${VERSTRING}" LIBRARY_VERSION=${PROJECT_VERSION_MAJOR} LIBRARY_REVISION=${PROJECT_VERSION_MINOR} LIBRARY_NAME="${PROJECT_NAME}")
set_property(TARGET graphics.library APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/ldscript.lds")
add_dependencies(graphics.library graphics)
target_link_libraries(graphics.library graphics)

if(GRAPHICS_CPU STREQUAL "68000")
    target_sources(graphics.library PRIVATE src/arithmetic.c)
endif()

if(GRAPHICS_FRAMEBUFFER)
    target_sources(graphics.library PRIVATE
        src/framebuffer.c
        src/framebuffer_tree.c
    )
    add_dependencies(graphics.library devicetree)
    target_link_libraries(graphics.library devicetree)
endif()

if(GRAPHICS_CHIPSET)
    target_sources(graphics.library PRIVATE src/chipset.c src/chipset_view.c src/chipset_gels.c)
endif()

target_compile_definitions(graphics.library PRIVATE
    GRAPHICS_FRAMEBUFFER=$<BOOL:${GRAPHICS_FRAMEBUFFER}>
    GRAPHICS_CHIPSET=$<BOOL:${GRAPHICS_CHIPSET}>
)

# Retain the complete ABI from the Resident while dropping unreachable backend
# sections, including framebuffer presentation helpers in shared view sources.
target_compile_options(graphics.library PRIVATE -ffunction-sections -fdata-sections)
target_link_options(graphics.library PRIVATE -Wl,--gc-sections -Wl,-u,_Init -Wl,-u,_graphicsFunctions -Wl,-u,_rom_end -Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/graphics.library.map)
target_link_options(graphics.library PRIVATE -nostdlib -nostartfiles -Wall -Wextra -s -Wl,-T${CMAKE_CURRENT_SOURCE_DIR}/ldscript.lds)
target_compile_options(graphics.library PUBLIC $<$<COMPILE_LANGUAGE:C>:-O2 -m${GRAPHICS_CPU} -fomit-frame-pointer -ffreestanding -fno-builtin -fno-common -Wall -Wextra -DDEBUG>)
check_rom(graphics.library graphics.library)
add_custom_command(TARGET graphics.library POST_BUILD
    COMMAND ${CMAKE_COMMAND}
        -DBACKEND=${GRAPHICS_BACKEND}
        -DMODULE=$<TARGET_FILE:graphics.library>
        -DMAP=${CMAKE_CURRENT_BINARY_DIR}/graphics.library.map
        -DREPORT=${CMAKE_CURRENT_BINARY_DIR}/graphics.library.footprint.txt
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/backend_footprint.cmake
    VERBATIM
)
bin_to_header(graphics.library)

# Set up information for Aminet
add_project_info(
		"graphics.library open source recreation"
		"marlon@amigadev.com (Marlon Beijer)"
		"driver/other"
		"m68k-amigaos"
)

add_aminet_readme_generator(graphics.library "graphics.library.readme")

install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/sfd/graphics_lib.sfd DESTINATION Developer/sfd)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/graphics DESTINATION Developer/include)
install(DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/include/ DESTINATION Developer/include)
install(FILES LICENSE LICENSE.APL LICENSE.Topaz LICENSE.Topaz9 HEADERS.md FONTS.md CHIPSET.md DESTINATION Developer)
install(PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/graphics.library DESTINATION Libs)
