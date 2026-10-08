# tests/lint/layering.cmake — jaal is the bottom layer: agentty → maya → jaal.
#
# jaal is the runtime and the platform layer. It knows nothing about the
# view layer (maya) or any app built on it (agentty): no includes, no
# names in code. Comments and string literals may mention them.
#
# Run with: cmake -DROOT=<jaal source dir> -P layering.cmake

if(NOT DEFINED ROOT)
    message(FATAL_ERROR "ROOT is required")
endif()

set(hits "")
foreach(dir include src tests)
    file(GLOB_RECURSE files ${ROOT}/${dir}/*.cpp ${ROOT}/${dir}/*.hpp ${ROOT}/${dir}/*.h)
    foreach(f IN LISTS files)
        file(STRINGS ${f} lines REGEX "maya|agentty")
        if(NOT lines)
            continue()
        endif()
        file(RELATIVE_PATH rel ${ROOT} ${f})
        foreach(l IN LISTS lines)
            string(REGEX REPLACE "//.*$" "" code "${l}")
            string(REGEX REPLACE "\"[^\"]*\"" "\"\"" code "${code}")
            if(code MATCHES "#[ \t]*include[ \t]*[<\"](maya|agentty)/"
               OR code MATCHES "(^|[^A-Za-z0-9_])(maya|agentty)::"
               OR code MATCHES "(^|[^A-Za-z0-9_])(MAYA|AGENTTY)_[A-Z]")
                string(STRIP "${l}" s)
                list(APPEND hits "${rel}: ${s}")
            endif()
        endforeach()
    endforeach()
endforeach()

if(hits)
    list(JOIN hits "\n  " msg)
    message(FATAL_ERROR
        "jaal reaches up into a layer above it:\n  ${msg}\n\n"
        "jaal is the bottom layer. Put what the upper layer needs behind a "
        "jaal concept or a host hook, and let maya/agentty supply it.")
endif()
message(STATUS "layering ok: jaal depends on nothing above it")
