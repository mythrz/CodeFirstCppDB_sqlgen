cmake_minimum_required(VERSION 4.0)
if(NOT DEFINED CMAKE_EXPERIMENTAL_CXX_IMPORT_STD)
    set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "f35a9ac6-8463-4d38-8eec-5d6008153e7d")
endif()
project(CodeFirstCppDB VERSION 1.0.0 LANGUAGES CXX)

option(CODEFIRST_BUILD_TESTS "Build CodeFirstCppDB unit tests" ${PROJECT_IS_TOP_LEVEL})
option(CODEFIRST_BUILD_EXAMPLES "Build CodeFirstCppDB Startup example" ${PROJECT_IS_TOP_LEVEL})
option(CODEFIRST_INCLUDE_SAMPLES "Build CodeFirstCppDB sample entities and repositories" ${PROJECT_IS_TOP_LEVEL})
option(CODEFIRST_ENABLE_CONTRACTS "Enable C++26 pre/post contracts on invariant checks" OFF)
if(CODEFIRST_BUILD_TESTS OR CODEFIRST_BUILD_EXAMPLES)
    set(CODEFIRST_INCLUDE_SAMPLES ON)
endif()

# set(CMAKE_CXX_STANDARD 20)
# set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Enable automatic module dependency scanning across targets
set(CMAKE_CXX_SCAN_FOR_MODULES ON)
set(CMAKE_CXX_MODULE_STD ON) # build std module

# NOT using vcpkg
set(SQLGEN_USE_VCPKG OFF CACHE BOOL "" FORCE)

# reflect-cpp (Submodule)
if(NOT TARGET reflectcpp)
    add_subdirectory(extern/reflect-cpp SYSTEM)
endif()

# sqlite3 (System Dependency)
find_package(SQLite3 REQUIRED)
if(NOT TARGET SQLite::SQLite3)
    add_library(SQLite::SQLite3 INTERFACE IMPORTED)
    target_link_libraries(SQLite::SQLite3 INTERFACE SQLite3::SQLite3)
endif()

# sqlgen (submodule)
option(CODEFIRST_ENABLE_POSTGRES "Enable sqlgen PostgreSQL driver" OFF)
if(NOT TARGET sqlgen)
    if(NOT DEFINED reflectcpp_DIR OR NOT EXISTS "${reflectcpp_DIR}/reflectcppConfig.cmake")
        set(reflectcpp_DIR "${CMAKE_CURRENT_BINARY_DIR}" CACHE PATH "" FORCE)
        file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/reflectcppConfig.cmake" "")
    endif()
    if(NOT DEFINED SQLite3_DIR OR NOT EXISTS "${SQLite3_DIR}/SQLite3Config.cmake")
        set(SQLite3_DIR "${CMAKE_CURRENT_BINARY_DIR}" CACHE PATH "" FORCE)
        file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/SQLite3Config.cmake" "include_directories(\"${SQLite3_INCLUDE_DIRS}\")")
    endif()
    set(SQLGEN_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(SQLGEN_POSTGRES ${CODEFIRST_ENABLE_POSTGRES} CACHE BOOL "" FORCE)
    set(SQLGEN_MYSQL OFF CACHE BOOL "" FORCE)
    set(SQLGEN_DUCKDB OFF CACHE BOOL "" FORCE)
    set(SQLGEN_SQLITE3 ON CACHE BOOL "" FORCE)
    add_subdirectory(extern/sqlgen SYSTEM)
endif()

# googleTest (submodule)
if(CODEFIRST_BUILD_TESTS)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    if(NOT TARGET gtest)
        add_subdirectory(extern/googletest SYSTEM)
    endif()
    enable_testing()
endif()

# projects
add_subdirectory(Core)
add_subdirectory(DAL)
if(CODEFIRST_INCLUDE_SAMPLES)
    add_subdirectory(Samples)
endif()
if(CODEFIRST_BUILD_EXAMPLES)
    add_subdirectory(Startup)
endif()

option(CODEFIRST_INSTALL "Enable install() rules and CMake package config generation" OFF)
if(CODEFIRST_INSTALL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    install(
        TARGETS CodeFirstCppDB_Core CodeFirstCppDB_DAL
        EXPORT CodeFirstCppDBTargets
        FILE_SET CXX_MODULES DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/CodeFirstCppDB/modules"
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    )

    if(TARGET sqlgen)
        get_target_property(_sqlgen_imported sqlgen IMPORTED)
        if(NOT _sqlgen_imported)
            install(
                TARGETS sqlgen
                EXPORT CodeFirstCppDBTargets
                ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
                LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
            )
            install(
                DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/extern/sqlgen/include/"
                DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
            )
        endif()
    endif()

    if(TARGET reflectcpp)
        get_target_property(_rfl_imported reflectcpp IMPORTED)
        if(NOT _rfl_imported)
            target_include_directories(reflectcpp PUBLIC "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/rfl/thirdparty>")
            install(
                TARGETS reflectcpp
                EXPORT CodeFirstCppDBTargets
                ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
                LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
            )
            install(
                DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/extern/reflect-cpp/include/"
                DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
            )
        endif()
    endif()

    install(
        EXPORT CodeFirstCppDBTargets
        FILE CodeFirstCppDBTargets.cmake
        NAMESPACE CodeFirstCppDB::
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/CodeFirstCppDB"
    )

    configure_package_config_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CodeFirstCppDBConfig.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/CodeFirstCppDBConfig.cmake"
        INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/CodeFirstCppDB"
    )
    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/CodeFirstCppDBConfigVersion.cmake"
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
    )
    install(
        FILES
            "${CMAKE_CURRENT_BINARY_DIR}/CodeFirstCppDBConfig.cmake"
            "${CMAKE_CURRENT_BINARY_DIR}/CodeFirstCppDBConfigVersion.cmake"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/CodeFirstCppDB"
    )
endif()
