cmake_minimum_required(VERSION 3.25)
project(rfeed
  VERSION 0.1.0
  DESCRIPTION "Market Data Feed Handler: MDP 3.0 feed handler in C++23"
  LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(RFEED_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wnon-virtual-dtor -Wold-style-cast)

option(RFEED_BUILD_TESTS "Build unit tests" ON)
option(RFEED_BUILD_BENCH "Build benchmarks"   OFF)
option(RFEED_WERROR      "Treat warnings as errors" ON)
set(RFEED_SANITIZE "" CACHE STRING "Sanitizer: address | undefined | thread (empty = off)")

if(RFEED_WERROR)
  list(APPEND RFEED_WARNINGS -Werror)
endif()

if(RFEED_SANITIZE STREQUAL "address")
  add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address)
elseif(RFEED_SANITIZE STREQUAL "undefined")
  add_compile_options(-fsanitize=undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=undefined)
elseif(RFEED_SANITIZE STREQUAL "thread")
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(Sbe)

rfeed_add_venue(ref)
rfeed_add_venue(alt)

add_library(rfeed STATIC
  src/packet.cpp
  src/pcap.cpp
  src/book_mbp.cpp
  src/instrument.cpp
  src/alt_codec.cpp
  src/alt_encoder.cpp
  src/ref_codec.cpp
  src/ref_encoder.cpp
  src/ref_replay.cpp
  src/replay_client.cpp
  src/transport_udp.cpp
)
target_include_directories(rfeed PUBLIC include)
target_link_libraries(rfeed PUBLIC rfeed_venue_ref rfeed_venue_alt)
target_compile_options(rfeed PRIVATE ${RFEED_WARNINGS})

if(RFEED_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(RFEED_BUILD_BENCH)
  add_subdirectory(bench)
endif()
