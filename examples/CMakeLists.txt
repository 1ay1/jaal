add_executable(ticker ticker.cpp)
target_link_libraries(ticker PRIVATE jaal::jaal jaal_warnings)

# The worked host from docs/hosts.md. Built (not run) so the guide can't rot.
add_executable(host host.cpp)
target_link_libraries(host PRIVATE jaal::jaal jaal_warnings)

# The other shape a host takes: many sockets, line framing, and writes that
# block (registration::modify). POSIX only: it uses BSD sockets directly.
if(UNIX)
    add_executable(server server.cpp)
    target_link_libraries(server PRIVATE jaal::jaal jaal_warnings)
endif()
