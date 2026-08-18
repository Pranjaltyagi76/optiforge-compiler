cmake_minimum_required(VERSION 3.20)
project(OptiForge VERSION 0.1.0 LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Global settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# Keep binaries in one predictable place so the test harness can find them.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

option(OPTIFORGE_WERROR "Treat compiler warnings as errors (NFR-08)" ON)
set(OPTIFORGE_SANITIZE "" CACHE STRING
    "Comma-separated sanitizer list, e.g. address,undefined. Empty to disable.")

# ---------------------------------------------------------------------------
# Version header (generated)
# ---------------------------------------------------------------------------
set(OPTIFORGE_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
configure_file(
  "${CMAKE_SOURCE_DIR}/include/optiforge/support/Version.h.in"
  "${OPTIFORGE_GENERATED_DIR}/optiforge/support/Version.h"
  @ONLY)

# ---------------------------------------------------------------------------
# Shared compile options / warnings
#
# Carried by an INTERFACE target so every module gets identical treatment and
# no target can quietly opt out. (architectural_design.md NFR-08)
# ---------------------------------------------------------------------------
add_library(optiforge_options INTERFACE)

target_include_directories(optiforge_options INTERFACE
  "${CMAKE_SOURCE_DIR}/include"
  "${OPTIFORGE_GENERATED_DIR}")

if(MSVC)
  target_compile_options(optiforge_options INTERFACE /W4 /permissive- /EHsc)
  if(OPTIFORGE_WERROR)
    target_compile_options(optiforge_options INTERFACE /WX)
  endif()
else()
  target_compile_options(optiforge_options INTERFACE
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wnon-virtual-dtor
    -Woverloaded-virtual
    -Wnull-dereference
    -Wformat=2)
  if(OPTIFORGE_WERROR)
    target_compile_options(optiforge_options INTERFACE -Werror)
  endif()
endif()

# ---------------------------------------------------------------------------
# Static runtime on MinGW
#
# This machine has three different libstdc++-6.dll copies on PATH (MSYS2,
# WinLibs, and a WinGet package). A binary built by one GCC would load
# whichever DLL PATH happened to find first, and the ABI mismatch produced
# silently wrong behaviour -- a failed ifstream open that did not set failbit.
#
# The same binary gave different answers depending on how it was launched,
# which breaks determinism (NFR-06) and makes test results meaningless.
# Linking the runtime statically makes our binaries self-contained and immune
# to DLL search order.
# ---------------------------------------------------------------------------
if(MINGW)
  target_link_options(optiforge_options INTERFACE
    -static
    -static-libgcc
    -static-libstdc++)
endif()

# ---------------------------------------------------------------------------
# Sanitizers
#
# MinGW does not ship libasan/libubsan, so requesting them there produces a
# confusing link failure rather than an honest one. Fail loudly at configure
# time instead. (deployment.md §12 — turn a two-hour bug into a one-line message)
# ---------------------------------------------------------------------------
if(OPTIFORGE_SANITIZE)
  if(MINGW)
    message(FATAL_ERROR
      "OPTIFORGE_SANITIZE=${OPTIFORGE_SANITIZE} was requested, but MinGW does not "
      "ship the sanitizer runtimes. Build sanitizer configurations under WSL2/Linux "
      "(see context/deployment.md §2), or reconfigure with -DOPTIFORGE_SANITIZE=.")
  elseif(MSVC)
    target_compile_options(optiforge_options INTERFACE /fsanitize=address)
  else()
    target_compile_options(optiforge_options INTERFACE
      -fsanitize=${OPTIFORGE_SANITIZE} -fno-omit-frame-pointer)
    target_link_options(optiforge_options INTERFACE
      -fsanitize=${OPTIFORGE_SANITIZE})
  endif()
endif()

# ---------------------------------------------------------------------------
# Modules
#
# Dependency rules from architectural_design.md §3 are enforced by the link
# graph below. Adding a forbidden dependency breaks the build, by design.
# ---------------------------------------------------------------------------
add_subdirectory(src/support)   # of_support  — depends on nothing
add_subdirectory(src/frontend)  # of_frontend — lexer, AST, parser
add_subdirectory(src/driver)    # optiforge   — the CLI executable

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()
add_subdirectory(tests)

# ---------------------------------------------------------------------------
# Configuration summary
# ---------------------------------------------------------------------------
message(STATUS "")
message(STATUS "OptiForge ${PROJECT_VERSION}")
message(STATUS "  Build type      : ${CMAKE_BUILD_TYPE}")
message(STATUS "  Compiler        : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  Warnings->errors: ${OPTIFORGE_WERROR}")
if(OPTIFORGE_SANITIZE)
  message(STATUS "  Sanitizers      : ${OPTIFORGE_SANITIZE}")
else()
  message(STATUS "  Sanitizers      : (none)")
endif()
message(STATUS "")
