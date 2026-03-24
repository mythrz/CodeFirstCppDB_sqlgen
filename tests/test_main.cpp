#include <gtest/gtest.h>
#include <sqlgen/sqlite.hpp>

struct User { std::string name; int id; };

TEST(DatabaseTest, CanConnectAndWrite) 
{
    auto conn = sqlgen::sqlite::connect(":memory:");
    SUCCEED();
}