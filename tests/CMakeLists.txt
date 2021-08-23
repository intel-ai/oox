enable_testing()
find_package(GTest REQUIRED)
include(GoogleTest)

list(APPEND OOX_UNITTESTS 
  test_oox
  )

find_package(TBB)  # not REQUIRED

foreach(unittest IN LISTS OOX_UNITTESTS)
  add_executable(${unittest} ${unittest}.cpp)
  target_include_directories(${unittest} PRIVATE ${PROJECT_NAME})
  target_link_libraries(${unittest} ${PROJECT_NAME} gtest)
  gtest_discover_tests(${unittest})
  if(TBB_FOUND) # build test in TBB mode as well
    add_executable(${unittest}_tbb ${unittest}.cpp)
    target_compile_definitions(${unittest}_tbb PRIVATE HAVE_TBB)
    target_include_directories(${unittest}_tbb PRIVATE ${PROJECT_NAME})
    target_link_libraries(${unittest}_tbb ${PROJECT_NAME} gtest TBB::tbb)
    gtest_discover_tests(${unittest}_tbb)
  endif()
endforeach()
