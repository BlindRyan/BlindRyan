cmake_minimum_required(VERSION 3.20)
project(RhinoAudioRouter VERSION 0.3.0 LANGUAGES CXX)

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

# Link the C++ runtime statically on MSVC so the plugin needs no extra DLLs.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
FetchContent_Declare(clap
  GIT_REPOSITORY https://github.com/free-audio/clap.git
  GIT_TAG 1.2.6
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(clap)

# clap-wrapper turns the CLAP into a VST3 for hosts other than REAPER. It
# downloads the VST3 SDK (MIT licensed) itself.
set(CLAP_SDK_ROOT "${clap_SOURCE_DIR}" CACHE PATH "" FORCE)
set(CLAP_WRAPPER_DOWNLOAD_DEPENDENCIES ON CACHE BOOL "" FORCE)
FetchContent_Declare(clap-wrapper
  GIT_REPOSITORY https://github.com/free-audio/clap-wrapper.git
  GIT_TAG 1cca996e96f29ab2be7ae9f8cfe532bbc92e1dd6)
FetchContent_MakeAvailable(clap-wrapper)

add_library(RhinoAudioRouter-impl STATIC
  src/entry_impl.cpp
  src/plugin.cpp
  src/engine.cpp
  src/win_audio.cpp
  src/gui.cpp
  src/announce.cpp)
target_link_libraries(RhinoAudioRouter-impl PUBLIC clap
  ole32 uuid mmdevapi comctl32 version user32 gdi32)
target_compile_definitions(RhinoAudioRouter-impl PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00
  AAB_VERSION="${PROJECT_VERSION}")

if(MSVC)
  target_compile_options(RhinoAudioRouter-impl PRIVATE /W4 /utf-8)
else()
  target_compile_options(RhinoAudioRouter-impl PRIVATE -Wall -Wextra)
endif()

make_clapfirst_plugins(
  TARGET_NAME RhinoAudioRouter
  IMPL_TARGET RhinoAudioRouter-impl
  OUTPUT_NAME "RhinoAudioRouter"
  ENTRY_SOURCE src/entry.cpp
  BUNDLE_IDENTIFIER "com.blindryan.rhino-audio-router"
  BUNDLE_VERSION ${PROJECT_VERSION}
  COPY_AFTER_BUILD FALSE
  WINDOWS_FOLDER_VST3 FALSE
  ASSET_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/plugins
  PLUGIN_FORMATS CLAP VST3)

if(NOT MSVC)
  # Link the runtime statically so the plugins need no extra DLLs.
  foreach(t RhinoAudioRouter_clap RhinoAudioRouter_vst3)
    if(TARGET ${t})
      target_link_options(${t} PRIVATE -static -static-libgcc -static-libstdc++)
      set_target_properties(${t} PROPERTIES PREFIX "")
    endif()
  endforeach()
endif()
