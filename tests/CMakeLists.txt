enable_testing()

list(APPEND OOX_UNITTESTS 
  test_oox
)

foreach(unittest IN LISTS OOX_UNITTESTS)
  add_executable(${unittest} ${unittest}.cpp)
  target_link_libraries(${unittest} ${PROJECT_NAME} tf::default_settings)
  target_include_directories(${unittest} PRIVATE ${OOX_3RD_PARTY_DIR}/doctest)
  doctest_discover_tests(${unittest})
endforeach()
