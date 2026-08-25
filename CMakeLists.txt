# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 The rknpu-submit authors

cmake_minimum_required(VERSION 3.16)
project(rknpu-submit VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# The submit seam this implements. Point at an installed librocketnpu, or at a source
# checkout with -DROCKETNPU_INCLUDE_DIR=<path>/rocket-userspace/include.
#
# Only the HEADER is needed to build: this provider DEFINES the seam's symbols rather
# than calling them, so it does not link librocketnpu. The consumer links both.
set(ROCKETNPU_INCLUDE_DIR "" CACHE PATH
    "Path to librocketnpu's include/ (defaults to the installed package)")

if(ROCKETNPU_INCLUDE_DIR)
    set(_rocketnpu_inc "${ROCKETNPU_INCLUDE_DIR}")
else()
    find_package(rocketnpu QUIET)
    if(NOT rocketnpu_FOUND)
        message(FATAL_ERROR
            "librocketnpu not found. Install it, or pass "
            "-DROCKETNPU_INCLUDE_DIR=<path>/rocket-userspace/include.")
    endif()
    get_target_property(_rocketnpu_inc rocketnpu::rocketnpu INTERFACE_INCLUDE_DIRECTORIES)
endif()

if(NOT EXISTS "${_rocketnpu_inc}/rocket_npu.h")
    message(FATAL_ERROR "rocket_npu.h not found under ${_rocketnpu_inc}")
endif()

include(GNUInstallDirs)

add_library(rknpu-submit STATIC src/rknpu_provider.c)
# A frontend's drop-in shape is a shared object (libggml-rocket.so, the TFLite
# delegate), and this archive is linked INTO it, so its objects must be
# position-independent or that link fails outright. librocketnpu already builds
# PIC for the same reason; a provider that does not is only usable by an
# executable.
set_target_properties(rknpu-submit PROPERTIES POSITION_INDEPENDENT_CODE ON)
# librocketnpu's header is a BUILD-time dependency only. This provider's own public
# header needs nothing but <stdint.h>, and a consumer of the installed package reaches
# rocket_npu.h through its own librocketnpu -- it has to, since it links that library
# too. Leaving the path unguarded would bake THIS machine's checkout into the exported
# targets file, where it either resolves to the wrong headers or to nothing at all.
target_include_directories(rknpu-submit
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/rknpu-submit>
            $<BUILD_INTERFACE:${_rocketnpu_inc}>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(rknpu-submit PRIVATE -O2 -Wall -Wextra)
find_package(Threads REQUIRED)
target_link_libraries(rknpu-submit PUBLIC Threads::Threads)
add_library(rknpu-submit::rknpu-submit ALIAS rknpu-submit)

# No libdrm and no kernel headers: the vendor uAPI is transcribed in src/rknpu_uapi.h
# with a _Static_assert on every struct size, so a BSP box with no development headers
# at all still builds this.

install(TARGETS rknpu-submit EXPORT rknpu-submit-targets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/rknpu-submit)
install(EXPORT rknpu-submit-targets NAMESPACE rknpu-submit::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rknpu-submit)
export(EXPORT rknpu-submit-targets NAMESPACE rknpu-submit::
       FILE ${CMAKE_CURRENT_BINARY_DIR}/rknpu-submit-targets.cmake)

# Package config, so a consumer can `find_package(rknpu-submit)` and pass the imported
# target to -DROCKETNPU_PROVIDER_LIB instead of hardcoding a path to the archive. The
# EXPORT above only ships the targets file; CMake needs a Config wrapper to find it.
# Generated into the build tree too, so an uninstalled build is find_package-able via
# -Drknpu-submit_DIR=<build> -- which is the shape the documented build has, since the
# provider and its consumer are configured back to back out of two source checkouts.
include(CMakePackageConfigHelpers)
configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/rknpu-submit-config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/rknpu-submit-config.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rknpu-submit)
# SameMinorVersion, not SameMajorVersion: at 0.x the minor carries the breaking
# changes, so under SameMajorVersion every 0.y would satisfy a consumer that asked for
# 0.1 -- including one that moved the seam. A request for 0.1 is not satisfied by 0.2.
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/rknpu-submit-config-version.cmake
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMinorVersion)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/rknpu-submit-config.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/rknpu-submit-config-version.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rknpu-submit)

# --- the seam this provider has to implement, checked ------------------------------
#
# librocketnpu reaches the kernel through one set of C symbols, and this file implements
# them for the vendor driver. When that set GROWS -- a capability probe added to
# librocketnpu's builtin provider and called from above the seam -- nothing here fails to
# compile, because a provider defines the seam rather than calling it. The break lands in
# whoever links the two together, as an undefined reference in dozens of their targets.
#
# So ask the driver tree what the seam is now. Only possible against a SOURCE checkout:
# the seam is read out of src/rocket_npu.c, which an installed package does not ship. That
# is also the case that needs it least -- an installed librocketnpu is a release, whereas a
# checkout beside this one is what moves under you.
get_filename_component(_rocketnpu_src "${_rocketnpu_inc}" DIRECTORY)
set(_seam_tool "${_rocketnpu_src}/tools/provider-seam.sh")
if(EXISTS "${_seam_tool}")
    enable_testing()
    add_test(NAME provider_seam COMMAND "${_seam_tool}" "$<TARGET_FILE:rknpu-submit>")
    message(STATUS "rknpu-submit: seam check registered against ${_rocketnpu_src}")
else()
    message(STATUS
        "rknpu-submit: seam check skipped -- no tools/provider-seam.sh under "
        "${_rocketnpu_src} (expected when building against an installed package)")
endif()
