# Copyright (C) 2021 Intel Corporation
#
# SPDX-License-Identifier: Apache-2.0

list(APPEND OOX_BENCHMARKS bench_fib)

find_package(benchmark REQUIRED)

if(OpenMP_FOUND)
  list(APPEND OOX_LOOP_MODES OMP_STATIC OMP_DYNAMIC)
  add_definitions("-DHAVE_OMP")
endif()

if(TBB_FOUND)
  message(STATUS "Building with TBB ${TBB_DIR}")
  add_definitions("-DHAVE_TBB")
  list(APPEND OOX_LIBS TBB::tbb TBB::tbbmalloc)
  list(APPEND OOX_LOOP_MODES TBB_SIMPLE TBB_AUTO TBB_AFFINITY TBB_RAPID)
endif()

if(Taskflow_FOUND)
  message(STATUS "Building with Taskflow ${Taskflow_DIR}")
  add_definitions("-DHAVE_TF")
  list(APPEND OOX_LIBS Taskflow::Taskflow)
  list(APPEND OOX_LOOP_MODES TF_FOR_EACH)
endif()

foreach(bench IN LISTS OOX_BENCHMARKS)
  add_executable(${bench} ${bench}.cpp)
  target_link_libraries(${bench} ${PROJECT_NAME} benchmark::benchmark ${OOX_LIBS})
  target_include_directories(${bench} PRIVATE ${PROJECT_NAME})
  add_test(${bench} ${bench} --benchmark_min_time=0 --benchmark_repetitions=1)
endforeach()

foreach(mode IN LISTS OOX_LOOP_MODES)
  set(bench bench_loops)
  add_executable(${bench}_${mode} ${bench}.cpp)
  target_compile_definitions(${bench}_${mode} PRIVATE PARALLEL=${mode})
  if("${mode}" MATCHES "^OMP_")
    set(TARGET_LIBS "OpenMP::OpenMP_CXX")
  else()
    set(TARGET_LIBS "")
  endif()
  target_link_libraries(${bench}_${mode} ${PROJECT_NAME} benchmark::benchmark ${OOX_LIBS} ${TARGET_LIBS})
  target_include_directories(${bench}_${mode} PRIVATE ${PROJECT_NAME})
  add_test(${bench}_${mode} ${bench}_${mode} --benchmark_min_time=0 --benchmark_repetitions=1)
endforeach()
