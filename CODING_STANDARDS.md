# Coding Standards & Engineering Guidelines

This document defines the coding, module, API, database-integrity, and build-system standards for **CodeFirstCppDB_sqlgen**. All contributions—whether core ORM internals, database drivers, sample entities, or tests—should follow these conventions to keep the library clean, warning-free, and straightforward for external C++26 consumers to adopt.

---

## 1. Core Engineering Principles

1. **Standard C++26 First (`import std;` & `<meta>`)**:
   - Prefer C++26 static reflection (`<meta>`, `std::meta::info`, `template for`) and concepts over preprocessor macros, runtime RTTI hacks, or external code-generation scripts.
   - Use `import std;` in module implementation/interface units after the Global Module Fragment (GMF).
2. **Global Module Fragment (GMF) Driver Isolation**:
   - Keep third-party header includes (`<sqlite3.h>`, `<sqlgen/...>`, `<gtest/gtest.h>`) strictly inside the Global Module Fragment (`module; ... export module ...;`) of `.ixx` or `.cpp` translation units.
   - Never leak macro-heavy or C-library headers across public `export` module boundaries or force external consumers to include raw driver headers in their domain modules.
3. **Strict Library vs. Sample/Test Separation**:
   - Entity-agnostic infrastructure (`core`, `dal`) must never import, mention, or compile sample domain entities (`Person`, `Something`, `Person_Something`) or sample repositories.
   - Sample models, DTOs, sample repositories, executable drivers, and test binaries belong in dedicated sample/test targets so downstream consumers do not compile or link unneeded symbols.
4. **Self-Documenting Code**:
   - Prefer clear type names, concepts, and structured return types over redundant comments explaining *what* a line does.
   - Document *why* non-obvious invariants, workarounds, or SQLite/C++26 compiler constraints exist (including concise docstrings on public classes and functions).

---

## 2. Formatting & Layout

| Rule | Standard |
| :--- | :--- |
| **Indentation** | **4 spaces** per indent level; no tabs anywhere (`core/`, `DAL/`, `Startup/`, `CMakeLists.txt`). |
| **Line Length** | Target **100–120 columns** max; wrap long constraint clauses, template parameter lists, and function signatures cleanly. |
| **Brace Style** | **Allman style** (opening brace on its own line) for namespaces, types, functions, and control blocks, consistent with existing `.ixx` / `.cpp` files. |
| **Pointer & Reference Alignment** | **Left-aligned** next to the type (`const DbError& error`, `sqlite3* handle`, `T* ptr`). |
| **Namespace Layout** | Do not indent top-level namespace contents excessively if a module uses a single namespace; keep nested helpers focused and shallow. |
| **Early Returns** | Validate inputs and propagate errors early (`if (!res) { return std::unexpected(res.error()); }`) instead of deep nested conditionals. |
| **Uniform Variant Handling** | Handle schema/column/binding variants via reflection loops, concept-constrained helpers, or mapping tables instead of duplicated per-entity or per-type `if`/`else if` chains. |

---

## 3. Naming Conventions & Reflection Field Matching

Static reflection in `CodeFirstCppDB_sqlgen` matches C++ domain model members directly against database DTO fields. Consistent naming ensures both readability and zero-boilerplate reflection mapping.

### 3.1 C++ Identifier Conventions

- **Types, Concepts, Modules & Namespaces**:
  - Classes, structs, enums, type aliases, and concepts: `PascalCase` (e.g., `DbContext`, `DbSet`, `DbError`, `SQLiteConnection`, `ValidQueryColumn`).
  - C++26 module names: lowercase (`core`, `dal`, `samples`) with colon-separated partitions (`:db_context`, `:schema_traits`, `:sqlite_connection`).
- **Functions, Methods, & Parameters**:
  - `snake_case` across library internals (`ensure_table_created`, `save_changes`, `try_execute_tuple_read`) to harmonize with STL (`import std;`) and `sqlgen`.
- **Template Parameters**:
  - Descriptive `PascalCase` (`Entity`, `DTO`, `Row`, `ErrorT`, `MemberTag`), avoiding single-letter names outside trivial math/index utilities.
- **Constants & Enumerators**:
  - Compile-time constants / `constexpr` globals: `PascalCase` or `kPascalCase` depending on scope; avoid `ALL_CAPS` macros unless interacting with C preprocessor/CMake flags.

### 3.2 Domain Member Naming & Reflection Interop

Consumer codebases frequently use different private/public member prefixes. The ORM's compile-time name-matching traits (`MapperTraits`) normalize member names to schema column identifiers using explicit, unambiguous rules:

| Consumer Member Style | Example Domain Member | Target Schema / DTO Field | Notes |
| :--- | :--- | :--- | :--- |
| **`snake_case`** | `first_name` | `first_name` | Exact canonical match. |
| **`camelCase`** | `firstName` | `first_name` | Converted via camelCase-to-snake normalization. |
| **Trailing underscore (`_`)** | `first_name_`, `firstName_` | `first_name` | Strip trailing `_` before normalizing. |
| **Leading `m_` prefix** | `m_id`, `m_firstName`, `m_first_name` | `id`, `first_name` | Strip leading `m_` before camelCase/snake normalization when supported by `MapperTraits`. |
| **Leading `m` without underscore** | *(Avoid)* `mId` | — | **Discouraged**: ambiguous against words naturally starting with `m` (e.g., `max`, `mode`, `month`). Prefer `m_id` or `id`. |

- Keep DTO field names cleanly in `snake_case` matching SQL column names, while allowing domain models (and private encapsulated members or public aggregates) to use `snake_case`, `camelCase`, `camelCase_`, or `m_camelCase`.

---

## 4. C++26 Static Reflection & Module Guidelines

1. **Module Export Hygiene**:
   - Public module interfaces (`core.ixx`, `dal.ixx`, `samples.ixx`) must export only intentional public partitions via `export import :partition;`.
   - Internal helper structs or implementation-only utilities should remain unexported inside partition translation units.
2. **Reflection Metafunction Purity**:
   - Perform `std::meta::nonstatic_data_members_of`, member-name resolution, and schema attribute validation inside `consteval` / `constexpr` helpers whenever possible so schema mismatches fail at compile time with clear `static_assert` diagnostics.
   - Prefer `template for` over recursive template instantiations for iterating reflected members.
3. **Header / Include Discipline**:
   - Standard library headers (`<vector>`, `<string>`, `<expected>`, `<print>`) should be consumed through `import std;` rather than mixed `#include` + `import std;` inside module units, preventing GCC module ODR / ICE issues.
   - Only include standard C++ reflection/driver headers in the GMF where `import std;` does not yet expose macros or third-party declarations (e.g., `<meta>` or third-party C/C++ headers as required by compiler support).
4. **No Compiler-Warning Leakage via `PUBLIC` Flags**:
   - Never attach warning-suppression flags (`-Wno-...`) as `PUBLIC` on exported library targets; warning suppressions required solely by third-party headers or internal workarounds should be `PRIVATE` so downstream projects keep their own strict warning levels intact.

---

## 5. Error Handling, Transactions & SQLite Integrity Rules

1. **Zero Silent Failures (`std::expected` & `[[nodiscard]]`)**:
   - Public fallible operations (connect, DDL creation, queries, saves, transactions) must return `std::expected<T, DbError>` (or `std::expected<void, DbError>`) marked `[[nodiscard]]`.
   - Do not swallow SQLite error codes or drop failed statements without returning structured `DbError` context.
2. **Enforce Foreign Keys on Every Connection**:
   - SQLite disables foreign-key constraints per connection by default. Every connection open path (`connect()`) must issue `PRAGMA foreign_keys = ON;` immediately upon opening the handle so relational invariants hold across all consumers.
3. **Schema Completeness (`UNIQUE`, Indexes & Junction Integrity)**:
   - DDL generation (`ensure_table_created<DTO>`) must materialize declared single-column and composite `UNIQUE` constraints / indexes—not just `CREATE TABLE` columns—so duplicate-key and many-to-many junction invariants are enforced by the database engine itself.
4. **Transactional Savepoint & Dirty-Flush Semantics**:
   - Multi-operation `save_changes()` calls must wrap pending inserts, updates, deletes, and relationship mutations inside a database transaction or savepoint so partial failures roll back cleanly without corrupting in-memory change tracking.
   - Dirty state / tracked entity state should only be marked clean after the transaction commits.
5. **Typed Keys (`Id<T>`, `Key<...>` / Composite Keys)**:
   - Domain relationships and identity fields should leverage strong typed identifiers rather than raw untyped integers where distinct entities share schemas, preventing accidental cross-entity ID swaps.

---

## 6. CMake Packaging, Isolation & Testing

1. **Consumer-Safe Top-Level Checks (`PROJECT_IS_TOP_LEVEL`)**:
   - Guard developer-only options, executable demos (`Startup`), test suites (`DAL/tests`), sanitizers, and test dependencies (`extern/googletest`) behind `PROJECT_IS_TOP_LEVEL` or explicit opt-in options (`CODEFIRST_BUILD_TESTS`, `CODEFIRST_BUILD_SAMPLES`) defaulting to `OFF` when consumed via `FetchContent` or `add_subdirectory`.
   - Use `${CMAKE_CURRENT_SOURCE_DIR}` and `${CMAKE_CURRENT_BINARY_DIR}` rather than `${CMAKE_SOURCE_DIR}` / `${CMAKE_BINARY_DIR}` inside subproject `CMakeLists.txt` files so the library builds cleanly as a subproject.
2. **Namespaced Library Targets**:
   - Export canonical CMake alias targets (`CodeFirstCppDB::Core`, `CodeFirstCppDB::DAL`, and optional `CodeFirstCppDB::Samples`) so consumers link cleanly with `target_link_libraries(MyApp PRIVATE CodeFirstCppDB::Core CodeFirstCppDB::DAL)` without manual include paths or transitive test dependencies.
3. **Warning-Clean Compilation**:
   - Core library targets must compile with zero compiler warnings on the supported GCC 16+ C++26 toolchain.
4. **Database-Backed Unit Tests (`:memory:` SQLite)**:
   - Unit and integration tests in `DAL/tests` should exercise real SQLite behavior (`:memory:` databases) rather than pure mocks for DDL generation, foreign keys, composite keys, M2M junctions, projections, change tracking, and transaction rollback.
   - Re-run the full build and test binary (`test_DAL`) before merging changes.

---

## References & Attribution

- The formatting, naming, and structural conventions in this document used the **Open 3D Engine (O3DE)** coding standards as a reference, adapted for C++26 modules, static reflection (`<meta>`), and Code-First ORM architecture.
