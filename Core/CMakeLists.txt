add_library(Core)

target_sources(Core
    PUBLIC
        FILE_SET CXX_MODULES FILES
            entities/Person.ixx
            entities/Something.ixx
            entities/Person_Something.ixx
            repositories/DbError.ixx
            repositories/IGenericRepository.ixx
            repositories/IPersonRepository.ixx
            repositories/ISomethingRepository.ixx
            repositories/IPerson_SomethingRepository.ixx
            core.ixx
)