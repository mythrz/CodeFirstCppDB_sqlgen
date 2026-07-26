add_library(DAL)

target_sources(DAL
    PUBLIC
        FILE_SET CXX_MODULES FILES
            schema/PersonDTO.ixx
            schema/SomethingDTO.ixx
            schema/Person_SomethingDTO.ixx
            mappers/MapperTraits.ixx
            repositories/GenericRepository.ixx
            repositories/SQLitePersonRepo.ixx
            repositories/SQLiteSomethingRepo.ixx
            repositories/SQLitePerson_SomethingRepo.ixx
            dal.ixx
    PRIVATE

)

target_link_libraries(DAL 
    PUBLIC 
        Core
        reflectcpp 
        sqlgen 
        SQLite3::SQLite3 ## SQLite::SQLite3 ## SQLite3::SQLite3 ?!
)

add_subdirectory(tests)