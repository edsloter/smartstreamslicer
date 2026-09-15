cmake_minimum_required(VERSION 3.18)
project(SmartStreamSlicer VERSION 0.0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT DEFINED CMAKE_POLICY_VERSION_MINIMUM)
  set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
endif()

include(FetchContent)
FetchContent_Declare(
  xxhash
  GIT_REPOSITORY https://github.com/Cyan4973/xxHash.git
  GIT_TAG        v0.8.2
  GIT_SHALLOW    TRUE
  SOURCE_SUBDIR  cmake_unofficial
)
option(SSS_STATIC "Link bundled libraries statically for a self-contained binary" ON)
set(XXHASH_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(XXHASH_BUILD_XXHSUM OFF CACHE BOOL "" FORCE)
set(XXHASH_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
if(NOT SSS_STATIC)
  set(XXHASH_BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)
endif()
FetchContent_MakeAvailable(xxhash)

# --- Vendored submodules (must be checked out via --recursive) --------------
foreach(_dep thirdparty/fastcdc thirdparty/vectorscan)
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_dep}"
     OR NOT IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/${_dep}")
    message(FATAL_ERROR
      "Missing vendored dependency '${_dep}'.\n"
      "Clone with submodules, or run from the repository root:\n"
      "  git submodule update --init --recursive\n"
      "then reconfigure.")
  endif()
endforeach()

# --- Vectorscan (hyperscan fork) for pattern-aware boundary scanning --------
option(SSS_ENABLE_VECTORSCAN "Build sss with Vectorscan pattern scanning" ON)

if(SSS_ENABLE_VECTORSCAN)
  # Boost headers-only dependency for Vectorscan. If BOOST_ROOT does not point
  # at a usable boost (boost/version.hpp present), download Boost 1.84.0
  # release headers automatically and extract them into the build tree.
  if(NOT DEFINED BOOST_ROOT OR NOT EXISTS "${BOOST_ROOT}/boost/version.hpp")
    message(STATUS "Boost headers not found via BOOST_ROOT; downloading Boost 1.84.0 headers...")
    set(_boost_dl  "${CMAKE_CURRENT_BINARY_DIR}/_deps/boost_1_84_0.tar.gz")
    set(_boost_dir "${CMAKE_CURRENT_BINARY_DIR}/_deps/boost_1_84_0")
    if(NOT EXISTS "${_boost_dir}/boost/version.hpp")
      if(NOT EXISTS "${_boost_dl}")
        file(DOWNLOAD
          https://archives.boost.io/release/1.84.0/source/boost_1_84_0.tar.gz
          "${_boost_dl}"
          EXPECTED_HASH SHA256=A5800F405508F5DF8114558CA9855D2640A2DE8F0445F051FA1C7C3383045724
          SHOW_PROGRESS)
      endif()
      file(ARCHIVE_EXTRACT INPUT "${_boost_dl}"
           DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/_deps")
    endif()
    set(BOOST_ROOT "${_boost_dir}"
        CACHE PATH "Boost root (downloaded headers auto-extracted)" FORCE)
  endif()
  if(NOT DEFINED RAGEL)
    set(RAGEL "ragel" CACHE FILEPATH "Ragel state machine compiler")
  endif()
  set(BUILD_UNIT OFF CACHE BOOL "" FORCE)
  set(BUILD_TOOLS OFF CACHE BOOL "" FORCE)
  set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
  set(BUILD_DOC OFF CACHE BOOL "" FORCE)
  set(FAT_RUNTIME OFF CACHE BOOL "" FORCE)
  if(SSS_STATIC)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
  else()
    set(BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)
    set(BUILD_STATIC_LIBS OFF CACHE BOOL "" FORCE)
  endif()
  add_subdirectory(thirdparty/vectorscan)
  if(NOT TARGET hs)
    message(FATAL_ERROR "Vectorscan build did not produce the 'hs' target")
  endif()
endif()

add_executable(sss
  src/main.cpp
  src/cli.cpp
  src/format.cpp
  src/fsutil.cpp
  src/sss_io.cpp
  src/hash.cpp
  src/chunker.cpp
  src/matcher.cpp
  src/encoder.cpp
  src/decoder.cpp
  src/progress.cpp
)

target_include_directories(sss PRIVATE src ${xxhash_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/fastcdc)
target_link_libraries(sss PRIVATE xxhash)

if(SSS_ENABLE_VECTORSCAN)
  target_link_libraries(sss PRIVATE hs)
  target_include_directories(sss PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/vectorscan/src
                                      ${CMAKE_CURRENT_BINARY_DIR}/thirdparty/vectorscan)
  target_compile_definitions(sss PRIVATE SSS_HAVE_VECTORSCAN=1)
endif()

if(MSVC)
  target_compile_options(sss PRIVATE /W4 /permissive- /EHsc)
else()
  target_compile_options(sss PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(SSS_STATIC AND WIN32 AND NOT MSVC)
  # Truly self-contained Windows executable (no MinGW runtime DLLs needed).
  target_link_options(sss PRIVATE -static)
endif()

if(NOT SSS_STATIC AND WIN32 AND TARGET hs_shared AND TARGET hs_runtime_shared)
  # Vectorscan names its DLLs libhs.dll / libhs_runtime.dll on MinGW (CMake
  # 'lib' prefix) while sss.exe is linked against the import names
  # hs.dll / hs_runtime.dll: stage the matching files next to the executable
  # so the shared build runs out of the box.
  add_custom_command(TARGET sss POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:hs_shared>"        "$<TARGET_FILE_DIR:sss>/hs.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:hs_runtime_shared>" "$<TARGET_FILE_DIR:sss>/hs_runtime.dll"
    COMMENT "Staging hs DLLs next to sss")
endif()

install(TARGETS sss RUNTIME DESTINATION bin)