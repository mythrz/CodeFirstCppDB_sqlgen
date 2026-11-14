# CodeFirstCppDB_sqlgen — Architecture, Code Review & Plug-and-Play Roadmap

> **Document Version:** 4.1 (Consolidated Master Specification & Planning Blueprint)  
> **Date:** 2026-11-14  
> **Active Branch:** `main` (at commit `ad5ac96`)  
> **Toolchain Baseline:** GCC 16.1+ (`-std=c++26 -freflection`), CMake 4.4+, Ninja, SQLite 3  
> **Scope:** Consolidates all prior project documentation (`README.md`, `README2.md`, `README3.md`, `Antigravity_opinion.md`, `ChatGPT_opinion.md`) and incorporates a fresh architectural audit of the current codebase for the next planning phase.

---

## Table of Contents

1. [Executive Summary & Project Vision](#1-executive-summary--project-vision)
2. [Quick Start & Developer Setup](#2-quick-start--developer-setup)
3. [Evolutionary Background](#3-evolutionary-background)
4. [Current Architecture (Baseline v4 on `main`)](#4-current-architecture-baseline-v4-on-main)
   - [Project Directory Structure](#project-directory-structure)
   - [Layer Boundaries & Separation of Concerns](#layer-boundaries--separation-of-concerns)
   - [Dependency Inversion & Context Flow](#dependency-inversion--context-flow)
   - [Implemented Technical Mechanisms](#implemented-technical-mechanisms)
5. [Honest Code Audit: Where We Are vs. True "Plug-and-Play"](#5-honest-code-audit-where-we-are-vs-true-plug-and-play)
   - [What Works Well Today](#what-works-well-today)
   - [Resolved Issues (Completed in Recent Sprints)](#resolved-issues-completed-in-recent-sprints)
   - [Critical Remaining Gaps & Hidden Bottlenecks](#critical-remaining-gaps--hidden-bottlenecks)
6. [Target Blueprint: "Entity Framework Core" Ergonomics in C++26](#6-target-blueprint-entity-framework-core-ergonomics-in-c26)
   - [Pillar 1: Zero-Boilerplate Entity Registration (`SchemaOf<Domain>`)](#pillar-1-zero-boilerplate-entity-registration-schemaofdomain)
   - [Pillar 2: Unified `DbSet<Domain>` for Single & Composite Keys](#pillar-2-unified-dbsetdomain-for-single--composite-keys)
   - [Pillar 3: Server-Side SQL Pushdown vs. In-Memory Filtering](#pillar-3-server-side-sql-pushdown-vs-in-memory-filtering)
   - [Pillar 4: True Unit of Work & Transactional `save_changes()`](#pillar-4-true-unit-of-work--transactional-save_changes)
   - [Pillar 5: Compile-Time `EntityMetadata<T>` & `ensure_created()`](#pillar-5-compile-time-entitymetadatat--ensure_created)
7. [Actionable Next-Steps Roadmap](#7-actionable-next-steps-roadmap)
8. [Architecture Comparison Matrix](#8-architecture-comparison-matrix)
9. [Appendix: Troubleshooting & Technical FAQ](#9-appendix-troubleshooting--technical-faq)

---

## 1. Executive Summary & Project Vision

**CodeFirstCppDB_sqlgen** is a modern C++26 data-access framework aiming to bring the **Code-First developer experience of C# and Entity Framework Core (EF Core)** to native C++.

By combining **C++26 static reflection (`std::meta`)**, **C++23 named modules (`import std;`)**, **C++20 concepts**, **monadic error handling (`std::expected`)**, and **`sqlgen`**, the project seeks to eliminate the repetitive boilerplate traditionally required in C++ database layers.

### The "Plug-and-Play" North Star
In C# EF Core, adding a persisted domain model requires only:
1. Writing a clean domain class (POCO).
2. Exposing a `DbSet<T>` property inside a `DbContext`.
3. Calling `db.Database.EnsureCreated()` (or running a migration) and using `db.Entities.Add(...)`,LINQ queries, and `db.SaveChanges()`.

Our goal is to achieve this **exact workflow in C++26 entirely at compile time**, without external code-generation scripts, preprocessor macros, or runtime reflection overhead:

```text
┌────────────────────────────────────────────────────────────────────────┐
│                        C++26 Domain Entities                           │
│     (Encapsulated classes or POCOs with SingleKey / CompositeKey)      │
└───────────────────────────────────┬────────────────────────────────────┘
                                    │
                       C++26 Static Reflection (std::meta)
                                    │
                                    ▼
┌────────────────────────────────────────────────────────────────────────┐
│               Compile-Time Relational Metadata Engine                  │
│       (Tables, Columns, Primary/Composite Keys, FKs, Nullability)       │
└───────┬───────────────────────────┼────────────────────────────┬───────┘
        │                           │                            │
        ▼                           ▼                            ▼
┌──────────────┐           ┌──────────────────┐           ┌──────────────┐
│ DDL Schema   │           │ Reflection Auto- │           │ DbContext &  │
│ Generation   │           │ Object Mapper    │           │ DbSet<T> API │
└───────┬──────┘           └────────┬─────────┘           └──────┬───────┘
        │                           │                            │
        └───────────────────────────┼────────────────────────────┘
                                    ▼
┌────────────────────────────────────────────────────────────────────────┐
│                  Database Provider Abstraction (sqlgen)                │
│                      (SQLite / PostgreSQL / DuckDB)                    │
└────────────────────────────────────────────────────────────────────────┘
```

---

## 2. Quick Start & Developer Setup

### Prerequisites & System Packages (Arch Linux / EndeavourOS)

```bash
sudo pacman -S --needed \
    base-devel \
    cmake \
    ninja \
    gdb \
    sqlite \
    postgresql-libs \
    mariadb-libs

# Verify required versions
g++ --version     # Must be GCC 16.x+ (supports -std=c++26 -freflection)
cmake --version   # Must be CMake 4.4+ (supports CMAKE_CXX_MODULE_STD)
ninja --version   # Ninja 1.11+
```

> [!NOTE]
> Standard trunk releases of Clang do not yet include native C++26 static reflection (`<meta>` / `-freflection`). **GCC 16.1+ is required** to build this project.

### Cloning, Building & Running

```bash
# 1. Clone and initialize submodules (sqlgen, reflect-cpp, googletest)
git clone <repo-url>
cd CodeFirstCppDB_sqlgen
git submodule update --init --recursive

# 2. Configure & build using CMake Presets (GCC Debug)
cmake --preset gcc-debug
cmake --build build/linux/arch/gcc/debug

# 3. Run the GoogleTest unit test suite (27 tests across 4 test suites)
./build/linux/arch/gcc/debug/DAL/tests/test_DAL
# Or via CTest:
ctest --test-dir build/linux/arch/gcc/debug --output-on-failure

# 4. Run the Startup demo application (creates and queries relationalDB.db)
./build/linux/arch/gcc/debug/Startup/Startup
# Pass --no-reset to preserve existing relationalDB.db across runs:
./build/linux/arch/gcc/debug/Startup/Startup --no-reset
```

### VS Code / VSCodium Extensions
- `kylinideteam.cppdebug` (GDB debugging)
- `kylinideteam.kylin-cmake-tools` (CMake Preset integration)
- `yy0931.vscode-sqlite3-editor` (Inspect `relationalDB.db` directly in the editor)

---

## 3. Evolutionary Background

1. **Header-to-Module Migration (`README.md` -> `README2.md` -> `README3.md`):**
   - Migrated from traditional `.h`/`.cpp` files to **C++23/26 Named Module Partitions (`.ixx`)** (`core`, `dal`) and `import std;`.
   - Quarantined third-party C/C++ headers (`<sqlgen.hpp>`, `<meta>`) inside global module fragments (`module;`).
2. **Reflection Mapper Evolution:**
   - Replaced manual per-entity field copying with a generic `MapperTraits<Domain, DTO>` powered by GCC 16's `<meta>` implementation (`std::meta::nonstatic_data_members_of`, `template for`, and unchecked splice expressions `obj.[:m:]`).
3. **Key Model & Context Evolution (Recent Sprints):**
   - Introduced `Core::SingleKey<K>` and `Core::CompositeKey<K...>`, structured error classification (`Core::DbError`), and initial `DAL::DbContextBase` / `DAL::DbSet` abstractions.

---

## 4. Current Architecture (Baseline v4 on `main`)

### Project Directory Structure

```text
CodeFirstCppDB_sqlgen/
├── CMakeLists.txt                  # Root CMake config (C++26, -freflection, import std)
├── CMakePresets.json               # Presets for gcc-debug, gcc-release, clang-*
├── readme4.md                      # This consolidated master specification
├── relationalDB.db                 # SQLite database file generated by Startup
│
├── Core/                           # Module: `core` (Pure Domain Layer — Zero DB deps)
│   ├── CMakeLists.txt
│   ├── core.ixx                    # Primary module interface (exports all partitions)
│   ├── Id.ixx                      # Strongly-typed ID wrapper: Core::Id<Tag>
│   ├── Key.ixx                     # Compile-time keys: SingleKey<T>, CompositeKey<T...>
│   ├── entities/                   # Encapsulated domain entities
│   │   ├── Person.ixx              # Key = SingleKey<PersonId>
│   │   ├── Something.ixx           # Key = SingleKey<SomethingId>
│   │   └── Person_Something.ixx    # Key = CompositeKey<PersonId, SomethingId>
│   └── repositories/               # Abstract repository contracts & error model
│       ├── DbError.ixx             # DbError, DbErrorCode & message classifier
│       ├── IGenericRepository.ixx  # IRepository<T> & IGenericRepository<T, KeyType>
│       ├── IPersonRepository.ixx
│       ├── ISomethingRepository.ixx
│       └── IPerson_SomethingRepository.ixx
│
├── DAL/                            # Module: `dal` (Data Access Layer)
│   ├── CMakeLists.txt
│   ├── dal.ixx                     # Primary module interface
│   ├── context/                    # EF Core-style Gateway & Connection Adapter
│   │   ├── DbContext.ixx           # DbContextBase<ConnectionHandle>
│   │   ├── DbSet.ixx               # DbSet<Domain, DTO, ConnectionHandle, KeyType>
│   │   └── SQLiteConnection.ixx    # Adapter wrapping sqlgen::sqlite::Connection
│   ├── schema/                     # Relational DTO structs & schema concepts
│   │   ├── SchemaTraits.ixx        # HasTableName, SinglePrimaryKey, CompositePrimaryKey
│   │   ├── PersonDTO.ixx
│   │   ├── SomethingDTO.ixx
│   │   └── Person_SomethingDTO.ixx
│   ├── mappers/                    # Compile-time Reflection Mapper
│   │   └── MapperTraits.ixx        # Bidirectional Domain <-> DTO via std::meta
│   ├── repositories/               # Concrete Repository Implementations
│   │   ├── GenericRepository.ixx
│   │   ├── SQLitePersonRepo.ixx
│   │   ├── SQLiteSomethingRepo.ixx
│   │   └── SQLitePerson_SomethingRepo.ixx
│   └── tests/                      # Unit Test Suite
│       ├── CMakeLists.txt
│       └── test_DAL.cpp            # 27 GoogleTests across 4 test suites
│
├── Startup/                        # Composition Root & Demo Executable
│   ├── CMakeLists.txt
│   └── main.cpp                    # Initializes SQLite, seeds & queries via AppDbContext
│
└── extern/                         # Git Submodules
    ├── googletest/
    ├── reflect-cpp/                # Required transitively by sqlgen
    └── sqlgen/                     # SQL generation & SQLite driver
```

### Layer Boundaries & Separation of Concerns

| Layer | Module / Namespace | Responsibilities | Architectural Rules |
| :--- | :--- | :--- | :--- |
| **Core** | `core` (`Core::*`) | Domain entities (`Person`, `Something`, `Person_Something`), strong IDs (`Id<Tag>`), key descriptors (`SingleKey`, `CompositeKey`), `DbError`, and repository interfaces. | **Zero external dependencies.** No `sqlgen`, `reflect-cpp`, or `SQLite3` headers. Immutable entity identities. |
| **DAL::Schema** | `dal` (`DAL::Schema::*`) | POD structs (`PersonDTO`, etc.) annotated with `sqlgen::PrimaryKey` and `sqlgen::ForeignKey`. | Public members for `sqlgen` serialization. Constrained by `RelationalEntity<T>`. |
| **DAL::Mappers** | `dal` (`DAL::Mappers::*`) | Compile-time `MapperTraits<Domain, DTO>` converting between private domain members and public DTO fields. | Uses C++26 `std::meta` reflection and `consteval` name matching (`snake_case` <-> `camelCase_`). |
| **DAL::Repositories** | `dal` (`DAL::Repositories::*`) | Generic and entity-specific repository implementations over a `DatabaseConnection`. | Implements `Core` repository contracts and maps driver errors to `Core::DbError`. |
| **DAL::Context** | `dal` (`DAL::*`) | `DbContextBase`, `DbSet`, and `SQLiteConnection` providing an EF Core-style façade. | Coordinates entity sets over a shared connection handle. |
| **Startup** | Executable (`main.cpp`) | Defines `AppDbContext`, connects to SQLite, creates tables, seeds domain models, and runs sample queries. | Interacts with domain entities through `AppDbContext` and repository interfaces. |

### Dependency Inversion & Context Flow

```mermaid
flowchart TD
    subgraph Startup_Layer ["Startup (main.cpp)"]
        Main["main()"]
        AppCtx["AppDbContext"]
        Main --> AppCtx
    end

    subgraph Core_Layer ["Core Module (Pure Domain)"]
        Entities["Person / Something / Person_Something"]
        Keys["Id&lt;Tag&gt; / SingleKey / CompositeKey"]
        IRepo["IRepository / IGenericRepository"]
        Err["DbError / DbErrorCode"]
        Entities --> Keys
        IRepo --> Entities
        IRepo --> Err
    end

    subgraph DAL_Layer ["DAL Module (Data Access)"]
        DbSet["DbSet&lt;Domain, DTO, Conn&gt;"]
        GenRepo["GenericRepository / SQLite*Repo"]
        Mapper["MapperTraits&lt;Domain, DTO&gt; (std::meta)"]
        DTOs["PersonDTO / SomethingDTO / Person_SomethingDTO"]
        Conn["SQLiteConnection / TestDatabaseConnection"]

        DbSet --> GenRepo
        GenRepo --> Mapper
        GenRepo --> Conn
        Mapper --> DTOs
        Mapper --> Entities
        GenRepo -.->|Implements| IRepo
    end

    AppCtx --> DbSet
    AppCtx --> Conn
```

### Implemented Technical Mechanisms

1. **Strong IDs & Compile-Time Key Types (`Core/Id.ixx`, `Core/Key.ixx`):**
   - `Core::Id<Tag>` prevents mixing IDs of different entities (`PersonId` vs `SomethingId`).
   - `Core::SingleKey<KeyType>` (`arity = 1`, `is_composite = false`) and `Core::CompositeKey<KeyTypes...>` (`arity >= 2`, `is_composite = true`) model primary keys with full `<=>` comparison, `std::formatter` support, and C++ structured bindings (`auto [pid, sid] = link.getKey();`).
2. **C++26 Static Reflection Auto-Mapper (`DAL/mappers/MapperTraits.ixx`):**
   - Reflects private domain members via `std::meta::nonstatic_data_members_of(^^Domain, std::meta::access_context::unchecked())` and DTO fields via `std::meta::access_context::current()`.
   - Matches field names at compile time (`consteval`) across exact match, trailing underscore (`id` <-> `id_`), and `snake_case` to `camelCase_` (`first_name` <-> `firstName_`).
   - Uses unchecked splice assignment (`result.[:dom_m:] = ...`) so domain classes keep private state and immutable IDs without needing `friend` declarations or setter methods.

---

## 5. Honest Code Audit: Where We Are vs. True "Plug-and-Play"

To plan the next engineering phases effectively, we must distinguish between what is **already working**, what was **recently fixed**, and what **hidden architectural bottlenecks** still remain in the code.

### What Works Well Today
- **Clean Module Compilation:** Builds cleanly with GCC 16.1+ using C++26 `-freflection` and CMake 4.4 `import std;`.
- **Zero-Setter Reflection Mapping:** Domain entities (`Person`, `Something`, `Person_Something`) have immutable primary keys (no `setId`), yet `MapperTraits` populates them effortlessly via C++26 member splicing.
- **Fast Unit Test Suite:** 27 unit tests in `test_DAL.cpp` verify round-trip reflection mapping, optional fields, CRUD operations, `DbError` classification, `SingleKey`/`CompositeKey` semantics, and `DbSet` usage.

### Resolved Issues (Completed in Recent Sprints)
- [x] **Entity ID Immutability:** Removed `setId`, `setPersonId`, and `setSomethingId` from domain entities.
- [x] **Key Type Abstractions:** Implemented `SingleKey<K>` and `CompositeKey<K...>` with concepts (`SingleKeyType`, `CompositeKeyType`, `Key`) and structured bindings.
- [x] **Base Repository Split:** Split `IRepository<T>` (common operations) from `IGenericRepository<T, KeyType>` (single-key operations) so junction tables are not forced to implement `get_by_id(int32_t)`.
- [x] **Error Message Classification:** Added `DbError::map_from_message` and factory methods to classify `DuplicateKey`, `ConstraintViolation`, `NotFound`, and `ConnectionError`.
- [x] **Initial `DbContext` / `DbSet` Façade:** Created `DbContextBase`, `DbSet`, and `SQLiteConnection`, removing direct `MapperTraits` calls from `Startup/main.cpp`.

---

### Critical Remaining Gaps & Hidden Bottlenecks

A close inspection of the current implementation reveals **7 important architectural gaps** that prevent the library from being truly reusable and plug-and-play like EF Core:

#### Gap 1: Full-Table Scans in `SQLiteConnection` & `DbSet` (Performance & Scalability Flaw)
- **What the code currently does:**
  - In [SQLiteConnection.ixx](file:///home/d5/Developer09/CodeFirstCppDB_sqlgen/DAL/context/SQLiteConnection.ixx), `get_by_id(id)` calls `fetch_all<DTO>()`—which executes `SELECT * FROM Table`, loads the **entire table** into a `std::vector<DTO>`, and loops over it in C++!
  - `count<DTO>()` calls `fetch_all<DTO>().size()`, pulling every row from SQLite just to count them!
  - `SQLitePerson_SomethingRepo` calls `fetch_all<Person_SomethingDTO>()` for `get_by_composite_id`, `find_by_person_id`, and `find_by_something_id`.
  - `DbSet::where(pred)` calls `to_vector()` and filters in C++ memory (equivalent to LINQ-to-Objects `IEnumerable`), rather than pushing a `WHERE` clause to SQLite.
- **What needs to change:**
  - Push `get_by_id`, `count`, `exists`, and composite-key queries down to `sqlgen` SQL queries (`sqlgen::select_from<DTO> | sqlgen::where(...)`) so SQLite uses its B-tree indexes in $O(\log N)$ time instead of $O(N)$ full-table deserialization.

#### Gap 2: Raw String SQL Formatting in `SQLiteConnection` Delete Operations
- **What the code currently does:**
  - `SQLiteConnection::delete_by_id` constructs raw SQL via `std::format("DELETE FROM {} WHERE id = {};", DTO::tablename, id);`.
  - `SQLiteConnection::delete_by_composite_id` hardcodes the table and column names: `"DELETE FROM Person_Something WHERE person_id = {} AND something_id = {};"`.
- **What needs to change:**
  - Use `sqlgen::delete_from<DTO> | sqlgen::where(...)` or reflection-driven column names so delete operations are type-safe, parameterized, and work for *any* entity or composite-key table, not just `Person_Something`.

#### Gap 3: Hardcoded Field Names in `CompositePrimaryKey` Concept
- **What the code currently does:**
  - In [SchemaTraits.ixx](file:///home/d5/Developer09/CodeFirstCppDB_sqlgen/DAL/schema/SchemaTraits.ixx):
    ```cpp
    template<typename T>
    concept CompositePrimaryKey = !SinglePrimaryKey<T> && requires(T dto) {
        { dto.person_id.value() } -> std::convertible_to<std::int32_t>;
        { dto.something_id.value() } -> std::convertible_to<std::int32_t>;
    };
    ```
  - This concept is hardcoded to `person_id` and `something_id`! If a user creates an `Order_ProductDTO` with `order_id` and `product_id`, it will fail `CompositePrimaryKey` and `RelationalEntity`.
- **What needs to change:**
  - Generalize `CompositePrimaryKey<T>` using C++26 reflection (`std::meta`) or a declarative `using KeyColumns = ...` / `CompositeKey` trait so *any* multi-key DTO satisfies the concept.

#### Gap 4: `DbSet` Does Not Yet Support Composite-Key Entities or Auto-Mapped DTOs
- **What the code currently does:**
  - In `Startup/main.cpp`, `AppDbContext` has `DbSet`s for `people` and `somethings`, but `Person_Something` had to be managed via a separate `SQLitePerson_SomethingRepo linkRepo` outside `AppDbContext` because `DbSet` assumes a single `KeyType = std::int32_t`!
  - Furthermore, declaring a `DbSet` requires repeating the DTO and Connection type:
    `DAL::DbSet<Core::Person, DAL::Schema::PersonDTO, DAL::SQLiteConnection> people;`
- **What needs to change:**
  - Make `DbSet` inspect `Domain::Key` (supporting both `SingleKey<K>` and `CompositeKey<K...>`) so `db.personSomethings.find(CompositeKey{...})` or `db.personSomethings.find(PersonId{1}, SomethingId{1})` works directly inside `AppDbContext`!
  - Provide a `SchemaOf<Domain>` trait (or default reflection DTO generator) so users only write `DbSet<Core::Person> people;`.

#### Gap 5: `DbContextBase::save_changes()` is Currently a No-Op
- **What the code currently does:**
  - `db.people.add(p)` executes `INSERT` immediately against SQLite.
  - `db.save_changes()` in [DbContext.ixx](file:///home/d5/Developer09/CodeFirstCppDB_sqlgen/DAL/context/DbContext.ixx) only checks `if (!is_connected()) return error; return {};`.
  - If inserting 3 related entities fails on the 3rd entity, the first 2 remain permanently written to the database.
- **What needs to change:**
  - Implement true transaction boundaries (`begin_transaction()`, `commit()`, `rollback()`) on `SQLiteConnection` (wrapping `sqlgen`'s transaction support) and either stage changes in `DbContext` or provide an explicit `db.transaction([&]{ ... })` / unit-of-work commit pipeline.

#### Gap 6: Read Operations Still Mask Database Errors
- **What the code currently does:**
  - `SQLiteConnection::fetch_all<DTO>()` ignores `res.error()` and returns `{}` when a query fails (e.g. missing table, locked DB, corrupt schema).
  - `IRepository::get_all()` and `IGenericRepository::get_by_id()` return `std::vector<T>` and `std::optional<T>` instead of `std::expected<..., DbError>`.
- **What needs to change:**
  - Propagate `std::expected<std::vector<Domain>, DbError>` and `std::expected<std::optional<Domain>, DbError>` (or provide try-read methods alongside convenience accessors) so database errors are never silently mistaken for empty tables.

#### Gap 7: Dual Boilerplate (Manual DTO + Manual Repo per Entity)
- **What the code currently does:**
  - Even with `GenericRepository` and `DbSet`, adding a new entity still requires defining a separate `DTO` struct in `DAL/schema/` and exporting partitions in multiple CMake/module files.
- **What needs to change:**
  - Allow users to either:
    1. Use a single reflected entity struct directly when strict Core/DAL separation isn't needed, **or**
    2. Use a lightweight `RegisterEntity<Domain, DTO>` trait so `DbContext` wires everything with zero bespoke repository classes.

---

## 6. Target Blueprint: "Entity Framework Core" Ergonomics in C++26

Here is the concrete engineering design to resolve the 7 gaps above and achieve a truly plug-and-play C++26 ORM.

### Pillar 1: Zero-Boilerplate Entity Registration (`SchemaOf<Domain>`)

By mapping a `Domain` type to its `DTO` (or defaulting to `Domain` itself when a user uses simple POCO structs), we eliminate repetitive template arguments:

```cpp
// Default: An entity can act as its own schema struct if desired (pure POCO mode)
template<typename Domain>
struct SchemaOf {
    using type = Domain;
};

// Optional Clean-Architecture specialization: keeps Core 100% free of sqlgen types
template<> struct SchemaOf<Core::Person> { using type = DAL::Schema::PersonDTO; };
template<> struct SchemaOf<Core::Something> { using type = DAL::Schema::SomethingDTO; };
template<> struct SchemaOf<Core::Person_Something> { using type = DAL::Schema::Person_SomethingDTO; };

template<typename Domain>
using SchemaOf_t = typename SchemaOf<Domain>::type;
```

### Pillar 2: Unified `DbSet<Domain>` for Single & Composite Keys

Using `typename Domain::Key` (`SingleKey<K>` vs. `CompositeKey<K1, K2, ...>`) and `if constexpr`, a single `DbSet<Domain, Conn>` template can seamlessly support both regular entities and junction tables:

```cpp
template<typename Domain, typename ConnectionHandle, typename DTO = SchemaOf_t<Domain>>
class DbSet {
public:
    using KeyType = typename Domain::Key;

    // Works for SingleKey (e.g. PersonId) AND CompositeKey (e.g. CompositeKey<PersonId, SomethingId>)
    [[nodiscard]] std::expected<std::optional<Domain>, Core::DbError> find(const KeyType& key) const;

    // Convenience overload for unpacking CompositeKey arguments directly:
    // e.g. db.personSomethings.find(PersonId{1}, SomethingId{2})
    template<typename... Args>
        requires (Core::CompositeKeyType<KeyType> && sizeof...(Args) == KeyType::arity)
    [[nodiscard]] std::expected<std::optional<Domain>, Core::DbError> find(Args&&... keyParts) const {
        return find(KeyType{ std::forward<Args>(keyParts)... });
    }

    std::expected<void, Core::DbError> add(const Domain& entity);
    std::expected<void, Core::DbError> update(const Domain& entity);
    std::expected<void, Core::DbError> remove(const KeyType& key);
};
```

With this design, `AppDbContext` becomes completely uniform:

```cpp
class AppDbContext : public DAL::DbContextBase<DAL::SQLiteConnection> {
public:
    Set<Core::Person> people{this};
    Set<Core::Something> somethings{this};
    Set<Core::Person_Something> personSomethings{this}; // First-class citizen!

    using DbContextBase::DbContextBase;
};
```

### Pillar 3: Server-Side SQL Pushdown vs. In-Memory Filtering

Instead of `fetch_all<DTO>()` + C++ loops, `SQLiteConnection` should use `sqlgen` query builders combined with C++26 reflection:
- **`count<DTO>()`:** Execute `SELECT COUNT(*) FROM <tablename>` or `sqlgen` aggregation.
- **`get_by_id<DTO>(id)`:** Execute `sqlgen::select_from<DTO>() | sqlgen::where("id"_c == id) | sqlgen::to<std::optional<DTO>>`.
- **`get_by_key<DTO>(CompositeKey)`:** Use C++26 `template for` over the reflected key columns of `DTO` to build the `WHERE` condition dynamically without hardcoding `person_id` or `something_id`!
- **`delete_by_key<DTO>(Key)`:** Use `sqlgen::delete_from<DTO> | sqlgen::where(...)`.

### Pillar 4: True Unit of Work & Transactional `save_changes()`

`DbContextBase` should support both **immediate execution within an explicit transaction** and **staged Unit-of-Work execution**:

```mermaid
sequenceDiagram
    participant App as Application Code
    participant Ctx as AppDbContext
    participant Conn as SQLiteConnection
    participant SQLite as SQLite Engine

    App->>Ctx: db.ensure_created()
    Ctx->>Conn: CREATE TABLE IF NOT EXISTS (Topological FK Order)
    App->>Ctx: db.people.add(alice) (Staged or in Transaction)
    App->>Ctx: db.somethings.add(laptop)
    App->>Ctx: db.personSomethings.add(link)
    App->>Ctx: db.save_changes()
    Ctx->>Conn: begin_transaction()
    Conn->>SQLite: BEGIN TRANSACTION;
    Ctx->>Conn: Flush staged inserts / updates / deletes
    alt All operations succeed
        Ctx->>Conn: commit()
        Conn->>SQLite: COMMIT;
        Ctx-->>App: std::expected<size_t, DbError>(3 rows affected)
    else Any operation fails (e.g. FK or Unique constraint)
        Ctx->>Conn: rollback()
        Conn->>SQLite: ROLLBACK;
        Ctx-->>App: std::unexpected(DbError::constraint_violation(...))
    end
```

### Pillar 5: Compile-Time `EntityMetadata<T>` & `ensure_created()`

When `AppDbContext` registers its entity sets (`DbContextBase<Conn, Person, Something, Person_Something>`), `db.ensure_created()` can iterate over a template parameter pack of entities at compile time:
```cpp
template<typename... Entities>
std::expected<void, Core::DbError> ensure_created() {
    // 1. Enable foreign keys: PRAGMA foreign_keys = ON;
    // 2. For each entity in topological order, run sqlgen::create_table<SchemaOf_t<Entities>> | sqlgen::if_not_exists
}
```
This eliminates the manual `rawConnRes.and_then(sqlgen::create_table<...>...)` boilerplate in `main.cpp`!

---

## 7. Actionable Next-Steps Roadmap

```mermaid
flowchart LR
    P1["Phase 1 & 2 (Done)<br/>Strong IDs, Single/CompositeKey,<br/>DbError, Initial DbSet"] --> P3["Phase 3 (Next)<br/>Generalize CompositeKey Concept<br/>& SchemaOf&lt;Domain&gt; Trait"]
    P3 --> P4["Phase 4<br/>Unified DbSet&lt;Domain&gt;<br/>Supporting CompositeKey &amp; Monadic Reads"]
    P4 --> P5["Phase 5<br/>SQL Pushdown in SQLiteConnection<br/>(Eliminate fetch_all Scans)"]
    P5 --> P6["Phase 6<br/>Real Transactions, Staged Unit of Work<br/>&amp; db.ensure_created()"]
```

### Phase 1 & Phase 2: Foundations & Key Model *(Completed)*
- [x] **Task 1.1–1.5:** Clean up CMake target links, strengthen `DatabaseConnection` concepts (`ReadableConnection`, `WritableConnection`), implement `DbError::map_from_message`, enforce immutable entity IDs, and remove raw mapper loops from `Startup/main.cpp`.
- [x] **Task 2.1–2.4:** Implement `SingleKey<KeyType>` and `CompositeKey<KeyTypes...>` in `Core/Key.ixx` with structured bindings, concepts, and unit tests; split `IRepository` from `IGenericRepository`; implement `SQLitePerson_SomethingRepo`.

### Phase 3: Reflection-Driven Schema Concepts & Zero-Boilerplate Traits *(Completed)*
- [x] **Task 3.1: Reflection-Based `SinglePrimaryKey<T>` & `CompositePrimaryKey<T>` (`DAL/schema/SchemaTraits.ixx`)**
  - **Problem:** `SinglePrimaryKey<T>` currently hardcodes `dto.id.value()`, and `CompositePrimaryKey<T>` hardcodes `dto.person_id.value()` and `dto.something_id.value()`. Any custom entity (e.g., `Order_ProductDTO`) with different key column names fails `RelationalEntity<T>`.
  - **Design:** Use C++26 static reflection (`#include <meta>`, `std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::current())`) to inspect field types at compile time:
    - `SqlgenPrimaryKeyField<F>`: detects `sqlgen::PrimaryKey<V, auto_incr>` via `typename F::ReflectionType`, `F::auto_incr`, and `f.value()`.
    - `SqlgenForeignKeyField<F>`: detects `sqlgen::ForeignKey<V, Table, Col>` via `typename F::ReflectionType`, `typename F::ForeignTableType`, `typename F::ColumnType`, and `f.value()`.
    - `consteval std::size_t primary_key_count<T>()` and `consteval std::size_t foreign_key_count<T>()` count key fields across all non-static data members of `T`.
    - `SinglePrimaryKey<T>`: `HasTableName<T> && (primary_key_count<T>() == 1)`.
    - `CompositePrimaryKey<T>`: `HasTableName<T> && !SinglePrimaryKey<T> && ((primary_key_count<T>() >= 2) || (primary_key_count<T>() == 0 && foreign_key_count<T>() >= 2))`.
- [x] **Task 3.2: Zero-Boilerplate `SchemaOf<Domain>` Trait (`DAL/schema/SchemaTraits.ixx` & `*DTO.ixx`)**
  - **Design:**
    - Define `template<typename Domain> struct SchemaOf { using type = Domain; };` and `template<typename Domain> using SchemaOf_t = typename SchemaOf<Domain>::type;`.
    - Define concept `template<typename Domain> concept HasSchema = RelationalEntity<SchemaOf_t<Domain>>;`.
    - Specialize `SchemaOf<Core::Person>`, `SchemaOf<Core::Something>`, and `SchemaOf<Core::Person_Something>` in their respective DTO partitions (`PersonDTO.ixx`, `SomethingDTO.ixx`, `Person_SomethingDTO.ixx`).
- [x] **Task 3.3: Compile-Time `EntityMetadata<Domain, DTO>` (`DAL/schema/EntityMetadata.ixx`)**
  - **Design:** Add new module partition `export module dal:entity_metadata;` (exported in `DAL/dal.ixx` and registered in `DAL/CMakeLists.txt`):
    - `table_name`: `static constexpr std::string_view table_name = DTO::tablename;`
    - `KeyType`: `using KeyType = typename Domain::Key;`
    - `is_composite_key`: `static constexpr bool is_composite_key = Core::CompositeKeyType<KeyType>;`
    - `key_arity`: `static constexpr std::size_t key_arity = KeyType::arity;`
    - `column_names()`: `consteval` function returning `std::array<std::string_view, N>` of all DTO field names via `std::meta::identifier_of`.
    - `key_column_names()`: `consteval` function returning `std::array<std::string_view, key_arity>` of key column names.
    - `extract_key(const Domain&)` & `extract_dto_key(const DTO&)`: extracts `typename Domain::Key` from either a `Domain` instance or a `DTO` instance using `MapperTraits<Domain, DTO>`.
    - `matches_key(const DTO& dto, const KeyType& key)`: compares `extract_dto_key(dto) == key` at runtime.

### Phase 4: Unified `DbSet<Domain>` & Monadic Read Semantics *(Completed)*
- [x] **Task 4.1: Generalize `GenericRepository`, `DbSet`, and `DbContextBase` for Single & Composite Keys**
  - **`DAL/repositories/GenericRepository.ixx`:**
    - Support `KeyType` being a scalar (`std::int32_t`), strong ID (`Core::Id<Tag>`), `Core::SingleKey<K>`, or `Core::CompositeKey<K1, K2, ...>`.
    - Use `if constexpr (Core::CompositeKeyType<KeyType>)` inside `get_by_id`, `delete_by_id`, and `exists_by_id`:
      - Single-key path unwraps `SingleKey`/`Id` to `std::int32_t` and delegates to `conn_->get_by_id<DTO>(raw_id)` / `delete_by_id<DTO>(raw_id)` / `exists<DTO>(raw_id)`.
      - Composite-key path matches DTOs via `Schema::EntityMetadata<Domain, DTO>::matches_key(dto, key)` for reads and unpacks key components into `conn_->delete_by_composite_id(...)` for deletes.
  - **`DAL/context/DbSet.ixx` & `DAL/context/DbContext.ixx`:**
    - Default `DTO = Schema::SchemaOf_t<Domain>` and `KeyType = typename Domain::Key` in `DbSet<Domain, DTO, ConnectionHandle, KeyType>`.
    - Add `template<typename Domain, typename DTO = Schema::SchemaOf_t<Domain>, typename KeyType = typename Domain::Key> using Set = DbSet<Domain, DTO, ConnectionHandle, KeyType>;` inside `DbContextBase<ConnectionHandle>`.
    - Add constructor overload on `DbSet` accepting `DbContextBase<ConnectionHandle>*` (or `std::shared_ptr<ConnectionHandle>`) so `Set<Core::Person> people{this};` works inline in `AppDbContext`.
    - Provide ergonomic key overloads on `DbSet::find`, `DbSet::try_find`, `DbSet::remove`, and `DbSet::contains`:
      - Direct `KeyType` (`SingleKey<K>` or `CompositeKey<K...>`)
      - Unwrapped value/scalar for `SingleKeyType` (`PersonId{1}` or `1`)
      - Variadic key components for `CompositeKeyType` (`db.personSomethings.find(Core::PersonId{1}, Core::SomethingId{1})`)
  - **`Startup/main.cpp`:**
    - Upgrade `AppDbContext` to declare `Set<Core::Person> people{this};`, `Set<Core::Something> somethings{this};`, and `Set<Core::Person_Something> personSomethings{this};`, removing the standalone `linkRepo` from `main.cpp`.
- [x] **Task 4.2: Monadic Read Semantics (`std::expected<..., Core::DbError>`)**
  - **`DAL/context/SQLiteConnection.ixx` & `TestDatabaseConnection` (`DAL/tests/test_DAL.cpp`):**
    - Add `try_fetch_all<DTO>() const -> std::expected<std::vector<DTO>, Core::DbError>` and `try_get_by_id<DTO>(std::int32_t) const -> std::expected<std::optional<DTO>, Core::DbError>`.
  - **`Core/repositories/IGenericRepository.ixx` & `DAL/repositories/GenericRepository.ixx`:**
    - Add `try_get_all() const -> std::expected<std::vector<Domain>, Core::DbError>` and `try_get_by_id(KeyType id) const -> std::expected<std::optional<Domain>, Core::DbError>` while preserving `get_all()` and `get_by_id()` for backward compatibility.
  - **`DAL/context/DbSet.ixx`:**
    - Expose `try_to_vector() const -> std::expected<std::vector<Domain>, Core::DbError>` and `try_find(...) const -> std::expected<std::optional<Domain>, Core::DbError>`.
  - **`DAL/tests/test_DAL.cpp`:**
    - Add unit tests covering generic `CompositePrimaryKey` on custom DTOs, `EntityMetadata` reflection queries, `SchemaOf_t`, `Set<Core::Person_Something>` composite CRUD via `DbContext`, and `try_to_vector()` / `try_find()` error propagation when the connection is closed or fails.

### Phase 5: SQL Query Pushdown (Eliminating Full-Table Scans)
- [ ] **Task 5.1:** Refactor `SQLiteConnection::get_by_id`, `exists`, `count`, and `delete_by_id` to execute targeted `sqlgen` SQL statements (`WHERE`, `COUNT`, `DELETE`) instead of `fetch_all<DTO>()` in-memory loops and raw string formatting.
- [ ] **Task 5.2:** Implement generic reflection-driven composite key lookup and deletion in `SQLiteConnection` (`get_by_composite_key<DTO>`, `delete_by_composite_key<DTO>`).

### Phase 6: True Unit of Work (`save_changes`) & Automatic Schema (`ensure_created`)
- [ ] **Task 6.1:** Add `begin_transaction()`, `commit()`, and `rollback()` to `SQLiteConnection` and `TestDatabaseConnection`.
- [ ] **Task 6.2:** Implement staged change tracking (or transactional execution) in `DbContextBase::save_changes()` with automatic rollback on failure.
- [ ] **Task 6.3:** Implement `db.ensure_created()` on `DbContextBase<Conn, Entities...>` to automatically create all registered tables and enable `PRAGMA foreign_keys = ON;`.
- [ ] **Task 6.4:** Add SQLite integration tests in `test_DAL.cpp` using `:memory:` SQLite databases alongside the mock connection tests.

---

## 8. Architecture Comparison Matrix

| Feature / Aspect | v3 Baseline (Earlier) | Current `main` (v4.1) | Target C++26 "EF Core" (Phases 3–6) | C# Entity Framework Core |
| :--- | :--- | :--- | :--- | :--- |
| **Entity Identity** | Mutable (`setId` exposed) | Immutable (`Id<Tag>`, `SingleKey`, `CompositeKey`) | Immutable + Auto-detected via `std::meta` | POCO Properties (`[Key]` or Convention) |
| **Composite Keys** | Broken (`static_assert` failure) | `CompositeKey<K...>` + `SQLitePerson_SomethingRepo` | Generic `DbSet<T>` & reflected composite SQL | `.HasKey(x => new { x.K1, x.K2 })` |
| **DbSet Ergonomics** | None (Manual Repos only) | `DbSet<Domain, DTO, Conn>` (Single-key only) | `Set<Domain>` (Single & Composite keys) | `DbSet<TEntity>` |
| **Query Execution** | Raw `sqlgen::read` in `main` | `fetch_all()` + in-memory C++ loops | Server-side SQL `WHERE` / `COUNT` pushdown | LINQ `IQueryable` -> SQL translation |
| **Unit of Work** | None | Stub `save_changes()` (immediate writes) | Staged changes + atomic `BEGIN/COMMIT/ROLLBACK` | `ChangeTracker` + `SaveChanges()` |
| **Schema Creation** | Manual `create_table` chain | Manual `create_table` chain in `main.cpp` | `db.ensure_created()` from registered entities | `db.Database.EnsureCreated()` / Migrations |
| **Error Handling** | Hardcoded `DbErrorCode::Unknown` | Classified `DbError` on writes; silent on reads | Monadic `std::expected<T, DbError>` on reads & writes | Exceptions (`DbUpdateException`) |

---

## 9. Appendix: Troubleshooting & Technical FAQ

### Q1: Why does Clang fail to build the project?
**Answer:** GCC 16.1+ includes experimental support for ISO C++26 static reflection (`P2996`) via `-std=c++26 -freflection` and `<meta>`. Standard Clang releases do not yet support `<meta>`. Always use the `gcc-debug` or `gcc-release` presets.

### Q2: Can we remove the `extern/reflect-cpp` submodule since we use C++26 `<meta>`?
**Answer:** Not while using `sqlgen`. While **our** `Core` and `DAL` code uses native C++26 `<meta>` (`std::meta::nonstatic_data_members_of`), `sqlgen` itself is built on top of `reflect-cpp` (`rfl::Result`, `rfl::Ref`, `rfl::Variant`, and `rfl` struct serialization). Therefore, `reflect-cpp` must remain initialized as a submodule for `sqlgen` to compile, though `Core` and `DAL` do not need to reference `rfl` directly.

### Q3: Why does CMake emit a deprecation warning for `SQLite::SQLite3`?
**Answer:** `extern/sqlgen/CMakeLists.txt` links against the legacy target name `SQLite::SQLite3`. Our root `CMakeLists.txt` aliases `SQLite::SQLite3` to modern CMake's `SQLite3::SQLite3`. The warning originates inside the `sqlgen` submodule and is benign.

### Q4: How does `MapperTraits` access `private` fields of `Core::Person` without `friend`?
**Answer:** C++26 static reflection provides `std::meta::access_context::unchecked()`. When combined with splice expressions (`result.[:dom_m:] = ...`), the reflection mapper can read and write private data members at compile time without polluting domain classes with ORM-specific `friend` declarations or public setters.
