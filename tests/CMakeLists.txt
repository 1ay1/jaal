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

# Every layer umbrella (<jaal/core.hpp> and friends) must compile alone and
# export what it claims: a header added to a layer but not its umbrella fails
# here.
jaal_static_test(layering_umbrellas layering/umbrellas.cpp)

# Frozen/shared have static checks AND runtime behaviour (lifetime, many
# readers), so they're a normal test: build, then run.
jaal_test(core_frozen_test core/frozen_test.cpp)
jaal_test(core_cmd_test core/cmd_test.cpp)
jaal_test(core_sub_test core/sub_test.cpp)
jaal_test(core_routing_test core/routing_test.cpp)
jaal_test(core_child_test core/child_test.cpp)
jaal_test(core_rng_test core/rng_test.cpp)
jaal_test(core_children_test core/children_test.cpp)
jaal_test(core_report_test core/report_test.cpp)
jaal_test(core_debounce_test core/debounce_test.cpp)
jaal_test(kernel_parts_test kernel/kernel_parts_test.cpp)
jaal_test(kernel_test kernel/kernel_test.cpp)
jaal_test(kernel_scope_test kernel/scope_test.cpp)
jaal_test(platform_conformance platform/conformance.cpp)
# modify() against a real socket send buffer. POSIX: uses socketpair().
if(UNIX)
    jaal_test(platform_modify_test platform/modify_test.cpp)
endif()
jaal_test(platform_signals_test platform/signals_test.cpp)
jaal_test(kernel_run_test kernel/run_test.cpp)
jaal_test(kernel_pacing_test kernel/pacing_test.cpp)
jaal_test(kernel_mailbox_race_test kernel/mailbox_race_test.cpp)
jaal_test(kernel_fault_test kernel/fault_test.cpp)
jaal_test(kernel_mailbox_bound_test kernel/mailbox_bound_test.cpp)
jaal_test(kernel_features_test kernel/features_test.cpp)
jaal_test(kernel_answer_test kernel/answer_test.cpp)
jaal_test(kernel_sim_test kernel/sim_test.cpp)
jaal_test(kernel_timeline_test kernel/timeline_test.cpp)
jaal_test(kernel_given_test kernel/given_test.cpp)
jaal_test(kernel_resume_test kernel/resume_test.cpp)
jaal_test(kernel_stream_stop_test kernel/stream_stop_test.cpp)
jaal_test(kernel_teardown_test kernel/teardown_test.cpp)
jaal_test(kernel_time_cache_test kernel/time_cache_test.cpp)
jaal_test(kernel_subs_key_test kernel/subs_key_test.cpp)
jaal_test(kernel_idle_gate_test kernel/idle_gate_test.cpp)
# ^C during shutdown: needs fork() and a real signal, so POSIX only.
if(UNIX)
    jaal_test(kernel_signal_shutdown_test kernel/signal_shutdown_test.cpp)
endif()

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

# scope / guarded
jaal_compile_fail(NAME guarded_no_escape  SOURCE compile_fail/scope.cpp CASE 1
                  MATCH "jaal: guarded<T>::with/read returns something that points into")
# gcc: "use of deleted function 'jaal::handle<T>::handle(jaal::handle<T>&&) [with T = int]'"
# clang: "call to deleted constructor of ... 'jaal::handle<int>'"
jaal_compile_fail(NAME scope_handle_pinned SOURCE compile_fail/scope.cpp CASE 2
                  MATCH "deleted (function|constructor)[^\n]*jaal::handle<")
jaal_compile_fail(NAME scope_nursery_private SOURCE compile_fail/scope.cpp CASE 3
                  MATCH "private")
# lock-order deadlock can't be written: no second lock inside the first
jaal_compile_fail(NAME guarded_no_nested_capture SOURCE compile_fail/scope.cpp CASE 4
                  MATCH "jaal: guarded<T>::with/read takes a CAPTURELESS function")
jaal_compile_fail(NAME guarded_no_nested_arg SOURCE compile_fail/scope.cpp CASE 5
                  MATCH "jaal: an argument to guarded<T>::with/read is not Sendable")

# kernel / hosts
jaal_compile_fail(NAME router_event_not_produced SOURCE compile_fail/kernel.cpp CASE 1
                  MATCH "jaal: a subscription routes events this host doesn't produce")
jaal_compile_fail(NAME timeline_needs_frozen_model SOURCE compile_fail/kernel.cpp CASE 2
                  MATCH "is not Frozen: it contains '[^']*shared_ptr[^']*', a pointer-like owner")
jaal_compile_fail(NAME child_wrap_not_in_parent SOURCE compile_fail/kernel.cpp CASE 3
                  MATCH "Wrap must be a case of Parent::Msg")
# the program shape: a missing case is named, with the line to add
jaal_compile_fail(NAME program_missing_case SOURCE compile_fail/kernel.cpp CASE 4
                  MATCH "has no update for message '[^']*Reset'; add: static Cmd update\\(Model&, [^)]*Reset\\)")
jaal_compile_fail(NAME program_effect_not_declared SOURCE compile_fail/kernel.cpp CASE 5
                  MATCH "jaal: this effect is not in the Cmd's row")
# shutdown order: the kernel can only be finished BY teardown, in order
jaal_compile_fail(NAME kernel_no_bare_finish SOURCE compile_fail/kernel.cpp CASE 6
                  MATCH "(too few arguments|no matching (member )?function|candidate function not viable)")
# nested Msg: the error names the leaf AND the path to it
jaal_compile_fail(NAME program_missing_leaf_in_group SOURCE compile_fail/kernel.cpp CASE 8
                  MATCH "has no update for message '[^']*CSubmit' \\(reached as .*CEnter.*-> .*CSubmit\\)")
jaal_compile_fail(NAME teardown_key_not_forgeable SOURCE compile_fail/kernel.cpp CASE 7
                  MATCH "(private|calling a private constructor|is private)")
# a host effect that answers must answer with the program's Msg
jaal_compile_fail(NAME host_answer_not_msg SOURCE compile_fail/kernel.cpp CASE 9
                  MATCH "jaal: a host handle\\(\\) that returns something must return the program's Msg")
# a child's reports (D41): they can't be lost by forgetting to wire them,
# and a child reports only what its Out lists
jaal_compile_fail(NAME child_reports_need_from SOURCE compile_fail/kernel.cpp CASE 10
                  MATCH "jaal::child: this child can report")
jaal_compile_fail(NAME report_not_in_out SOURCE compile_fail/kernel.cpp CASE 11
                  MATCH "jaal: this report isn't one of the child's Out cases")

# ── layering: a header may only include its own layer or layers below ───
add_test(NAME layering
         COMMAND ${CMAKE_COMMAND}
                 -DROOT=${PROJECT_SOURCE_DIR}/include/jaal
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/layering/check.cmake)
set_tests_properties(layering PROPERTIES LABELS "static")

# ── banlist: raw threads/locks/atomics only where jaal implements them ───
add_test(NAME banlist
         COMMAND ${CMAKE_COMMAND}
                 -DROOT=${PROJECT_SOURCE_DIR}/include/jaal
                 -DALLOW=${CMAKE_CURRENT_SOURCE_DIR}/lint/allowlist.txt
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/lint/banlist.cmake)
set_tests_properties(banlist PROPERTIES LABELS "static")

# Same rule for the compiled backends in src/, against their own allowlist.
add_test(NAME banlist_src
         COMMAND ${CMAKE_COMMAND}
                 -DROOT=${PROJECT_SOURCE_DIR}/src
                 -DALLOW=${CMAKE_CURRENT_SOURCE_DIR}/lint/allowlist_src.txt
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/lint/banlist.cmake)
set_tests_properties(banlist_src PROPERTIES LABELS "static")

# ── macOS compile check ──────────────────────────────────────────────────
# There's no Mac here, so the darwin backends can't RUN in this suite. zig
# ships macOS libc headers, so they can at least COMPILE for arm64 macOS on
# every build. That catches missing includes and wrong kqueue calls; it
# proves nothing about behaviour.
if(NOT APPLE)
    find_program(JAAL_ZIG zig)
    if(JAAL_ZIG)
        foreach(src darwin/kqueue_reactor posix/poll_reactor posix/signals)
            string(REPLACE "/" "_" tname "${src}")
            add_test(NAME macos_compile.${tname}
                     COMMAND ${JAAL_ZIG} c++ -target aarch64-macos -std=c++2c
                             -I${PROJECT_SOURCE_DIR}/include -Wall -Wextra
                             -c ${PROJECT_SOURCE_DIR}/src/platform/${src}.cpp
                             -o ${CMAKE_CURRENT_BINARY_DIR}/macos_${tname}.o)
            set_tests_properties(macos_compile.${tname} PROPERTIES LABELS "cross")
        endforeach()
    endif()
endif()
