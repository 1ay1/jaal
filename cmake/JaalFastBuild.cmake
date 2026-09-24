# JaalFastBuild — make edit→build→test as short as possible.
#
# Only included when jaal is the top-level project. Everything here is
# detected, never required: a machine without ccache or lld still builds.
#
# What it does, and why:
#   ccache         a rebuild after switching branches or `rm -rf build` is
#                  mostly cache hits instead of recompiles.
#   mold / lld     linking is the serial step at the end of every build;
#                  GNU ld is several times slower on template-heavy objects.
#   split DWARF    debug info goes into .dwo files, so the linker doesn't
#                  copy it into the executable. Links get much smaller and
#                  faster. --gdb-index keeps gdb startup fast too.
#   no module scan C++20 module dependency scanning runs a scan step per TU.
#                  jaal uses no modules, so it's pure overhead.
#   -O0 debug      the default Debug flags (-g, no optimisation) are the
#                  fastest to compile; nothing here raises them.

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_COLOR_DIAGNOSTICS       ON)
set(CMAKE_CXX_SCAN_FOR_MODULES    OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

set(_jaal_summary "")

# ── ccache ───────────────────────────────────────────────────────────────
if(JAAL_CCACHE AND NOT CMAKE_CXX_COMPILER_LAUNCHER)
    find_program(JAAL_CCACHE_EXE NAMES ccache sccache)
    if(JAAL_CCACHE_EXE)
        set(CMAKE_CXX_COMPILER_LAUNCHER ${JAAL_CCACHE_EXE})
        list(APPEND _jaal_summary "launcher=${JAAL_CCACHE_EXE}")
    else()
        list(APPEND _jaal_summary "launcher=none (install ccache for faster rebuilds)")
    endif()
elseif(CMAKE_CXX_COMPILER_LAUNCHER)
    list(APPEND _jaal_summary "launcher=${CMAKE_CXX_COMPILER_LAUNCHER} (user)")
endif()

# ── linker ───────────────────────────────────────────────────────────────
# CMAKE_LINKER_TYPE picks the right flag per compiler (-fuse-ld=... for
# GCC/Clang). Apple's ld64 and MSVC's link.exe are already fast; leave them.
if(JAAL_FAST_LINKER AND NOT DEFINED CMAKE_LINKER_TYPE AND NOT APPLE AND NOT WIN32)
    find_program(JAAL_MOLD_EXE mold)
    find_program(JAAL_LLD_EXE  ld.lld)
    if(JAAL_MOLD_EXE)
        set(CMAKE_LINKER_TYPE MOLD)
    elseif(JAAL_LLD_EXE)
        set(CMAKE_LINKER_TYPE LLD)
    endif()
endif()
if(DEFINED CMAKE_LINKER_TYPE)
    list(APPEND _jaal_summary "linker=${CMAKE_LINKER_TYPE}")
else()
    list(APPEND _jaal_summary "linker=default")
endif()

# ── split debug info ─────────────────────────────────────────────────────
if(JAAL_SPLIT_DWARF AND NOT APPLE AND NOT WIN32
   AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(_dbg "$<CONFIG:Debug,RelWithDebInfo>")
    add_compile_options("$<${_dbg}:-gsplit-dwarf>")
    if(CMAKE_LINKER_TYPE MATCHES "MOLD|LLD")
        add_link_options("$<${_dbg}:LINKER:--gdb-index>")
    endif()
    list(APPEND _jaal_summary "split-dwarf=on")
endif()

# ── unity ────────────────────────────────────────────────────────────────
if(JAAL_UNITY)
    set(CMAKE_UNITY_BUILD ON)
    list(APPEND _jaal_summary "unity=on")
endif()

list(JOIN _jaal_summary ", " _jaal_summary)
message(STATUS "jaal: C++${JAAL_CXX_STANDARD}, ${CMAKE_BUILD_TYPE}, ${_jaal_summary}")
