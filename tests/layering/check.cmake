# tests/layering/check.cmake — enforce DESIGN.md §7: a file's path says what
# it may depend on.
#
#   meta      → (nothing in jaal)
#   core      → meta
#   platform  → meta, core
#   kernel    → meta, core, platform
#   host      → meta, core, platform, kernel
#
# Also: OS headers (<windows.h>, <sys/epoll.h>, <unistd.h>, ...) must never
# appear under include/jaal/. They belong in src/platform/<os>/.
#
# Run with: cmake -DROOT=<include/jaal> -P check.cmake

if(NOT ROOT)
    message(FATAL_ERROR "ROOT not set")
endif()

set(rank_meta     0)
set(rank_core     1)
set(rank_platform 2)
set(rank_kernel   3)
set(rank_host     4)

set(os_headers
    "windows\\.h" "winsock2?\\.h" "sys/epoll\\.h" "sys/event\\.h"
    "sys/eventfd\\.h" "sys/signalfd\\.h" "unistd\\.h" "fcntl\\.h"
    "termios\\.h" "pthread\\.h" "poll\\.h" "signal\\.h")

set(errors "")
file(GLOB_RECURSE headers RELATIVE ${ROOT} ${ROOT}/*.hpp)

foreach(h IN LISTS headers)
    # which layer is this file in? (top-level umbrella headers are exempt)
    string(REGEX MATCH "^([a-z]+)/" _ "${h}")
    set(layer "${CMAKE_MATCH_1}")
    if(NOT layer OR NOT DEFINED rank_${layer})
        continue()
    endif()
    set(my_rank ${rank_${layer}})

    file(STRINGS ${ROOT}/${h} includes REGEX "^[ \t]*#[ \t]*include")
    foreach(line IN LISTS includes)
        # OS headers are banned everywhere in the public tree
        foreach(os IN LISTS os_headers)
            if(line MATCHES "<${os}>")
                list(APPEND errors "${h}: OS header in public tree: ${line}")
            endif()
        endforeach()

        # jaal includes: <jaal/X/...> or relative "../X/..." / "X/..."
        set(target "")
        if(line MATCHES "<jaal/([a-z]+)/")
            set(target "${CMAKE_MATCH_1}")
        elseif(line MATCHES "\"\\.\\./([a-z]+)/")
            set(target "${CMAKE_MATCH_1}")
        endif()
        if(target AND DEFINED rank_${target})
            if(rank_${target} GREATER my_rank)
                list(APPEND errors
                     "${h}: layer '${layer}' may not include layer '${target}': ${line}")
            endif()
        endif()
    endforeach()
endforeach()

list(LENGTH headers n)
if(errors)
    list(JOIN errors "\n  " msg)
    message(FATAL_ERROR "layering violations:\n  ${msg}")
endif()
message(STATUS "layering ok (${n} headers)")
