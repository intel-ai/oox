enable_testing()

list(APPEND OOX_UNITTESTS 
  test_oox
)

foreach(unittest IN LISTS OOX_UNITTESTS)
  add_executable(${unittest} ${unittest}.cpp)
  target_link_libraries(${unittest} ${PROJECT_NAME})
  target_include_directories(${unittest} PRIVATE ${PROJECT_NAME})
endforeach()
