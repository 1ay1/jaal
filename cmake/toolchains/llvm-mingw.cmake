# Cross-compile to Windows with llvm-mingw.
#
#   cmake --preset mingw
#
# Finds the toolchain at $LLVM_MINGW, else ~/.local/opt/llvm-mingw, else on
# PATH. When wine is installed, tests run under it (CMAKE_CROSSCOMPILING_EMULATOR),
# so `ctest --preset mingw` actually executes the Windows binaries.

set(CMAKE_SYSTEM_NAME      Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(_triple x86_64-w64-mingw32)
set(_hints "")
if(DEFINED ENV{LLVM_MINGW})
    list(APPEND _hints "$ENV{LLVM_MINGW}/bin")
endif()
list(APPEND _hints "$ENV{HOME}/.local/opt/llvm-mingw/bin" "/opt/llvm-mingw/bin")

find_program(_cxx NAMES ${_triple}-clang++ HINTS ${_hints} REQUIRED)
get_filename_component(_bin "${_cxx}" DIRECTORY)

set(CMAKE_CXX_COMPILER "${_cxx}")
set(CMAKE_RC_COMPILER  "${_bin}/${_triple}-windres")
set(CMAKE_AR           "${_bin}/llvm-ar")
set(CMAKE_RANLIB       "${_bin}/llvm-ranlib")

get_filename_component(_root "${_bin}" DIRECTORY)
set(CMAKE_FIND_ROOT_PATH "${_root}/${_triple}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# Static runtime, so test .exe files run under wine with no DLL hunting.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")

find_program(_wine NAMES wine64 wine)
if(_wine)
    set(CMAKE_CROSSCOMPILING_EMULATOR "${_wine}")
endif()
