add_executable(Startup main.cpp)
target_link_libraries(Startup PRIVATE Core DAL sqlgen SQLite3::SQLite3)
target_compile_features(Startup PRIVATE cxx_std_26)
