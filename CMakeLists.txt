cmake_minimum_required(VERSION 3.22)
project(runtime_ownership_lab VERSION 0.2.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(ROL_SANITIZER "none" CACHE STRING "none, address (ASan + UBSan) or thread")
option(ROL_FUZZ "Build the libFuzzer targets (clang only)" OFF)
set(ROL_LIBURING_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.cache/liburing-2.14/install"
    CACHE PATH "Prefix of the pinned liburing build (scripts/build-liburing.sh)")

find_package(Threads REQUIRED)

if(NOT EXISTS "${ROL_LIBURING_ROOT}/lib/liburing.a")
  message(FATAL_ERROR "liburing 2.14 not found under ${ROL_LIBURING_ROOT}. "
                      "Run scripts/build-liburing.sh first.")
endif()
add_library(uring STATIC IMPORTED)
set_target_properties(uring PROPERTIES
  IMPORTED_LOCATION "${ROL_LIBURING_ROOT}/lib/liburing.a"
  INTERFACE_INCLUDE_DIRECTORIES "${ROL_LIBURING_ROOT}/include")

if(ROL_SANITIZER STREQUAL "address")
  add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=undefined
                      -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
elseif(ROL_SANITIZER STREQUAL "thread")
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread)
elseif(NOT ROL_SANITIZER STREQUAL "none")
  message(FATAL_ERROR "Unknown ROL_SANITIZER '${ROL_SANITIZER}'")
endif()

set(ROL_WARNINGS -Wall -Wextra -Wpedantic -Werror)

add_library(rol src/reactor.cpp src/buffers.cpp src/runtime.cpp src/ipc.cpp)
target_include_directories(rol PUBLIC include)
target_link_libraries(rol PUBLIC uring Threads::Threads)
target_compile_options(rol PRIVATE ${ROL_WARNINGS} -Wconversion -Wsign-conversion)

# The key-value service: engine (no I/O), then the server with its front ends.
add_library(kvcore kv/resp.cpp kv/engine.cpp)
target_include_directories(kvcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(kvcore PRIVATE ${ROL_WARNINGS} -Wconversion -Wsign-conversion)

set(ROL_ASIO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.cache/asio-1-36-0"
    CACHE PATH "Standalone Asio headers (scripts/fetch-asio.sh)")
add_executable(kv_server server/main.cpp server/net.cpp server/uring_server.cpp
                         server/epoll_server.cpp)
target_link_libraries(kv_server PRIVATE rol kvcore)
target_compile_options(kv_server PRIVATE ${ROL_WARNINGS})
set(ROL_SERVER_IOS uring epoll)
if(EXISTS "${ROL_ASIO_ROOT}/include/asio.hpp")
  target_sources(kv_server PRIVATE server/asio_server.cpp)
  target_include_directories(kv_server SYSTEM PRIVATE "${ROL_ASIO_ROOT}/include")
  target_compile_definitions(kv_server PRIVATE ROL_HAVE_ASIO ASIO_NO_DEPRECATED)
  if(ROL_SANITIZER STREQUAL "thread" AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # Asio's fenced blocks use std::atomic_thread_fence, which GCC warns TSan
    # does not model. Each io_context here runs on one thread; queues between
    # threads are mutex-protected, which TSan does see.
    set_source_files_properties(server/asio_server.cpp PROPERTIES COMPILE_OPTIONS -Wno-tsan)
  endif()
  list(APPEND ROL_SERVER_IOS asio)
else()
  message(STATUS "Asio not found under ${ROL_ASIO_ROOT}: the Asio front end is not built "
                 "(scripts/fetch-asio.sh)")
endif()

include(CTest)
foreach(suite task reactor buffer runtime ipc)
  add_executable(${suite}_tests tests/${suite}_tests.cpp)
  target_link_libraries(${suite}_tests PRIVATE rol)
  target_compile_options(${suite}_tests PRIVATE ${ROL_WARNINGS})
  add_test(NAME ${suite} COMMAND ${suite}_tests)
  set_tests_properties(${suite} PROPERTIES TIMEOUT 300)
endforeach()

add_executable(kv_tests tests/kv_tests.cpp)
target_link_libraries(kv_tests PRIVATE rol kvcore)
target_compile_options(kv_tests PRIVATE ${ROL_WARNINGS})
add_test(NAME kv COMMAND kv_tests)

add_executable(server_tests tests/server_tests.cpp)
target_link_libraries(server_tests PRIVATE rol)
target_compile_options(server_tests PRIVATE ${ROL_WARNINGS})
foreach(io ${ROL_SERVER_IOS})
  add_test(NAME server_${io} COMMAND server_tests --server $<TARGET_FILE:kv_server> --io ${io})
  set_tests_properties(server_${io} PROPERTIES TIMEOUT 600)
endforeach()
# The io_uring front end over the doorbell cross-core transport.
add_test(NAME server_uring_doorbell COMMAND server_tests --server $<TARGET_FILE:kv_server>
         --io uring --server-arg --xcore --server-arg doorbell)
set_tests_properties(server_uring_doorbell PROPERTIES TIMEOUT 600)

if(ROL_SANITIZER STREQUAL "none")
  foreach(bench sanity_bench xcore_bench)
    add_executable(${bench} bench/${bench}.cpp)
    target_link_libraries(${bench} PRIVATE rol)
    target_compile_options(${bench} PRIVATE ${ROL_WARNINGS})
  endforeach()
  # Load generator: plain epoll, no dependency on the runtime.
  add_executable(kvload bench/kvload.cpp)
  target_link_libraries(kvload PRIVATE Threads::Threads)
  target_compile_options(kvload PRIVATE ${ROL_WARNINGS})
endif()

if(ROL_FUZZ)
  if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "ROL_FUZZ needs clang (libFuzzer)")
  endif()
  add_executable(ipc_packet_fuzz fuzz/ipc_packet_fuzz.cpp)
  target_link_libraries(ipc_packet_fuzz PRIVATE rol)
  target_compile_options(ipc_packet_fuzz PRIVATE -fsanitize=fuzzer ${ROL_WARNINGS})
  target_link_options(ipc_packet_fuzz PRIVATE -fsanitize=fuzzer)
  add_executable(resp_parser_fuzz fuzz/resp_parser_fuzz.cpp)
  target_link_libraries(resp_parser_fuzz PRIVATE kvcore)
  target_compile_options(resp_parser_fuzz PRIVATE -fsanitize=fuzzer ${ROL_WARNINGS})
  target_link_options(resp_parser_fuzz PRIVATE -fsanitize=fuzzer)
endif()
