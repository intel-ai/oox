enable_testing()
find_package(GTest REQUIRED)
include(GoogleTest)

list(APPEND OOX_UNITTESTS 
  test_oox
)

foreach(unittest IN LISTS OOX_UNITTESTS)
  add_executable(${unittest} ${unittest}.cpp)
  target_link_libraries(${unittest} ${PROJECT_NAME} gtest)
  target_include_directories(${unittest} PRIVATE ${PROJECT_NAME})
  gtest_add_tests(TARGET ${unittest})
endforeach()
