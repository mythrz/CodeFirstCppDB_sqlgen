# CodeFirstCppDB_sqlgen

A C++26 module-based, reflection-driven Code-First ORM and Unit-of-Work library built on top of `sqlgen`, `reflect-cpp`, and SQLite3.

## Requirements

- **Compiler:** **GCC 16.1+** (verified on GCC 16.2+; required for C++26 reflection `-freflection`, `std::meta::define_aggregate`, `std::meta::parameters_of`, `import std`, and C++26 modules).
- **Build System:** **CMake 4.5+** and Ninja.
- **Dependencies:** SQLite3 development headers/libraries (or an injected CMake target via `-DCODEFIRST_SQLITE_TARGET=...`). Submodules `extern/reflect-cpp`, `extern/sqlgen`, and `extern/googletest` are included.

## Quick Start

Zero-boilerplate annotated C++26 entities using concise P3394 annotations (no `std::define_static_string` needed) and C++26 `^^Entity::field` reflection queries:

```cpp
#include <meta>

import std;
import codefirst.core;
import codefirst.dal;

struct [[=DAL::Schema::table("Authors")]] Author
{
    [[=DAL::Schema::primary_key{ true }]] std::int32_t id = 0;
    [[=DAL::Schema::unique{}]] std::string handle;
    [[=DAL::Schema::index{}]] std::string name;
    [[=DAL::Schema::version{}]] std::int64_t row_version = 1;
    [[=DAL::Schema::ignore{}]] std::string cached_display;
};

struct [[=DAL::Schema::table("Books")]] Book
{
    [[=DAL::Schema::primary_key{ true }]] std::int32_t id = 0;
    [[=DAL::Schema::references<Author>("id", DAL::Schema::FkAction::Cascade)]] std::int32_t author_id = 0;
    [[=DAL::Schema::column("book_title")]] std::string title;
    std::chrono::year_month_day published_on{};
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
    auto conn = DAL::SQLiteConnection::open_in_memory();
    if (!conn)
    {
        std::println("Connection failed: {}", conn.error());
        return 1;
    }

    LibraryContext db(*conn);
    if (auto created = db.ensure_created(); !created)
    {
        std::println("Schema creation failed: {}", created.error());
        return 1;
    }

    auto created_book = db.transaction(
        DAL::TransactionMode::Immediate,
        [&]() -> std::expected<Book, Core::DbError>
        {
            auto author = db.authors.add_returning(
                Author{ .id = 0, .handle = "@ada", .name = "Ada Lovelace" });
            if (!author)
            {
                return std::unexpected{ author.error() };
            }
            return db.books.add_returning(Book{
                .id = 0,
                .author_id = author->id,
                .title = "Sketch of the Analytical Engine",
                .published_on = std::chrono::year{ 1843 } / std::chrono::August / std::chrono::day{ 1 },
            });
        });

    if (!created_book)
    {
        std::println("Transaction failed: {}", created_book.error());
        return 1;
    }

    // Type-safe C++26 reflection queries & lazy std::generator streaming
    for (auto row : db.books.where_iequal<^^Book::title>("sketch of the analytical engine").stream())
    {
        if (row)
        {
            std::println("Loaded book: {}", row->title);
        }
    }
    return 0;
}
```

For encapsulated domain entities with strong IDs, separate DTOs, and repository interfaces/implementations, see the reference domain in [Samples/](Samples/) and [Startup/main.cpp](Startup/main.cpp).

## Usability & C++26 Highlights

- **Concise P3394 Schema Annotations:** `[[=table()]]` (defaults to the C++ class name), `[[=table("...")]]`, `[[=column("...")]]`, `[[=primary_key{ true/false }]]`, `[[=unique{}]]`, `[[=ignore{}]]`, `[[=version{}]]` (optimistic concurrency returning `Core::DbErrorCode::ConcurrencyConflict` on stale updates), `[[=index{}]]`, `[[=composite_index("name", "col1, col2")]]`, `[[=default_sql("...")]]`, `[[=check("...")]]`, nullable `std::optional<T>` + `[[=references<Parent>("id", FkAction::SetNull/Cascade/Restrict)]]`, and self-referencing trees via `[[=references_self("id", FkAction::Cascade)]]`.
- **First-Class Modern C++ STL Types:** Direct persistence and `WHERE` pushdown for `std::chrono::sys_time<Duration>` (ISO-8601 UTC `TEXT`), `std::chrono::year_month_day` (`YYYY-MM-DD`), `std::filesystem::path`, `std::array<std::byte, N>` (exact-length `BLOB`), `enum class` (stored as `std::underlying_type_t`), and `Core::Id<Tag, V>` directly inside annotated structs. Includes `std::hash` specializations for `Core::Id` / `SingleKey` / `CompositeKey`, `std::formatter` for `Core::DbError` / `Core::DbErrorCode`, and `std::ranges::input_range` overloads for batch operations (`add_range`, `add_range_returning`, `upsert_range`, `where_in_range`, `query_*_batch`).
- **Expressive Querying, Streaming & Joins:**
  - C++26 `^^Entity::field` NTTP reflection overloads alongside string-literal column names (`where_equal<^^Book::title>`, `order_by<^^Book::id>`, `try_select<^^Book::title>`, `update_where<^^Book::title>`, `group_by<^^Book::author_id>`).
  - Parenthesized boolean groups (`where_group` / `or_where_group`), case-insensitive & negated pattern predicates (`where_iequal`, `where_icontains`, `where_not_contains`, `where_not_starts_with`, `where_not_ends_with`), `try_single()` / `try_single_or_default()`, and `GROUP BY` aggregations (`group_by<Col>().try_count()` / `try_sum` / `try_avg` / `try_min` / `try_max`).
  - Bulk `remove_where()` / `update_where()` (and `remove_where_equal` / `update_where_equal`) return `std::expected<std::size_t, Core::DbError>` with the exact affected row count.
  - Typed ad-hoc SQL hydration (`db.query_as<SummaryRow>(sql, args...)` and `db.stream_as<SummaryRow>(...)`), lazy C++23/26 `std::generator` row streaming (`query.stream()`), and single-query `db.join<Child, Parent>()` / `db.left_join<Child, Parent>()`.
- **Migrations, Schema Introspection & Operations:** Declarative `db.migrate(steps)` runner, `db.generate_schema_sql()`, `db.verify_schema()` drift detection (`missing_tables`, `missing_columns`, `extra_columns`, `missing_indexes`), `SQLiteConnection::connect(std::filesystem::path, opts)`, `open_in_memory()`, `backup_to()`, `checkpoint_wal()`, `vacuum()`, `integrity_check()`, and parameterized SQL template tracing via `SQLiteConnectionOptions::on_query` / `set_query_tracer(...)`.

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
- **Strict Numeric & Query Semantics:** Typed reads validate SQLite storage classes (`std::string` rejects raw `SQLITE_BLOB`, `std::vector<std::byte>` requires `SQLITE_BLOB`), reject trailing garbage or out-of-range narrowing, reject `std::uint64_t > INT64_MAX` writes/predicates and `limit`/`offset > INT64_MAX`, widen `try_sum<Col>()` to 64-bit integer / `double`, and reject `limit(0)` / `page(0)` with `ConstraintViolation` (or P2900 contract violation when `CODEFIRST_ENABLE_CONTRACTS=ON`).
- **Parameterized Queries & Redacted Diagnostics:** All CRUD, `upsert`, `INSERT ... RETURNING`, `DbQuery` projections/aggregates, bulk `update_where`, and bulk/paginated `remove_where` statements compile into `?`-bound `sqlite3_stmt` queries (including subnormal/IEEE-754 floats and long embedded-NUL strings, with zero SQL literal interpolation of runtime values). Decode and range errors report column indices and byte lengths without echoing raw cell or key values.

## Security & Hardening

See [SECURITY.md](SECURITY.md) for the full threat model and vulnerability reporting policy.

- **Hardened `SQLiteConnectionOptions` defaults:** `SQLiteConnection::connect(...)` enables `SQLITE_DBCONFIG_DEFENSIVE` (`defensive = true`), disables `TRUSTED_SCHEMA` (`trusted_schema = false`), disables double-quoted string literals (`allow_dqs = false`), disables extension loading (`allow_load_extension = false`), blocks `ATTACH DATABASE` (`max_attached = 0`), and enables `SQLITE_OPEN_NOFOLLOW` (`no_follow = true`). Least-privilege open options are also available: `read_only = true`, `create_if_missing = false`, and (on POSIX) `owner_only_permissions = true` (`0600`).
- **Safe substring / prefix / suffix filters:** For untrusted user input, use `where_contains`, `where_starts_with`, and `where_ends_with` (plus `or_where_*` variants), which escape `%`, `_`, and `\` using `ESCAPE '\'`. Reserve `where_like` / `where_not_like` for caller-controlled SQL wildcard patterns.
- **Raw SQL trust boundary:** `execute_raw(...)` runs multi-statement SQL via `sqlite3_exec` and is intended strictly for trusted migration/DDL scripts. For ad-hoc runtime queries, use `query_rows(sql, params)` or `execute_prepared(sql, params)` with single-statement preparation and bound `DAL::DynamicCell` parameters.

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
  - `find_tracked(...)` returns `std::shared_ptr<Domain>` owned by the change tracker; prefer `find_tracked(...)` whenever a handle spans multiple statements or crosses mutating calls.
  - `find_tracked_ref(...)` returns a non-owning `std::optional<Domain&>` directly into the tracker entry (returning `std::nullopt` once the owning `DbContext` is destroyed). Any operation that replaces, evicts, or clears that tracker entry — including explicit `update(copy)` / `upsert(copy)`, `remove(...)`, bulk `update_where` / `remove_where`, `ensure_deleted()`, `clear_change_tracker()`, or `DbContext` destruction — invalidates previously obtained `Domain&` references.
  - Primary-key fields of tracked entities are immutable; modifying a tracked entity's primary key fails `save_changes()` with `ConstraintViolation`.
  - Context-bound `DbSet` copies outliving their owning `DbContext` reject all mutating calls (`add`, `update`, `remove`, `upsert`, `remove_where`, `update_where`) with `ConnectionError`.

## Supported Model Shapes & Identity Strategies

- **Annotated entities (`[[=DAL::Schema::table]]`):** Zero-boilerplate domain structs synthesized into `AnnotatedDto` at compile time, supporting `[[=primary_key{ true/false }]]`, `[[=references<Parent>{ "col" }]]`, `[[=unique{}]]`, `[[=column{ "name" }]]`, composed `[[=references]]` + `[[=unique]]`, and shared `[[=primary_key{ false }]]` + `[[=references]]`. Column/table identifiers must be valid unquoted SQL identifiers (or use an explicit DTO when custom `sqlgen` types/expressions are needed).
- **Explicit DTOs & encapsulated domains:** Separate domain classes (with strong IDs `Core::Id<Tag, ValueType>` and private fields hydrated via P3096 constructor reflection) paired with a `sqlgen` DTO via `DAL::Schema::SchemaOf<Domain>`.
- **Derived `DbContext` inheritance:** `ensure_created()`, `ensure_deleted()`, `migrate_to()`, and FK navigation helpers (`query_children`, `query_parent`, `query_*_batch`, `query_many_to_many*`) recursively discover inherited `Set<...>` members across `DbContextBase` inheritance hierarchies and honor the exact parent column declared in `sqlgen::ForeignKey` / `[[=references]]`.
- **Database-generated vs. assigned keys:** Use `add_returning(entity)` (backed by `INSERT ... RETURNING`) for `AUTOINCREMENT` keys (`sqlgen::PrimaryKey<T, true>` / `[[=primary_key{ true }]]`); assign keys up front for manual or composite keys.

## Thread Safety & Connection Pooling

> **Important:** Individual `DAL::DbContextBase`, `DAL::DbSet`, and `DAL::SQLiteConnection` instances are **not thread-safe**; never share a single `DbContext` or `SQLiteConnection` across concurrent threads.

For multi-threaded applications (HTTP servers, worker pools, background jobs), use the built-in thread-safe **`DAL::SQLiteConnectionPool`**, which enables WAL mode and busy timeouts by default, manages bounded RAII connection leases (`PooledConnection`, automatically rolling back any leaked transaction on return), and exposes `pool->with_context<MyContext>([](MyContext& db) { ... })`:

```cpp
auto pool = DAL::SQLiteConnectionPool::create("app.db", { .min_size = 2, .max_size = 8 });
auto count = (*pool)->with_context<LibraryContext>(
    [](LibraryContext& db) -> std::expected<std::size_t, Core::DbError>
    {
        return db.books.try_count();
    });
```

For manual multi-connection concurrency on disk, configure `SQLiteConnectionOptions{ .foreign_keys = true, .busy_timeout = std::chrono::milliseconds{ 5000 }, .wal_mode = true }` and use `DAL::TransactionMode::Immediate` for write transactions.

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
