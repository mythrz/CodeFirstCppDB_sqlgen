add_library(CodeFirstCppDB_DAL ${_codefirst_lib_type})
set_target_properties(CodeFirstCppDB_DAL PROPERTIES
    OUTPUT_NAME DAL
    EXPORT_NAME DAL
    UNITY_BUILD OFF
    CXX_SCAN_FOR_MODULES ON
    CXX_MODULE_STD ON
)
add_library(CodeFirstCppDB::DAL ALIAS CodeFirstCppDB_DAL)
add_library(CodeFirst::DAL ALIAS CodeFirstCppDB_DAL)

target_sources(CodeFirstCppDB_DAL
    PUBLIC
        FILE_SET CXX_MODULES FILES
            schema/SchemaTraits.ixx
            schema/EntityMetadata.ixx
            mappers/MapperTraits.ixx
            repositories/GenericRepository.ixx
            context/DbSet.ixx
            context/DbContext.ixx
            context/SQLiteConnection.ixx
            dal.ixx
    PRIVATE
)

target_link_libraries(CodeFirstCppDB_DAL
    PUBLIC
        CodeFirstCppDB::Core
        sqlgen
        SQLite3::SQLite3
)

target_compile_options(CodeFirstCppDB_DAL
    PRIVATE
        $<$<CXX_COMPILER_ID:GNU>:-Wall;-Wextra;-Wpedantic;-Wshadow>
        $<$<CXX_COMPILER_ID:GNU>:-Wno-expose-global-module-tu-local>
        $<$<CXX_COMPILER_ID:GNU>:-Wno-deprecated-literal-operator>
)

if(CODEFIRST_BUILD_TESTS)
    add_subdirectory(tests)
endif()
