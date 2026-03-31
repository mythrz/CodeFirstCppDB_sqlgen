/// sample 1 from their docs. Working
#include <gtest/gtest.h>
#include <sqlgen/sqlite.hpp>
#include <vector>
#include <string>

struct User {
    std::string name;
    int age;
};


TEST(SqlgenSqlite, WriteAndReadUser) {
    // const auto conn = sqlgen::sqlite::connect(":memory:");
    const auto conn = sqlgen::sqlite::connect("test.db");
    ASSERT_TRUE(static_cast<bool>(conn)) << "Failed to open sqlite connection";

    auto create_res = conn.and_then(sqlgen::create_table<User> | sqlgen::if_not_exists);
    ASSERT_TRUE(create_res) << "Failed to create User table";

    const auto user = User{.name = "John", .age = 18};
    auto write_res = conn.and_then(sqlgen::write(user));
    ASSERT_TRUE(write_res) << "Failed to write user";

    auto read_res = sqlgen::read<std::vector<User>>(conn);
    ASSERT_TRUE(read_res) << "Read operation failed";
    const auto users = read_res.value();

    ASSERT_EQ(users.size(), 1u);
    EXPECT_EQ(users[0].name, "John");
    EXPECT_EQ(users[0].age, 30);
}
