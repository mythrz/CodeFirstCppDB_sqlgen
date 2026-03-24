cmake_minimum_required(VERSION 3.23)
project(SqlGenIsolationTest LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# configure sqlgen
set(SQLGEN_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SQLGEN_SQLITE3 ON CACHE BOOL "" FORCE)
add_subdirectory(extern/sqlgen)

# configure Google Test
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
add_subdirectory(extern/googletest)

# code & tests
add_executable(unit_tests 
    src/database_logic.cpp 
    tests/test_main.cpp
)

target_include_directories(unit_tests 
    PRIVATE 
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(unit_tests 
    PRIVATE 
        sqlgen::sqlgen 
        GTest::gtest_main
)
