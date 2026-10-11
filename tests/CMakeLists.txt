include(FetchContent)

# GoogleTest pinned. Single source of truth for the version.
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.15.2
)
set(BUILD_GMOCK     OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST   OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(rfeed_tests
  book_test.cpp
  byte_reader_test.cpp
  codec_test.cpp
  line_arbiter_test.cpp
  packet_test.cpp
  pcap_test.cpp
  transport_udp_test.cpp
  recovery_test.cpp
  replay_test.cpp
)
target_link_libraries(rfeed_tests PRIVATE rfeed GTest::gtest_main)
target_include_directories(rfeed_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(rfeed_tests PRIVATE ${RFEED_WARNINGS})

include(GoogleTest)
gtest_discover_tests(rfeed_tests)
