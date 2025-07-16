# SPDX-FileCopyrightText:  2019-2025 Andy Curtis <contactandyc@gmail.com>
# SPDX-FileCopyrightText: 2024-2025 Knode.ai
# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.10)

# Project Configuration
project(the-io-library VERSION 0.1.1)
# Variables
set(INCLUDE_DIR_NAME "the-io-library")
set(EXTRA_FILES README.md AUTHORS NEWS.md CHANGELOG.md LICENSE NOTICE)
set(CUSTOM_PACKAGES a-memory-library the-lz4-library the-macro-library)
set(THIRD_PARTY_PACKAGES ZLIB)

# Source files
file(GLOB SOURCE_FILES src/*.c)

find_package(a-cmake-library REQUIRED)

include(LibraryConfig)
include(LibraryBuild)

# Testing
if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()
