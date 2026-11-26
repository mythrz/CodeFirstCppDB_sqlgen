add_library(CodeFirstCppDB_Samples ${_codefirst_lib_type})
set_target_properties(CodeFirstCppDB_Samples PROPERTIES
    OUTPUT_NAME Samples
    UNITY_BUILD OFF
    CXX_SCAN_FOR_MODULES ON
    CXX_MODULE_STD ON
)
add_library(CodeFirstCppDB::Samples ALIAS CodeFirstCppDB_Samples)
add_library(CodeFirst::Samples ALIAS CodeFirstCppDB_Samples)

target_sources(CodeFirstCppDB_Samples
    PUBLIC
        FILE_SET CXX_MODULES
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
        FILES
            SampleIds.ixx
            entities/Person.ixx
            entities/Something.ixx
            entities/Person_Something.ixx
            repositories/IPersonRepository.ixx
            repositories/ISomethingRepository.ixx
            repositories/IPerson_SomethingRepository.ixx
            schema/PersonDTO.ixx
            schema/SomethingDTO.ixx
            schema/Person_SomethingDTO.ixx
            repositories/SQLitePersonRepo.ixx
            repositories/SQLiteSomethingRepo.ixx
            repositories/SQLitePerson_SomethingRepo.ixx
            samples.ixx
)

if(CODEFIRST_LEGACY_MODULE_ALIASES)
    target_sources(CodeFirstCppDB_Samples
        PUBLIC
            FILE_SET CXX_MODULES
            BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
            FILES
                legacy_samples.ixx
    )
endif()

target_link_libraries(CodeFirstCppDB_Samples
    PUBLIC
        CodeFirstCppDB::Core
        CodeFirstCppDB::DAL
)

target_compile_options(CodeFirstCppDB_Samples
    PRIVATE
        $<$<CXX_COMPILER_ID:GNU>:-Wall;-Wextra;-Wpedantic;-Wshadow>
)
