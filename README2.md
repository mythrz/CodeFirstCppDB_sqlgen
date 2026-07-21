# CodeFirstCppDB_sqlgen

Written and tested with the help of coding agents, architectured by me. It is both amazing, and scary! The notes of this file are meant to keep all parties up to date.

Code-First Database with sqlgen, C++20/23, ArchLinux EOS. No main project yet, the tests will have the db-generator.

## Quick Start

```bash
# Clone and initialize submodules
git clone <repo-url>
cd CodeFirstCppDB_sqlgen
git submodule update --init --recursive

# Configure and build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Run tests
./build/DAL/DAL_unit_tests
```

---

## Architecture Overview

### Current Project Structure

```
CodeFirstCppDB_sqlgen/
├── .gitignore
├── .gitmodules
├── CMakeLists.txt           # Root CMake configuration
├── CMakePresets.json
├── README.md                # Original project documentation
├── README2.md               # This file (improved documentation)
├── relationalDB.db          # SQLite database (generated)
├── Core/                    # Core library (not currently built)
│   ├── CMakeLists.txt
│   ├── include/
│   │   ├── entities/
│   │   │   └── Person.h     # Core entity (class in Core namespace)
│   │   └── repositories/
│   │       └── IPersonRepository.hpp  # Interface for repository
│   └── source/
│       └── entities/
│           └── Person.cpp
├── DAL/                     # Data Access Layer (active)
│   ├── CMakeLists.txt
│   ├── include/
│   │   └── DAL/
│   │       ├── entities/
│   │       │   ├── Person.hpp           # DTO for sqlgen mapping
│   │       │   ├── Something.hpp        # DTO for related entity
│   │       │   └── Person_Something.hpp # Junction table DTO
│   │       └── repositories/
│   │           ├── PersonRepo.hpp       # Person repository implementation
│   │           └── generic/
│   │               └── RepoGen001.hpp   # Generic repository template
│   └── tests/
│       └── test_DAL.cpp                 # Unit tests
├── extern/                  # Git submodules
│   ├── googletest/          # Testing framework
│   ├── reflect-cpp/         # Reflection library
│   └── sqlgen/              # SQL generation library
└── Startup/                 # Module to link Core and DAL (not yet implemented)
```

### Layer Responsibilities

| Layer | Contains | Avoid |
|-------|----------|-------|
| **Core** | Domain entities (classes with .h/.cpp), repository interfaces, business logic | Dependencies on DAL or sqlgen, database-specific code, structs (use classes) |
| **DAL** | DTO entities (structs for sqlgen), repository implementations, DTO-to-Core mapping, generic/specialized repositories | Business logic, Core entity definitions |
| **Startup** | Module linking, dependency injection, initialization | Business logic, data access logic |

---

## Areas for Improvement

1. **Empty folders**: `Startup/` is empty - will be populated as the project grows
2. **Validation logic in Core entity setters**: Consider adding validation logic in the future (e.g., non-empty names, ID range checks)
3. **Startup Implementation**: For the Startup module, are you planning to use a DI container, or will it be manual wiring?
4. **C++20 Modules**: Consider migrating from traditional headers to C++20 modules for better compilation performance and encapsulation. This would involve creating module interface files (.cppm) and updating the build system.
    - (Step 1) Upgrade CMake Dependency Scanning. cmake_minimum_required(VERSION 3.28); Ensure explicit compliance flag properties are set on target environments set(CMAKE_CXX_STANDARD 20) and set(CMAKE_CXX_STANDARD_REQUIRED ON)
    - (Step 2) Instead of .h or .hpp headers, create primary module interface units to define the boundary lines of your layers. Use .ixx or .cppm extensions to denote module code.
        - Core Module Boundary: Create Core/core.ixx. Declare the module via export module core;. Use the export keyword before the namespace Core, which automatically makes your domain classes and interface classes (like IPersonRepository) visible to importing layers.
        - DAL Module Boundary: Create DAL/dal.ixx. Declare it via export module dal; and immediately pull in your core boundaries using import core;. Use export to expose your specialized and generic repositories.
    - (Step 3) Transition DTOs & Concepts into a Schema Sub-Module. Rather than placing database mappings in loose structs across files, create an isolated structural module Schema/schema.ixx.
        - Define C++20 compile-time properties using std::concepts to validate that generated database types contain necessary metadata (e.g., TableName or PrimaryKey attributes) before sqlgen reads them.
        - Import this module exclusively within your DAL layer, ensuring your domain logic layer (Core) remains perfectly untainted by database definitions.
    - (Step 4) Refactor CMakeLists.txt to use CXX_MODULES File Sets. Replace standard target_sources(target PRIVATE ...) setups with modern file-set definitions. This alerts the compiler to scan these specific files for export and import dependencies before execution.
        - Modify DAL/CMakeLists.txt and Core/CMakeLists.txt to mirror the following structure
        ```
        add_library(dal)
        target_sources(dal
            PUBLIC FILE_SET CXX_MODULES FILES
                dal.ixx
            PRIVATE
                source/dal_impl.cpp
        )
        target_link_libraries(dal PUBLIC core schema)
        ```
    - (Step 5) Isolate SQL Generation Implementation Details. Keep your module compilation boundaries tight by splitting implementation details away from the primary interface file.
        - Place raw structural template declarations and interfaces in the .ixx files.
        - Move the underlying query formatting (std::format), database drivers, and raw pointer mappings to an implementation unit file (e.g., DAL/source/dal_impl.cpp) starting with the signature module dal;.
        - Benefit: Modifying database driver internals or manual query optimizations will only trigger a fast compilation of that isolated translation unit, rather than cascading a full recompile down into your testing suites or the Startup layer.

### Steps to Incorporate External Headers inside Modules

Both sqlgen and reflect-cpp currently rely exclusively on traditional header-include mechanisms (#include <sqlgen/sqlite.hpp>, #include <rfl.hpp>). They do not supply native, pre-compiled C++20 .ixx / .cppm module binaries out of the box.

Because both libraries rely heavily on advanced compile-time template metaprogramming (such as custom user-defined string literals like "age"_c for schema mapping and structural bindings for static reflection), trying to drop them directly into a module interface export boundary will trigger compiler errors or leak internal configuration text.

The Architectural Guard Pattern: To adapt a traditional header-only ecosystem into your clean C++20 module architecture, we must use Global Module Fragments. This approach cleanly isolates the macro inclusion steps, preventing them from bleeding metadata or slowing down your clean build artifacts.

1. Wrap `sqlgen` and `reflect-cpp` within the **Global Module Fragment** block (`module;`) at the top of the interface files (`.ixx`).
2. Define structural DTOs (`PersonDTO`) within the module namespace boundary (`export namespace DAL`).
3. Keep the heavy template evaluation and data access implementation details inside an isolated implementation file (`dal.cpp`) to optimize build paths.

### For reference only to help contextualize

THESE ARE JUST AN EXAMPLE, NOT A GUIDE!!

The Global Fragmentation Interface (DAL/dal.ixx). This file uses the global fragment area (declared via module;) to safely introduce the legacy headers before declaring the module path boundary.

```
module;

// Global Module Fragment: Includes traditional headers without leaking their text macros
#include <sqlgen/sqlite.hpp>
#include <rfl.hpp>

export module dal;

export import core;
import <string>;
import <vector>;
import <memory>;
import <expected>;
import <system_error>;

export namespace DAL {

    // Code-First Structural DTO mapping to the Relational Layout
    // Matches the exact struct patterns required by reflect-cpp / sqlgen
    struct PersonDTO {
        int id;
        std::string first_name;
        std::string last_name;
    };

    // Concrete implementation wrapping the raw connection handles cleanly via RAII
    class SQLitePersonRepository : public Core::IPersonRepository {
    private:
        std::string m_dbPath;
        
        // Internal helper to establish active connections safely
        [[nodiscard]] auto connect() const {
            return sqlgen::sqlite::connect(m_dbPath);
        }

    public:
        explicit SQLitePersonRepository(std::string dbPath) 
            : m_dbPath(std::move(dbPath)) {}

        ~SQLitePersonRepository() override = default;

        // Implementation signatures fulfilling the strict Core contracts
        [[nodiscard]] std::unique_ptr<Core::Person> findById(int id) override;
        [[nodiscard]] std::vector<Core::Person> fetchAll() override;
        void persist(const Core::Person& person) override;
    };
}
```

Isolated Implementation Execution Module (DAL/source/dal.cpp). By implementing the actual runtime execution logic inside a standard translation unit, any changes to how queries execute or map won't force a full recompile across the rest of the application.

```
module dal;

// Re-inject the local macro handles inside the translation unit block safely
module;
#include <sqlgen/sqlite.hpp>
#include <rfl.hpp>

namespace DAL {

    std::unique_ptr<Core::Person> SQLitePersonRepository::findById(int id) {
        using namespace sqlgen::literals;
        auto conn = connect();

        // Safe compile-time checked filtering logic using sqlgen expressions
        auto queryResult = sqlgen::read<std::vector<PersonDTO>>(conn);
        
        if (!queryResult.has_value() || queryResult.value().empty()) {
            return nullptr;
        }

        // Search within the typed vector using standard modern ranges
        for (const auto& dto : queryResult.value()) {
            if (dto.id == id) {
                // Wide string conversion step mapping DTO variables back into clean Core aggregates
                return std::make_unique<Core::Person>(
                    dto.id,
                    std::wstring(dto.first_name.begin(), dto.first_name.end()),
                    std::wstring(dto.last_name.begin(), dto.last_name.end())
                );
            }
        }
        return nullptr;
    }

    std::vector<Core::Person> SQLitePersonRepository::fetchAll() {
        auto conn = connect();
        std::vector<Core::Person> domainEntities;

        auto queryResult = sqlgen::read<std::vector<PersonDTO>>(conn);
        if (!queryResult.has_value()) {
            return domainEntities;
        }

        for (const auto& dto : queryResult.value()) {
            domainEntities.emplace_back(
                dto.id,
                std::wstring(dto.first_name.begin(), dto.first_name.end()),
                std::wstring(dto.last_name.begin(), dto.last_name.end())
            );
        }

        return domainEntities;
    }

    void SQLitePersonRepository::persist(const Core::Person& person) {
        auto conn = connect();

        // Construct narrow string copy elements to feed into reflect-cpp serialization paths
        std::string fName(person.getFirstName().begin(), person.getFirstName().end());
        std::string lName(person.getLastName().begin(), person.getLastName().end());

        PersonDTO dto {
            .id = person.getId(),
            .first_name = fName,
            .last_name = lName
        };

        // Leverage physical RAII writing hooks from sqlgen core
        sqlgen::write(conn, dto);
    }
}
```

---