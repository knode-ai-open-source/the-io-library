# SPDX-FileCopyrightText:  2019-2025 Andy Curtis <contactandyc@gmail.com>
# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.10)
project(MyProject)

find_package(a-memory-library REQUIRED)
find_package(the-macro-library REQUIRED)
find_package(the-lz4-library REQUIRED)
find_package(ZLIB REQUIRED)
find_package(the-io-library REQUIRED)

# List of executables
set(EXECUTABLES convert_interactions list_files dump_files_1 dump_files_2 dump_files_3 dump_files_4 dump_files_5 dump_files_6)

# Loop through each executable
foreach(EXECUTABLE IN LISTS EXECUTABLES)
    # Create executable
    add_executable(${EXECUTABLE} src/${EXECUTABLE}.c)

    # Set compile definitions
    target_compile_definitions(${EXECUTABLE} PUBLIC -D_AML_DEBUG_)

    # Set compile options
    target_compile_options(${EXECUTABLE} PRIVATE -g -O0)

    # Add the macro library
    target_include_directories(${EXECUTABLE} PRIVATE ${the-macro-library_INCLUDE_DIRS})

    # Link libraries
    target_link_libraries(${EXECUTABLE} PRIVATE a-memory-library_debug the-lz4-library_debug the-io-library_debug ZLIB::ZLIB)
endforeach()