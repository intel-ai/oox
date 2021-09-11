list(APPEND OOX_BENCHMARKS 
  bench_fib
)

enable_testing()
find_package(benchmark REQUIRED)

set(OOX_LIBS "")

if(TBB_FOUND)
  message(STATUS "Building with TBB ${TBB_DIR}")
  add_definitions("-DHAVE_TBB")
  list(APPEND OOX_LIBS TBB::tbb TBB::tbbmalloc)
endif()

if(Taskflow_FOUND)
  message(STATUS "Building with Taskflow ${Taskflow_DIR}")
  add_definitions("-DHAVE_TF")
  list(APPEND OOX_LIBS Taskflow::Taskflow)
endif()

foreach(bench IN LISTS OOX_BENCHMARKS)
  add_executable(${bench} ${bench}.cpp)
  target_link_libraries(${bench} ${PROJECT_NAME} benchmark::benchmark ${OOX_LIBS})
  target_include_directories(${bench} PRIVATE ${PROJECT_NAME})
endforeach()
