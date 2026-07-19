add_library(DAL INTERFACE)

add_executable(DAL_unit_tests 
    tests/test_DAL.cpp
)

target_include_directories(DAL_unit_tests 
    PRIVATE 
        ${CMAKE_CURRENT_SOURCE_DIR}/include/DAL
        ${CMAKE_SOURCE_DIR}/Core/include
        
        ${CMAKE_SOURCE_DIR}/extern/sqlgen/include
        ${CMAKE_SOURCE_DIR}/extern/reflect-cpp/include
)

target_link_libraries(DAL_unit_tests 
    PRIVATE 
        sqlgen::sqlgen 
        GTest::gtest_main
)