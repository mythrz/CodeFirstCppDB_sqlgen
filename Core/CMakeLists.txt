add_library(Core)

# Target-level isolation: enable std module exclusively for Core
# set_target_properties(Core PROPERTIES CXX_MODULE_STD ON)

target_sources(Core
    PUBLIC
        FILE_SET CXX_MODULES FILES
            entities/Person.ixx
            entities/Something.ixx
            entities/Person_Something.ixx
            repositories/IPersonRepository.ixx
            core.ixx
)