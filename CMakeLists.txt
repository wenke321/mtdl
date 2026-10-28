cmake_minimum_required(VERSION 3.16)
project(mtdl LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

add_executable(mtdl
  src/main.cpp
  src/util/log.cpp
  src/util/strings.cpp
  src/net/url.cpp
  src/net/resolver.cpp
  src/net/socket.cpp
  src/net/stream.cpp
  src/net/buffered.cpp
  src/net/tls.cpp
  src/net/http/request.cpp
  src/net/http/response.cpp
  src/net/http/client.cpp
  src/core/config.cpp
  src/core/prober.cpp
  src/core/segment.cpp
  src/core/endpoint_pool.cpp
  src/core/file_sink.cpp
  src/core/state.cpp
  src/core/progress.cpp
  src/core/worker.cpp
  src/core/downloader.cpp
)

target_include_directories(mtdl PRIVATE include)
target_link_libraries(mtdl PRIVATE OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
target_compile_options(mtdl PRIVATE -O3 -Wall -Wextra -Wno-unused-parameter)
