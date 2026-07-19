# CodeFirstCppDB_sqlgen

Written and tested with the help of coding agents. It is both amazing, and scary! The notes here are meant to keep all parties up to date.

Code-First Database with sqlgen, C++20, ArchLinux EOS.

No main module yet, the tests will have the generator of the database.

## Current Project Structure

```
CodeFirstCppDB_sqlgen/
├── .gitignore
├── .gitmodules
├── CMakeLists.txt           # Root CMake configuration
├── CMakePresets.json
├── README.md                
├── README2.md            
├── relationalDB.db          # SQLite database (created in project root)
├── Core/                    # Core library (not currently built)
│   ├── CMakeLists.txt
│   ├── include/
│   │   ├── entities/
│   │   │   └── Person.h     # Core entity (class with .h/.cpp separation)
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

## Architecture Overview

### Core Module
- Contains **domain entities** as proper C++ classes (with .h/.cpp separation)
- Defines **repository interfaces** (e.g., `IPersonRepository`)
- **No dependency** on DAL or sqlgen - pure business logic
- Allows swapping the data layer in the future

### DAL Module
- Contains **DTO entities** as structs (for sqlgen mapping)
- Implements **repository interfaces** from Core
- Handles **DTO-to-Core entity mapping**
- Contains **generic repository** (`RepoGen001<T>`) for common CRUD operations
- Contains **specialized repositories** (e.g., `PersonRepo`) for custom queries

### Startup Module
- Will **link Core and DAL** together
- Handles **dependency injection** and initialization
- Core is used in production, DAL is injected at startup

## Current Goals

1. **Repository Pattern Implementation** - Successfully moved:
   - Generic `RepoGen001<T>` class to `DAL/include/DAL/repositories/generic/RepoGen001.hpp`
   - Specialized `PersonRepo` class to `DAL/include/DAL/repositories/PersonRepo.hpp`

2. **Test Coverage** - All 7 unit tests pass:
   - QueryTest001
   - SelectFrom002
   - GenericRepositoryGetAll
   - CustomRepositoryFindById
   - CustomRepositoryFindByLastName
   - InsertOne_ShouldPersistPerson
   - InsertThenDelete_PersistPersonToThenDelete

3. **Header Organization** - Moved .hpp files to traditional `include/DAL/` location:
   - Entities in `DAL/include/DAL/entities/`
   - Repositories in `DAL/include/DAL/repositories/`

## My Opinion on the Structure

### Strengths
- **Clean separation**: Core (business logic) and DAL (data access) are properly isolated
- **Generic repository pattern**: The `RepoGen001<T>` template provides reusable CRUD operations
- **Specialized repositories**: `PersonRepo` extends the generic with domain-specific queries
- **Interface-based design**: Core defines interfaces, DAL implements them
- **DTO pattern**: Separate struct-based entities for database mapping vs class-based for business logic
- **CMake organization**: Each module has its own CMakeLists.txt
- **Traditional include structure**: Headers in `include/DAL/` following C++ conventions

### Areas for Improvement
1. **Empty folders**: `Startup/` is empty - will be populated as the project grows

## Upcoming Tasks

### Interface Implementation
- Define `IPersonRepository` in Core with all repository methods
- Have `PersonRepo` implement the interface
- Create mapping layer between DAL structs and Core classes (future)

### Startup Module
- Link Core and DAL together

## Questions for You

1. **DTO Mapping**: When you implement the Core-DAL link, will you need a mapping layer to convert between DAL structs (for sqlgen) and Core classes? Should I prepare any infrastructure for this?

2. **Repository Interface**: Should `IPersonRepository` in Core define all methods that `PersonRepo` currently implements, or just the essential CRUD operations?

3. **Startup Implementation**: For the Startup module, are you planning to use a DI container, or will it be manual wiring?

4. **Entity Expansion**: For the M:M relationship testing (Person-Something), are there any other relationship patterns you'd like to add for completeness?

5. **Build Configuration**: Should the Core module be built as a separate library that DAL links against, or will they remain independent until Startup ties them together?
