### A. Context

Code-First Database tests with sqlgen, C++20/23/26, ArchLinux EOS. This is currently working for gcc 16.1 (it is using reflection). Clang has not adopted reflection in their main branch yet (it is in a separate clang branch, so I am not covering that in this project).

These are the required libs. You can probably get away with fewer than the current list, depending on your needs.

```bash
sudo pacman -S --needed base-devel cmake ninja autoconf bison flex postgresql-libs postgresql mariadb-libs mariadb duckdb sqlite
```

---
---
---

### B. Minimum steps to reproduce this project

If you want to make your own project, open a terminal in the root of your personal project (these are the external dependencies. try with 20 first, then upper if they allow it, since these are not maintained by me, I have no control over it)

```bash
mkdir extern

git submodule add https://github.com/getml/sqlgen.git extern/sqlgen
git submodule update --init --recursive

# testing if sqlgen has all it needs
cd extern/sqlgen
cmake -S . -B build -DCMAKE_CXX_STANDARD=20 -DCMAKE_BUILD_TYPE=Release
cmake --build build 

# add Google Test
cd ../..
git submodule add https://github.com/google/googletest.git extern/googletest

# add reflect-cpp
git submodule add https://github.com/getml/reflect-cpp.git extern/reflect-cpp
git submodule update --init --recursive
```


Your project CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.23)
project(SqlGenIsolationTest LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 1. Configure sqlgen (Disable what you don't need to speed up build)
set(SQLGEN_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SQLGEN_SQLITE3 ON CACHE BOOL "" FORCE) # Enable SQLite for local testing
add_subdirectory(extern/sqlgen)

# 2. Configure Google Test
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
add_subdirectory(extern/googletest)

# 3. Your Code & Tests
add_executable(unit_tests 
    src/database_logic.cpp 
    tests/test_main.cpp
)

# Add this if you run into "header not found" for your own src files
target_include_directories(unit_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# Ensure GTest headers are available to your test file
target_link_libraries(unit_tests 
    PRIVATE 
        sqlgen::sqlgen 
        GTest::gtest_main  # Using the namespaced target is safer
)
```

Once the external dependencies are handled, you can write your own code.

If you are testing this project, you can configure, build, run. Check the CMakePresets.json and the .vscode files (VSCode/Codium IDE). Working with gcc 16.1+.

---
---
---
