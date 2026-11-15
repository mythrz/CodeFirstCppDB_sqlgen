add_library(DAL)
add_library(CodeFirstCppDB::DAL ALIAS DAL)
add_library(CodeFirst::DAL ALIAS DAL)

target_sources(DAL
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

target_link_libraries(DAL 
    PUBLIC 
        Core
        sqlgen 
        SQLite3::SQLite3
)

target_compile_options(DAL
    PUBLIC
        -Wno-expose-global-module-tu-local
        -Wno-deprecated-literal-operator
)

if(CODEFIRST_BUILD_TESTS)
    add_subdirectory(tests)
endif()
