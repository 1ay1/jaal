# JaalCompileFail — tests that pass when code does NOT compile.
#
#   jaal_compile_fail(NAME <name> SOURCE <file> [MATCH <regex>])
#
# Each case is one compiler run on one source file, done by ctest at test
# time. It calls the compiler DIRECTLY (not `cmake --build`), so cases
# don't share the build tree and ctest runs them in parallel. Measured:
# through `cmake --build` they needed a lock on the tree and ran one at a
# time, 26 s of a 29 s dev test run; direct, they take as long as the
# slowest one.
#
# With MATCH, the compiler output must also contain <regex>, so a case
# can't "pass" by failing for the wrong reason (a typo, a missing include).
#
# One source can hold several cases selected by -DJAAL_CASE=<n>:
#
#   jaal_compile_fail(NAME widen_bad SOURCE rows.cpp CASE 1 MATCH "widening")
#
# Cost: nothing at configure time, one small TU per case at test time.

function(jaal_compile_fail)
    cmake_parse_arguments(ARG "" "NAME;SOURCE;MATCH;CASE" "" ${ARGN})
    if(NOT ARG_NAME OR NOT ARG_SOURCE)
        message(FATAL_ERROR "jaal_compile_fail: NAME and SOURCE are required")
    endif()

    # The same flags a real jaal TU gets: the include dir, the standard, and
    # the project's warning/define flags for this config.
    get_filename_component(_src "${ARG_SOURCE}" ABSOLUTE)
    set(_flags ${CMAKE_CXX_FLAGS})
    separate_arguments(_flags)
    if(CMAKE_BUILD_TYPE)
        string(TOUPPER "${CMAKE_BUILD_TYPE}" _bt)
        set(_bt_flags ${CMAKE_CXX_FLAGS_${_bt}})
        separate_arguments(_bt_flags)
        list(APPEND _flags ${_bt_flags})
    endif()
    # The standard the project builds with (CMAKE_CXX_STANDARD, e.g. 26).
    set(_std "${CMAKE_CXX${CMAKE_CXX_STANDARD}_STANDARD_COMPILE_OPTION}")
    if(CMAKE_CXX_EXTENSIONS AND CMAKE_CXX${CMAKE_CXX_STANDARD}_EXTENSION_COMPILE_OPTION)
        set(_std "${CMAKE_CXX${CMAKE_CXX_STANDARD}_EXTENSION_COMPILE_OPTION}")
    endif()
    if(JAAL_STD_FLAG)
        set(_std "${JAAL_STD_FLAG}")
    endif()
    set(_cmd ${CMAKE_CXX_COMPILER} ${CMAKE_CXX_COMPILER_ARG1} ${_flags} ${_std}
             "-I${PROJECT_SOURCE_DIR}/include")
    if(DEFINED ARG_CASE)
        list(APPEND _cmd "-DJAAL_CASE=${ARG_CASE}")
    endif()
    # -fsyntax-only: a compile-fail case only needs the front end.
    list(APPEND _cmd -fsyntax-only "${_src}")

    set(_test "compile_fail.${ARG_NAME}")
    add_test(NAME ${_test} COMMAND ${_cmd})
    set_tests_properties(${_test} PROPERTIES LABELS "compile_fail")
    if(ARG_MATCH)
        # Success = the build printed the expected diagnostic. The regex
        # check replaces the exit-code check, so the (failing) build passes
        # only if it failed for the right reason.
        set_tests_properties(${_test} PROPERTIES
            PASS_REGULAR_EXPRESSION "${ARG_MATCH}")
    else()
        set_tests_properties(${_test} PROPERTIES WILL_FAIL TRUE)
    endif()
endfunction()
