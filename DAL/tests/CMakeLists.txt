add_executable(test_DAL test_DAL.cpp)
target_link_libraries(test_DAL PRIVATE Core DAL gtest gtest_main)
add_test(NAME test_DAL COMMAND test_DAL)
