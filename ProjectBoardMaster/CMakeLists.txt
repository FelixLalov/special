cmake_minimum_required(VERSION 3.20)
project(BoardMaster CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
  message(FATAL_ERROR "BoardMaster is a Win32 application and only builds on Windows.")
endif()

add_executable(BoardMaster WIN32
  src/main.cpp
  src/Json.cpp
  src/Config.cpp
  src/Profile.cpp
  src/Comm.cpp
  src/MainDlg.cpp
  src/BoardMaster.rc
)

target_include_directories(BoardMaster PRIVATE src)

# ANSI Win32 API (the -A entry points) - keep UNICODE undefined on purpose.
target_compile_definitions(BoardMaster PRIVATE
  WIN32_LEAN_AND_MEAN
  _CRT_SECURE_NO_WARNINGS
  NOMINMAX
)

target_link_libraries(BoardMaster PRIVATE comctl32 ws2_32 comdlg32 shell32 ole32)

if(MSVC)
  target_compile_options(BoardMaster PRIVATE /W4 /permissive- /utf-8)
endif()

# Keep the binary next to a predictable root so the default "..\dat" lookup works
# the same in every configuration.
set_target_properties(BoardMaster PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY         "${CMAKE_SOURCE_DIR}/bin"
  RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${CMAKE_SOURCE_DIR}/bin"
  RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/bin"
)

# ----------------------------------------------------------------- tests
#
# Three console programs that check the pieces the GUI cannot show:
#   test_profile  parses real machine-configuration directories
#   test_config   round-trips the JSON configuration file
#   test_comm     drives the framing and register messages against a fake board
#
# Build and run:  cmake --build build --target run_tests
option(BOARDMASTER_TESTS "Build the console self-checks" ON)

if(BOARDMASTER_TESTS)
  add_executable(test_profile tests/test_profile.cpp src/Profile.cpp)
  add_executable(test_config  tests/test_config.cpp  src/Config.cpp src/Json.cpp)
  add_executable(test_comm    tests/test_comm.cpp    src/Comm.cpp)

  foreach(t test_profile test_config test_comm)
    target_include_directories(${t} PRIVATE src)
    target_compile_definitions(${t} PRIVATE WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS NOMINMAX)
    if(MSVC)
      target_compile_options(${t} PRIVATE /W4 /permissive- /utf-8)
    endif()
    set_target_properties(${t} PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY         "${CMAKE_SOURCE_DIR}/bin"
      RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${CMAKE_SOURCE_DIR}/bin"
      RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/bin"
    )
  endforeach()

  target_link_libraries(test_comm PRIVATE ws2_32)

  add_custom_target(run_tests
    COMMAND test_config
    COMMAND test_comm
    COMMAND test_profile
    DEPENDS test_config test_comm test_profile
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    COMMENT "Running the BoardMaster self-checks"
  )
endif()
