cmake_minimum_required(VERSION 3.20)
project(the_io_library VERSION 1.0.0 LANGUAGES C)

# ---- Shared build variant ----
set(A_BUILD_VARIANT "debug" CACHE STRING "Build variant (debug|memory|coverage|static|shared)")
set_property(CACHE A_BUILD_VARIANT PROPERTY STRINGS debug memory coverage static shared)

# ---- Flavor-specific knobs ----
set(A_BUILD_MEMORY_DEFINE "_AML_DEBUG_" CACHE STRING "Macro for memory variant")
set(A_BUILD_DEBUG_POSTFIX ""            CACHE STRING "Debug filename postfix")
set(A_BUILD_IMPORT_NAME   ""            CACHE STRING "Exported target name for this flavor")

# ---- Sources ----
add_library(${PROJECT_NAME}
    src/io.c
    src/io_in.c
    src/io_in_base.c
    src/io_out.c
)

target_include_directories(${PROJECT_NAME} PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

set_target_properties(${PROJECT_NAME} PROPERTIES
  C_STANDARD 99
  C_STANDARD_REQUIRED YES
)

# ---- Variant-specific compile settings ----
if(A_BUILD_VARIANT STREQUAL "memory")
  target_compile_definitions(${PROJECT_NAME} PUBLIC ${A_BUILD_MEMORY_DEFINE})
elseif(A_BUILD_VARIANT STREQUAL "coverage")
  if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    target_compile_options(${PROJECT_NAME} PRIVATE -O0 -g -fprofile-instr-generate -fcoverage-mapping)
    target_link_options(${PROJECT_NAME}    PRIVATE -fprofile-instr-generate)
  elseif(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    target_compile_options(${PROJECT_NAME} PRIVATE -O0 -g --coverage)
    target_link_options(${PROJECT_NAME}    PRIVATE --coverage)
  endif()
endif()

# Naming tweaks
if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND A_BUILD_DEBUG_POSTFIX)
  set_target_properties(${PROJECT_NAME} PROPERTIES DEBUG_POSTFIX "${A_BUILD_DEBUG_POSTFIX}")
endif()
if(WIN32 AND NOT BUILD_SHARED_LIBS)
  set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "${PROJECT_NAME}_s")
endif()

# ---- Dependencies ----
find_package(a_memory_library CONFIG REQUIRED)
find_package(the_macro_library CONFIG REQUIRED)
find_package(the_lz4_library CONFIG REQUIRED)
find_package(ZLIB REQUIRED)

target_link_libraries(${PROJECT_NAME} PUBLIC
  a_memory_library::a_memory_library
  the_lz4_library::the_lz4_library
  the_macro_library::the_macro_library
  ZLIB::ZLIB
)

# ---- Install/export ----
include(GNUInstallDirs)
install(TARGETS ${PROJECT_NAME} EXPORT ${PROJECT_NAME}Targets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# Auto-name exported imported target per variant (matches config’s expectation)
if(NOT A_BUILD_IMPORT_NAME)
  set(A_BUILD_IMPORT_NAME "${PROJECT_NAME}_${A_BUILD_VARIANT}")
endif()
set_target_properties(${PROJECT_NAME} PROPERTIES EXPORT_NAME "${A_BUILD_IMPORT_NAME}")

# Export file suffix equals the variant
set(A_BUILD_EXPORT_FILE_SUFFIX "${A_BUILD_VARIANT}")

install(EXPORT ${PROJECT_NAME}Targets
  FILE "${PROJECT_NAME}Targets-${A_BUILD_EXPORT_FILE_SUFFIX}.cmake"
  NAMESPACE ${PROJECT_NAME}::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME})

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# ---- Package config template (runtime pick; function-scoped) ----
set(A_BUILD_VARIANTS "debug;memory;coverage;static;shared")
set(_CFG_IN "${CMAKE_CURRENT_BINARY_DIR}/config.cmake.in")

file(WRITE "${_CFG_IN}" [=[
@PACKAGE_INIT@

function(_@A_BUILD_TARGET_BASENAME@_cfg)
  include(CMakeFindDependencyMacro)

  set(_pkg "@A_BUILD_TARGET_BASENAME@")
  set(_ns  "@A_BUILD_EXPORT_NAMESPACE@")
  set(_deps "@A_BUILD_DEPS@")
  set(_flavors "@A_BUILD_VARIANTS@")

  foreach(_dep IN LISTS _deps)
    if(_dep)
      find_dependency("${_dep}" REQUIRED)
    endif()
  endforeach()

  if(NOT TARGET ${_ns}::${_pkg})
    add_library(${_ns}::${_pkg} INTERFACE IMPORTED)
  endif()

  if(NOT DEFINED A_BUILD_VARIANT)
    set(A_BUILD_VARIANT "debug")
  endif()
  list(FIND _flavors "${A_BUILD_VARIANT}" _idx)
  if(_idx EQUAL -1)
    message(FATAL_ERROR "${_pkg}: variant '${A_BUILD_VARIANT}' not recognized (must be one of: ${_flavors})")
  endif()

  set(_picked "${A_BUILD_VARIANT}")
  set(_dir "${CMAKE_CURRENT_LIST_DIR}")
  set(_f "${_pkg}Targets-${_picked}.cmake")
  if(EXISTS "${_dir}/${_f}")
    include("${_dir}/${_f}")
  else()
    message(FATAL_ERROR "${_pkg}: variant '${_picked}' not installed.")
  endif()

  string(REPLACE "-" "_" _picked_us "${_picked}")
  set(_pick ${_ns}::${_pkg}_${_picked_us})
  if(TARGET "${_pick}")
    target_link_libraries(${_ns}::${_pkg} INTERFACE "${_pick}")
  else()
    message(FATAL_ERROR "${_pkg}: expected target '${_pick}' not defined by export.")
  endif()

  set(CMAKE_MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release)
  set(CMAKE_MAP_IMPORTED_CONFIG_MINSIZEREL    Release)
endfunction()

_@A_BUILD_TARGET_BASENAME@_cfg()
]=])

# Inject variables and configure
set(A_BUILD_TARGET_BASENAME "${PROJECT_NAME}")
set(A_BUILD_EXPORT_NAMESPACE "${PROJECT_NAME}")
set(A_BUILD_DEPS "a_memory_library;the_macro_library;the_lz4_library;ZLIB")

include(CMakePackageConfigHelpers)
configure_package_config_file(
  "${_CFG_IN}"
  "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}Config.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}"
)

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}Config.cmake"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}")
