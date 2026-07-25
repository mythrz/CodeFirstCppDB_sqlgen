# if(NOT DEFINED CMAKE_PROJECT_NAME OR CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
#   project(Core LANGUAGES CXX)
# endif()

add_executable(Startup main.cpp)
target_link_libraries(Startup PRIVATE Core DAL)