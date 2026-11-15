add_library(Samples)
add_library(CodeFirstCppDB::Samples ALIAS Samples)
add_library(CodeFirst::Samples ALIAS Samples)

target_sources(Samples
    PUBLIC
        FILE_SET CXX_MODULES
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/..
        FILES
            ../Core/entities/Person.ixx
            ../Core/entities/Something.ixx
            ../Core/entities/Person_Something.ixx
            ../Core/repositories/IPersonRepository.ixx
            ../Core/repositories/ISomethingRepository.ixx
            ../Core/repositories/IPerson_SomethingRepository.ixx
            ../DAL/schema/PersonDTO.ixx
            ../DAL/schema/SomethingDTO.ixx
            ../DAL/schema/Person_SomethingDTO.ixx
            ../DAL/repositories/SQLitePersonRepo.ixx
            ../DAL/repositories/SQLiteSomethingRepo.ixx
            ../DAL/repositories/SQLitePerson_SomethingRepo.ixx
            samples.ixx
)

target_link_libraries(Samples
    PUBLIC
        Core
        DAL
)
