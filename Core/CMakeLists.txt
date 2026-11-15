add_library(CodeFirstCppDB_Core)
set_target_properties(CodeFirstCppDB_Core PROPERTIES OUTPUT_NAME Core)
add_library(CodeFirstCppDB::Core ALIAS CodeFirstCppDB_Core)
add_library(CodeFirst::Core ALIAS CodeFirstCppDB_Core)

target_sources(CodeFirstCppDB_Core
    PUBLIC
    FILE_SET CXX_MODULES FILES
    Id.ixx
    Key.ixx
    repositories/DbError.ixx
    repositories/IGenericRepository.ixx
    core.ixx
)

target_compile_options(CodeFirstCppDB_Core
    PUBLIC
        $<$<CXX_COMPILER_ID:GNU>:-freflection>
        $<$<AND:$<BOOL:${CODEFIRST_ENABLE_CONTRACTS}>,$<CXX_COMPILER_ID:GNU>>:-fcontracts>
    PRIVATE
        $<$<CXX_COMPILER_ID:GNU>:-Wall;-Wextra;-Wpedantic;-Wshadow>
)
