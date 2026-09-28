cmake_minimum_required(VERSION 3.15)

project(libskyrc C)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0)

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/bin)

include_directories(PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_library(libskyrc-bt SHARED
    src/skyrc-bt.c
    include/skyrc-bt.h
)
target_compile_definitions(libskyrc-bt PRIVATE libskyrc_bt_EXPORTS)
target_link_libraries(libskyrc-bt PRIVATE PkgConfig::GIO)
target_compile_options(libskyrc-bt PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang>:
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wcast-qual
        -Wcast-align
        -Wstrict-prototypes
        -Wmissing-prototypes
        -Wmissing-declarations
        -Wredundant-decls
        -Wnull-dereference
        -Wformat=2
        -Wformat-overflow=2
        -Wformat-truncation=2
        -Wvla
        -Wwrite-strings
        -Wconversion
        -Wsign-conversion
        -Wdouble-promotion
        -Wundef
        -Wuninitialized
        -Winit-self
        -Wlogical-op
        -Werror
    >
)

set_target_properties(libskyrc-bt PROPERTIES
    PUBLIC_HEADER "../include/skyrc-bt.h"
    INTERPROCEDURAL_OPTIMIZATION TRUE
    C_VISIBILITY_PRESET hidden
    OUTPUT_NAME "skyrc-bt"
    VERSION "1.89.0"
    SOVERSION "1.89"
)
