add_executable(test_DAL
    test_keys_and_mapper.cpp
    test_query_pushdown.cpp
    test_uow_and_transactions.cpp
    test_introspection_and_schema.cpp
    test_p3394_and_reflection.cpp
)
target_link_libraries(test_DAL PRIVATE CodeFirstCppDB::Core CodeFirstCppDB::DAL CodeFirstCppDB::Samples gtest gtest_main)
target_compile_options(test_DAL PRIVATE $<$<CXX_COMPILER_ID:GNU>:-Wall;-Wextra;-Wpedantic;-Wshadow>)
include(GoogleTest)
gtest_discover_tests(test_DAL)
