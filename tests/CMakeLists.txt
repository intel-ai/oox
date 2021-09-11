enable_testing()
find_package(GTest REQUIRED)
include(GoogleTest)

list(APPEND OOX_UNITTESTS 
  test_oox
  )

foreach(unittest IN LISTS OOX_UNITTESTS)
  # Fallback implementation
  add_executable(${unittest} ${unittest}.cpp)
  target_include_directories(${unittest} PRIVATE ${PROJECT_NAME})
  target_link_libraries(${unittest} ${PROJECT_NAME} gtest)
  gtest_discover_tests(${unittest})
  # Serial debugging implementation
  add_executable(${unittest}_serial ${unittest}.cpp)
  target_compile_definitions(${unittest}_serial PRIVATE OOX_SERIAL_DEBUG)
  target_include_directories(${unittest}_serial PRIVATE ${PROJECT_NAME})
  target_link_libraries(${unittest}_serial ${PROJECT_NAME} gtest)
  gtest_discover_tests(${unittest}_serial)
  if(TBB_FOUND) # build test in TBB mode as well
    add_executable(${unittest}_tbb ${unittest}.cpp)
    target_compile_definitions(${unittest}_tbb PRIVATE HAVE_TBB)
    target_include_directories(${unittest}_tbb PRIVATE ${PROJECT_NAME})
    target_link_libraries(${unittest}_tbb ${PROJECT_NAME} gtest TBB::tbb)
    gtest_discover_tests(${unittest}_tbb)
  endif()
  if(Taskflow_FOUND) # build test in TF mode as well
    add_executable(${unittest}_tf ${unittest}.cpp)
    target_compile_definitions(${unittest}_tf PRIVATE HAVE_TF)
    target_include_directories(${unittest}_tf PRIVATE ${PROJECT_NAME})
    target_link_libraries(${unittest}_tf ${PROJECT_NAME} gtest Taskflow::Taskflow)
    gtest_discover_tests(${unittest}_tf)
  endif()
endforeach()
