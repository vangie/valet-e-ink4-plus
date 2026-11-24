# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "2.0.82")

add_compile_options(-Wno-missing-field-initializers)

# FreeType：不要字体里的 PNG 彩色字形（彩色 emoji，CBDT/sbix）。墨水屏只画黑白轮廓字形，
# 带上它要多链进整个 libpng（约 56 KB）。
set(FT_DISABLE_PNG ON CACHE BOOL "No PNG glyphs in FreeType" FORCE)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(Metalio-E-Ink4-Plus)

# 随 app 一并烧录 fontpack 到 font_data（替换 epd_font24）
set(FONTPACK_FILE "${CMAKE_SOURCE_DIR}/main/assets/fonts/font.fontpack")
if(EXISTS "${FONTPACK_FILE}")
    esptool_py_flash_to_partition(flash font_data "${FONTPACK_FILE}")
    message(STATUS "font_data flash: ${FONTPACK_FILE}")
else()
    message(WARNING "main/assets/fonts/font.fontpack missing — font_data will not be flashed")
endif()
