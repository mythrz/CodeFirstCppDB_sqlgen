### Context

Code-First Database tests with sqlgen, C++20, ArchLinux EOS

Folder hierarchy of the project. The extern folder will hold the submodules sqlgen and googletest. The source code will go into src folder. No main file, the tests folder will have the generator of the database.

```
my_isolated_test/
├── .gitignore
├── CMakeLists.txt        
├── extern/
│   ├── googletest/
│   └── sqlgen/
├── src/
│   ├── database_logic.hpp
│   └── database_logic.cpp
└── tests/
    └── test_main.cpp  
```

Requirements

```bash
sudo pacman -S --needed base-devel cmake ninja autoconf bison flex postgresql-libs postgresql mariadb-libs mariadb duckdb sqlite
```

Setup the modules. Open a terminal in the root of your personal project.

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

Your tests acting as the main

```bash
mkdir tests
```

```cpp
#include <gtest/gtest.h>
#include <sqlgen/sqlite.hpp>

struct User { std::string name; int id; };

TEST(DatabaseTest, CanConnectAndWrite) 
{
    auto conn = sqlgen::sqlite::connect(":memory:"); // In-memory for pure isolation
    // Your sqlgen logic here...
    SUCCEED();
}
```

Configure, build, run (your project root)

```bash
# cmake -S . -B build -DCMAKE_CXX_STANDARD=20 -G Ninja -DCMAKE_BUILD_TYPE=Release
# cmake -S . -B build -DCMAKE_CXX_STANDARD=20 -DCMAKE_BUILD_TYPE=Release
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release

cmake --build build

./build/unit_tests
```

---

### Their suggestion

The build is working for the sqlgen in isolation. The issue comes when you try to call sqlgen from your project

```bash
sudo pacman -Syu

sudo pacman -S base-devel cmake git bison flex

git clone https://github.com/getml/sqlgen.git
cd sqlgen/

git submodule update --init

cmake -S . -B build -DCMAKE_CXX_STANDARD=20 -DCMAKE_BUILD_TYPE=Release

cmake --build build 
```

---

### Current problem

- [ ] sqlgen configures and builds correctly. The issue is when you reference sqlgen in your own project.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
CMake Error at extern/sqlgen/CMakeLists.txt:136 (find_package):
  Could not find a package configuration file provided by
  "unofficial-sqlite3" with any of the following names:

    unofficial-sqlite3Config.cmake
    unofficial-sqlite3-config.cmake

  Add the installation prefix of "unofficial-sqlite3" to CMAKE_PREFIX_PATH or
  set "unofficial-sqlite3_DIR" to a directory containing one of the above
  files.  If "unofficial-sqlite3" provides a separate development package or
  SDK, be sure it has been installed.


-- Configuring incomplete, errors occurred!
```