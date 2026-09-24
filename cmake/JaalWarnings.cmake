# JaalWarnings — warning flags for jaal's OWN targets (tests, examples).
#
# Kept on a separate, non-exported interface target so consumers of
# jaal::jaal never inherit our warning policy.

add_library(jaal_warnings INTERFACE)

# The C++26 flag CMake didn't know about (CMakeLists.txt, JAAL_STD_FLAG).
# Comes after CMake's own -std=c++23, so it wins.
if(JAAL_STD_FLAG)
    target_compile_options(jaal_warnings INTERFACE ${JAAL_STD_FLAG})
endif()

if(MSVC)
    target_compile_options(jaal_warnings INTERFACE
        /W4 /permissive- /Zc:__cplusplus /Zc:preprocessor
        $<$<BOOL:${JAAL_WERROR}>:/WX>)
else()
    target_compile_options(jaal_warnings INTERFACE
        -Wall -Wextra -Wpedantic
        -Wconversion -Wsign-conversion -Wshadow
        -Wnon-virtual-dtor -Wold-style-cast -Wcast-align
        -Wnull-dereference -Wimplicit-fallthrough
        $<$<BOOL:${JAAL_WERROR}>:-Werror>)
endif()
