cmake_minimum_required(VERSION 3.20)
project(AppAudioBridge VERSION 0.1.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(AAB_BUILD_TESTS "Build the platform-independent unit tests" ON)

if(AAB_BUILD_TESTS)
  enable_testing()
  add_executable(ring_buffer_test tests/ring_buffer_test.cpp)
  target_include_directories(ring_buffer_test PRIVATE src)
  add_test(NAME ring_buffer_test COMMAND ring_buffer_test)
endif()

if(NOT WIN32)
  message(STATUS "Not targeting Windows: only the unit tests will be built.")
  return()
endif()

include(FetchContent)
FetchContent_Declare(clap
  GIT_REPOSITORY https://github.com/free-audio/clap.git
  GIT_TAG 1.2.6
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(clap)

add_library(AppAudioBridge MODULE
  src/entry.cpp
  src/plugin.cpp
  src/engine.cpp
  src/win_audio.cpp
  src/gui.cpp
  src/announce.cpp)
target_link_libraries(AppAudioBridge PRIVATE clap
  ole32 uuid mmdevapi comctl32 version user32 gdi32)
target_compile_definitions(AppAudioBridge PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00
  AAB_VERSION="${PROJECT_VERSION}")
set_target_properties(AppAudioBridge PROPERTIES PREFIX "" SUFFIX ".clap")

if(MSVC)
  target_compile_options(AppAudioBridge PRIVATE /W4 /utf-8)
  set_property(TARGET AppAudioBridge PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
  target_compile_options(AppAudioBridge PRIVATE -Wall -Wextra)
  # Link the runtime statically so the plugin needs no extra DLLs.
  target_link_options(AppAudioBridge PRIVATE -static -static-libgcc -static-libstdc++)
endif()
