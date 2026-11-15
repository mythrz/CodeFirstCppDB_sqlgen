add_library(CodeFirstCppDB_Samples)
set_target_properties(CodeFirstCppDB_Samples PROPERTIES OUTPUT_NAME Samples)
add_library(CodeFirstCppDB::Samples ALIAS CodeFirstCppDB_Samples)
add_library(CodeFirst::Samples ALIAS CodeFirstCppDB_Samples)

target_sources(CodeFirstCppDB_Samples
    PUBLIC
        FILE_SET CXX_MODULES
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/..
        FILES
            SampleIds.ixx
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

target_link_libraries(CodeFirstCppDB_Samples
    PUBLIC
        CodeFirstCppDB::Core
        CodeFirstCppDB::DAL
)

target_compile_options(CodeFirstCppDB_Samples
    PRIVATE
        $<$<CXX_COMPILER_ID:GNU>:-Wall;-Wextra;-Wpedantic;-Wshadow>
)
