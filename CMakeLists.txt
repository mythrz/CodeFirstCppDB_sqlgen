cmake_minimum_required(VERSION 4.5)
project(CodeFirstCppDB VERSION 1.0.0 LANGUAGES CXX)

option(CODEFIRST_BUILD_TESTS "Build CodeFirstCppDB unit tests" ${PROJECT_IS_TOP_LEVEL})
option(CODEFIRST_BUILD_EXAMPLES "Build CodeFirstCppDB Startup example" ${PROJECT_IS_TOP_LEVEL})
option(CODEFIRST_INCLUDE_SAMPLES "Build CodeFirstCppDB sample entities and repositories" ${PROJECT_IS_TOP_LEVEL})
option(CODEFIRST_ENABLE_CONTRACTS "Enable C++26 pre/post contracts on invariant checks" OFF)
option(CODEFIRST_BUILD_SHARED "Build CodeFirstCppDB libraries and bundled dependencies as shared libraries" OFF)
option(CODEFIRST_ENABLE_STACKTRACE "Enable Debug std::stacktrace capture in Core::DbError" ON)
option(CODEFIRST_INSTALL "Enable install() rules and CMake package config generation" OFF)
option(CODEFIRST_LEGACY_MODULE_ALIASES "Export deprecated 'core'/'dal'/'samples' module aliases re-exporting 'codefirst.*'" ON)

set(_codefirst_parent_disables_exceptions OFF)
get_directory_property(_codefirst_dir_compile_options COMPILE_OPTIONS)
string(JOIN " " _codefirst_inherited_flags
    "${CMAKE_CXX_FLAGS}"
    "${CMAKE_CXX_FLAGS_DEBUG}"
    "${CMAKE_CXX_FLAGS_RELEASE}"
    "${CMAKE_CXX_FLAGS_RELWITHDEBINFO}"
    "${CMAKE_CXX_FLAGS_MINSIZEREL}"
    "${CMAKE_CXX_FLAGS_PROFILE}"
    "${_codefirst_dir_compile_options}"
)
if(_codefirst_inherited_flags MATCHES "(^|[ ;:,])-fno-exceptions($|[ ;:,>])")
    set(_codefirst_parent_disables_exceptions ON)
endif()
option(
    CODEFIRST_EXPLICIT_EXCEPTIONS
    "Export -fexceptions PUBLIC on CodeFirstCppDB_Core (auto-enabled when parent sets -fno-exceptions)"
    ${_codefirst_parent_disables_exceptions}
)

set(CODEFIRST_SQLITE_TARGET "" CACHE STRING "Optional existing CMake SQLite target to use instead of find_package(SQLite3)")
if(CODEFIRST_BUILD_TESTS OR CODEFIRST_BUILD_EXAMPLES)
    set(CODEFIRST_INCLUDE_SAMPLES ON)
endif()

if(CODEFIRST_BUILD_SHARED)
    set(_codefirst_lib_type SHARED)
else()
    set(_codefirst_lib_type STATIC)
endif()

# set(CMAKE_CXX_STANDARD 20)
# set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# NOT using vcpkg
set(SQLGEN_USE_VCPKG OFF CACHE BOOL "" FORCE)

# reflect-cpp (Submodule)
if(NOT TARGET reflectcpp)
    if(NOT DEFINED REFLECTCPP_BUILD_SHARED)
        set(REFLECTCPP_BUILD_SHARED ${CODEFIRST_BUILD_SHARED} CACHE BOOL "Build reflect-cpp as shared library")
    endif()
    add_subdirectory(extern/reflect-cpp SYSTEM)
    if(TARGET reflectcpp)
        set_target_properties(reflectcpp PROPERTIES CXX_SCAN_FOR_MODULES OFF)
        target_compile_options(reflectcpp
            PRIVATE
                $<$<CXX_COMPILER_ID:GNU>:-fexceptions>
                $<$<AND:$<NOT:$<BOOL:${PROJECT_IS_TOP_LEVEL}>>,$<CXX_COMPILER_ID:GNU>>:-Wno-error>
        )
    endif()
endif()

# sqlite3 (Injected Target or System Dependency)
function(_codefirst_resolve_sqlite_provider out_var target_name)
    set(_cur "${target_name}")
    foreach(_step RANGE 0 4)
        if(NOT TARGET "${_cur}")
            break()
        endif()
        get_target_property(_aliased "${_cur}" ALIASED_TARGET)
        if(_aliased)
            set(_cur "${_aliased}")
            continue()
        endif()
        get_target_property(_ttype "${_cur}" TYPE)
        if("${_ttype}" STREQUAL "INTERFACE_LIBRARY")
            get_target_property(_iface_libs "${_cur}" INTERFACE_LINK_LIBRARIES)
            if(_iface_libs)
                list(LENGTH _iface_libs _lib_count)
                if(_lib_count EQUAL 1 AND TARGET "${_iface_libs}")
                    set(_cur "${_iface_libs}")
                    continue()
                endif()
            endif()
        endif()
        break()
    endforeach()
    set(${out_var} "${_cur}" PARENT_SCOPE)
endfunction()

if(CODEFIRST_SQLITE_TARGET)
    if(CODEFIRST_INSTALL)
        message(SEND_ERROR "CODEFIRST_INSTALL=ON cannot be combined with CODEFIRST_SQLITE_TARGET (external non-exported SQLite target).")
    endif()
    if(NOT TARGET "${CODEFIRST_SQLITE_TARGET}")
        message(FATAL_ERROR "CODEFIRST_SQLITE_TARGET '${CODEFIRST_SQLITE_TARGET}' is not a defined CMake target.")
    endif()
    _codefirst_resolve_sqlite_provider(_cf_expected_provider "${CODEFIRST_SQLITE_TARGET}")
    foreach(_cf_sqlite_alias IN ITEMS SQLite3::SQLite3 SQLite::SQLite3)
        if(TARGET "${_cf_sqlite_alias}")
            _codefirst_resolve_sqlite_provider(_cf_alias_provider "${_cf_sqlite_alias}")
            if(NOT "${_cf_alias_provider}" STREQUAL "${_cf_expected_provider}")
                message(FATAL_ERROR
                    "Conflicting pre-existing SQLite target '${_cf_sqlite_alias}' (resolved to '${_cf_alias_provider}') "
                    "detected while CODEFIRST_SQLITE_TARGET='${CODEFIRST_SQLITE_TARGET}' (resolved to '${_cf_expected_provider}') "
                    "is set. Remove the conflicting SQLite provider or unset CODEFIRST_SQLITE_TARGET so all targets link one SQLite implementation.")
            endif()
        else()
            add_library("${_cf_sqlite_alias}" INTERFACE IMPORTED)
            target_link_libraries("${_cf_sqlite_alias}" INTERFACE "${CODEFIRST_SQLITE_TARGET}")
        endif()
    endforeach()
else()
    find_package(SQLite3 REQUIRED)
    if(TARGET SQLite::SQLite3)
        # CMake 4.5 marks FindSQLite3's legacy SQLite::SQLite3 alias deprecated; clear the
        # deprecation noise emitted when bundled extern/sqlgen links SQLite::SQLite3.
        set_target_properties(SQLite::SQLite3 PROPERTIES DEPRECATION "")
    endif()
    if(NOT TARGET SQLite::SQLite3 AND TARGET SQLite3::SQLite3)
        add_library(SQLite::SQLite3 INTERFACE IMPORTED)
        target_link_libraries(SQLite::SQLite3 INTERFACE SQLite3::SQLite3)
    endif()
    if(NOT TARGET SQLite3::SQLite3 AND TARGET SQLite::SQLite3)
        add_library(SQLite3::SQLite3 INTERFACE IMPORTED)
        target_link_libraries(SQLite3::SQLite3 INTERFACE SQLite::SQLite3)
    endif()
endif()

# sqlgen (submodule)
option(CODEFIRST_ENABLE_POSTGRES "Enable sqlgen PostgreSQL driver" OFF)
if(NOT TARGET sqlgen)
    if(NOT DEFINED SQLGEN_BUILD_SHARED)
        set(SQLGEN_BUILD_SHARED ${CODEFIRST_BUILD_SHARED} CACHE BOOL "Build sqlgen as shared library")
    endif()
    # Keep vendored find_package() shims strictly directory-scoped (never CACHE FORCE)
    # so sibling subprojects in parent builds are never polluted.
    set(_codefirst_stub_dir "${CMAKE_CURRENT_BINARY_DIR}/_codefirst_cmake_stubs")
    file(MAKE_DIRECTORY "${_codefirst_stub_dir}")
    if(NOT DEFINED reflectcpp_DIR OR NOT EXISTS "${reflectcpp_DIR}/reflectcppConfig.cmake")
        set(reflectcpp_DIR "${_codefirst_stub_dir}")
        file(WRITE "${_codefirst_stub_dir}/reflectcppConfig.cmake" "")
    endif()
    if(NOT DEFINED SQLite3_DIR OR NOT EXISTS "${SQLite3_DIR}/SQLite3Config.cmake")
        set(SQLite3_DIR "${_codefirst_stub_dir}")
        if(CODEFIRST_SQLITE_TARGET)
            file(WRITE "${_codefirst_stub_dir}/SQLite3Config.cmake" "")
        else()
            set(_codefirst_sqlite3_stub "")
            foreach(_inc IN LISTS SQLite3_INCLUDE_DIRS)
                string(APPEND _codefirst_sqlite3_stub "include_directories([=[${_inc}]=])\n")
            endforeach()
            file(WRITE "${_codefirst_stub_dir}/SQLite3Config.cmake" "${_codefirst_sqlite3_stub}")
        endif()
    endif()
    set(SQLGEN_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(SQLGEN_POSTGRES ${CODEFIRST_ENABLE_POSTGRES} CACHE BOOL "" FORCE)
    set(SQLGEN_MYSQL OFF CACHE BOOL "" FORCE)
    set(SQLGEN_DUCKDB OFF CACHE BOOL "" FORCE)
    set(SQLGEN_SQLITE3 ON CACHE BOOL "" FORCE)
    add_subdirectory(extern/sqlgen SYSTEM)
    if(TARGET sqlgen)
        set_target_properties(sqlgen PROPERTIES CXX_SCAN_FOR_MODULES OFF)
        target_compile_options(sqlgen
            PRIVATE
                $<$<CXX_COMPILER_ID:GNU>:-fexceptions>
                $<$<AND:$<NOT:$<BOOL:${PROJECT_IS_TOP_LEVEL}>>,$<CXX_COMPILER_ID:GNU>>:-Wno-error>
        )
    endif()
endif()

# googleTest (submodule) — added before enabling module scanning so gtest/gmock
# do not scan for C++26 modules or synthesize redundant std module BMIs.
if(CODEFIRST_BUILD_TESTS)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    if(NOT TARGET gtest)
        add_subdirectory(extern/googletest SYSTEM)
    endif()
    enable_testing()
endif()

# Enable automatic module dependency scanning and import std for CodeFirstCppDB targets
set(CMAKE_CXX_SCAN_FOR_MODULES ON)
set(CMAKE_CXX_MODULE_STD ON)

# projects
add_subdirectory(Core)
add_subdirectory(DAL)
if(CODEFIRST_INCLUDE_SAMPLES)
    add_subdirectory(Samples)
endif()
if(CODEFIRST_BUILD_EXAMPLES)
    add_subdirectory(Startup)
endif()

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

    if(CODEFIRST_ENABLE_POSTGRES)
        set(CODEFIRST_POSTGRES_FIND_DEPENDENCY "find_dependency(PostgreSQL)")
    else()
        set(CODEFIRST_POSTGRES_FIND_DEPENDENCY "")
    endif()

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
    install(
        FILES
            "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/CodeFirstCppDB"
    )
    if(TARGET sqlgen)
        get_target_property(_sqlgen_imported_lic sqlgen IMPORTED)
        if(NOT _sqlgen_imported_lic AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/extern/sqlgen/LICENSE")
            install(
                FILES "${CMAKE_CURRENT_SOURCE_DIR}/extern/sqlgen/LICENSE"
                DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/CodeFirstCppDB"
                RENAME "LICENSE.sqlgen"
            )
        endif()
    endif()
    if(TARGET reflectcpp)
        get_target_property(_rfl_imported_lic reflectcpp IMPORTED)
        if(NOT _rfl_imported_lic)
            if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/extern/reflect-cpp/LICENSE")
                install(
                    FILES "${CMAKE_CURRENT_SOURCE_DIR}/extern/reflect-cpp/LICENSE"
                    DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/CodeFirstCppDB"
                    RENAME "LICENSE.reflect-cpp"
                )
            endif()
            file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/LICENSE.yyjson"
"MIT License (yyjson, bundled via reflect-cpp)

Copyright (c) 2020 YaoYuan <ibireme@gmail.com>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the \"Software\"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
")
            install(
                FILES "${CMAKE_CURRENT_BINARY_DIR}/LICENSE.yyjson"
                DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/CodeFirstCppDB"
            )
        endif()
    endif()
endif()
