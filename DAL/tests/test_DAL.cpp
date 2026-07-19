#include "sqlgen/if_not_exists.hpp"
#include <gtest/gtest.h>
#include <sqlgen/sqlite/connect.hpp>
#include <filesystem>
#include <sqlgen.hpp>
#include <vector>
#include "entities/Person_Something.hpp"
#include "repositories/generic/RepoGen001.hpp"
#include "repositories/PersonRepo.hpp"

namespace fs = std::filesystem;

using namespace sqlgen;
using namespace sqlgen::literals;

class SqlgenExampleTest: public::testing::Test{
protected:
    static std::string dbPath;

    static void SetUpTestSuite()
    {
        if (fs::exists(dbPath))
        {
            fs::remove(dbPath);
        }

        SqlgenExampleTest::CreateMock_DB();
    }

    static void CreateMock_DB()
    {
        auto conn = sqlgen::sqlite::connect(dbPath);
        auto people = std::vector<Person>
        {
            Person{.Id = 1, .FirstName = "test1", .LastName = "test11"}, 
            Person{.Id = 2, .FirstName = "test2", .LastName= "test22"}
        };

        auto something = std::vector<Something>
        {
            Something{.Id = 1, .Name = "Item1"}, 
            Something{.Id = 2, .Name = "Item2"}
        };

        auto personthings = std::vector<Person_Something>
        {
            Person_Something{.PersonId = 1, .SomethingId = 1}, 
            Person_Something{.PersonId = 2, .SomethingId = 2}
        };

        conn.and_then(sqlgen::create_table<Person> | sqlgen::if_not_exists)
            // .and_then(sqlgen::create_table<Relationship> | sqlgen::if_not_exists)
            .and_then(sqlgen::create_table<Something> | sqlgen::if_not_exists)
            .and_then(sqlgen::create_table<Person_Something> | sqlgen::if_not_exists)
            .and_then(sqlgen::insert(std::ref(people)))
            // .and_then(sqlgen::insert(std::ref(relationships)))
            .and_then(sqlgen::insert(std::ref(something)))
            .and_then(sqlgen::insert(std::ref(personthings)))
        ;
    }

    // void SetUp() override { // run before every UT}
};

std::string SqlgenExampleTest::dbPath = "relationalDB.db";

/// connect and query teh database
TEST_F(SqlgenExampleTest, QueryTest001)
{
    const auto conn = sqlgen::sqlite::connect(dbPath);
    
    const auto people = sqlgen::read<std::vector<Person>>(conn).value();
    
    for (const auto& u : people)
    {
        std::cout << "result: " << u.LastName << ", " << u.FirstName << " ! \n";
    }

    ASSERT_FALSE(people.empty()) << "No users returned from query";
    ASSERT_FALSE(people[0].FirstName.empty()) << "First person's first_name is empty";
    ASSERT_FALSE(people[0].LastName.empty()) << "First person's last_name is empty";
}

TEST_F(SqlgenExampleTest, SelectFrom002)
{
    const auto conn = sqlgen::sqlite::connect(dbPath);

    const auto query = select_from<Person>("FirstName"_c, "LastName"_c, "Id"_c)
    | where("Id"_c == 1);

    const auto person1 = conn
        // .and_then(query | to<std::vector<Person>>)
        .and_then(query | to<Person>)
        .value();

    ASSERT_EQ(person1.Id.value(), 1);
    ASSERT_EQ(person1.FirstName, "test1");
    ASSERT_EQ(person1.LastName, "test11");
}

TEST_F(SqlgenExampleTest, GenericRepositoryGetAll) 
{
    auto conn = sqlgen::sqlite::connect(dbPath);
    RepoGen001<Person> repo(conn);

    const auto people = repo.get_all();

    ASSERT_EQ(people.size(), 2);
    ASSERT_EQ(people[0].FirstName, "test1");
}

TEST_F(SqlgenExampleTest, CustomRepositoryFindById) 
{
    auto conn = sqlgen::sqlite::connect(dbPath);
    PersonRepo repo(conn);

    const auto person = repo.find_by_id(1);

    ASSERT_TRUE(person.has_value());
    ASSERT_EQ(person->FirstName, "test1");
    ASSERT_EQ(person->LastName, "test11");
}

TEST_F(SqlgenExampleTest, CustomRepositoryFindByLastName) 
{
    auto conn = sqlgen::sqlite::connect(dbPath);
    PersonRepo repo(conn);

    const auto people = repo.find_by_last_name("test22");

    ASSERT_EQ(people.size(), 1);
    ASSERT_EQ(people[0].Id.value(), 2);
}

TEST_F(SqlgenExampleTest, InsertOne_ShouldPersistPerson)
{
    auto conn = sqlgen::sqlite::connect(dbPath);
    PersonRepo repo(conn);

    Person p{.Id = 3, .FirstName = "test3", .LastName = "test33"};

    auto inserted = repo.insert_one(p);
    ASSERT_TRUE(inserted) << "insert_one failed";

    auto found = repo.find_by_id(3);
    ASSERT_TRUE(found.has_value()) << "Inserted row was not found";

    ASSERT_EQ(found->Id.value(), 3);
    ASSERT_EQ(found->FirstName, "test3");
    ASSERT_EQ(found->LastName, "test33");
}

TEST_F(SqlgenExampleTest, InsertThenDelete_PersistPersonToThenDelete)
{
    auto conn = sqlgen::sqlite::connect(dbPath);
    PersonRepo repo(conn);

    Person p{.Id = 4, .FirstName = "test4", .LastName = "test44"};

    auto inserted = repo.insert_one(p);
    ASSERT_TRUE(inserted) << "insert_one failed";

    auto found = repo.find_by_id(4);
    ASSERT_TRUE(found.has_value()) << "Inserted row was not found";

    ASSERT_EQ(found->Id.value(), 4);
    ASSERT_EQ(found->FirstName, "test4");
    ASSERT_EQ(found->LastName, "test44");

    auto before = repo.get_by_id(4);
    ASSERT_TRUE(before.has_value());

    // auto deleted = repo.delete_by_id(1);
    repo.delete_by_id(4);
    // ASSERT_TRUE(deleted);

    auto after = repo.get_by_id(4);
    ASSERT_FALSE(after.has_value());
}
