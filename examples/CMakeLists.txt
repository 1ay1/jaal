add_executable(ticker ticker.cpp)
target_link_libraries(ticker PRIVATE jaal::jaal jaal_warnings)

# The worked host from docs/hosts.md. Built (not run) so the guide can't rot.
add_executable(host host.cpp)
target_link_libraries(host PRIVATE jaal::jaal jaal_warnings)
