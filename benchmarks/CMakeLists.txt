list(APPEND OOX_BENCHMARKS 
  bench_fib
)

enable_testing()
find_package(benchmark REQUIRED)

set(TBB_LIBS "")
find_package(TBB)
if(TBB_FOUND)
  message(STATUS "Building with TBB ${TBB_DIR}")
  add_definitions("-DHAVE_TBB")
  list(APPEND TBB_LIBS TBB::tbb TBB::tbbmalloc)
endif()

foreach(bench IN LISTS OOX_BENCHMARKS)
  add_executable(${bench} ${bench}.cpp)
  target_link_libraries(${bench} ${PROJECT_NAME} benchmark::benchmark ${TBB_LIBS})
  target_include_directories(${bench} PRIVATE ${PROJECT_NAME})
endforeach()
