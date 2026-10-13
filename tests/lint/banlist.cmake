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
#   cap_access:: / thread_root(      only jaal mints lock capabilities
#                                    (core/capability.hpp)
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
# ban_thread catches jthread/thread references EXCEPT `std::thread::id` /
# `std::thread::hardware_concurrency()` (using the class as a namespace).
# Everything else — declarations, constructors, container element types,
# emplace_back<std::jthread> — is a thread the file is managing.
set(ban_thread      "std::j?thread([^_:]|$)")
set(ban_detach      "\\.detach\\(\\)")
set(ban_async       "std::async[^_]")
set(ban_mutex       "std::(shared_|recursive_|timed_)?mutex[^_]")
set(ban_atomic      "std::atomic[^_]")
set(ban_thread_local "thread_local")
set(ban_const_cast  "const_cast")
set(ban_sink_access "sink_access")
set(ban_loop_key    "loop_key")
# `[(]`, not `\\(`: MSYS grep's argv parsing eats the backslash.
set(ban_cap_mint    "cap_access::|thread_root[(]")
set(ban_names thread detach async mutex atomic thread_local const_cast sink_access loop_key cap_mint)

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

# Find every source file in ROOT. The check is line-oriented, but doing that
# line split in CMake is a trap: file(STRINGS) treats `;` as a list separator
# so a run of `;`-ending C++ lines collapses into ONE giant string, and the
# greedy `//.*` strip below then discards everything past the first inline
# comment in that merged blob — measured on one 779-line file, STRINGS
# produced 166 items with a 30 KB monster among them and the scanner missed
# every `std::thread` in the run_block_body it hid. file(READ) + MATCHALL /
# REPLACE hit the same class of failures at the escape boundary.
#
# Delegate line boundaries to `grep`, which is line-oriented by definition.
# ONE `execute_process` per ban name over ALL files (`grep -nHE`, each hit
# prefixed `<file>:<lineno>:`), not one per file per name: on Windows a
# process spawn costs tens of milliseconds, and ~1000 files x 10 names took
# the lint past its 300 s timeout. Each hit is then filtered against
# comment-only lines (prose about std::thread isn't a use) and the per-file
# allowlist. grep is present on every platform jaal supports.
file(GLOB_RECURSE files RELATIVE ${ROOT} ${ROOT}/*.hpp ${ROOT}/*.cpp)
find_program(GREP_EXE grep REQUIRED)
set(errors "")
list(SORT files)
foreach(name IN LISTS ban_names)
    if(NOT files)
        break()
    endif()
    execute_process(
        COMMAND ${GREP_EXE} -nHE "${ban_${name}}" ${files}
        WORKING_DIRECTORY ${ROOT}
        OUTPUT_VARIABLE hits
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE grep_rc
        ERROR_QUIET)
    # rc=1 means no match (fine); rc=0 means one or more; rc>1 is an
    # error. Any non-empty output is a candidate.
    if(NOT hits)
        continue()
    endif()
    string(ASCII 1 _sc)
    string(REPLACE ";" "${_sc}" hits "${hits}")
    string(REGEX MATCHALL "[^\n]+" hit_lines "${hits}")
    foreach(hl IN LISTS hit_lines)
        string(REPLACE "${_sc}" ";" hl "${hl}")
        # grep -nH prefixes `<file>:<lineno>:`. File names are the RELATIVE
        # paths we passed (no drive colon), so the first two colons split.
        string(REGEX MATCH "^([^:]+):([0-9]+):(.*)$" _ "${hl}")
        set(f "${CMAKE_MATCH_1}")
        set(lineno "${CMAKE_MATCH_2}")
        set(content "${CMAKE_MATCH_3}")
        # skip comment-only lines
        if(content MATCHES "^[ \t]*//")
            continue()
        endif()
        # skip if the hit is entirely inside a trailing `//` comment
        string(REGEX REPLACE "//.*" "" code "${content}")
        if(NOT code MATCHES "${ban_${name}}")
            continue()
        endif()
        string(MAKE_C_IDENTIFIER "${f}" key)
        set(allowed "${allow_${key}}")
        list(FIND allowed "${name}" idx)
        if(idx EQUAL -1)
            list(APPEND errors "${f}:${lineno}: '${name}' not allowed here: ${content}")
        endif()
    endforeach()
endforeach()

list(LENGTH files nfiles)
if(errors)
    list(JOIN errors "\n  " msg)
    message(FATAL_ERROR "banned concurrency primitives:\n  ${msg}\n"
            "Use jaal's safe types, or add the file to ${ALLOW} with a reason.")
endif()
# A check that scanned nothing is not a check that passed. ROOT is used as a
# glob prefix, so a relative or misspelled path silently matches zero files and
# every violation in the tree reads as clean -- the one failure mode that looks
# exactly like success. Refuse instead.
if(nfiles EQUAL 0)
    message(FATAL_ERROR
            "banlist scanned 0 files under ROOT=${ROOT}\n"
            "Nothing was checked, so this is a failure, not a pass. Pass an "
            "ABSOLUTE path (file(GLOB_RECURSE) resolves a relative ROOT "
            "against the cwd, which for `cmake -P` is wherever it was invoked).")
endif()
message(STATUS "banlist ok (${nfiles} files)")
