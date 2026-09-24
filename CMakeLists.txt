cmake_minimum_required(VERSION 3.29...4.4)   # 3.29: CMAKE_LINKER_TYPE

project(jaal
    VERSION     0.1.0
    DESCRIPTION "A typed Elm runtime and the platform layer it runs on"
    LANGUAGES   CXX)

list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake)

# ── options ──────────────────────────────────────────────────────────────
option(JAAL_BUILD_TESTS    "Build jaal's tests"                        ${PROJECT_IS_TOP_LEVEL})
option(JAAL_BUILD_EXAMPLES "Build the examples"                        OFF)
option(JAAL_BUILD_BENCH    "Build the benchmarks"                      OFF)
option(JAAL_WERROR         "Treat warnings as errors (CI)"             OFF)

# Build-speed knobs. They only apply when jaal is the top-level project; a
# parent project (maya) keeps control of its own toolchain.
option(JAAL_CCACHE         "Compile through ccache when it's installed" ON)
option(JAAL_FAST_LINKER    "Link with mold or lld when installed"       ON)
option(JAAL_SPLIT_DWARF    "Split debug info out of objects (faster links)" ON)
option(JAAL_UNITY          "Unity build (for CI; slower incremental)"    OFF)

# ── language level ───────────────────────────────────────────────────────
# C++26 where the compiler advertises it, C++23 otherwise (MSVC). The public
# target only REQUIRES C++23, so consumers on 23 still work.
if("cxx_std_26" IN_LIST CMAKE_CXX_COMPILE_FEATURES AND NOT MSVC)
    set(JAAL_CXX_STANDARD 26)
else()
    set(JAAL_CXX_STANDARD 23)
endif()
set(CMAKE_CXX_STANDARD          ${JAAL_CXX_STANDARD})
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)

if(PROJECT_IS_TOP_LEVEL)
    include(JaalFastBuild)
endif()
include(JaalWarnings)

# ── the library ──────────────────────────────────────────────────────────
# core/ and kernel/ are header-only templates over the user's Msg. The
# platform backends are compiled: OS headers (<poll.h>, <windows.h>) live
# ONLY in src/platform/<os>/, so they never leak into user code. The
# library is STATIC; there's no stable ABI to promise yet.
set(JAAL_PLATFORM_SOURCES "")
if(WIN32)
    list(APPEND JAAL_PLATFORM_SOURCES src/platform/windows/wait_reactor.cpp)
elseif(UNIX)
    list(APPEND JAAL_PLATFORM_SOURCES
         src/platform/posix/poll_reactor.cpp
         src/platform/posix/signals.cpp)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        list(APPEND JAAL_PLATFORM_SOURCES src/platform/linux/epoll_reactor.cpp)
    elseif(APPLE OR CMAKE_SYSTEM_NAME MATCHES "BSD")
        list(APPEND JAAL_PLATFORM_SOURCES src/platform/darwin/kqueue_reactor.cpp)
    endif()
endif()

add_library(jaal STATIC ${JAAL_PLATFORM_SOURCES})
add_library(jaal::jaal ALIAS jaal)

file(GLOB_RECURSE JAAL_HEADERS CONFIGURE_DEPENDS
     ${PROJECT_SOURCE_DIR}/include/jaal/*.hpp)
target_sources(jaal PUBLIC
    FILE_SET HEADERS
    BASE_DIRS ${PROJECT_SOURCE_DIR}/include
    FILES     ${JAAL_HEADERS})
target_compile_features(jaal PUBLIC cxx_std_23)
find_package(Threads REQUIRED)
target_link_libraries(jaal PUBLIC Threads::Threads)
# jaal's own sources get the strict warnings. Taken as a genex over the
# private target's options rather than linking it: a PRIVATE link on a
# STATIC library still lands in the install export set.
target_compile_options(jaal PRIVATE
    $<TARGET_PROPERTY:jaal_warnings,INTERFACE_COMPILE_OPTIONS>)
# A static library with no sources on some platform is still valid.
set_target_properties(jaal PROPERTIES LINKER_LANGUAGE CXX)

# ── tests / examples / bench ─────────────────────────────────────────────
if(JAAL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
if(JAAL_BUILD_EXAMPLES AND EXISTS ${PROJECT_SOURCE_DIR}/examples/CMakeLists.txt)
    add_subdirectory(examples)
endif()
if(JAAL_BUILD_BENCH AND EXISTS ${PROJECT_SOURCE_DIR}/bench/CMakeLists.txt)
    add_subdirectory(bench)
endif()

# ── install ──────────────────────────────────────────────────────────────
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS jaal EXPORT jaalTargets FILE_SET HEADERS)
install(EXPORT jaalTargets
        NAMESPACE   jaal::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/jaal)
configure_package_config_file(
    cmake/jaal-config.cmake.in
    ${PROJECT_BINARY_DIR}/jaal-config.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/jaal)
write_basic_package_version_file(
    ${PROJECT_BINARY_DIR}/jaal-config-version.cmake
    COMPATIBILITY SameMinorVersion)   # 0.x: minor bumps may break
install(FILES
        ${PROJECT_BINARY_DIR}/jaal-config.cmake
        ${PROJECT_BINARY_DIR}/jaal-config-version.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/jaal)
