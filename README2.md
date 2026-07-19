# CodeFirstCppDB_sqlgen

Written and tested with the help of coding agents, architectured by me. It is both amazing, and scary! The notes of this file are meant to keep all parties up to date.

Code-First Database with sqlgen, C++20, ArchLinux EOS. No main project yet, the tests will have the db-generator.

---
---

## Architecture Overview

### Current Project Structure

```
CodeFirstCppDB_sqlgen/
├── .gitignore
├── .gitmodules
├── CMakeLists.txt           # Root CMake configuration
├── CMakePresets.json
├── README.md                
├── README2.md            
├── relationalDB.db          # SQLite db 
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
│   │       │   ├── Person.hpp
│   │       │   ├── Something.hpp
│   │       │   └── Person_Something.hpp
│   │       └── repositories/
│   │           ├── PersonRepo.hpp
│   │           └── generic/
│   │               └── RepoGen001.hpp
│   └── tests/
│       └── test_DAL.cpp
├── extern/                  # Git submodules
│   ├── googletest/
│   ├── reflect-cpp/
│   └── sqlgen/
└── Startup/                 # Module to link Core and DAL (not yet implemented)
```
---

### Layer Responsibilities

| Layer | Contains | Avoid |
|-------|----------|-------|
| **Core** | Domain entities (classes with .h/.cpp), repository interfaces, business logic | Dependencies on DAL or sqlgen, database-specific code, structs (use classes) |
| **DAL** | DTO entities (structs for sqlgen), repository implementations, DTO-to-Core mapping, generic/specialized repositories | Business logic, Core entity definitions |
| **Startup** | Module linking, dependency injection, initialization | Business logic, data access logic |

---

## Areas for Improvement

1. **Empty folders**: `Startup/` is empty - will be populated as the project grows
2. **C++20 Modules**: Consider migrating from traditional headers to C++20 modules for better compilation performance and encapsulation. This would involve creating module interface files (.cppm) and updating the build system.
3. **Validation logic in Core entity setters**: Consider adding validation logic in the future (e.g., non-empty names, ID range checks)
4. **Startup Implementation**: For the Startup module, are you planning to use a DI container, or will it be manual wiring?

---