# Copyright (C) 2021 Intel Corporation
#
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required (VERSION 3.12)
MESSAGE(STATUS "CMAKE_ROOT=" ${CMAKE_ROOT})

# Project name
project(OOX VERSION 0.2.0 LANGUAGES CXX)

# build options
option(OOX_BUILD_BENCHMARKS "Enables builds of benchmarks" ON)
option(OOX_BUILD_TESTS "Enables builds of tests" ON)
option(OOX_ENABLE_TBB "Enable TBB for threading (priority)" ON)
option(OOX_LOCAL_TBB "Force local copy of TBB" OFF)
option(OOX_ENABLE_TF "Enable Taskflow for threading" ON)
option(OOX_LOCAL_TF "Force local copy of TaskFlow" OFF)
option(OOX_ENABLE_OMP "Enable OpenMP for comparison (if found)" ON)

# Global features
if(OOX_SANITIZE)
  add_compile_options(-fsanitize=${OOX_SANITIZE} -g -O1)
  add_link_options(-fsanitize=${OOX_SANITIZE} -g -O1)
  MESSAGE(STATUS "Building with -fsanitize=" ${OOX_SANITIZE})
  set(TBB_SANITIZE ${OOX_SANITIZE})
endif()

# Turn on the verbose
#set(CMAKE_VERBOSE_MAKEFILE ON)

# Include additional language check
include(CheckLanguage)

# Adhere to GNU conventions
include(GNUInstallDirs)

# Enable test
include(CTest)

# Find packages
find_package(Threads REQUIRED)

if(OOX_ENABLE_TBB)
  if(NOT OOX_LOCAL_TBB)
    find_package(TBB 2021 QUIET)  # not REQUIRED
  endif()
  if(NOT TBB_FOUND)
    execute_process(COMMAND git submodule update --init --depth=1 thirdparty/oneTBB/
                    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} )
    option(TBB_TEST OFF)
    add_subdirectory(thirdparty/oneTBB onetbb)
    set(TBB_FOUND ON)
  endif()
endif()

if(OOX_ENABLE_TF)
  if(NOT OOX_LOCAL_TF)
    find_package(Taskflow QUIET)  # not REQUIRED
  endif()
  if(NOT Taskflow_FOUND)
    execute_process(COMMAND git submodule update --init --depth=1 thirdparty/taskflow/
                    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} )
    option(TF_BUILD_TESTS OFF)
    option(TF_BUILD_EXAMPLES OFF)
    add_subdirectory(thirdparty/taskflow taskflow)
    add_library(Taskflow::Taskflow ALIAS Taskflow)
    set(Taskflow_FOUND ON)
  endif()
endif()

if(OOX_ENABLE_OMP)
  find_package(OpenMP)
endif()

# Library
add_library(${PROJECT_NAME} INTERFACE)
add_library(${PROJECT_NAME}::${PROJECT_NAME} ALIAS ${PROJECT_NAME})
target_compile_features(${PROJECT_NAME} INTERFACE cxx_std_11)
target_link_libraries(${PROJECT_NAME} INTERFACE Threads::Threads)
target_include_directories(${PROJECT_NAME} INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
  $<INSTALL_INTERFACE:include/> 
)

enable_testing()
if(OOX_BUILD_TESTS)
  add_subdirectory(tests)
endif(OOX_BUILD_TESTS)

if(OOX_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif(OOX_BUILD_BENCHMARKS)

# -----------------------------------------------------------------------------
# create find_package(OOX CONFIG)
# -----------------------------------------------------------------------------

# install header
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/oox DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# export target
set_target_properties(${PROJECT_NAME} PROPERTIES EXPORT_NAME ${PROJECT_NAME})

export(
  TARGETS ${PROJECT_NAME} 
  NAMESPACE ${PROJECT_NAME}:: 
  FILE ${PROJECT_NAME}Targets.cmake
)
export(PACKAGE ${PROJECT_NAME})

install(TARGETS ${PROJECT_NAME} EXPORT ${PROJECT_NAME}Targets)
install(
  EXPORT ${PROJECT_NAME}Targets 
  NAMESPACE ${PROJECT_NAME}:: 
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}
)

# set up config
include(CMakePackageConfigHelpers)

configure_package_config_file(
  ${PROJECT_NAME}Config.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}Config.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}
  PATH_VARS CMAKE_INSTALL_FULL_INCLUDEDIR
)

write_basic_package_version_file(
  ${PROJECT_NAME}ConfigVersion.cmake 
  COMPATIBILITY SameMajorVersion
)

install(
  FILES ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}Config.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}ConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}
)
