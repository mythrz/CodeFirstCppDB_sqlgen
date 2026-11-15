# CodeFirstCppDB_sqlgen

A C++26 module-based, reflection-driven Code-First ORM and Unit-of-Work library built on top of `sqlgen`, `reflect-cpp`, and SQLite3.

## Requirements

- **Compiler:** **GCC 16.1+** (required for C++26 reflection `-freflection`, `std::meta::define_aggregate`, `std::meta::parameters_of`, `import std`, and C++26 modules).
- **Build System:** CMake 3.30+ and Ninja.
- **Dependencies:** SQLite3 development headers/libraries (submodules `extern/reflect-cpp`, `extern/sqlgen`, and `extern/googletest` are included).

## Quick Start

```cpp
import std;
import core;
import dal;
import samples;

int main()
{
    DAL::Context::DbContext db;
    if (!db.open("app.db") || !db.ensure_created())
    {
        return 1;
    }

    auto inserted = db.people.add_returning(Core::Entities::Person{
        Core::Entities::PersonId{ 0 }, "Ada", "Lovelace", 36
    });
    if (!inserted || !db.save_changes())
    {
        return 1;
    }

    auto found = db.people.find(inserted->id().get());
    return found ? 0 : 1;
}
```

## Using via `FetchContent`

```cmake
include(FetchContent)
FetchContent_Declare(
    CodeFirstCppDB
    GIT_REPOSITORY https://github.com/your-org/CodeFirstCppDB_sqlgen.git
    GIT_TAG        main
)
FetchContent_MakeAvailable(CodeFirstCppDB)

target_link_libraries(my_app PRIVATE CodeFirstCppDB::Core CodeFirstCppDB::DAL)
```

## Identity Strategy Guide

- **Strong IDs (`Core::Id<Tag, ValueType>`):** Domain entities use strongly-typed IDs (e.g., `Samples::PersonId`) with `explicit` conversion to the underlying integer/string type. Call `.get()` when passing raw key values to `find(...)` or DTOs.
- **Database-Generated Keys (`sqlgen::PrimaryKey<T, true>`):** Pass a placeholder ID (e.g., `0`) and use `dbSet.add_returning(entity)` so SQLite assigns the `AUTOINCREMENT` key and returns the hydrated entity with its persisted ID.
- **Application-Assigned Keys (`sqlgen::PrimaryKey<T, false>`):** Assign the key before calling `dbSet.add(entity)` and `db.save_changes()`.

## Thread Safety

> **Important:** `DAL::Context::DbContext`, `DAL::Context::DbSet`, and `DAL::Context::SQLiteConnection` are **not thread-safe**. Create one `DbContext` instance per thread or unit of work and do not share a `DbContext` across concurrent threads.

## Building & Testing

```bash
cmake --preset gcc-debug
cmake --build --preset build-gcc-debug
ctest --preset test-gcc-debug
```

To run with AddressSanitizer + UndefinedBehaviorSanitizer:

```bash
cmake --preset gcc-asan
cmake --build --preset build-gcc-asan
ctest --preset test-gcc-asan
```
