
#include "sqlgen/if_not_exists.hpp"
#include <gtest/gtest.h>
#include <sqlgen/sqlite/connect.hpp>
#include <filesystem>
#include <sqlgen.hpp>
#include <vector>

namespace fs = std::filesystem;

struct Person {
    sqlgen::PrimaryKey<uint32_t> id;
    std::string first_name;
    std::string last_name;
};

struct Relationship {
    sqlgen::ForeignKey<uint32_t, Person, "id"> parent_id;
    uint32_t child_id;
};

class SqlgenExampleTest: public::testing::Test{
protected:
    std::string dbPath = "relationalDB.db";

    void SetUp() override 
    {
        if (fs::exists(dbPath))
        {
            fs::remove(dbPath);
        }
    }
};

TEST_F(SqlgenExampleTest, Test1)
{
    auto conn = sqlgen::sqlite::connect(dbPath);
    auto people = std::vector<Person>
    {
        Person{.id = 1, .first_name = "test1", .last_name = "test11"}, Person{.id = 2, .first_name = "test2", .last_name= "test22"}
    };

    auto relationships = std::vector<Relationship>
    {
        Relationship{.parent_id = 1, .child_id = 3}, Relationship{.parent_id = 2, .child_id = 4}
    };

    conn.and_then(sqlgen::create_table<Person> | sqlgen::if_not_exists)
        .and_then(sqlgen::create_table<Relationship> | sqlgen::if_not_exists)
        .and_then(sqlgen::insert(std::ref(people)))
        .and_then(sqlgen::insert(std::ref(relationships)));
}
