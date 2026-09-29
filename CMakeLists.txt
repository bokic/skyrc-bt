cmake_minimum_required(VERSION 3.15)

project(libskyrc C)

if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "/usr" CACHE PATH "Install prefix" FORCE)
endif()

set(LIBSKYRC_VERSION "" CACHE STRING "Override the library version (X.Y.Z)")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0)

find_package(Git QUIET)

set(LIBSKYRC_GIT_TAG "")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git" AND Git_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" describe --tags --abbrev=0
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE LIBSKYRC_GIT_TAG
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
endif()
if(LIBSKYRC_VERSION STREQUAL "" AND DEFINED ENV{LIBSKYRC_VERSION} AND NOT "$ENV{LIBSKYRC_VERSION}" STREQUAL "")
    set(LIBSKYRC_VERSION "$ENV{LIBSKYRC_VERSION}")
endif()
if(LIBSKYRC_VERSION STREQUAL "")
    if(LIBSKYRC_GIT_TAG STREQUAL "")
        message(FATAL_ERROR "Git tag is not set; set LIBSKYRC_VERSION to build without a Git tag")
    endif()
    string(REGEX REPLACE "^v" "" LIBSKYRC_VERSION "${LIBSKYRC_GIT_TAG}")
endif()
if(NOT LIBSKYRC_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "Version '${LIBSKYRC_VERSION}' must be a semantic version (for example, 4.2.0)")
endif()
string(REGEX REPLACE "^([0-9]+\\.[0-9]+)\\.[0-9]+$" "\\1" LIBSKYRC_SOVERSION "${LIBSKYRC_VERSION}")

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
    PUBLIC_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/include/skyrc-bt.h"
    INTERPROCEDURAL_OPTIMIZATION TRUE
    C_VISIBILITY_PRESET hidden
    OUTPUT_NAME "skyrc-bt"
    VERSION "${LIBSKYRC_VERSION}"
    SOVERSION "${LIBSKYRC_SOVERSION}"
)

add_executable(skyrc
    src/main.c
)
set_target_properties(skyrc PROPERTIES OUTPUT_NAME "skyrc-bt")
target_link_libraries(skyrc PRIVATE libskyrc-bt)
target_compile_options(skyrc PRIVATE
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

include(GNUInstallDirs)

install(TARGETS libskyrc-bt skyrc
    RUNTIME       DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY       DESTINATION ${CMAKE_INSTALL_LIBDIR}
    PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
