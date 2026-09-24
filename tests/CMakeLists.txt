include(JaalCompileFail)

# One helper so every test target gets the same flags and ctest entry.
function(jaal_test name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE jaal::jaal jaal_warnings)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

# ── static tests: compiling IS passing ───────────────────────────────────
jaal_test(meta_list_test meta/list_test.cpp)
set_tests_properties(meta_list_test PROPERTIES LABELS "static")

# ── compile-fail tests ───────────────────────────────────────────────────
jaal_compile_fail(NAME meta_at_oob        SOURCE compile_fail/meta.cpp CASE 1)
jaal_compile_fail(NAME meta_concat_nonlist SOURCE compile_fail/meta.cpp CASE 2)
jaal_compile_fail(NAME meta_require_msg   SOURCE compile_fail/meta.cpp CASE 3
                  MATCH "jaal-test: duplicate effect in row")

# ── layering: a header may only include its own layer or layers below ───
add_test(NAME layering
         COMMAND ${CMAKE_COMMAND}
                 -DROOT=${PROJECT_SOURCE_DIR}/include/jaal
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/layering/check.cmake)
set_tests_properties(layering PROPERTIES LABELS "static")
