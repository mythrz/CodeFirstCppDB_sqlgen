add_library(Core)

target_sources(Core
    PUBLIC
        FILE_SET CXX_MODULES FILES
            entities/Person.ixx
            entities/Something.ixx
            entities/Person_Something.ixx
            repositories/IPersonRepository.ixx
            core.ixx
)