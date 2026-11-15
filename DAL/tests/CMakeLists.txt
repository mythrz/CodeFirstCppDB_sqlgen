add_executable(test_DAL test_DAL.cpp)
target_link_libraries(test_DAL PRIVATE CodeFirstCppDB::Core CodeFirstCppDB::DAL CodeFirstCppDB::Samples gtest gtest_main)
include(GoogleTest)
gtest_discover_tests(test_DAL)
