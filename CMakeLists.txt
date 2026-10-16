cmake_minimum_required(VERSION 3.22)

project(masceffects LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The preloader SDK. Point PRELOADER_ANDROID_ROOT at a preloader-android
# checkout, or let it default to the vendored submodule path.
get_filename_component(MASC_DIR "${CMAKE_CURRENT_LIST_DIR}" ABSOLUTE)
set(PRELOADER_ANDROID_ROOT
    "${MASC_DIR}/app/src/main/cpp/preloader"
    CACHE PATH "Path to a ChimeraAnt-DEV/preloader-android checkout")

# The SDK's public headers are not self-contained: pl/Logger.hpp needs fmt and
# pl/Config.hpp needs boost.pfr, magic_enum and nlohmann_json. Fetch the same
# pinned versions the official example does, and reuse an existing target when
# the parent build already provides one.
include(FetchContent)

set(MASC_SDK_DEPS)
if(NOT TARGET fmt::fmt)
  FetchContent_Declare(
      fmt
      GIT_REPOSITORY https://github.com/fmtlib/fmt.git
      GIT_TAG 11.2.0)
  set(FMT_DOC OFF CACHE BOOL "Disable fmt docs" FORCE)
  set(FMT_TEST OFF CACHE BOOL "Disable fmt tests" FORCE)
  set(FMT_INSTALL OFF CACHE BOOL "Disable fmt install rules" FORCE)
  list(APPEND MASC_SDK_DEPS fmt)
endif()
if(NOT TARGET boost_pfr)
  FetchContent_Declare(
      boost_pfr
      GIT_REPOSITORY https://github.com/boostorg/pfr.git
      GIT_TAG boost-1.88.0)
  list(APPEND MASC_SDK_DEPS boost_pfr)
endif()
if(NOT TARGET magic_enum::magic_enum)
  FetchContent_Declare(
      magic_enum
      GIT_REPOSITORY https://github.com/Neargye/magic_enum.git
      GIT_TAG v0.9.7)
  list(APPEND MASC_SDK_DEPS magic_enum)
endif()
if(NOT TARGET nlohmann_json::nlohmann_json)
  FetchContent_Declare(
      nlohmann_json
      GIT_REPOSITORY https://github.com/nlohmann/json.git
      GIT_TAG v3.11.3)
  list(APPEND MASC_SDK_DEPS nlohmann_json)
endif()
if(MASC_SDK_DEPS)
  FetchContent_MakeAvailable(${MASC_SDK_DEPS})
endif()

set(MASC_SDK_LIBS
    fmt::fmt
    Boost::pfr
    magic_enum::magic_enum
    nlohmann_json::nlohmann_json)

# ---------------------------------------------------------------------------
# Host build: the pure effect maths and their tests. No Android, no GLES.
# ---------------------------------------------------------------------------
if(NOT ANDROID)
  enable_testing()

  add_library(masc_effect_math STATIC
      src/EffectMath.cpp)
  target_include_directories(masc_effect_math PUBLIC "${MASC_DIR}/src")
  target_compile_options(masc_effect_math PRIVATE
      $<$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>:-Wall -Wextra -Wpedantic>)

  add_executable(masc_effect_tests
      tests/EffectMathTest.cpp)
  target_link_libraries(masc_effect_tests PRIVATE masc_effect_math)
  target_compile_options(masc_effect_tests PRIVATE
      $<$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>:-Wall -Wextra -Wpedantic>)

  add_test(NAME masc_effect_tests COMMAND masc_effect_tests)

  # The config generator uses pl::config, so it needs the SDK headers but not
  # the preloader runtime; PL_CONFIG_NO_RUNTIME removes the NativeMod coupling.
  add_executable(masceffects_config_gen
      tools/GenerateConfig.cpp)
  target_include_directories(masceffects_config_gen PRIVATE
      "${MASC_DIR}/src"
      "${PRELOADER_ANDROID_ROOT}/include")
  target_compile_definitions(masceffects_config_gen PRIVATE PL_CONFIG_NO_RUNTIME)
  target_link_libraries(masceffects_config_gen PRIVATE ${MASC_SDK_LIBS})
  target_compile_options(masceffects_config_gen PRIVATE
      $<$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>:-Wall -Wextra -Wpedantic>)

  return()
endif()

# ---------------------------------------------------------------------------
# Android build: the mod library.
# ---------------------------------------------------------------------------
if(NOT EXISTS "${PRELOADER_ANDROID_ROOT}/include/pl/Mod.hpp")
  message(FATAL_ERROR
      "PRELOADER_ANDROID_ROOT must point to a preloader-android checkout. "
      "Missing: ${PRELOADER_ANDROID_ROOT}/include/pl/Mod.hpp")
endif()

# The preloader loads mods with dlopen(RTLD_NOW | RTLD_GLOBAL) and itself
# links the *shared* libc++ (libc++_shared.so ships in the client APK). If the
# mod statically embeds a second copy of libc++ and exports its symbols, those
# symbols land in the process-global scope and interpose over the preloader's
# own runtime, which is a classic source of "missing symbol" load failures,
# duplicate-runtime crashes and undefined behaviour. Use the same shared C++
# runtime the preloader uses so there is exactly one libc++ in the process.
set(ANDROID_STL c++_shared)

# Only the registration entry point (PLGetModRegistration, PL_REGISTER_MOD)
# is part of the mod's public contract. Everything else is internal to the
# library and must not be exported: the preloader dlopens with RTLD_GLOBAL, so
# every exported function is added to the process-global symbol scope.
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

if(ANDROID_ABI)
  set(MASC_OUTPUT_ABI "${ANDROID_ABI}")
else()
  set(MASC_OUTPUT_ABI "android")
endif()
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY
    "${CMAKE_BINARY_DIR}/out/${MASC_OUTPUT_ABI}")

find_library(MASC_LOG_LIB log)

add_library(masceffects SHARED
    src/EffectMath.cpp
    src/EffectEngine.cpp
    src/GameFeed.cpp
    src/WorldRenderer.cpp
    src/HudOverlay.cpp
    src/MasCeffectsMod.cpp)

target_include_directories(masceffects PRIVATE
    "${MASC_DIR}/src"
    "${PRELOADER_ANDROID_ROOT}/include")

target_link_libraries(masceffects PRIVATE ${MASC_SDK_LIBS})

target_compile_options(masceffects PRIVATE
    $<$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>:-Wall -Wextra -Wpedantic>
    $<$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>:-fvisibility=hidden>
    $<$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>:-fvisibility-inlines-hidden>)

# Internal library symbols (libc++, fmt, the SDK deps) must never leak into the
# global symbol table. --exclude-libs keeps the bundled static libraries'
# symbols local even where the visibility flags above cannot reach them.
target_link_options(masceffects PRIVATE
    -Wl,--exclude-libs,ALL
    -Wl,--exclude-libs,libc++_static.a
    -Wl,--exclude-libs,libc++abi.a)

if(MASC_LOG_LIB)
  target_link_libraries(masceffects PRIVATE "${MASC_LOG_LIB}")
endif()

# The SDK's own symbols (PL_REGISTER_MOD, pl::modmenu::*, pl::render::*) are
# provided by the preloader that loaded this library, so they are left
# unresolved at link time rather than linked against a second copy.
if(TARGET preloader)
  target_link_libraries(masceffects PRIVATE preloader)
else()
  target_link_options(masceffects PRIVATE
      -Wl,--unresolved-symbols=ignore-all)
endif()
