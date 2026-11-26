# CodeFirstCppDB_sqlgen

A C++26 module-based, reflection-driven Code-First ORM and Unit-of-Work library built on top of `sqlgen`, `reflect-cpp`, and SQLite3.

## Requirements

- **Compiler:** **GCC 16.1+** (verified on GCC 16.2+; required for C++26 reflection `-freflection`, `std::meta::define_aggregate`, `std::meta::parameters_of`, `import std`, and C++26 modules).
- **Build System:** **CMake 4.5+** and Ninja.
- **Dependencies:** SQLite3 development headers/libraries (or an injected CMake target via `-DCODEFIRST_SQLITE_TARGET=...`). Submodules `extern/reflect-cpp`, `extern/sqlgen`, and `extern/googletest` are included.

## Quick Start

Zero-boilerplate annotated C++26 entities using P3394 annotations (`[[=DAL::Schema::table]]`, `[[=DAL::Schema::primary_key]]`, `[[=DAL::Schema::references]]`, `[[=DAL::Schema::unique]]`, `[[=DAL::Schema::column]]`):

```cpp
import std;
import codefirst.core;
import codefirst.dal;

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
    // Use ":memory:" for ephemeral runs, or a file path with idempotent upsert/reopen.
    auto conn = DAL::SQLiteConnection::connect(":memory:");
    if (!conn)
    {
        std::println("Connection failed: {}", conn.error().to_string());
        return 1;
    }

    LibraryContext db(*conn);
    if (auto created = db.ensure_created(); !created)
    {
        std::println("Schema creation failed: {}", created.error().to_string());
        return 1;
    }

    auto author = db.authors.add_returning(
        Author{ .id = 0, .handle = "@ada", .name = "Ada Lovelace" });
    if (!author)
    {
        std::println("Insert author failed: {}", author.error().to_string());
        return 1;
    }

    auto book = db.books.add_returning(
        Book{ .id = 0, .author_id = author->id, .title = "Sketch of the Analytical Engine" });
    if (!book)
    {
        std::println("Insert book failed: {}", book.error().to_string());
        return 1;
    }

    auto found = db.books.try_find(book->id);
    if (!found || !found->has_value())
    {
        std::println("Lookup failed: {}", found ? "not found" : found.error().to_string());
        return 1;
    }
    std::println("Loaded book: {}", (*found)->title);
    return 0;
}
```

For encapsulated domain entities with strong IDs, separate DTOs, and repository interfaces/implementations, see the reference domain in [Samples/](Samples/) and [Startup/main.cpp](Startup/main.cpp).

## Module Names & Legacy Aliases

- **Primary module names:** `codefirst.core`, `codefirst.dal`, and (when `CODEFIRST_INCLUDE_SAMPLES=ON`) `codefirst.samples`.
- **Backward-compatibility shims:** `CODEFIRST_LEGACY_MODULE_ALIASES` defaults to `ON`, exporting thin re-export shims named `core`, `dal`, and `samples` so existing `import core; import dal;` consumers continue to compile unchanged.
- **Collision-free integration:** Pass `-DCODEFIRST_LEGACY_MODULE_ALIASES=OFF` when your host application defines its own `export module core;` or `export module dal;`.

## Consuming in Your Project

### Option 1: `FetchContent`

```cmake
cmake_minimum_required(VERSION 4.5)
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
    GIT_TAG        <pinned-commit-or-tag> # Pin a tested commit SHA or release tag
)
set(CODEFIRST_LEGACY_MODULE_ALIASES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(CodeFirstCppDB)

add_executable(my_app main.cpp)
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

### CMake Options & Dependency Contracts

| Option | Default | Notes |
| --- | --- | --- |
| `CODEFIRST_LEGACY_MODULE_ALIASES` | `ON` | Exports deprecated `core` / `dal` / `samples` shims re-exporting `codefirst.*`. Set `OFF` to avoid module-name collisions. |
| `CODEFIRST_ENABLE_STACKTRACE` | `ON` | Captures up to 8 `std::stacktrace` frames in `Core::DbError` in `Debug` builds (`libstdc++exp` is linked `PRIVATE` only when `ON` in `Debug`). Queryable at runtime via `Core::DbError::is_stacktrace_enabled()`. |
| `CODEFIRST_ENABLE_CONTRACTS` | `OFF` | Enables enforced C++26 P2900 `pre(...)` checks (`-fcontracts`, defining `CODEFIRST_CONTRACTS_ENFORCED`). When `OFF`, GCC compiles with `-fcontract-evaluation-semantic=ignore` and runtime `std::expected` guards remain active. |
| `CODEFIRST_SQLITE_TARGET` | `""` | Injects a host-provided CMake SQLite target instead of calling `find_package(SQLite3)`. Must not conflict with an unrelated pre-existing `SQLite3::SQLite3` or `SQLite::SQLite3` target. |
| `CODEFIRST_INSTALL` | `OFF` | Installs libraries, module sources, CMake package config, `LICENSE`, and third-party MIT notices (`LICENSE.sqlgen`, `LICENSE.reflect-cpp`, `LICENSE.yyjson`). |

**Host-supplied `sqlgen` / `reflectcpp`:** If a parent project defines `reflectcpp` or `sqlgen` before `add_subdirectory()`, CodeFirstCppDB reuses those targets instead of adding its bundled submodules. Ensure any host-supplied `sqlgen` target is built with SQLite3 support enabled (`SQLGEN_SQLITE3=ON`).

## Error Handling Policy

- **Primary Fallible API (`[[nodiscard]] std::expected<T, Core::DbError>`):** All connection, DDL, mutation, transaction, and `try_*` read methods return `std::expected<T, Core::DbError>` (`SQLiteConnection::connect`, `ensure_created`, `add`, `add_returning`, `update`, `remove`, `upsert`, `try_find`, `try_to_vector`, `try_first_or_default`, `try_count`, `try_any`, `try_sum`, `try_avg`, `save_changes`, `transaction`, etc.).
- **Convenience Query Wrappers:** Unprefixed read helpers (`find`, `to_vector`, `first_or_default`, `count`, `any`) return safe defaults (`std::nullopt`, `{}`, `0`, `false`) if an underlying error occurs. Prefer `try_*` whenever errors must be inspected or propagated.
- **Strict Numeric & Query Semantics:** Typed reads validate SQLite storage classes, reject trailing garbage or out-of-range narrowing, reject `std::uint64_t > INT64_MAX` writes, widen `try_sum<Col>()` to 64-bit integer / `double`, and reject `limit(0)` / `page(0)` with `ConstraintViolation` (or P2900 contract violation when `CODEFIRST_ENABLE_CONTRACTS=ON`).
- **Diagnostics & Bound Queries:** Core CRUD, `upsert`, and `INSERT ... RETURNING` use parameterized `sqlite3_stmt` bindings directly. For `WHERE` or `SET` predicates involving subnormal/high-precision or non-finite floating-point values, prefer `query_rows` / `execute_prepared` with bound `DAL::DynamicCell` parameters (as upstream `sqlgen`'s SQL literal serializer uses `std::to_string`).

## Unit of Work, Change Tracking & Handle Lifetimes

- **Failed deferred `save_changes()` semantics:** When `db.save_changes()` fails, the database transaction/savepoint rolls back, tracked-entity baselines are restored to their pre-flush snapshot, and **explicitly staged operations** (`stage_add`, `stage_update`, `stage_remove`, `stage_upsert`, or deferred `add`/`update`/`remove`/`upsert`) are **cleared** from the pending queue so a broken batch is never silently replayed on the next call. By contrast, **in-memory mutations on tracked entities** (`find_tracked` / `find_tracked_ref` / `track_copy`) remain dirty relative to their restored baseline and will be re-detected on a subsequent `save_changes()` once corrected:

```cpp
db.enable_change_tracking();
db.defer_changes(true);

auto alice = db.authors.find_tracked(1);
alice->name = "Alice (Corrected)";                 // tracked edit
(void)db.authors.add(Author{ .id = 0, .handle = "@dup", .name = "Conflict" }); // staged op

if (auto res = db.save_changes(); !res)
{
    // Transaction rolled back; staged duplicate add was cleared.
    // alice->name is still "Alice (Corrected)" vs. restored baseline, so calling
    // db.save_changes() again persists Alice's edit cleanly (or re-stage corrected ops).
    auto retry = db.save_changes();
}
```

- **Retained-handle lifetimes & eviction:**
  - `find_tracked(...)` returns `std::shared_ptr<Domain>` owned by the change tracker; `find_tracked_ref(...)` returns `std::optional<Domain&>` directly into that entry (returning `std::nullopt` once the owning `DbContext` is destroyed).
  - Explicit `update(copy)` / `upsert(copy)` with a modified separate value replaces the tracker's internal instance so `save_changes()` does not revert the explicit write; re-fetch handles via `find_tracked(...)` after explicit value overwrites, `remove(...)`, bulk `update_where` / `remove_where` (which evicts only the affected primary keys), or `ensure_deleted()` / `clear_change_tracker()`.
  - Primary-key fields of tracked entities are immutable; modifying a tracked entity's primary key fails `save_changes()` with `ConstraintViolation`.
  - Context-bound `DbSet` copies outliving their owning `DbContext` reject all mutating calls (`add`, `update`, `remove`, `upsert`, `remove_where`, `update_where`) with `ConnectionError`.

## Supported Model Shapes & Identity Strategies

- **Annotated entities (`[[=DAL::Schema::table]]`):** Zero-boilerplate domain structs synthesized into `AnnotatedDto` at compile time, supporting `[[=primary_key{ true/false }]]`, `[[=references<Parent>{ "col" }]]`, `[[=unique{}]]`, `[[=column{ "name" }]]`, composed `[[=references]]` + `[[=unique]]`, and shared `[[=primary_key{ false }]]` + `[[=references]]`. Column/table identifiers must be valid unquoted SQL identifiers (or use an explicit DTO when custom `sqlgen` types/expressions are needed).
- **Explicit DTOs & encapsulated domains:** Separate domain classes (with strong IDs `Core::Id<Tag, ValueType>` and private fields hydrated via P3096 constructor reflection) paired with a `sqlgen` DTO via `DAL::Schema::SchemaOf<Domain>`.
- **Derived `DbContext` inheritance:** `ensure_created()`, `ensure_deleted()`, `migrate_to()`, and FK navigation helpers (`query_children`, `query_parent`, `query_*_batch`, `query_many_to_many*`) recursively discover inherited `Set<...>` members across `DbContextBase` inheritance hierarchies and honor the exact parent column declared in `sqlgen::ForeignKey` / `[[=references]]`.
- **Database-generated vs. assigned keys:** Use `add_returning(entity)` (backed by `INSERT ... RETURNING`) for `AUTOINCREMENT` keys (`sqlgen::PrimaryKey<T, true>` / `[[=primary_key{ true }]]`); assign keys up front for manual or composite keys.

## Thread Safety

> **Important:** `DAL::DbContextBase`, `DAL::DbSet`, and `DAL::SQLiteConnection` are **not thread-safe**. Create one `DbContext` / connection per thread or unit of work and do not share them across concurrent threads. For multi-connection concurrency on disk, configure `SQLiteConnectionOptions{ .foreign_keys = true, .busy_timeout = ..., .wal_mode = true }`.

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

This project is licensed under the MIT License — see [LICENSE](LICENSE) for details. Bundled submodules (`sqlgen`, `reflect-cpp`, and `yyjson`) are also MIT-licensed; their notices are installed alongside `LICENSE` when `CODEFIRST_INSTALL=ON`.
