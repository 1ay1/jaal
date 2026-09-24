# JaalCompileFail — tests that pass when code does NOT compile.
#
#   jaal_compile_fail(NAME <name> SOURCE <file> [MATCH <regex>])
#
# Each case is a tiny OBJECT library excluded from `all`, so a normal build
# never touches it. A ctest entry builds just that target and expects the
# build to fail. With MATCH, the compiler output must also contain <regex>,
# so a case can't "pass" by failing for the wrong reason (a typo, a missing
# include).
#
# One source can hold several cases selected by -DJAAL_CASE=<n>:
#
#   jaal_compile_fail(NAME widen_bad SOURCE rows.cpp CASE 1 MATCH "widening")
#
# Cost: nothing at configure time (no try_compile), one small TU per case at
# test time, cached by ccache like everything else.

function(jaal_compile_fail)
    cmake_parse_arguments(ARG "" "NAME;SOURCE;MATCH;CASE" "" ${ARGN})
    if(NOT ARG_NAME OR NOT ARG_SOURCE)
        message(FATAL_ERROR "jaal_compile_fail: NAME and SOURCE are required")
    endif()

    set(_tgt "jaal_cf_${ARG_NAME}")
    add_library(${_tgt} OBJECT EXCLUDE_FROM_ALL ${ARG_SOURCE})
    target_link_libraries(${_tgt} PRIVATE jaal::jaal)
    if(DEFINED ARG_CASE)
        target_compile_definitions(${_tgt} PRIVATE JAAL_CASE=${ARG_CASE})
    endif()

    set(_test "compile_fail.${ARG_NAME}")
    add_test(NAME ${_test}
             COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}
                     --target ${_tgt} --config $<CONFIG>)
    set_tests_properties(${_test} PROPERTIES
        LABELS      "compile_fail"
        # Several compile-fail builds must not run the build tool on the same
        # tree at once.
        RESOURCE_LOCK jaal_build_tree)
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
