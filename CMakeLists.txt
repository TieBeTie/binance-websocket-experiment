cmake_minimum_required(VERSION 3.16)
project(webhook_parsing LANGUAGES C CXX)

set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
set(THREADS_PREFER_PTHREAD_FLAG TRUE)


option(BUILD_TESTING "Build tests" OFF)

find_package(Boost REQUIRED COMPONENTS system context coroutine)
find_package(OpenSSL REQUIRED)

add_executable(webhook_parsing
  src/url.cpp
  include/stream_merger.hpp
  include/reactor.hpp
  include/runner.hpp
  include/sessions/isession.hpp
  include/sessions/async_session.hpp
  include/sessions/session.hpp
  src/main.cpp
)

target_include_directories(webhook_parsing PRIVATE ${Boost_INCLUDE_DIRS})
target_include_directories(webhook_parsing PRIVATE include)
target_link_libraries(webhook_parsing PRIVATE
  Boost::system
  Boost::context
  Boost::coroutine
  OpenSSL::SSL
  OpenSSL::Crypto
)

if (NOT MSVC)
  target_link_options(webhook_parsing PRIVATE -pthread)
  target_compile_options(webhook_parsing PRIVATE -pthread)
endif()

if (MSVC)
  target_compile_options(webhook_parsing PRIVATE /W4)
else()
  target_compile_options(webhook_parsing PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Enforce C++23 via compile flag (compatible with older CMake) and GCC 13+
include(CheckCXXCompilerFlag)
if (NOT MSVC)
  check_cxx_compiler_flag("-std=c++23" HAS_CXX23)
  if (HAS_CXX23)
    target_compile_options(webhook_parsing PRIVATE -std=c++23)
  else()
    message(FATAL_ERROR "Compiler does not support -std=c++23. Please use GCC 13+ (g++-13).")
  endif()
endif()


