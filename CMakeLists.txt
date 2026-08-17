cmake_minimum_required(VERSION 4.4)
set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "f35a9ac6-8463-4d38-8eec-5d6008153e7d")
project(SqlGenIsolationTest LANGUAGES CXX)

# set(CMAKE_CXX_STANDARD 20)
# set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Enable C++26 reflection. gcc 16+ only
add_compile_options(-freflection)

# Enable automatic module dependency scanning across targets
set(CMAKE_CXX_SCAN_FOR_MODULES ON)
set(CMAKE_CXX_MODULE_STD ON) # build std module

# NOT using vcpkg
set(SQLGEN_USE_VCPKG OFF CACHE BOOL "" FORCE)

# reflect-cpp (Submodule)
add_subdirectory(extern/reflect-cpp)
set(reflectcpp_DIR "${CMAKE_CURRENT_BINARY_DIR}" CACHE PATH "" FORCE)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/reflectcppConfig.cmake" "")

# sqlite3 (System Dependency)
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

# projects
add_subdirectory(Core)
add_subdirectory(DAL)
add_subdirectory(Startup)
