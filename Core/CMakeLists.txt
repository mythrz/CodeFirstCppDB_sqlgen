add_library(CodeFirstCppDB_Core ${_codefirst_lib_type})
set_target_properties(CodeFirstCppDB_Core PROPERTIES
    OUTPUT_NAME Core
    EXPORT_NAME Core
    UNITY_BUILD OFF
    CXX_SCAN_FOR_MODULES ON
    CXX_MODULE_STD ON
)
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
    PRIVATE
        repositories/DbError.cpp
)

target_compile_features(CodeFirstCppDB_Core PUBLIC cxx_std_26)

target_compile_definitions(CodeFirstCppDB_Core
    PUBLIC
        $<$<BOOL:${CODEFIRST_ENABLE_CONTRACTS}>:CODEFIRST_CONTRACTS_ENFORCED>
    PRIVATE
        $<$<AND:$<BOOL:${CODEFIRST_ENABLE_STACKTRACE}>,$<CONFIG:Debug>>:CODEFIRST_ENABLE_STACKTRACE>
)

target_compile_options(CodeFirstCppDB_Core
    PUBLIC
        $<$<CXX_COMPILER_ID:GNU>:-freflection>
        $<$<AND:$<BOOL:${CODEFIRST_EXPLICIT_EXCEPTIONS}>,$<CXX_COMPILER_ID:GNU>>:-fexceptions>
        $<$<AND:$<BOOL:${CODEFIRST_ENABLE_CONTRACTS}>,$<CXX_COMPILER_ID:GNU>>:-fcontracts>
        $<$<AND:$<NOT:$<BOOL:${CODEFIRST_ENABLE_CONTRACTS}>>,$<CXX_COMPILER_ID:GNU>>:-fcontract-evaluation-semantic=ignore>
    PRIVATE
        $<$<CXX_COMPILER_ID:GNU>:-Wall;-Wextra;-Wpedantic;-Wshadow>
)

target_link_libraries(CodeFirstCppDB_Core
    PUBLIC
        $<$<CXX_COMPILER_ID:GNU>:stdc++exp>
)
