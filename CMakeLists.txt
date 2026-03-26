cmake_minimum_required(VERSION 3.23)
project(SqlGenIsolationTest LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# NOT using vcpkg
set(SQLGEN_USE_VCPKG OFF CACHE BOOL "" FORCE) 

# reflect-cpp (Submodule)
add_subdirectory(extern/reflect-cpp)
set(reflectcpp_DIR "${CMAKE_CURRENT_BINARY_DIR}" CACHE PATH "" FORCE)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/reflectcppConfig.cmake" "")

# sqlite3 (System)
find_package(SQLite3 REQUIRED)
if(NOT TARGET SQLite::SQLite3)
    add_library(SQLite::SQLite3 INTERFACE IMPORTED)
    target_link_libraries(SQLite::SQLite3 INTERFACE SQLite3::SQLite3)
endif()
set(SQLite3_DIR "${CMAKE_CURRENT_BINARY_DIR}" CACHE PATH "" FORCE)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/SQLite3Config.cmake" "include_directories(\"${SQLite3_INCLUDE_DIRS}\")")

# sqlgen (submodule)
set(SQLGEN_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SQLGEN_SQLITE3 ON CACHE BOOL "" FORCE)
add_subdirectory(extern/sqlgen)

# googleTest (submodule)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
add_subdirectory(extern/googletest)

# code & tests
add_executable(unit_tests 
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
