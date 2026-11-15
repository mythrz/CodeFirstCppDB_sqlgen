add_library(DAL)

target_sources(DAL
    PUBLIC
        FILE_SET CXX_MODULES FILES
            schema/PersonDTO.ixx
            schema/SomethingDTO.ixx
            schema/Person_SomethingDTO.ixx
            schema/SchemaTraits.ixx
            schema/EntityMetadata.ixx
            mappers/MapperTraits.ixx
            repositories/GenericRepository.ixx
            repositories/SQLitePersonRepo.ixx
            repositories/SQLiteSomethingRepo.ixx
            repositories/SQLitePerson_SomethingRepo.ixx
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

add_subdirectory(tests)
