cmake_minimum_required(VERSION 3.21)

# zterminal only supports Apple Silicon on macOS. This must be set before
# project() -- setting CMAKE_OSX_ARCHITECTURES any later is too late for the
# compiler checks project() runs. Guarded so an explicit
# -DCMAKE_OSX_ARCHITECTURES=... on the command line still wins.
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin" AND NOT CMAKE_OSX_ARCHITECTURES)
  set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "Build architectures for macOS" FORCE)
endif()
# Minimum macOS for zterminal.app (Info.plist LSMinimumSystemVersion). CI links
# Homebrew Qt bottles built for macOS 14 (Sonoma), so nothing older can work.
set(ZTERMINAL_MACOS_MIN "14.0")
# Bundle id must be available in app/ scope (where the zterminal target lives)
# when CMake configures Info.plist. MacBundle.cmake also sets this; keep a
# top-level value so @ZTERMINAL_BUNDLE_ID@ would not expand empty if reintroduced.
set(ZTERMINAL_BUNDLE_ID "ee.sbj.zterminal")
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin" AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "${ZTERMINAL_MACOS_MIN}" CACHE STRING "Minimum macOS version" FORCE)
endif()

# Single source of truth for the version: bump ONLY the VERSION here.
# configure_file() turns it into generated/version.hpp (window title, About,
# --version, update checks); CPack names the .deb / .dmg from PROJECT_VERSION.
project(zterminal VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(ZTERMINAL_BUILD_TESTS "Build the Qt Test suite" ON)

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets SerialPort Network)
# libsodium (Argon2id + XChaCha20-Poly1305) for the password vault.
find_package(PkgConfig REQUIRED)
pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium>=1.0.18)

include(cmake/CompilerWarnings.cmake)

set(ZTERMINAL_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/src/version.hpp.in"
  "${ZTERMINAL_GENERATED_DIR}/version.hpp"
  @ONLY)

add_subdirectory(third_party/libvterm)
add_subdirectory(core)
add_subdirectory(askpass)
add_subdirectory(app)

# macOS: zterminal.app bundle (Info.plist, icon, askpass, macdeployqt at install).
if(APPLE)
  include(cmake/MacBundle.cmake)
endif()

if(ZTERMINAL_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

include(cmake/Packaging.cmake)
