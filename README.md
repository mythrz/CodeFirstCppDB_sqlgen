# CodeFirstCppDB_sqlgen

A C++26 module-based, reflection-driven Code-First ORM and Unit-of-Work library built on top of `sqlgen`, `reflect-cpp`, and SQLite3.

## Requirements

- **Compiler:** **GCC 16.1+** (required for C++26 reflection `-freflection`, `std::meta::define_aggregate`, `std::meta::parameters_of`, `import std`, and C++26 modules).
- **Build System:** CMake 4.0+ and Ninja.
- **Dependencies:** SQLite3 development headers/libraries (submodules `extern/reflect-cpp`, `extern/sqlgen`, and `extern/googletest` are included).

## Quick Start

Zero-boilerplate annotated C++26 entities using P3394 annotations (`[[=DAL::Schema::table]]`, `[[=DAL::Schema::primary_key]]`, `[[=DAL::Schema::references]]`, `[[=DAL::Schema::unique]]`, `[[=DAL::Schema::column]]`):

```cpp
import std;
import core;
import dal;

struct [[=DAL::Schema::table{ std::define_static_string("Authors") }]] Author
{
    [[=DAL::Schema::primary_key{ true }]] std::int32_t id = 0;
    [[=DAL::Schema::unique{}]] std::string handle;
    std::string name;
};

struct [[=DAL::Schema::table{ std::define_static_string("Books") }]] Book
{
    [[=DAL::Schema::primary_key{ true }]] std::int32_t id = 0;
    [[=DAL::Schema::references<Author>{ std::define_static_string("id") }]] std::int32_t author_id = 0;
    [[=DAL::Schema::column{ std::define_static_string("book_title") }]] std::string title;
};

struct LibraryContext : DAL::DbContextBase<DAL::SQLiteConnection>
{
    Set<Book> books{ this };     // Automatically topologically sorted after Author
    Set<Author> authors{ this };

    explicit LibraryContext(std::shared_ptr<DAL::SQLiteConnection> c)
        : DAL::DbContextBase<DAL::SQLiteConnection>(std::move(c))
    {
    }
};

int main()
{
    auto conn = DAL::SQLiteConnection::connect("library.db");
    if (!conn)
    {
        std::println("Connection failed: {}", conn.error().to_string());
        return 1;
    }

    LibraryContext db(*conn);
    if (!db.ensure_created())
    {
        return 1;
    }

    auto author = db.authors.add_returning(
        Author{ .id = 0, .handle = "@ada", .name = "Ada Lovelace" });
    if (!author)
    {
        return 1;
    }

    auto book = db.books.add_returning(
        Book{ .id = 0, .author_id = author->id, .title = "Sketch of the Analytical Engine" });
    if (!book)
    {
        return 1;
    }

    auto found = db.books.try_find(book->id);
    return (found && found->has_value()) ? 0 : 1;
}
```

For encapsulated domain entities with strong IDs, separate DTOs, and repository interfaces/implementations, see the reference domain in [Samples/](Samples/) and [Startup/main.cpp](Startup/main.cpp).

## Consuming in Your Project

### Option 1: `FetchContent`

```cmake
cmake_minimum_required(VERSION 4.0)
project(my_app LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES ON)
set(CMAKE_CXX_MODULE_STD ON)

include(FetchContent)
FetchContent_Declare(
    CodeFirstCppDB
    GIT_REPOSITORY https://github.com/mythrz/CodeFirstCppDB_sqlgen.git
    GIT_TAG        main
)
FetchContent_MakeAvailable(CodeFirstCppDB)

target_link_libraries(my_app PRIVATE CodeFirstCppDB::Core CodeFirstCppDB::DAL)
```

### Option 2: Installed Package (`find_package`)

```bash
cmake --preset gcc-release -DCODEFIRST_INSTALL=ON -DCMAKE_INSTALL_PREFIX=/opt/codefirst
cmake --build --preset build-gcc-release
cmake --install build/linux/arch/gcc/release
```

```cmake
find_package(CodeFirstCppDB CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE CodeFirstCppDB::Core CodeFirstCppDB::DAL)
```

## Error Handling Policy

- **Primary Fallible API (`[[nodiscard]] std::expected<T, Core::DbError>`):** All connection, DDL, mutation, transaction, and `try_*` read methods return `std::expected<T, Core::DbError>` (`SQLiteConnection::connect`, `ensure_created`, `add`, `add_returning`, `update`, `remove`, `upsert`, `try_find`, `try_to_vector`, `try_first_or_default`, `try_count`, `try_any`, `try_avg`, `save_changes`, `transaction`, etc.).
- **Convenience Query Wrappers:** Unprefixed read helpers (`find`, `to_vector`, `first_or_default`, `count`, `any`) return safe defaults (`std::nullopt`, `{}`, `0`, `false`) if an underlying error occurs. Prefer `try_*` whenever errors must be inspected or propagated.
- **Debug Diagnostics:** In Debug builds (`#ifndef NDEBUG`), `Core::DbError` captures up to 8 frames via `std::stacktrace::current(...)` and formats them in `err.to_string()`.

## Identity Strategy Guide

- **Strong IDs (`Core::Id<Tag, ValueType>`):** Domain entities can use strongly-typed IDs (e.g., `Core::PersonId`) with `explicit` conversion to the underlying integer/string type.
- **Database-Generated Keys (`sqlgen::PrimaryKey<T, true>` or `[[=DAL::Schema::primary_key{ true }]]`):** Pass a placeholder ID (e.g., `0`) and use `dbSet.add_returning(entity)` so SQLite assigns the `AUTOINCREMENT` key and returns the hydrated entity with its persisted ID.
- **Application-Assigned Keys (`sqlgen::PrimaryKey<T, false>`):** Assign the key before calling `dbSet.add(entity)` and `db.save_changes()`.

## C++26 Features & Contracts

- **P2996 / P3394 Static Reflection & Annotations:** Compile-time schema synthesis (`AnnotatedDto`), topological FK sorting, constructor hydration (`MapperTraits` with P3096 `parameters_of`), and domain-to-column query pushdown (`where_equal<"fieldName">`) with zero runtime RTTI or JSON round-trips.
- **P2900 Contracts:** Pass `-DCODEFIRST_ENABLE_CONTRACTS=ON` to enable compiler-checked `-fcontracts` preconditions on `DbSet`, `DbQuery`, and `GenericRepository` invariants.
- **P2988 `std::optional<T&>`:** `DbSet::find_tracked_ref(...)` returns a non-owning `std::optional<Domain&>` directly into the change tracker.

## Thread Safety

> **Important:** `DAL::DbContextBase`, `DAL::DbSet`, and `DAL::SQLiteConnection` are **not thread-safe**. Create one `DbContext` / connection per thread or unit of work and do not share them across concurrent threads.

## Building & Testing

```bash
cmake --preset gcc-debug
cmake --build --preset build-gcc-debug -j 4
ctest --preset test-gcc-debug --output-on-failure
```

To run with AddressSanitizer + UndefinedBehaviorSanitizer:

```bash
cmake --preset gcc-asan
cmake --build --preset build-gcc-asan -j 4
ctest --preset test-gcc-asan --output-on-failure
```

## License

This project is licensed under the MIT License — see [LICENSE](LICENSE) for details.
