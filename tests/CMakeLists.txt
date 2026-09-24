include(JaalCompileFail)

# One helper so every test target gets the same flags and ctest entry.
function(jaal_test name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE jaal::jaal jaal_warnings)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

# A static test's assertions run at COMPILE time, so "the test passed"
# means "the TU compiled". Running a leftover binary would report a pass
# for a build that just failed. So the ctest entry REBUILDS the target
# itself: if the current source doesn't compile, the test fails.
function(jaal_static_test name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE jaal::jaal jaal_warnings)
    add_test(NAME ${name}
             COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}
                     --target ${name} --config $<CONFIG>)
    set_tests_properties(${name} PROPERTIES
        LABELS        "static"
        RESOURCE_LOCK jaal_build_tree)
endfunction()

# ── static tests: compiling IS passing ───────────────────────────────────
jaal_static_test(meta_list_test meta/list_test.cpp)
jaal_static_test(core_sendable_test core/sendable_test.cpp)

# ── compile-fail tests ───────────────────────────────────────────────────
jaal_compile_fail(NAME meta_at_oob        SOURCE compile_fail/meta.cpp CASE 1)
jaal_compile_fail(NAME meta_concat_nonlist SOURCE compile_fail/meta.cpp CASE 2)
jaal_compile_fail(NAME meta_require_msg   SOURCE compile_fail/meta.cpp CASE 3
                  MATCH "jaal-test: duplicate effect in row")

# Sendable: each must fail AND name what's wrong. The regexes are the
# message jaal prints on C++26 compilers (computed static_assert text).
jaal_compile_fail(NAME sendable_hidden_view   SOURCE compile_fail/sendable.cpp CASE 1
                  MATCH "not Sendable: it contains '[^']*string_view[^']*', a view that borrows")
# Type names are spelled by the compiler (clang: 'const char *', gcc:
# 'const char*'), so the regexes allow for that.
jaal_compile_fail(NAME sendable_hidden_ptr    SOURCE compile_fail/sendable.cpp CASE 2
                  MATCH "not Sendable: it contains 'const char ?\\*', a pointer")
jaal_compile_fail(NAME sendable_shared_mut    SOURCE compile_fail/sendable.cpp CASE 3
                  MATCH "shared ownership of data another thread can change")
jaal_compile_fail(NAME sendable_opaque        SOURCE compile_fail/sendable.cpp CASE 4
                  MATCH "specialise jaal::sendable_opt_in")

# ── layering: a header may only include its own layer or layers below ───
add_test(NAME layering
         COMMAND ${CMAKE_COMMAND}
                 -DROOT=${PROJECT_SOURCE_DIR}/include/jaal
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/layering/check.cmake)
set_tests_properties(layering PROPERTIES LABELS "static")
