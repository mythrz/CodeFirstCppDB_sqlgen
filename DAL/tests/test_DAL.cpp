#include <gtest/gtest.h>
// #include "../extern/sqlgen/include/sqlgen/Result.hpp"
#include "../../extern/sqlgen/include/sqlgen/Result.hpp"

import std;
import core;
import dal;
namespace
{

    // In-Memory Database Connection mock simulating a database driver satisfying DAL::Repositories::DatabaseConnection concept
    class TestDatabaseConnection
    {
    public:
        [[nodiscard]] bool is_open() const noexcept
        {
            return true;
        }

        template<typename DTO>
        std::vector<DTO>& get_storage()
        {
            if constexpr (std::is_same_v<DTO, DAL::Schema::PersonDTO>)
            {
                return persons_;
            }
            else if constexpr (std::is_same_v<DTO, DAL::Schema::SomethingDTO>)
            {
                return somethings_;
            }
            else if constexpr (std::is_same_v<DTO, DAL::Schema::Person_SomethingDTO>)
            {
                return person_somethings_;
            }
        }

        template<typename DTO>
        const std::vector<DTO>& get_storage() const
        {
            if constexpr (std::is_same_v<DTO, DAL::Schema::PersonDTO>)
            {
                return persons_;
            }
            else if constexpr (std::is_same_v<DTO, DAL::Schema::SomethingDTO>)
            {
                return somethings_;
            }
            else if constexpr (std::is_same_v<DTO, DAL::Schema::Person_SomethingDTO>)
            {
                return person_somethings_;
            }
        }

        template<typename DTO>
        std::vector<DTO> fetch_all() const
        {
            return get_storage<DTO>();
        }

        template<typename DTO>
        std::optional<DTO> get_by_id(std::int32_t id) const
        {
            const auto& storage = get_storage<DTO>();
            for (const auto& item : storage)
            {
                if constexpr (requires { item.id; })
                {
                    if (item.id == id)
                        return item;
                }
            }
            return std::nullopt;
        }

        // Updated to return sqlgen::Result<sqlgen::Nothing> preserving error information

        template<typename DTO>
        sqlgen::Result<sqlgen::Nothing> insert(const DTO& item)
        {
            get_storage<DTO>().push_back(item);
            // success -> default-constructed Result (has value)
            return sqlgen::Result<sqlgen::Nothing>{ sqlgen::Nothing{} };
        }

        template<typename DTO>
        sqlgen::Result<sqlgen::Nothing> insert_many(const std::vector<DTO>& items)
        {
            auto& storage = get_storage<DTO>();
            storage.insert(storage.end(), items.begin(), items.end());
            return sqlgen::Result<sqlgen::Nothing>{ sqlgen::Nothing{} };
        }

        template<typename DTO>
        sqlgen::Result<sqlgen::Nothing> update(const DTO& item)
        {
            auto& storage = get_storage<DTO>();
            for (auto& elem : storage)
            {
                if constexpr (requires { elem.id; })
                {
                    if (elem.id == item.id)
                    {
                        elem = item;
                        return sqlgen::Result<sqlgen::Nothing>{ sqlgen::Nothing{} };
                    }
                }
            }
            // Return an error if the item was not found
            return sqlgen::error("Update failed: item not found");
        }

        template<typename DTO>
        void delete_by_id(std::int32_t id)
        {
            auto& storage = get_storage<DTO>();
            std::erase_if(
                storage,
                [id](const DTO& item)
                {
                    if constexpr (requires { item.id; })
                    {
                        return item.id == id;
                    }
                    return false;
                });
        }

        template<typename DTO>
        bool exists(std::int32_t id) const
        {
            return get_by_id<DTO>(id).has_value();
        }

        template<typename DTO>
        std::size_t count() const
        {
            return get_storage<DTO>().size();
        }

        template<typename DTO>
        struct QueryBuilder
        {
            const TestDatabaseConnection& conn;
            std::string column;
            std::string value;

            QueryBuilder& where(std::string_view col, std::string_view val)
            {
                column = col;
                value = val;
                return *this;
            }

            std::vector<DTO> execute() const
            {
                std::vector<DTO> result;
                const auto& storage = conn.get_storage<DTO>();
                for (const auto& item : storage)
                {
                    if constexpr (std::is_same_v<DTO, DAL::Schema::PersonDTO>)
                    {
                        if (column == "last_name" && item.last_name == value)
                        {
                            result.push_back(item);
                        }
                    }
                }
                return result;
            }
        };

        template<typename DTO>
        QueryBuilder<DTO> query() const
        {
            return QueryBuilder<DTO>{ *this, "", "" };
        }

    private:
        std::vector<DAL::Schema::PersonDTO> persons_;
        std::vector<DAL::Schema::SomethingDTO> somethings_;
        std::vector<DAL::Schema::Person_SomethingDTO> person_somethings_;
    };

    class SqlgenExampleTest : public ::testing::Test
    {
    protected:
        TestDatabaseConnection conn;

        void SetUp() override
        {
            conn.insert(DAL::Schema::PersonDTO{ .id = 1, .first_name = "test1", .last_name = "test11" });
            conn.insert(DAL::Schema::PersonDTO{ .id = 2, .first_name = "test2", .last_name = "test22" });

            conn.insert(DAL::Schema::SomethingDTO{ .id = 1, .name = "Item1", .category = "Cat1", .description = "Desc1" });
            conn.insert(DAL::Schema::SomethingDTO{ .id = 2, .name = "Item2", .category = "Cat2", .description = "Desc2" });

            conn.insert(DAL::Schema::Person_SomethingDTO{ .person_id = 1, .something_id = 1, .association_type = "Owner" });
            conn.insert(DAL::Schema::Person_SomethingDTO{ .person_id = 2, .something_id = 2, .association_type = "User" });
        }
    };

    TEST_F(SqlgenExampleTest, GenericRepositoryGetAll)
    {
        DAL::Repositories::SQLitePersonRepo<TestDatabaseConnection> repo(conn);

        const auto people = repo.get_all();

        ASSERT_EQ(people.size(), 2);
        ASSERT_EQ(people[0].getId(), 1);
        ASSERT_EQ(people[0].getFirstName(), "test1");
        ASSERT_EQ(people[0].getLastName(), "test11");
        ASSERT_EQ(people[1].getId(), 2);
        ASSERT_EQ(people[1].getFirstName(), "test2");
        ASSERT_EQ(people[1].getLastName(), "test22");
    }

    TEST_F(SqlgenExampleTest, CustomRepositoryFindById)
    {
        DAL::Repositories::SQLitePersonRepo<TestDatabaseConnection> repo(conn);

        const auto person = repo.get_by_id(1);

        ASSERT_TRUE(person.has_value());
        ASSERT_EQ(person->getId(), 1);
        ASSERT_EQ(person->getFirstName(), "test1");
        ASSERT_EQ(person->getLastName(), "test11");
    }

    TEST_F(SqlgenExampleTest, CustomRepositoryFindByLastName)
    {
        DAL::Repositories::SQLitePersonRepo<TestDatabaseConnection> repo(conn);

        const auto people = repo.find_by_last_name("test22");

        ASSERT_EQ(people.size(), 1);
        ASSERT_EQ(people[0].getId(), 2);
        ASSERT_EQ(people[0].getFirstName(), "test2");
        ASSERT_EQ(people[0].getLastName(), "test22");
    }

    TEST_F(SqlgenExampleTest, InsertOne_ShouldPersistPerson)
    {
        DAL::Repositories::SQLitePersonRepo<TestDatabaseConnection> repo(conn);

        Core::Person p(3, "test3", "test33");

        // const bool inserted = repo.insert_one(p);
        // ASSERT_TRUE(inserted) << "insert_one failed";
        auto result = repo.insert_one(p);
        ASSERT_TRUE(result.has_value()) << result.error();

        const auto found = repo.get_by_id(3);
        ASSERT_TRUE(found.has_value()) << "Inserted row was not found";

        ASSERT_EQ(found->getId(), 3);
        ASSERT_EQ(found->getFirstName(), "test3");
        ASSERT_EQ(found->getLastName(), "test33");
    }

    TEST_F(SqlgenExampleTest, InsertMany_ShouldPersistMultiplePersons)
    {
        DAL::Repositories::SQLitePersonRepo<TestDatabaseConnection> repo(conn);

        std::vector<Core::Person> newPeople{ Core::Person(10, "batch1", "last1"), Core::Person(11, "batch2", "last2") };

        // const bool inserted = repo.insert_many(newPeople);
        // ASSERT_TRUE(inserted);
        auto result = repo.insert_many(newPeople);
        ASSERT_TRUE(result.has_value()) << result.error();

        ASSERT_EQ(repo.count(), 4);
        ASSERT_TRUE(repo.exists_by_id(10));
        ASSERT_TRUE(repo.exists_by_id(11));
    }

    TEST_F(SqlgenExampleTest, UpdateOne_ShouldUpdateExistingPerson)
    {
        DAL::Repositories::SQLitePersonRepo<TestDatabaseConnection> repo(conn);

        Core::Person updatedPerson(1, "test1_updated", "test11_updated");
        // const bool updated = repo.update_one(updatedPerson);
        // ASSERT_TRUE(updated);
        auto result = repo.update_one(updatedPerson);
        ASSERT_TRUE(result.has_value()) << result.error();

        const auto found = repo.get_by_id(1);
        ASSERT_TRUE(found.has_value());
        ASSERT_EQ(found->getFirstName(), "test1_updated");
        ASSERT_EQ(found->getLastName(), "test11_updated");
    }

    TEST_F(SqlgenExampleTest, InsertThenDelete_PersistPersonToThenDelete)
    {
        DAL::Repositories::SQLitePersonRepo<TestDatabaseConnection> repo(conn);

        Core::Person p(4, "test4", "test44");

        // const bool inserted = repo.insert_one(p);
        // ASSERT_TRUE(inserted) << "insert_one failed";
        auto result = repo.insert_one(p);
        ASSERT_TRUE(result.has_value()) << result.error();

        auto before = repo.get_by_id(4);
        ASSERT_TRUE(before.has_value());
        ASSERT_EQ(before->getId(), 4);
        ASSERT_EQ(before->getFirstName(), "test4");

        // repo.delete_by_id(4);
        auto delete_result = repo.delete_by_id(4);
        ASSERT_TRUE(delete_result.has_value()) << delete_result.error();

        auto after = repo.get_by_id(4);
        ASSERT_FALSE(after.has_value());
        ASSERT_FALSE(repo.exists_by_id(4));
    }

    TEST_F(SqlgenExampleTest, DependencyInjection_InterfacePolymorphism)
    {
        // Test Dependency Injection via Core::IPersonRepository interface pointer
        std::unique_ptr<Core::IPersonRepository> repo = std::make_unique<DAL::Repositories::SQLitePersonRepo<TestDatabaseConnection>>(conn);

        // Verify polymorphic access through Core interface
        const auto people = repo->get_all();
        ASSERT_EQ(people.size(), 2);

        const auto byLastName = repo->find_by_last_name("test11");
        ASSERT_EQ(byLastName.size(), 1);
        ASSERT_EQ(byLastName[0].getFirstName(), "test1");

        Core::Person p(5, "DI_User", "DI_LastName");
        // ASSERT_TRUE(repo->insert_one(p));
        auto result = repo->insert_one(p);
        ASSERT_TRUE(result.has_value()) << result.error();
        ASSERT_TRUE(repo->exists_by_id(5));
    }

    TEST_F(SqlgenExampleTest, SomethingRepository_GenericRepoOperations)
    {
        DAL::Repositories::SQLiteSomethingRepo<TestDatabaseConnection> somethingRepo(conn);

        const auto items = somethingRepo.get_all();
        ASSERT_EQ(items.size(), 2);
        ASSERT_EQ(items[0].getName(), "Item1");
        ASSERT_EQ(items[0].getCategory(), "Cat1");
        ASSERT_EQ(items[0].getDescription().value_or(""), "Desc1");

        Core::Something newItem(3, "Item3", "Cat3", "Desc3");
        // ASSERT_TRUE(somethingRepo.insert_one(newItem));
        auto result = somethingRepo.insert_one(newItem);
        ASSERT_TRUE(result.has_value()) << result.error();
        ASSERT_EQ(somethingRepo.count(), 3);
    }

} // namespace
