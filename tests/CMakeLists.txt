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

# Frozen/shared have static checks AND runtime behaviour (lifetime, many
# readers), so they're a normal test: build, then run.
jaal_test(core_frozen_test core/frozen_test.cpp)
jaal_test(core_cmd_test core/cmd_test.cpp)

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

# Frozen / shared<T>
jaal_compile_fail(NAME frozen_mutable     SOURCE compile_fail/frozen.cpp CASE 1
                  MATCH "is not Frozen: it has a `mutable` field of type 'int'")
jaal_compile_fail(NAME frozen_unique      SOURCE compile_fail/frozen.cpp CASE 2
                  MATCH "is not Frozen: it contains '[^']*unique_ptr[^']*', a pointer-like owner")
jaal_compile_fail(NAME frozen_view        SOURCE compile_fail/frozen.cpp CASE 3
                  MATCH "is not Sendable: it contains '[^']*string_view[^']*', a view")
# These three are ordinary type errors, not jaal diagnostics, but each is
# pinned to the reason anyway so a typo can't make them "pass".
jaal_compile_fail(NAME shared_no_write    SOURCE compile_fail/frozen.cpp CASE 4
                  MATCH "const std::(__cxx11::)?(basic_)?string")
jaal_compile_fail(NAME shared_no_null     SOURCE compile_fail/frozen.cpp CASE 5
                  MATCH "shared<Doc>::shared\\(\\)|no matching constructor")
jaal_compile_fail(NAME shared_no_rawptr   SOURCE compile_fail/frozen.cpp CASE 6
                  MATCH "const Doc ?\\*")

# Cmd / rows / tasks
jaal_compile_fail(NAME cmd_effect_not_in_row SOURCE compile_fail/cmd.cpp CASE 1
                  MATCH "jaal: this effect is not in the Cmd's row")
jaal_compile_fail(NAME cmd_no_narrowing      SOURCE compile_fail/cmd.cpp CASE 2
                  MATCH "jaal: can't convert to a Cmd with fewer effects")
jaal_compile_fail(NAME task_no_capture       SOURCE compile_fail/cmd.cpp CASE 3
                  MATCH "jaal: a task body must not capture anything")
jaal_compile_fail(NAME task_arg_sendable     SOURCE compile_fail/cmd.cpp CASE 4
                  MATCH "jaal: a task argument is not Sendable")
jaal_compile_fail(NAME task_map_captureless  SOURCE compile_fail/cmd.cpp CASE 5
                  MATCH "jaal: Cmd::map on a Cmd holding a task needs a captureless mapper")
jaal_compile_fail(NAME sink_msg_sendable     SOURCE compile_fail/cmd.cpp CASE 6
                  MATCH "jaal: a Sink's Msg must be Sendable")

# ── layering: a header may only include its own layer or layers below ───
add_test(NAME layering
         COMMAND ${CMAKE_COMMAND}
                 -DROOT=${PROJECT_SOURCE_DIR}/include/jaal
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/layering/check.cmake)
set_tests_properties(layering PROPERTIES LABELS "static")
