find_package(Threads REQUIRED)

add_executable(orderpulse_server
    server_main.cpp
)
target_include_directories(orderpulse_server PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/httplib
    ${CMAKE_SOURCE_DIR}/third_party
)
target_link_libraries(orderpulse_server PRIVATE orderpulse_core Threads::Threads)
target_compile_options(orderpulse_server PRIVATE -Wall -Wextra)

# cpp-httplib needs OpenSSL only for HTTPS, which this server doesn't use
# (Render terminates TLS at its edge proxy) -- CPPHTTPLIB_OPENSSL_SUPPORT
# is left undefined so no OpenSSL dependency is pulled in at all.
