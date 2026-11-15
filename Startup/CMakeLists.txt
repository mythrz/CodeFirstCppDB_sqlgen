add_executable(Startup main.cpp)
target_link_libraries(Startup PRIVATE CodeFirstCppDB::Core CodeFirstCppDB::DAL CodeFirstCppDB::Samples sqlgen SQLite3::SQLite3)
target_compile_options(Startup PRIVATE $<$<CXX_COMPILER_ID:GNU>:-Wall;-Wextra;-Wpedantic;-Wshadow>)
target_compile_features(Startup PRIVATE cxx_std_26)
