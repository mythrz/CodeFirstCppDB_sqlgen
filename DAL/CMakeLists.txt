# if(NOT DEFINED CMAKE_PROJECT_NAME OR CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
#   project(Core LANGUAGES CXX)
# endif()

add_library(DAL)

# Target-level isolation: enable std module exclusively for DAL
# set_target_properties(DAL PROPERTIES CXX_MODULE_STD ON)

target_sources(DAL
    PUBLIC
        FILE_SET CXX_MODULES FILES
            schema/PersonDTO.ixx
            repositories/SQLitePersonRepo.ixx
            dal.ixx
    PRIVATE
        repositories/SQLitePersonRepo.cpp
)

target_link_libraries(DAL 
    PUBLIC 
        Core
        reflectcpp 
        sqlgen 
        SQLite3::SQLite3 ## SQLite::SQLite3 ## SQLite3::SQLite3 ?!
)