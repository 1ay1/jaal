# tests/lint/banlist.cmake — keep raw concurrency primitives out of code
# that should go through jaal's safe types (docs/concurrency.md §7).
#
# Fails when any of these appear in a scanned file that is NOT on the
# allowlist:
#
#   std::thread / std::jthread      use a task, isolated_task or scope
#   .detach()                        nothing in jaal's API detaches
#   std::async                       use a task
#   std::mutex / std::shared_mutex   use guarded<T>, or an owner + messages
#   std::atomic                      same
#   thread_local                     use loop_bound<T>
#   const_cast                       breaks Frozen and const-correctness
#   sink_access                      only the kernel mints sinks
#   loop_key                         only the kernel mints loop tokens
#
# The allowlist is the list of files a human checked by hand: jaal's own
# kernel/platform code, where these ARE the implementation. Each entry says
# which names it may use, so a file allowed std::mutex still can't sneak in
# a .detach().
#
# Run with: cmake -DROOT=<dir> -DALLOW=<allowlist file> -P banlist.cmake
# The allowlist format is one line per file:
#     path/relative/to/ROOT: name name name
# and `#` comments.

if(NOT ROOT OR NOT ALLOW)
    message(FATAL_ERROR "ROOT and ALLOW are required")
endif()

# name → regex. Names are what the allowlist refers to.
set(ban_thread      "std::j?thread[^_]")
set(ban_detach      "\\.detach\\(\\)")
set(ban_async       "std::async[^_]")
set(ban_mutex       "std::(shared_|recursive_|timed_)?mutex[^_]")
set(ban_atomic      "std::atomic[^_]")
set(ban_thread_local "thread_local")
set(ban_const_cast  "const_cast")
set(ban_sink_access "sink_access")
set(ban_loop_key    "loop_key")
set(ban_names thread detach async mutex atomic thread_local const_cast sink_access loop_key)

# Parse the allowlist.
file(STRINGS ${ALLOW} allow_lines)
foreach(line IN LISTS allow_lines)
    string(REGEX REPLACE "#.*" "" line "${line}")
    string(STRIP "${line}" line)
    if(line STREQUAL "")
        continue()
    endif()
    string(REGEX MATCH "^([^:]+):(.*)$" _ "${line}")
    set(f "${CMAKE_MATCH_1}")
    string(STRIP "${CMAKE_MATCH_2}" names)
    string(REPLACE " " ";" names "${names}")
    string(MAKE_C_IDENTIFIER "${f}" key)
    set(allow_${key} "${names}")
endforeach()

file(GLOB_RECURSE files RELATIVE ${ROOT} ${ROOT}/*.hpp ${ROOT}/*.cpp)
set(errors "")
foreach(f IN LISTS files)
    string(MAKE_C_IDENTIFIER "${f}" key)
    set(allowed "${allow_${key}}")
    file(STRINGS ${ROOT}/${f} lines)
    set(n 0)
    foreach(line IN LISTS lines)
        math(EXPR n "${n} + 1")
        # skip comment-only lines: prose about std::thread isn't a use
        if(line MATCHES "^[ \t]*//")
            continue()
        endif()
        string(REGEX REPLACE "//.*" "" code "${line}")
        foreach(name IN LISTS ban_names)
            if(code MATCHES "${ban_${name}}")
                list(FIND allowed "${name}" idx)
                if(idx EQUAL -1)
                    list(APPEND errors "${f}:${n}: '${name}' not allowed here: ${line}")
                endif()
            endif()
        endforeach()
    endforeach()
endforeach()

list(LENGTH files nfiles)
if(errors)
    list(JOIN errors "\n  " msg)
    message(FATAL_ERROR "banned concurrency primitives:\n  ${msg}\n"
            "Use jaal's safe types, or add the file to ${ALLOW} with a reason.")
endif()
message(STATUS "banlist ok (${nfiles} files)")
