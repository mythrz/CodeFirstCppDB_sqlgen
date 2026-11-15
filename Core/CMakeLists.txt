add_library(Core)
add_library(CodeFirstCppDB::Core ALIAS Core)
add_library(CodeFirst::Core ALIAS Core)

target_sources(Core
    PUBLIC
    FILE_SET CXX_MODULES FILES
    Id.ixx
    Key.ixx
    repositories/DbError.ixx
    repositories/IGenericRepository.ixx
    core.ixx
)

target_compile_options(Core PUBLIC -freflection)
