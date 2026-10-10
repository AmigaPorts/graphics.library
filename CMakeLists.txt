# SPDX-License-Identifier: MPL-2.0
cmake_minimum_required(VERSION 3.14.0)
project(graphics.library LANGUAGES C VERSION 47.10)

set(CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

include(CPM-init)

get_verstring(VERSTRING)

if(NOT TARGET devicetree)
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
		src/framebuffer.c
		src/framebuffer_tree.c
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
add_dependencies(graphics.library devicetree graphics)
target_link_libraries(graphics.library graphics devicetree)
target_link_options(graphics.library PRIVATE -nostdlib -nostartfiles -Wall -Wextra -s -Wl,-T${CMAKE_CURRENT_SOURCE_DIR}/ldscript.lds)
target_compile_options(graphics.library PUBLIC $<$<COMPILE_LANGUAGE:C>:-O2 -m68040 -fomit-frame-pointer -ffreestanding -fno-builtin -fno-common -Wall -Wextra -DDEBUG>)

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
install(FILES LICENSE LICENSE.APL LICENSE.Topaz LICENSE.Topaz9 HEADERS.md FONTS.md DESTINATION Developer)
install(PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/graphics.library DESTINATION Libs)
