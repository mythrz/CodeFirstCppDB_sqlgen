#include <gtest/gtest.h>
#include <sqlgen.hpp>
#include <sqlgen/sqlite.hpp>

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
            return open_;
        }

        void set_open(bool open) noexcept
        {
            open_ = open;
        }

        template<typename DTO>
        std::expected<void, Core::DbError> ensure_table_created()
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{
                    Core::DbError::connection_error("TestDatabaseConnection is closed")
                };
            }
            return {};
        }

        template<typename DTO>
        std::expected<void, Core::DbError> ensure_table_dropped()
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{
                    Core::DbError::connection_error("TestDatabaseConnection is closed")
                };
            }
            get_storage<DTO>().clear();
            return {};
        }

        std::expected<void, Core::DbError> enable_foreign_keys()
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{
                    Core::DbError::connection_error("TestDatabaseConnection is closed")
                };
            }
            return {};
        }

        std::expected<void, Core::DbError> begin_transaction()
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{
                    Core::DbError::connection_error("TestDatabaseConnection is closed")
                };
            }
            if (in_transaction_)
            {
                return std::unexpected<Core::DbError>{
                    Core::DbError::connection_error("Transaction is already active")
                };
            }
            snapshot_ = std::make_tuple(persons_, somethings_, person_somethings_);
            in_transaction_ = true;
            return {};
        }

        std::expected<void, Core::DbError> commit()
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{
                    Core::DbError::connection_error("TestDatabaseConnection is closed")
                };
            }
            if (!in_transaction_)
            {
                return std::unexpected<Core::DbError>{
                    Core::DbError::connection_error("No active transaction to commit")
                };
            }
            in_transaction_ = false;
            return {};
        }

        std::expected<void, Core::DbError> rollback()
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{
                    Core::DbError::connection_error("TestDatabaseConnection is closed")
                };
            }
            if (!in_transaction_)
            {
                return std::unexpected<Core::DbError>{
                    Core::DbError::connection_error("No active transaction to rollback")
                };
            }
            persons_ = std::move(std::get<0>(snapshot_));
            somethings_ = std::move(std::get<1>(snapshot_));
            person_somethings_ = std::move(std::get<2>(snapshot_));
            in_transaction_ = false;
            return {};
        }

        [[nodiscard]] bool in_transaction() const noexcept
        {
            return in_transaction_;
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
        std::expected<std::vector<DTO>, Core::DbError> try_fetch_all() const
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{
                    Core::DbError::connection_error("TestDatabaseConnection is closed")
                };
            }
            return get_storage<DTO>();
        }

        template<typename DTO>
        std::vector<DTO> fetch_all() const
        {
            return get_storage<DTO>();
        }

        template<typename DTO>
        std::expected<std::optional<DTO>, Core::DbError> try_get_by_id(std::int32_t id) const
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{
                    Core::DbError::connection_error("TestDatabaseConnection is closed")
                };
            }
            return std::optional<DTO>{ get_by_id<DTO>(id) };
        }

        template<typename DTO>
        std::optional<DTO> get_by_id(std::int32_t id) const
        {
            const auto& storage = get_storage<DTO>();
            for (const auto& item : storage)
            {
                if constexpr (requires { item.id; })
                {
                    if (item.id.value() == id)
                        return item;
                }
            }
            return std::nullopt;
        }

        template<typename DTO, typename... Parts>
        std::expected<std::optional<DTO>, Core::DbError> try_get_by_composite_id(const Parts&... parts) const
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{
                    Core::DbError::connection_error("TestDatabaseConnection is closed")
                };
            }
            return std::optional<DTO>{ get_by_composite_id<DTO>(parts...) };
        }

        template<typename DTO, typename... Parts>
        std::optional<DTO> get_by_composite_id(const Parts&... parts) const
        {
            const auto& storage = get_storage<DTO>();
            auto key_tuple = std::make_tuple(static_cast<std::int32_t>(parts)...);
            for (const auto& item : storage)
            {
                if constexpr (std::is_same_v<DTO, DAL::Schema::Person_SomethingDTO>)
                {
                    if (item.person_id.value() == std::get<0>(key_tuple)
                        && item.something_id.value() == std::get<1>(key_tuple))
                    {
                        return item;
                    }
                }
            }
            return std::nullopt;
        }

        template<typename DTO, typename... Parts>
        bool exists_by_composite_id(const Parts&... parts) const
        {
            return get_by_composite_id<DTO>(parts...).has_value();
        }

        // Updated to return sqlgen::Result<sqlgen::Nothing> preserving error information

        template<typename DTO>
        sqlgen::Result<sqlgen::Nothing> insert(const DTO& item)
        {
            auto& storage = get_storage<DTO>();
            if constexpr (requires { item.id; })
            {
                for (const auto& existing : storage)
                {
                    if (existing.id.value() == item.id.value())
                    {
                        return sqlgen::error("UNIQUE constraint failed: duplicate primary key");
                    }
                }
            }
            else if constexpr (std::is_same_v<DTO, DAL::Schema::Person_SomethingDTO>)
            {
                for (const auto& existing : storage)
                {
                    if (existing.person_id.value() == item.person_id.value()
                        && existing.something_id.value() == item.something_id.value())
                    {
                        return sqlgen::error("UNIQUE constraint failed: duplicate composite key");
                    }
                }
            }
            storage.push_back(item);
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
                    if (elem.id.value() == item.id.value())
                    {
                        elem = item;
                        return sqlgen::Result<sqlgen::Nothing>{ sqlgen::Nothing{} };
                    }
                }
                else if constexpr (std::is_same_v<DTO, DAL::Schema::Person_SomethingDTO>)
                {
                    if (elem.person_id.value() == item.person_id.value()
                        && elem.something_id.value() == item.something_id.value())
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
        sqlgen::Result<sqlgen::Nothing> delete_by_id(std::int32_t id)
        {
            auto& storage = get_storage<DTO>();
            std::erase_if(
                storage,
                [id](const DTO& item)
                {
                    if constexpr (requires { item.id; })
                    {
                        return item.id.value() == id;
                    }
                    return false;
                });
            return sqlgen::Result<sqlgen::Nothing>{ sqlgen::Nothing{} };
        }

        template<typename DTO = DAL::Schema::Person_SomethingDTO, typename... Parts>
        sqlgen::Result<sqlgen::Nothing> delete_by_composite_id(const Parts&... parts)
        {
            auto& storage = get_storage<DTO>();
            auto key_tuple = std::make_tuple(static_cast<std::int32_t>(parts)...);
            std::erase_if(
                storage,
                [&key_tuple](const DTO& item)
                {
                    if constexpr (std::is_same_v<DTO, DAL::Schema::Person_SomethingDTO>)
                    {
                        return item.person_id.value() == std::get<0>(key_tuple)
                            && item.something_id.value() == std::get<1>(key_tuple);
                    }
                    return false;
                });
            return sqlgen::Result<sqlgen::Nothing>{ sqlgen::Nothing{} };
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
        bool open_{ true };
        bool in_transaction_{ false };
        std::vector<DAL::Schema::PersonDTO> persons_;
        std::vector<DAL::Schema::SomethingDTO> somethings_;
        std::vector<DAL::Schema::Person_SomethingDTO> person_somethings_;
        std::tuple<
            std::vector<DAL::Schema::PersonDTO>,
            std::vector<DAL::Schema::SomethingDTO>,
            std::vector<DAL::Schema::Person_SomethingDTO>>
            snapshot_;
    };

    class SqlgenExampleTest : public ::testing::Test
    {
    protected:
        std::shared_ptr<TestDatabaseConnection> conn;

        void SetUp() override
        {
            conn = std::make_shared<TestDatabaseConnection>();
            conn->insert(DAL::Schema::PersonDTO{ .id = 1, .first_name = "test1", .last_name = "test11" });
            conn->insert(DAL::Schema::PersonDTO{ .id = 2, .first_name = "test2", .last_name = "test22" });

            conn->insert(DAL::Schema::SomethingDTO{ .id = 1, .name = "Item1", .category = "Cat1", .description = "Desc1" });
            conn->insert(DAL::Schema::SomethingDTO{ .id = 2, .name = "Item2", .category = "Cat2", .description = "Desc2" });

            conn->insert(DAL::Schema::Person_SomethingDTO{ .person_id = 1, .something_id = 1, .association_type = "Owner" });
            conn->insert(DAL::Schema::Person_SomethingDTO{ .person_id = 2, .something_id = 2, .association_type = "User" });
        }
    };

    TEST_F(SqlgenExampleTest, GenericRepositoryGetAll)
    {
        DAL::Repositories::SQLitePersonRepo<TestDatabaseConnection> repo(conn);

        const auto people = repo.get_all();

        ASSERT_EQ(people.size(), 2);
        ASSERT_EQ(people[0].getId().get(), 1);
        ASSERT_EQ(people[0].getFirstName(), "test1");
        ASSERT_EQ(people[0].getLastName(), "test11");
        ASSERT_EQ(people[1].getId().get(), 2);
        ASSERT_EQ(people[1].getFirstName(), "test2");
        ASSERT_EQ(people[1].getLastName(), "test22");
    }

    TEST_F(SqlgenExampleTest, CustomRepositoryFindById)
    {
        DAL::Repositories::SQLitePersonRepo<TestDatabaseConnection> repo(conn);

        const auto person = repo.get_by_id(1);

        ASSERT_TRUE(person.has_value());
        ASSERT_EQ(person->getId().get(), 1);
        ASSERT_EQ(person->getFirstName(), "test1");
        ASSERT_EQ(person->getLastName(), "test11");
    }

    TEST_F(SqlgenExampleTest, CustomRepositoryFindByLastName)
    {
        DAL::Repositories::SQLitePersonRepo<TestDatabaseConnection> repo(conn);

        const auto people = repo.find_by_last_name("test22");

        ASSERT_EQ(people.size(), 1);
        ASSERT_EQ(people[0].getId().get(), 2);
        ASSERT_EQ(people[0].getFirstName(), "test2");
        ASSERT_EQ(people[0].getLastName(), "test22");
    }

    TEST_F(SqlgenExampleTest, InsertOne_ShouldPersistPerson)
    {
        DAL::Repositories::SQLitePersonRepo<TestDatabaseConnection> repo(conn);

        Core::Person p(Core::PersonId{ 3 }, "test3", "test33");

        auto result = repo.insert_one(p);
        ASSERT_TRUE(result.has_value()) << result.error().to_string();

        const auto found = repo.get_by_id(3);
        ASSERT_TRUE(found.has_value()) << "Inserted row was not found";

        ASSERT_EQ(found->getId().get(), 3);
        ASSERT_EQ(found->getFirstName(), "test3");
        ASSERT_EQ(found->getLastName(), "test33");
    }

    TEST_F(SqlgenExampleTest, InsertMany_ShouldPersistMultiplePersons)
    {
        DAL::Repositories::SQLitePersonRepo<TestDatabaseConnection> repo(conn);

        std::vector<Core::Person> newPeople{ Core::Person(Core::PersonId{ 10 }, "batch1", "last1"),
                                             Core::Person(Core::PersonId{ 11 }, "batch2", "last2") };

        auto result = repo.insert_many(newPeople);
        ASSERT_TRUE(result.has_value()) << result.error().to_string();

        ASSERT_EQ(repo.count(), 4);
        ASSERT_TRUE(repo.exists_by_id(10));
        ASSERT_TRUE(repo.exists_by_id(11));
    }

    TEST_F(SqlgenExampleTest, UpdateOne_ShouldUpdateExistingPerson)
    {
        DAL::Repositories::SQLitePersonRepo<TestDatabaseConnection> repo(conn);

        Core::Person updatedPerson(Core::PersonId{ 1 }, "test1_updated", "test11_updated");
        auto result = repo.update_one(updatedPerson);
        ASSERT_TRUE(result.has_value()) << result.error().to_string();

        const auto found = repo.get_by_id(1);
        ASSERT_TRUE(found.has_value());
        ASSERT_EQ(found->getFirstName(), "test1_updated");
        ASSERT_EQ(found->getLastName(), "test11_updated");
    }

    TEST_F(SqlgenExampleTest, InsertThenDelete_PersistPersonToThenDelete)
    {
        DAL::Repositories::SQLitePersonRepo<TestDatabaseConnection> repo(conn);

        Core::Person p(Core::PersonId{ 4 }, "test4", "test44");

        auto result = repo.insert_one(p);
        ASSERT_TRUE(result.has_value()) << result.error().to_string();

        auto before = repo.get_by_id(4);
        ASSERT_TRUE(before.has_value());
        ASSERT_EQ(before->getId().get(), 4);
        ASSERT_EQ(before->getFirstName(), "test4");

        auto delete_result = repo.delete_by_id(4);
        ASSERT_TRUE(delete_result.has_value()) << delete_result.error().to_string();

        auto after = repo.get_by_id(4);
        ASSERT_FALSE(after.has_value());
        ASSERT_FALSE(repo.exists_by_id(4));
    }

    TEST_F(SqlgenExampleTest, UpdateOne_ShouldReturnError_WhenItemNotFound)
    {
        DAL::Repositories::SQLitePersonRepo<TestDatabaseConnection> repo(conn);

        // Attempt to update an item that doesn't exist
        Core::Person nonExistentPerson(Core::PersonId{ 999 }, "Ghost", "Protocol");
        auto result = repo.update_one(nonExistentPerson);

        ASSERT_FALSE(result.has_value()) << "Expected update_one to fail";

        // Ensure the error type is correctly classified via DbError::map_from_message
        Core::DbError err = result.error();
        EXPECT_EQ(err.code, Core::DbErrorCode::NotFound);
        EXPECT_EQ(err.message, "Update failed: item not found");
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

        Core::Person p(Core::PersonId{ 5 }, "DI_User", "DI_LastName");
        auto result = repo->insert_one(p);
        ASSERT_TRUE(result.has_value()) << result.error().to_string();
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

        Core::Something newItem(Core::SomethingId{ 3 }, "Item3", "Cat3", "Desc3");
        auto result = somethingRepo.insert_one(newItem);
        ASSERT_TRUE(result.has_value()) << result.error().to_string();
        ASSERT_EQ(somethingRepo.count(), 3);
    }

    // MapperTraits direct tests. These exercise to_domain and to_dto independently of the repository layer, verifying every field for each
    // entity/DTO pair. Person PersonDTO
    TEST(MapperTraitsTest, PersonDTO_to_domain_MapsAllFields)
    {
        DAL::Schema::PersonDTO dto{ .id = 42, .first_name = "Alice", .last_name = "Smith" };

        const Core::Person person = DAL::Mappers::MapperTraits<Core::Person, DAL::Schema::PersonDTO>::to_domain(dto);

        EXPECT_EQ(person.getId().get(), 42);
        EXPECT_EQ(person.getFirstName(), "Alice");
        EXPECT_EQ(person.getLastName(), "Smith");
    }

    TEST(MapperTraitsTest, Person_to_dto_MapsAllFields)
    {
        Core::Person person(Core::PersonId{ 7 }, "Bob", "Jones");

        const DAL::Schema::PersonDTO dto = DAL::Mappers::MapperTraits<Core::Person, DAL::Schema::PersonDTO>::to_dto(person);

        EXPECT_EQ(dto.id.value(), 7);
        EXPECT_EQ(dto.first_name, "Bob");
        EXPECT_EQ(dto.last_name, "Jones");
    }

    TEST(MapperTraitsTest, Person_RoundTrip_DomainToDtoToDomain)
    {
        Core::Person original(Core::PersonId{ 99 }, "Charlie", "Brown");

        const auto dto = DAL::Mappers::MapperTraits<Core::Person, DAL::Schema::PersonDTO>::to_dto(original);
        const auto back = DAL::Mappers::MapperTraits<Core::Person, DAL::Schema::PersonDTO>::to_domain(dto);

        EXPECT_EQ(back.getId().get(), original.getId().get());
        EXPECT_EQ(back.getFirstName(), original.getFirstName());
        EXPECT_EQ(back.getLastName(), original.getLastName());
    }

    TEST(MapperTraitsTest, Person_RoundTrip_DtoToDomainToDto)
    {
        DAL::Schema::PersonDTO original{ .id = 5, .first_name = "Diana", .last_name = "Prince" };

        const auto domain = DAL::Mappers::MapperTraits<Core::Person, DAL::Schema::PersonDTO>::to_domain(original);
        const auto back = DAL::Mappers::MapperTraits<Core::Person, DAL::Schema::PersonDTO>::to_dto(domain);

        EXPECT_EQ(back.id.value(), original.id.value());
        EXPECT_EQ(back.first_name, original.first_name);
        EXPECT_EQ(back.last_name, original.last_name);
    }

    // Something SomethingDTO
    TEST(MapperTraitsTest, SomethingDTO_to_domain_MapsAllFields_WithDescription)
    {
        DAL::Schema::SomethingDTO dto{ .id = 10, .name = "Widget", .category = "Tools", .description = "A useful widget" };

        const Core::Something item = DAL::Mappers::MapperTraits<Core::Something, DAL::Schema::SomethingDTO>::to_domain(dto);

        EXPECT_EQ(item.getId().get(), 10);
        EXPECT_EQ(item.getName(), "Widget");
        EXPECT_EQ(item.getCategory(), "Tools");
        ASSERT_TRUE(item.getDescription().has_value());
        EXPECT_EQ(item.getDescription().value(), "A useful widget");
    }

    TEST(MapperTraitsTest, SomethingDTO_to_domain_MapsAllFields_NulloptDescription)
    {
        DAL::Schema::SomethingDTO dto{ .id = 11, .name = "Gadget", .category = "Electronics", .description = std::nullopt };

        const Core::Something item = DAL::Mappers::MapperTraits<Core::Something, DAL::Schema::SomethingDTO>::to_domain(dto);

        EXPECT_EQ(item.getId().get(), 11);
        EXPECT_EQ(item.getName(), "Gadget");
        EXPECT_EQ(item.getCategory(), "Electronics");
        EXPECT_FALSE(item.getDescription().has_value());
    }

    TEST(MapperTraitsTest, Something_to_dto_MapsAllFields)
    {
        Core::Something item(Core::SomethingId{ 20 }, "Thingamajig", "Misc", "Some description");

        const DAL::Schema::SomethingDTO dto = DAL::Mappers::MapperTraits<Core::Something, DAL::Schema::SomethingDTO>::to_dto(item);

        EXPECT_EQ(dto.id.value(), 20);
        EXPECT_EQ(dto.name, "Thingamajig");
        EXPECT_EQ(dto.category, "Misc");
        ASSERT_TRUE(dto.description.has_value());
        EXPECT_EQ(dto.description.value(), "Some description");
    }

    TEST(MapperTraitsTest, Something_RoundTrip_DomainToDtoToDomain)
    {
        Core::Something original(Core::SomethingId{ 55 }, "Doohickey", "Widgets", "Round-trip desc");

        const auto dto = DAL::Mappers::MapperTraits<Core::Something, DAL::Schema::SomethingDTO>::to_dto(original);
        const auto back = DAL::Mappers::MapperTraits<Core::Something, DAL::Schema::SomethingDTO>::to_domain(dto);

        EXPECT_EQ(back.getId().get(), original.getId().get());
        EXPECT_EQ(back.getName(), original.getName());
        EXPECT_EQ(back.getCategory(), original.getCategory());
        EXPECT_EQ(back.getDescription().value_or(""), original.getDescription().value_or(""));
    }

    TEST(MapperTraitsTest, Something_RoundTrip_NulloptDescription_PreservedThroughDto)
    {
        Core::Something original(Core::SomethingId{ 56 }, "Whatchamacallit", "Unknown", std::nullopt);

        const auto dto = DAL::Mappers::MapperTraits<Core::Something, DAL::Schema::SomethingDTO>::to_dto(original);
        const auto back = DAL::Mappers::MapperTraits<Core::Something, DAL::Schema::SomethingDTO>::to_domain(dto);

        EXPECT_FALSE(back.getDescription().has_value());
    }

    // Person_SomethingPerson_SomethingDTO
    TEST(MapperTraitsTest, Person_SomethingDTO_to_domain_MapsAllFields)
    {
        DAL::Schema::Person_SomethingDTO dto{ .person_id = 1, .something_id = 2, .association_type = "Owner" };

        const Core::Person_Something ps =
            DAL::Mappers::MapperTraits<Core::Person_Something, DAL::Schema::Person_SomethingDTO>::to_domain(dto);

        EXPECT_EQ(ps.getPersonId().get(), 1);
        EXPECT_EQ(ps.getSomethingId().get(), 2);
        EXPECT_EQ(ps.getAssociationType(), "Owner");
    }

    TEST(MapperTraitsTest, Person_Something_to_dto_MapsAllFields)
    {
        Core::Person_Something ps(Core::PersonId{ 3 }, Core::SomethingId{ 4 }, "Viewer");

        const DAL::Schema::Person_SomethingDTO dto =
            DAL::Mappers::MapperTraits<Core::Person_Something, DAL::Schema::Person_SomethingDTO>::to_dto(ps);

        EXPECT_EQ(dto.person_id.value(), 3);
        EXPECT_EQ(dto.something_id.value(), 4);
        EXPECT_EQ(dto.association_type, "Viewer");
    }

    TEST(MapperTraitsTest, Person_Something_RoundTrip_DomainToDtoToDomain)
    {
        Core::Person_Something original(Core::PersonId{ 7 }, Core::SomethingId{ 8 }, "Editor");

        const auto dto = DAL::Mappers::MapperTraits<Core::Person_Something, DAL::Schema::Person_SomethingDTO>::to_dto(original);
        const auto back = DAL::Mappers::MapperTraits<Core::Person_Something, DAL::Schema::Person_SomethingDTO>::to_domain(dto);

        EXPECT_EQ(back.getPersonId().get(), original.getPersonId().get());
        EXPECT_EQ(back.getSomethingId().get(), original.getSomethingId().get());
        EXPECT_EQ(back.getAssociationType(), original.getAssociationType());
    }

    // Composite key and junction table operations
    TEST_F(SqlgenExampleTest, Person_SomethingRepository_CompositeKeyOperations)
    {
        DAL::Repositories::SQLitePerson_SomethingRepo<TestDatabaseConnection> psRepo(conn);

        // Fetch all junction rows
        const auto allLinks = psRepo.get_all();
        ASSERT_EQ(allLinks.size(), 2);

        // Composite key lookup
        const auto link = psRepo.get_by_composite_id(Core::PersonId{ 1 }, Core::SomethingId{ 1 });
        ASSERT_TRUE(link.has_value());
        EXPECT_EQ(link->getPersonId().get(), 1);
        EXPECT_EQ(link->getSomethingId().get(), 1);
        EXPECT_EQ(link->getAssociationType(), "Owner");

        // Filter by person
        const auto byPerson = psRepo.find_by_person_id(Core::PersonId{ 1 });
        ASSERT_EQ(byPerson.size(), 1);
        EXPECT_EQ(byPerson[0].getAssociationType(), "Owner");

        // Filter by something
        const auto bySomething = psRepo.find_by_something_id(Core::SomethingId{ 2 });
        ASSERT_EQ(bySomething.size(), 1);
        EXPECT_EQ(bySomething[0].getAssociationType(), "User");

        // Existence check
        EXPECT_TRUE(psRepo.exists_by_composite_id(Core::PersonId{ 2 }, Core::SomethingId{ 2 }));
        EXPECT_FALSE(psRepo.exists_by_composite_id(Core::PersonId{ 99 }, Core::SomethingId{ 99 }));

        // Delete by composite ID
        auto delRes = psRepo.delete_by_composite_id(Core::PersonId{ 1 }, Core::SomethingId{ 1 });
        EXPECT_TRUE(delRes.has_value());
        EXPECT_FALSE(psRepo.exists_by_composite_id(Core::PersonId{ 1 }, Core::SomethingId{ 1 }));
        EXPECT_EQ(psRepo.count(), 1);
    }

    // DbSet and DbContext plug-and-play tests
    class MockDbContext : public DAL::DbContextBase<TestDatabaseConnection>
    {
    public:
        DAL::DbSet<Core::Person, DAL::Schema::PersonDTO, TestDatabaseConnection> people;
        DAL::DbSet<Core::Something, DAL::Schema::SomethingDTO, TestDatabaseConnection> somethings;

        explicit MockDbContext(std::shared_ptr<TestDatabaseConnection> conn)
            : DAL::DbContextBase<TestDatabaseConnection>(conn)
            , people(conn)
            , somethings(conn)
        {
        }
    };

    TEST_F(SqlgenExampleTest, DbSet_And_DbContext_PlugAndPlayOperations)
    {
        MockDbContext db(conn);
        EXPECT_TRUE(db.is_connected());

        // Find by ID via DbSet
        const auto person = db.people.find(1);
        ASSERT_TRUE(person.has_value());
        EXPECT_EQ(person->getFirstName(), "test1");

        // Insert new entity via DbSet::add
        Core::Person newPerson(Core::PersonId{ 50 }, "Plug", "Play");
        auto addRes = db.people.add(newPerson);
        ASSERT_TRUE(addRes.has_value()) << addRes.error().to_string();
        EXPECT_TRUE(db.people.contains(50));
        EXPECT_EQ(db.people.count(), 3);

        // LINQ-style in-memory filtering via DbSet::where
        auto playPeople = db.people.where([](const Core::Person& p) { return p.getLastName() == "Play"; });
        ASSERT_EQ(playPeople.size(), 1);
        EXPECT_EQ(playPeople[0].getFirstName(), "Plug");

        // First or default
        auto foundFirst = db.people.first_or_default([](const Core::Person& p) { return p.getId().get() == 50; });
        ASSERT_TRUE(foundFirst.has_value());
        EXPECT_EQ(foundFirst->getLastName(), "Play");

        // Remove via DbSet::remove
        auto removeRes = db.people.remove(50);
        EXPECT_TRUE(removeRes.has_value());
        EXPECT_FALSE(db.people.contains(50));
        EXPECT_EQ(db.people.count(), 2);

        // Save changes
        auto saveRes = db.save_changes();
        EXPECT_TRUE(saveRes.has_value());
    }

    // DbError structured error classification tests
    TEST(DbErrorDiagnosticsTest, FactoryAndMappingTests)
    {
        auto notFound = Core::DbError::not_found("Custom not found");
        EXPECT_EQ(notFound.code, Core::DbErrorCode::NotFound);
        EXPECT_EQ(notFound.code_name(), "NotFound");

        auto dup = Core::DbError::duplicate_key("Primary key collision");
        EXPECT_EQ(dup.code, Core::DbErrorCode::DuplicateKey);
        EXPECT_EQ(dup.code_name(), "DuplicateKey");

        auto fk = Core::DbError::constraint_violation("FK missing");
        EXPECT_EQ(fk.code, Core::DbErrorCode::ConstraintViolation);
        EXPECT_EQ(fk.code_name(), "ConstraintViolation");

        // Automated message parsing
        auto parsedUnique = Core::DbError::map_from_message("UNIQUE constraint failed: Person.id");
        EXPECT_EQ(parsedUnique.code, Core::DbErrorCode::DuplicateKey);

        auto parsedFk = Core::DbError::map_from_message("FOREIGN KEY constraint failed on Person_Something");
        EXPECT_EQ(parsedFk.code, Core::DbErrorCode::ConstraintViolation);

        auto parsedNotFound = Core::DbError::map_from_message("Entity not found in collection");
        EXPECT_EQ(parsedNotFound.code, Core::DbErrorCode::NotFound);

        auto parsedBusy = Core::DbError::map_from_message("database connection is busy");
        EXPECT_EQ(parsedBusy.code, Core::DbErrorCode::ConnectionError);

        auto parsedUnknown = Core::DbError::map_from_message("Syntax error near SELECT");
        EXPECT_EQ(parsedUnknown.code, Core::DbErrorCode::Unknown);
    }

    // Task 2.1: Compile-time SingleKey and CompositeKey tests
    TEST(CompileTimeKeysTest, SingleKey_PropertiesAndOperations)
    {
        // 1. Compile-time concepts and traits
        static_assert(Core::SingleKeyType<Core::SingleKey<Core::PersonId>>);
        static_assert(Core::Key<Core::SingleKey<Core::PersonId>>);
        static_assert(!Core::CompositeKeyType<Core::SingleKey<Core::PersonId>>);
        static_assert(Core::SingleKey<Core::PersonId>::arity == 1);
        static_assert(!Core::SingleKey<Core::PersonId>::is_composite);

        // 2. Construction and value access
        Core::SingleKey<Core::PersonId> key(Core::PersonId{ 42 });
        EXPECT_EQ(key.value.get(), 42);
        EXPECT_EQ(key.get().get(), 42);

        // Implicit conversion to underlying key type
        Core::PersonId rawId = key;
        EXPECT_EQ(rawId.get(), 42);

        // 3. Helper factory deduction
        auto deducedKey = Core::make_single_key(Core::SomethingId{ 100 });
        static_assert(std::is_same_v<decltype(deducedKey), Core::SingleKey<Core::SomethingId>>);
        EXPECT_EQ(deducedKey.get().get(), 100);

        // 4. Comparison
        Core::SingleKey<int> k1(5);
        Core::SingleKey<int> k2(5);
        Core::SingleKey<int> k3(10);
        EXPECT_EQ(k1, k2);
        EXPECT_LT(k1, k3);

        // 5. Entity integration
        Core::Person person(Core::PersonId{ 7 }, "Ada", "Lovelace");
        auto personKey = person.getKey();
        static_assert(std::is_same_v<decltype(personKey), Core::Person::Key>);
        EXPECT_EQ(personKey.get().get(), 7);

        // 6. std::formatter support
        std::string formatted = std::format("{}", k1);
        EXPECT_EQ(formatted, "SingleKey(5)");
    }

    TEST(CompileTimeKeysTest, CompositeKey_PropertiesAndOperations)
    {
        // 1. Compile-time concepts and traits
        using TwoPartKey = Core::CompositeKey<Core::PersonId, Core::SomethingId>;
        static_assert(Core::CompositeKeyType<TwoPartKey>);
        static_assert(Core::Key<TwoPartKey>);
        static_assert(!Core::SingleKeyType<TwoPartKey>);
        static_assert(TwoPartKey::arity == 2);
        static_assert(TwoPartKey::is_composite);

        // 2. Construction
        TwoPartKey key(Core::PersonId{ 1 }, Core::SomethingId{ 99 });
        EXPECT_EQ(key.get<0>().get(), 1);
        EXPECT_EQ(key.get<1>().get(), 99);

        // Type-based element access
        EXPECT_EQ(key.get<Core::PersonId>().get(), 1);
        EXPECT_EQ(key.get<Core::SomethingId>().get(), 99);

        // 3. Structured binding support
        auto [pid, sid] = key;
        EXPECT_EQ(pid.get(), 1);
        EXPECT_EQ(sid.get(), 99);

        // 4. Factory helper and 3-column composite key
        auto tripleKey = Core::make_composite_key(std::string("tenant_A"), 42, true);
        static_assert(decltype(tripleKey)::arity == 3);
        static_assert(decltype(tripleKey)::is_composite);
        auto [tenant, idVal, flag] = tripleKey;
        EXPECT_EQ(tenant, "tenant_A");
        EXPECT_EQ(idVal, 42);
        EXPECT_TRUE(flag);

        // 5. Comparison
        TwoPartKey otherKey(Core::PersonId{ 1 }, Core::SomethingId{ 99 });
        TwoPartKey diffKey(Core::PersonId{ 1 }, Core::SomethingId{ 100 });
        EXPECT_EQ(key, otherKey);
        EXPECT_LT(key, diffKey);

        // 6. Entity integration (Person_Something junction entity)
        Core::Person_Something link(Core::PersonId{ 3 }, Core::SomethingId{ 4 }, "Admin");
        auto linkKey = link.getKey();
        static_assert(std::is_same_v<decltype(linkKey), Core::Person_Something::Key>);
        EXPECT_EQ(linkKey.get<0>().get(), 3);
        EXPECT_EQ(linkKey.get<1>().get(), 4);

        // 7. std::formatter support
        std::string formatted = std::format("{}", key);
        EXPECT_EQ(formatted, "CompositeKey(1, 99)");
    }

    // =========================================================================
    // Phase 3 & 4 Tests: Reflection Schema Concepts, EntityMetadata,
    // Unified Composite-Key DbSet, and Monadic Read Semantics
    // =========================================================================

    struct CustomOrderDTO
    {
        static constexpr auto tablename = "Orders";
        sqlgen::PrimaryKey<std::int32_t, true> order_id{ 0 };
        std::string code;
    };

    struct CustomProductDTO
    {
        static constexpr auto tablename = "Products";
        sqlgen::PrimaryKey<std::int32_t, true> product_id{ 0 };
        std::string sku;
    };

    struct Order_ProductDTO
    {
        static constexpr auto tablename = "Order_Product";
        sqlgen::ForeignKey<std::int32_t, CustomOrderDTO, "order_id"> order_fk{ 0 };
        sqlgen::ForeignKey<std::int32_t, CustomProductDTO, "product_id"> product_fk{ 0 };
        std::int32_t quantity{ 1 };
    };

    struct MultiPrimaryKeyDTO
    {
        static constexpr auto tablename = "Tenant_Users";
        sqlgen::PrimaryKey<std::int32_t, false> tenant_id{ 0 };
        sqlgen::PrimaryKey<std::int32_t, false> user_id{ 0 };
        std::string role;
    };

    struct PlainNonEntityDTO
    {
        static constexpr auto tablename = "Logs";
        std::string message;
    };

    TEST(ReflectionSchemaTest, SingleAndCompositePrimaryKeyDetection)
    {
        static_assert(DAL::Schema::SinglePrimaryKey<CustomOrderDTO>);
        static_assert(!DAL::Schema::CompositePrimaryKey<CustomOrderDTO>);
        static_assert(DAL::Schema::RelationalEntity<CustomOrderDTO>);

        static_assert(DAL::Schema::CompositePrimaryKey<Order_ProductDTO>);
        static_assert(!DAL::Schema::SinglePrimaryKey<Order_ProductDTO>);
        static_assert(DAL::Schema::RelationalEntity<Order_ProductDTO>);

        static_assert(DAL::Schema::CompositePrimaryKey<MultiPrimaryKeyDTO>);
        static_assert(!DAL::Schema::SinglePrimaryKey<MultiPrimaryKeyDTO>);
        static_assert(DAL::Schema::RelationalEntity<MultiPrimaryKeyDTO>);

        static_assert(!DAL::Schema::SinglePrimaryKey<PlainNonEntityDTO>);
        static_assert(!DAL::Schema::CompositePrimaryKey<PlainNonEntityDTO>);
        static_assert(!DAL::Schema::RelationalEntity<PlainNonEntityDTO>);

        static_assert(std::same_as<DAL::Schema::SchemaOf_t<Core::Person>, DAL::Schema::PersonDTO>);
        static_assert(std::same_as<DAL::Schema::SchemaOf_t<Core::Something>, DAL::Schema::SomethingDTO>);
        static_assert(std::same_as<DAL::Schema::SchemaOf_t<Core::Person_Something>, DAL::Schema::Person_SomethingDTO>);
        static_assert(DAL::Schema::HasSchema<Core::Person>);
        static_assert(DAL::Schema::HasSchema<Core::Something>);
        static_assert(DAL::Schema::HasSchema<Core::Person_Something>);
        SUCCEED();
    }

    TEST(ReflectionSchemaTest, EntityMetadataQueriesAndKeyMatching)
    {
        using PersonMeta = DAL::Schema::EntityMetadata<Core::Person>;
        static_assert(PersonMeta::table_name == "Person");
        static_assert(!PersonMeta::is_composite_key);
        static_assert(PersonMeta::key_arity == 1);
        constexpr auto personCols = PersonMeta::column_names();
        static_assert(personCols.size() == 3);
        static_assert(personCols[0] == "id");
        static_assert(personCols[1] == "first_name");
        static_assert(personCols[2] == "last_name");
        constexpr auto personKeyCols = PersonMeta::key_column_names();
        static_assert(personKeyCols.size() == 1);
        static_assert(personKeyCols[0] == "id");

        using LinkMeta = DAL::Schema::EntityMetadata<Core::Person_Something>;
        static_assert(LinkMeta::table_name == "Person_Something");
        static_assert(LinkMeta::is_composite_key);
        static_assert(LinkMeta::key_arity == 2);
        constexpr auto linkCols = LinkMeta::column_names();
        static_assert(linkCols.size() == 3);
        static_assert(linkCols[0] == "person_id");
        static_assert(linkCols[1] == "something_id");
        static_assert(linkCols[2] == "association_type");
        constexpr auto linkKeyCols = LinkMeta::key_column_names();
        static_assert(linkKeyCols.size() == 2);
        static_assert(linkKeyCols[0] == "person_id");
        static_assert(linkKeyCols[1] == "something_id");

        Core::Person_Something link(Core::PersonId{ 7 }, Core::SomethingId{ 11 }, "Maintainer");
        auto extracted = LinkMeta::extract_key(link);
        EXPECT_EQ(extracted, (Core::CompositeKey<Core::PersonId, Core::SomethingId>{ Core::PersonId{ 7 }, Core::SomethingId{ 11 } }));

        DAL::Schema::Person_SomethingDTO dto{ .person_id = 7, .something_id = 11, .association_type = "Maintainer" };
        EXPECT_TRUE(LinkMeta::matches_key(dto, extracted));
        EXPECT_FALSE(LinkMeta::matches_key(dto, Core::CompositeKey{ Core::PersonId{ 7 }, Core::SomethingId{ 12 } }));
    }

    TEST_F(SqlgenExampleTest, UnifiedDbContextSetWithSingleAndCompositeKeys)
    {
        struct TestAppDbContext : public DAL::DbContextBase<TestDatabaseConnection>
        {
            Set<Core::Person> people{ this };
            Set<Core::Something> somethings{ this };
            Set<Core::Person_Something> personSomethings{ this };

            explicit TestAppDbContext(std::shared_ptr<TestDatabaseConnection> c)
                : DAL::DbContextBase<TestDatabaseConnection>(std::move(c))
            {
            }
        };

        TestAppDbContext db(conn);

        // Single-key ergonomic find/contains/remove (scalar, strong ID, and SingleKey)
        auto pByInt = db.people.find(1);
        ASSERT_TRUE(pByInt.has_value());
        EXPECT_EQ(pByInt->getFirstName(), "test1");

        auto pById = db.people.find(Core::PersonId{ 2 });
        ASSERT_TRUE(pById.has_value());
        EXPECT_EQ(pById->getFirstName(), "test2");

        EXPECT_TRUE(db.people.contains(Core::SingleKey{ Core::PersonId{ 1 } }));

        // Composite-key CRUD via Set<Core::Person_Something>
        const std::size_t initialCount = db.personSomethings.count();
        Core::Person_Something link1(Core::PersonId{ 10 }, Core::SomethingId{ 10 }, "Owner");
        Core::Person_Something link2(Core::PersonId{ 20 }, Core::SomethingId{ 20 }, "Viewer");
        ASSERT_TRUE(db.personSomethings.add(link1).has_value());
        ASSERT_TRUE(db.personSomethings.add(link2).has_value());
        EXPECT_EQ(db.personSomethings.count(), initialCount + 2);

        // Find via variadic composite key components
        auto foundVariadic = db.personSomethings.find(Core::PersonId{ 10 }, Core::SomethingId{ 10 });
        ASSERT_TRUE(foundVariadic.has_value());
        EXPECT_EQ(foundVariadic->getAssociationType(), "Owner");

        // Find via explicit CompositeKey
        Core::Person_Something::Key key2(Core::PersonId{ 20 }, Core::SomethingId{ 20 });
        auto foundKey = db.personSomethings.find(key2);
        ASSERT_TRUE(foundKey.has_value());
        EXPECT_EQ(foundKey->getAssociationType(), "Viewer");

        EXPECT_TRUE(db.personSomethings.contains(Core::PersonId{ 10 }, Core::SomethingId{ 10 }));
        EXPECT_FALSE(db.personSomethings.contains(Core::PersonId{ 10 }, Core::SomethingId{ 99 }));

        // Remove via variadic composite key components
        ASSERT_TRUE(db.personSomethings.remove(Core::PersonId{ 10 }, Core::SomethingId{ 10 }).has_value());
        EXPECT_FALSE(db.personSomethings.contains(Core::PersonId{ 10 }, Core::SomethingId{ 10 }));
        EXPECT_EQ(db.personSomethings.count(), initialCount + 1);
    }

    TEST_F(SqlgenExampleTest, MonadicReadErrorPropagationWhenConnectionClosed)
    {
        DAL::DbSet<Core::Person, DAL::Schema::PersonDTO, TestDatabaseConnection> people(conn);
        DAL::DbSet<Core::Person_Something, DAL::Schema::Person_SomethingDTO, TestDatabaseConnection> links(conn);

        ASSERT_TRUE(links.add(Core::Person_Something(Core::PersonId{ 10 }, Core::SomethingId{ 10 }, "Owner")).has_value());

        // Succeeds while open
        auto allPeople = people.try_to_vector();
        ASSERT_TRUE(allPeople.has_value());
        EXPECT_EQ(allPeople->size(), 2u);

        auto foundPerson = people.try_find(1);
        ASSERT_TRUE(foundPerson.has_value());
        EXPECT_TRUE(foundPerson->has_value());

        auto foundLink = links.try_find(Core::PersonId{ 10 }, Core::SomethingId{ 10 });
        ASSERT_TRUE(foundLink.has_value());
        EXPECT_TRUE(foundLink->has_value());

        // Fails with ConnectionError when connection is closed
        conn->set_open(false);

        auto closedAll = people.try_to_vector();
        ASSERT_FALSE(closedAll.has_value());
        EXPECT_EQ(closedAll.error().code, Core::DbErrorCode::ConnectionError);

        auto closedFind = people.try_find(1);
        ASSERT_FALSE(closedFind.has_value());
        EXPECT_EQ(closedFind.error().code, Core::DbErrorCode::ConnectionError);

        auto closedLinkFind = links.try_find(Core::PersonId{ 10 }, Core::SomethingId{ 10 });
        ASSERT_FALSE(closedLinkFind.has_value());
        EXPECT_EQ(closedLinkFind.error().code, Core::DbErrorCode::ConnectionError);
    }

    // =========================================================================
    // Phase 5 Tests: Live In-Memory SQLite Pushdown & True SQL UPDATE
    // =========================================================================

    class Phase5SqlPushdownTest : public ::testing::Test
    {
    protected:
        std::shared_ptr<DAL::SQLiteConnection> conn;

        void SetUp() override
        {
            auto rawConnRes = sqlgen::sqlite::connect(":memory:");
            ASSERT_TRUE(rawConnRes.has_value());

            auto ddlResult = rawConnRes.and_then(sqlgen::create_table<DAL::Schema::PersonDTO> | sqlgen::if_not_exists)
                                       .and_then(sqlgen::create_table<DAL::Schema::SomethingDTO> | sqlgen::if_not_exists)
                                       .and_then(sqlgen::create_table<DAL::Schema::Person_SomethingDTO> | sqlgen::if_not_exists);
            ASSERT_TRUE(ddlResult.has_value());

            auto rawConn = rawConnRes.value();
            ASSERT_TRUE(rawConn->execute("PRAGMA foreign_keys = ON;").has_value());
            conn = std::make_shared<DAL::SQLiteConnection>(rawConn);
        }
    };

    TEST_F(Phase5SqlPushdownTest, SingleKeyPushdown_GetCountExistsUpdateDelete)
    {
        DAL::DbSet<Core::Person> people(conn);

        Core::Person p1(Core::PersonId{ 1 }, "Ada", "Lovelace");
        Core::Person p2(Core::PersonId{ 2 }, "Alan", "Turing");
        ASSERT_TRUE(people.add(p1).has_value());
        ASSERT_TRUE(people.add(p2).has_value());

        // Server-side COUNT(*)
        EXPECT_EQ(people.count(), 2u);
        auto countRes = conn->try_count<DAL::Schema::PersonDTO>();
        ASSERT_TRUE(countRes.has_value());
        EXPECT_EQ(countRes.value(), 2u);

        // Pushdown WHERE + LIMIT 1
        auto found = people.try_find(1);
        ASSERT_TRUE(found.has_value());
        ASSERT_TRUE(found->has_value());
        EXPECT_EQ(found->value().getFirstName(), "Ada");
        EXPECT_EQ(found->value().getLastName(), "Lovelace");

        EXPECT_TRUE(people.contains(2));
        EXPECT_FALSE(people.contains(999));

        // True SQL UPDATE on existing row
        Core::Person p1Updated(Core::PersonId{ 1 }, "Augusta Ada", "King");
        auto updRes = people.update(p1Updated);
        ASSERT_TRUE(updRes.has_value()) << updRes.error().to_string();

        auto reloaded = people.find(1);
        ASSERT_TRUE(reloaded.has_value());
        EXPECT_EQ(reloaded->getFirstName(), "Augusta Ada");
        EXPECT_EQ(reloaded->getLastName(), "King");

        // UPDATE on missing row returns DbErrorCode::NotFound
        Core::Person missing(Core::PersonId{ 999 }, "Ghost", "User");
        auto missingUpd = people.update(missing);
        ASSERT_FALSE(missingUpd.has_value());
        EXPECT_EQ(missingUpd.error().code, Core::DbErrorCode::NotFound);

        // Pushdown DELETE
        ASSERT_TRUE(people.remove(2).has_value());
        EXPECT_FALSE(people.contains(2));
        EXPECT_EQ(people.count(), 1u);
    }

    TEST_F(Phase5SqlPushdownTest, CompositeKeyPushdown_DbSetAndRepository)
    {
        DAL::DbSet<Core::Person> people(conn);
        DAL::DbSet<Core::Something> somethings(conn);
        DAL::DbSet<Core::Person_Something> links(conn);
        DAL::Repositories::SQLitePerson_SomethingRepo<DAL::SQLiteConnection> linkRepo(conn);

        ASSERT_TRUE(people.add(Core::Person(Core::PersonId{ 1 }, "Grace", "Hopper")).has_value());
        ASSERT_TRUE(people.add(Core::Person(Core::PersonId{ 2 }, "Edsger", "Dijkstra")).has_value());
        ASSERT_TRUE(somethings.add(Core::Something(Core::SomethingId{ 1 }, "Compiler", "Software")).has_value());
        ASSERT_TRUE(somethings.add(Core::Something(Core::SomethingId{ 2 }, "Graph", "Algorithm")).has_value());

        ASSERT_TRUE(links.add(Core::Person_Something(Core::PersonId{ 1 }, Core::SomethingId{ 1 }, "Creator")).has_value());
        ASSERT_TRUE(links.add(Core::Person_Something(Core::PersonId{ 1 }, Core::SomethingId{ 2 }, "Reader")).has_value());
        ASSERT_TRUE(links.add(Core::Person_Something(Core::PersonId{ 2 }, Core::SomethingId{ 2 }, "Author")).has_value());

        EXPECT_EQ(links.count(), 3u);

        // Composite-key try_find & find
        auto linkRes = links.try_find(Core::PersonId{ 1 }, Core::SomethingId{ 1 });
        ASSERT_TRUE(linkRes.has_value());
        ASSERT_TRUE(linkRes->has_value());
        EXPECT_EQ(linkRes->value().getAssociationType(), "Creator");

        EXPECT_TRUE(links.contains(Core::PersonId{ 2 }, Core::SomethingId{ 2 }));
        EXPECT_FALSE(links.contains(Core::PersonId{ 2 }, Core::SomethingId{ 1 }));

        // Composite-key SQL UPDATE
        Core::Person_Something updatedLink(Core::PersonId{ 1 }, Core::SomethingId{ 1 }, "Pioneer");
        auto updRes = links.update(updatedLink);
        ASSERT_TRUE(updRes.has_value()) << updRes.error().to_string();

        auto reloadedLink = links.find(Core::PersonId{ 1 }, Core::SomethingId{ 1 });
        ASSERT_TRUE(reloadedLink.has_value());
        EXPECT_EQ(reloadedLink->getAssociationType(), "Pioneer");

        // Foreign-key pushdown via SQLitePerson_SomethingRepo
        auto byPerson1 = linkRepo.find_by_person_id(Core::PersonId{ 1 });
        EXPECT_EQ(byPerson1.size(), 2u);

        auto bySomething2 = linkRepo.find_by_something_id(Core::SomethingId{ 2 });
        EXPECT_EQ(bySomething2.size(), 2u);

        // Composite-key SQL DELETE
        ASSERT_TRUE(links.remove(Core::PersonId{ 1 }, Core::SomethingId{ 2 }).has_value());
        EXPECT_FALSE(links.contains(Core::PersonId{ 1 }, Core::SomethingId{ 2 }));
        EXPECT_EQ(links.count(), 2u);
    }

    // =========================================================================
    // Phase 6 Tests: Reflection-Driven ensure_created(), Staged Unit of Work,
    // Atomic Rollback, and Scoped Transactions
    // =========================================================================

    class Phase6AppDbContext : public DAL::DbContextBase<DAL::SQLiteConnection>
    {
    public:
        Set<Core::Person> people{ this };
        Set<Core::Something> somethings{ this };
        Set<Core::Person_Something> personSomethings{ this };

        explicit Phase6AppDbContext(std::shared_ptr<DAL::SQLiteConnection> conn)
            : DAL::DbContextBase<DAL::SQLiteConnection>(std::move(conn))
        {
        }
    };

    class Phase6MockDbContext : public DAL::DbContextBase<TestDatabaseConnection>
    {
    public:
        Set<Core::Person> people{ this };
        Set<Core::Something> somethings{ this };
        Set<Core::Person_Something> personSomethings{ this };

        explicit Phase6MockDbContext(std::shared_ptr<TestDatabaseConnection> conn)
            : DAL::DbContextBase<TestDatabaseConnection>(std::move(conn))
        {
        }
    };

    TEST(Phase6UnitOfWorkAndSchemaTest, EnsureCreated_AutoCreatesTablesAndEnforcesForeignKeys)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());

        Phase6AppDbContext db(connRes.value());
        auto schemaRes = db.ensure_created();
        ASSERT_TRUE(schemaRes.has_value()) << schemaRes.error().to_string();

        // All tables exist and are initially empty
        EXPECT_EQ(db.people.count(), 0u);
        EXPECT_EQ(db.somethings.count(), 0u);
        EXPECT_EQ(db.personSomethings.count(), 0u);

        // PRAGMA foreign_keys = ON is enforced: inserting orphan junction row fails
        auto fkErr = db.personSomethings.add(
            Core::Person_Something(Core::PersonId{ 99 }, Core::SomethingId{ 88 }, "Orphan"));
        ASSERT_FALSE(fkErr.has_value());
        EXPECT_EQ(fkErr.error().code, Core::DbErrorCode::ConstraintViolation);
    }

    TEST(Phase6UnitOfWorkAndSchemaTest, DeferredUnitOfWork_StagesUntilSaveChanges)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());

        Phase6AppDbContext db(connRes.value());
        ASSERT_TRUE(db.ensure_created().has_value());

        db.defer_changes(true);
        EXPECT_TRUE(db.is_deferred());

        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 1 }, "Ada", "Lovelace")).has_value());
        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 2 }, "Alan", "Turing")).has_value());
        ASSERT_TRUE(db.people.update(Core::Person(Core::PersonId{ 1 }, "Augusta", "King")).has_value());
        ASSERT_TRUE(db.people.remove(2).has_value());

        // Not visible in DB prior to save_changes()
        EXPECT_EQ(db.pending_changes_count(), 4u);
        EXPECT_EQ(db.people.count(), 0u);

        auto saveRes = db.save_changes();
        ASSERT_TRUE(saveRes.has_value()) << saveRes.error().to_string();
        EXPECT_EQ(db.pending_changes_count(), 0u);
        EXPECT_FALSE(db.in_transaction());

        // Visible after save_changes()
        EXPECT_EQ(db.people.count(), 1u);
        auto p1 = db.people.find(1);
        ASSERT_TRUE(p1.has_value());
        EXPECT_EQ(p1->getFirstName(), "Augusta");
        EXPECT_EQ(p1->getLastName(), "King");
    }

    TEST(Phase6UnitOfWorkAndSchemaTest, SaveChanges_AtomicRollbackOnMidBatchFailure_SQLite)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());

        Phase6AppDbContext db(connRes.value());
        ASSERT_TRUE(db.ensure_created().has_value());

        db.defer_changes(true);
        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 1 }, "Grace", "Hopper")).has_value());
        ASSERT_TRUE(db.somethings.add(Core::Something(Core::SomethingId{ 1 }, "Compiler", "Software")).has_value());
        // Invalid foreign key -> fails mid-batch
        ASSERT_TRUE(db.personSomethings.add(
            Core::Person_Something(Core::PersonId{ 1 }, Core::SomethingId{ 999 }, "Broken")).has_value());

        auto saveRes = db.save_changes();
        ASSERT_FALSE(saveRes.has_value());
        EXPECT_EQ(saveRes.error().code, Core::DbErrorCode::ConstraintViolation);
        EXPECT_EQ(db.pending_changes_count(), 0u);
        EXPECT_FALSE(db.in_transaction());

        // Zero partial writes left in SQLite
        EXPECT_EQ(db.people.count(), 0u);
        EXPECT_EQ(db.somethings.count(), 0u);
        EXPECT_EQ(db.personSomethings.count(), 0u);
    }

    TEST(Phase6UnitOfWorkAndSchemaTest, SaveChanges_AtomicRollbackOnDuplicateKey_MockConnection)
    {
        auto mockConn = std::make_shared<TestDatabaseConnection>();
        Phase6MockDbContext db(mockConn);
        ASSERT_TRUE(db.ensure_created().has_value());

        // Seed 1 existing person
        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 1 }, "Existing", "Person")).has_value());
        EXPECT_EQ(db.people.count(), 1u);

        db.defer_changes(true);
        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 2 }, "New", "Person")).has_value());
        // Duplicate key (id = 1) -> fails mid-batch
        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 1 }, "Dup", "Person")).has_value());

        auto saveRes = db.save_changes();
        ASSERT_FALSE(saveRes.has_value());
        EXPECT_EQ(saveRes.error().code, Core::DbErrorCode::DuplicateKey);
        EXPECT_EQ(db.pending_changes_count(), 0u);
        EXPECT_FALSE(db.in_transaction());

        // Rolled back cleanly to pre-batch state (only id=1 remains)
        EXPECT_EQ(db.people.count(), 1u);
        EXPECT_FALSE(db.people.contains(2));
    }

    TEST(Phase6UnitOfWorkAndSchemaTest, ScopedTransaction_CommitRollbackAndDiscardChanges)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());

        Phase6AppDbContext db(connRes.value());
        ASSERT_TRUE(db.ensure_created().has_value());

        // 1. Successful scoped transaction commits
        auto txOk = db.transaction([&]() -> std::expected<void, Core::DbError> {
            auto r1 = db.people.add(Core::Person(Core::PersonId{ 1 }, "Linus", "Torvalds"));
            if (!r1) return r1;
            return db.somethings.add(Core::Something(Core::SomethingId{ 1 }, "Git", "VCS"));
        });
        ASSERT_TRUE(txOk.has_value());
        EXPECT_FALSE(db.in_transaction());
        EXPECT_EQ(db.people.count(), 1u);
        EXPECT_EQ(db.somethings.count(), 1u);

        // 2. Failing scoped transaction rolls back
        auto txFail = db.transaction([&]() -> std::expected<void, Core::DbError> {
            auto r = db.people.add(Core::Person(Core::PersonId{ 2 }, "Temp", "User"));
            if (!r) return r;
            return std::unexpected<Core::DbError>{ Core::DbError::unknown("Simulated business rule failure") };
        });
        ASSERT_FALSE(txFail.has_value());
        EXPECT_FALSE(db.in_transaction());
        EXPECT_EQ(db.people.count(), 1u);
        EXPECT_FALSE(db.people.contains(2));

        // 3. discard_changes() clears staged operations without executing them
        db.defer_changes(true);
        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 3 }, "Discarded", "User")).has_value());
        EXPECT_EQ(db.pending_changes_count(), 1u);
        db.discard_changes();
        EXPECT_EQ(db.pending_changes_count(), 0u);
        ASSERT_TRUE(db.save_changes().has_value());
        EXPECT_EQ(db.people.count(), 1u);
    }

    // =========================================================================
    // Phase 7 Tests: Lifetime Hardening, TransactionScope, close(), & ensure_deleted()
    // =========================================================================

    TEST(Phase7LifecycleAndDeleteTest, DbContextBase_IsNonCopyableAndNonMovable)
    {
        static_assert(!std::is_copy_constructible_v<Phase6AppDbContext>);
        static_assert(!std::is_copy_assignable_v<Phase6AppDbContext>);
        static_assert(!std::is_move_constructible_v<Phase6AppDbContext>);
        static_assert(!std::is_move_assignable_v<Phase6AppDbContext>);
        SUCCEED();
    }

    TEST(Phase7LifecycleAndDeleteTest, TransactionScope_AutoRollbackOnScopeExitAndExplicitCommit)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());

        Phase6AppDbContext db(connRes.value());
        ASSERT_TRUE(db.ensure_created().has_value());

        // 1. Scope exits without commit() -> automatic rollback
        {
            DAL::TransactionScope scope(db);
            ASSERT_TRUE(scope.begin_result().has_value());
            EXPECT_TRUE(scope.is_active());
            EXPECT_TRUE(db.in_transaction());

            ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 1 }, "Uncommitted", "User")).has_value());
            EXPECT_EQ(db.people.count(), 1u);
        }
        EXPECT_FALSE(db.in_transaction());
        EXPECT_EQ(db.people.count(), 0u);

        // 2. Scope with explicit commit() -> persists changes
        {
            DAL::TransactionScope scope(db);
            ASSERT_TRUE(scope.begin_result().has_value());
            ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 2 }, "Committed", "User")).has_value());
            ASSERT_TRUE(scope.commit().has_value());
            EXPECT_FALSE(scope.is_active());
        }
        EXPECT_FALSE(db.in_transaction());
        EXPECT_EQ(db.people.count(), 1u);
        auto persisted = db.people.find(1);
        ASSERT_TRUE(persisted.has_value());
        EXPECT_EQ(persisted->getFirstName(), "Committed");
    }

    TEST(Phase7LifecycleAndDeleteTest, SQLiteConnection_CloseRollsBackAndRejectsSubsequentOps)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());
        auto conn = connRes.value();

        Phase6AppDbContext db(conn);
        ASSERT_TRUE(db.ensure_created().has_value());
        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 1 }, "Persisted", "User")).has_value());

        // Start a transaction and insert an uncommitted row, then close()
        ASSERT_TRUE(db.begin_transaction().has_value());
        EXPECT_TRUE(conn->in_transaction());
        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 2 }, "Temp", "User")).has_value());

        conn->close();
        EXPECT_FALSE(conn->is_open());
        EXPECT_FALSE(conn->in_transaction());
        EXPECT_FALSE(db.is_connected());

        // Subsequent reads/writes fail with ConnectionError
        auto readRes = db.people.try_to_vector();
        ASSERT_FALSE(readRes.has_value());
        EXPECT_EQ(readRes.error().code, Core::DbErrorCode::ConnectionError);

        auto writeRes = db.people.add(Core::Person(Core::PersonId{ 3 }, "AfterClose", "User"));
        ASSERT_FALSE(writeRes.has_value());
        EXPECT_EQ(writeRes.error().code, Core::DbErrorCode::ConnectionError);
    }

    TEST(Phase7LifecycleAndDeleteTest, EnsureDeleted_DropsInReverseOrderAndRecreatesCleanly)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());

        Phase6AppDbContext db(connRes.value());
        ASSERT_TRUE(db.ensure_created().has_value());

        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 1 }, "Alice", "Smith")).has_value());
        ASSERT_TRUE(db.somethings.add(Core::Something(Core::SomethingId{ 1 }, "Book", "Media")).has_value());
        ASSERT_TRUE(db.personSomethings.add(
            Core::Person_Something(Core::PersonId{ 1 }, Core::SomethingId{ 1 }, "Reader")).has_value());

        EXPECT_EQ(db.people.count(), 1u);
        EXPECT_EQ(db.somethings.count(), 1u);
        EXPECT_EQ(db.personSomethings.count(), 1u);

        // Drop all tables in reverse order (junction table before parent tables)
        auto dropRes = db.ensure_deleted();
        ASSERT_TRUE(dropRes.has_value()) << dropRes.error().to_string();

        // Recreate schema cleanly -> tables are empty
        ASSERT_TRUE(db.ensure_created().has_value());
        EXPECT_EQ(db.people.count(), 0u);
        EXPECT_EQ(db.somethings.count(), 0u);
        EXPECT_EQ(db.personSomethings.count(), 0u);
    }

    // =========================================================================
    // Phase 8 Tests: Zero-DTO Single-Struct POCO Mode & Auto-Inferred Keys
    // =========================================================================

    struct PocoUser
    {
        static constexpr auto tablename = "PocoUsers";
        sqlgen::PrimaryKey<std::int32_t, true> user_id{ 0 };
        std::string username;
    };

    struct PocoRole
    {
        static constexpr auto tablename = "PocoRoles";
        sqlgen::PrimaryKey<std::int32_t, true> role_id{ 0 };
        std::string role_name;
    };

    struct PocoUserRole
    {
        static constexpr auto tablename = "PocoUserRoles";
        sqlgen::ForeignKey<std::int32_t, PocoUser, "user_id"> user_id{ 0 };
        sqlgen::ForeignKey<std::int32_t, PocoRole, "role_id"> role_id{ 0 };
        std::string assigned_by;
    };

    class PocoDbContext : public DAL::DbContextBase<DAL::SQLiteConnection>
    {
    public:
        Set<PocoUser> users{ this };
        Set<PocoRole> roles{ this };
        Set<PocoUserRole> userRoles{ this };

        explicit PocoDbContext(std::shared_ptr<DAL::SQLiteConnection> conn)
            : DAL::DbContextBase<DAL::SQLiteConnection>(std::move(conn))
        {
        }
    };

    TEST(Phase8PocoModeTest, IdentityMapperAndInferredKeyTraits)
    {
        // Identity fast-path
        static_assert(DAL::Mappers::Mappable<PocoUser, PocoUser>);
        static_assert(DAL::Mappers::Mappable<PocoUserRole, PocoUserRole>);

        PocoUser u{ .user_id = 42, .username = "alice" };
        auto mappedDomain = DAL::Mappers::MapperTraits<PocoUser, PocoUser>::to_domain(u);
        auto mappedDto = DAL::Mappers::MapperTraits<PocoUser, PocoUser>::to_dto(u);
        EXPECT_EQ(mappedDomain.user_id.value(), 42);
        EXPECT_EQ(mappedDomain.username, "alice");
        EXPECT_EQ(mappedDto.user_id.value(), 42);

        // InferredKey_t & EntityKey_t
        static_assert(std::same_as<DAL::Schema::InferredKey_t<PocoUser>, Core::SingleKey<std::int32_t>>);
        static_assert(std::same_as<DAL::Schema::InferredKey_t<PocoUserRole>, Core::CompositeKey<std::int32_t, std::int32_t>>);
        static_assert(std::same_as<DAL::Schema::EntityKey_t<PocoUser>, Core::SingleKey<std::int32_t>>);
        static_assert(std::same_as<DAL::Schema::EntityKey_t<PocoUserRole>, Core::CompositeKey<std::int32_t, std::int32_t>>);
        static_assert(std::same_as<DAL::Schema::EntityKey_t<Core::Person>, Core::Person::Key>);

        // EntityMetadata on POCO structs
        using UserMeta = DAL::Schema::EntityMetadata<PocoUser>;
        static_assert(UserMeta::table_name == "PocoUsers");
        static_assert(!UserMeta::is_composite_key);
        static_assert(UserMeta::key_arity == 1);
        EXPECT_EQ(UserMeta::extract_key(u), Core::SingleKey<std::int32_t>{ 42 });

        using UserRoleMeta = DAL::Schema::EntityMetadata<PocoUserRole>;
        static_assert(UserRoleMeta::table_name == "PocoUserRoles");
        static_assert(UserRoleMeta::is_composite_key);
        static_assert(UserRoleMeta::key_arity == 2);
        PocoUserRole ur{ .user_id = 42, .role_id = 7, .assigned_by = "admin" };
        EXPECT_EQ(UserRoleMeta::extract_key(ur), (Core::CompositeKey<std::int32_t, std::int32_t>{ 42, 7 }));
    }

    TEST(Phase8PocoModeTest, ZeroDtoPocoCrudWithLiveSQLite)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());

        PocoDbContext db(connRes.value());
        ASSERT_TRUE(db.ensure_created().has_value());

        ASSERT_TRUE(db.users.add(PocoUser{ .username = "alice" }).has_value());
        ASSERT_TRUE(db.roles.add(PocoRole{ .role_name = "Admin" }).has_value());
        ASSERT_TRUE(db.userRoles.add(PocoUserRole{ .user_id = 1, .role_id = 1, .assigned_by = "system" }).has_value());

        EXPECT_EQ(db.users.count(), 1u);
        EXPECT_EQ(db.roles.count(), 1u);
        EXPECT_EQ(db.userRoles.count(), 1u);

        // Single-key POCO find & update
        auto foundUser = db.users.find(1);
        ASSERT_TRUE(foundUser.has_value());
        EXPECT_EQ(foundUser->username, "alice");

        ASSERT_TRUE(db.users.update(PocoUser{ .user_id = 1, .username = "alice_updated" }).has_value());
        auto reloadedUser = db.users.try_find(1);
        ASSERT_TRUE(reloadedUser.has_value());
        ASSERT_TRUE(reloadedUser->has_value());
        EXPECT_EQ(reloadedUser->value().username, "alice_updated");

        // Composite-key POCO find, update, contains, remove
        auto foundLink = db.userRoles.find(1, 1);
        ASSERT_TRUE(foundLink.has_value());
        EXPECT_EQ(foundLink->assigned_by, "system");

        ASSERT_TRUE(db.userRoles.update(PocoUserRole{ .user_id = 1, .role_id = 1, .assigned_by = "root" }).has_value());
        auto reloadedLink = db.userRoles.find(1, 1);
        ASSERT_TRUE(reloadedLink.has_value());
        EXPECT_EQ(reloadedLink->assigned_by, "root");

        EXPECT_TRUE(db.userRoles.contains(1, 1));
        ASSERT_TRUE(db.userRoles.remove(1, 1).has_value());
        EXPECT_FALSE(db.userRoles.contains(1, 1));
        EXPECT_EQ(db.userRoles.count(), 0u);

        ASSERT_TRUE(db.ensure_deleted().has_value());
    }

    // =========================================================================
    // Phase 9 Tests: Compile-Time Topological Foreign-Key Sorting
    // =========================================================================

    class ReversedOrderPocoDbContext : public DAL::DbContextBase<DAL::SQLiteConnection>
    {
    public:
        // Child junction table intentionally declared FIRST, parents AFTER
        Set<PocoUserRole> userRoles{ this };
        Set<PocoRole> roles{ this };
        Set<PocoUser> users{ this };

        explicit ReversedOrderPocoDbContext(std::shared_ptr<DAL::SQLiteConnection> conn)
            : DAL::DbContextBase<DAL::SQLiteConnection>(std::move(conn))
        {
        }
    };

    TEST(Phase9TopologicalFkSortTest, CompileTimeTopologicalOrder_ParentsBeforeChildren)
    {
        // Input order: 0 = PocoUserRole (child), 1 = PocoRole (parent), 2 = PocoUser (parent)
        constexpr auto order = DAL::Schema::topological_dto_order<PocoUserRole, PocoRole, PocoUser>();
        static_assert(order[0] == 1); // PocoRole
        static_assert(order[1] == 2); // PocoUser
        static_assert(order[2] == 0); // PocoUserRole (after both parents)

        constexpr auto rev = DAL::Schema::reverse_index_array(order);
        static_assert(rev[0] == 0); // PocoUserRole dropped first
        static_assert(rev[1] == 2); // PocoUser
        static_assert(rev[2] == 1); // PocoRole
        SUCCEED();
    }

    TEST(Phase9TopologicalFkSortTest, EnsureCreatedAndDeleted_IndependentOfMemberDeclarationOrder)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());

        ReversedOrderPocoDbContext db(connRes.value());
        ASSERT_TRUE(db.ensure_created().has_value());

        ASSERT_TRUE(db.users.add(PocoUser{ .username = "bob" }).has_value());
        ASSERT_TRUE(db.roles.add(PocoRole{ .role_name = "Editor" }).has_value());
        ASSERT_TRUE(db.userRoles.add(PocoUserRole{ .user_id = 1, .role_id = 1, .assigned_by = "admin" }).has_value());

        EXPECT_EQ(db.users.count(), 1u);
        EXPECT_EQ(db.roles.count(), 1u);
        EXPECT_EQ(db.userRoles.count(), 1u);

        // Even with PRAGMA foreign_keys = ON and child declared first, ensure_deleted()
        // drops PocoUserRoles before PocoUsers and PocoRoles without FK errors.
        auto dropRes = db.ensure_deleted();
        ASSERT_TRUE(dropRes.has_value()) << dropRes.error().to_string();

        // Recreate cleanly
        ASSERT_TRUE(db.ensure_created().has_value());
        EXPECT_EQ(db.users.count(), 0u);
        EXPECT_EQ(db.roles.count(), 0u);
        EXPECT_EQ(db.userRoles.count(), 0u);
    }

    // =========================================================================
    // Phase 10: Fluent Server-Side Query Pushdown & Snapshot ChangeTracker
    // =========================================================================

    TEST(Phase10QueryBuilderAndChangeTrackerTest, ServerSideQueryPushdown_WhereOrderByAndPagination)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());

        struct PersonQueryDbContext : public DAL::DbContextBase<DAL::SQLiteConnection>
        {
            Set<Core::Person> people{ this };
            explicit PersonQueryDbContext(std::shared_ptr<DAL::SQLiteConnection> c)
                : DAL::DbContextBase<DAL::SQLiteConnection>(std::move(c))
            {}
        };

        PersonQueryDbContext db(connRes.value());
        ASSERT_TRUE(db.ensure_created().has_value());

        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 1 }, "Charlie", "Smith")).has_value());
        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 2 }, "Alice", "Smith")).has_value());
        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 3 }, "Bob", "Smith")).has_value());
        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 4 }, "Diana", "Jones")).has_value());
        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 5 }, "Eve", "Smith")).has_value());

        // where_equal + ascending order_by
        auto smithAsc = db.people
            .where_equal<"last_name">("Smith")
            .order_by<"first_name">(false)
            .to_vector();
        ASSERT_EQ(smithAsc.size(), 4u);
        EXPECT_EQ(smithAsc[0].getFirstName(), "Alice");
        EXPECT_EQ(smithAsc[1].getFirstName(), "Bob");
        EXPECT_EQ(smithAsc[2].getFirstName(), "Charlie");
        EXPECT_EQ(smithAsc[3].getFirstName(), "Eve");

        // where_equal + descending order_by + page(limit=2, offset=1)
        auto smithDescPage = db.people
            .where_equal<"last_name">("Smith")
            .order_by<"first_name">(true)
            .page(2, 1)
            .to_vector();
        ASSERT_EQ(smithDescPage.size(), 2u);
        EXPECT_EQ(smithDescPage[0].getFirstName(), "Charlie");
        EXPECT_EQ(smithDescPage[1].getFirstName(), "Bob");

        // where_sql with raw sqlgen condition + first_or_default()
        auto firstJones = db.people
            .where_sql(sqlgen::Col<"last_name">{} == std::string("Jones"))
            .first_or_default();
        ASSERT_TRUE(firstJones.has_value());
        EXPECT_EQ(firstJones->getFirstName(), "Diana");

        // Chained order_by + page regardless of filtering
        auto pagedAll = db.people
            .order_by<"id">(true)
            .page(2, 2)
            .to_vector();
        ASSERT_EQ(pagedAll.size(), 2u);
        EXPECT_EQ(pagedAll[0].getFirstName(), "Bob");
        EXPECT_EQ(pagedAll[1].getFirstName(), "Alice");
    }

    TEST(Phase10QueryBuilderAndChangeTrackerTest, ChangeTracker_IdentityMapAndSnapshotDirtyChecking)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());

        PocoDbContext db(connRes.value());
        ASSERT_TRUE(db.ensure_created().has_value());
        db.enable_change_tracking(true);

        ASSERT_TRUE(db.users.add(PocoUser{ .username = "alice" }).has_value());
        ASSERT_TRUE(db.users.add(PocoUser{ .username = "bob" }).has_value());

        // 1. Identity Map via find_tracked(): same key returns exact same shared_ptr instance
        auto alice1 = db.users.find_tracked(1);
        auto alice2 = db.users.find_tracked(1);
        ASSERT_NE(alice1, nullptr);
        EXPECT_EQ(alice1.get(), alice2.get());
        EXPECT_EQ(db.tracked_entities_count(), 1u);

        // Mutate in-memory tracked entity without calling db.users.update()
        alice1->username = "alice.updated";
        ASSERT_TRUE(db.save_changes().has_value());

        // Verify persisted to SQLite
        auto aliceReloaded = db.users.find(1);
        ASSERT_TRUE(aliceReloaded.has_value());
        EXPECT_EQ(aliceReloaded->username, "alice.updated");

        // 2. find() snapshots original DB state; mutating local copy and calling track()
        //    preserves the original snapshot and emits UPDATE on save_changes().
        auto bobLocal = db.users.find(2);
        ASSERT_TRUE(bobLocal.has_value());
        bobLocal->username = "bob_v2";
        db.users.track(*bobLocal);

        ASSERT_TRUE(db.save_changes().has_value());

        auto bobReloaded = db.users.find(2);
        ASSERT_TRUE(bobReloaded.has_value());
        EXPECT_EQ(bobReloaded->username, "bob_v2");
    }

    // =========================================================================
    // Phase 11: Zero-JSON Reflected ChangeTracker & remove() Lifecycle Sync
    // =========================================================================

    TEST(Phase11ZeroJsonChangeTrackerTest, DtoEquals_SingleAndCompositeKeyDTOs)
    {
        DAL::Schema::PersonDTO p1{ .id = 1, .first_name = "Alice", .last_name = "Smith" };
        DAL::Schema::PersonDTO p2{ .id = 1, .first_name = "Alice", .last_name = "Smith" };
        DAL::Schema::PersonDTO p3{ .id = 1, .first_name = "Bob", .last_name = "Smith" };

        EXPECT_TRUE(DAL::Schema::dto_equals(p1, p2));
        EXPECT_FALSE(DAL::Schema::dto_equals(p1, p3));

        DAL::Schema::Person_SomethingDTO link1{ .person_id = 1, .something_id = 2, .association_type = "Owner" };
        DAL::Schema::Person_SomethingDTO link2{ .person_id = 1, .something_id = 2, .association_type = "Owner" };
        DAL::Schema::Person_SomethingDTO link3{ .person_id = 1, .something_id = 2, .association_type = "User" };

        EXPECT_TRUE(DAL::Schema::dto_equals(link1, link2));
        EXPECT_FALSE(DAL::Schema::dto_equals(link1, link3));
    }

    TEST(Phase11ZeroJsonChangeTrackerTest, RemoveEvictsTrackedEntity_NoPhantomUpdate)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());

        PocoDbContext db(connRes.value());
        ASSERT_TRUE(db.ensure_created().has_value());
        db.enable_change_tracking(true);

        ASSERT_TRUE(db.users.add(PocoUser{ .username = "alice" }).has_value());
        auto alice = db.users.find_tracked(1);
        ASSERT_NE(alice, nullptr);
        EXPECT_EQ(db.tracked_entities_count(), 1u);

        // Mutate tracked entity and then remove it before save_changes()
        alice->username = "alice_mutated";
        ASSERT_TRUE(db.users.remove(1).has_value());
        EXPECT_EQ(db.tracked_entities_count(), 0u);

        // save_changes() should succeed without emitting a phantom UPDATE for deleted row
        ASSERT_TRUE(db.save_changes().has_value());
        EXPECT_FALSE(db.users.find(1).has_value());
        EXPECT_EQ(db.users.count(), 0u);
    }

    // =========================================================================
    // Phase 12: Composable DbQuery, Rich Predicates & Server-Side Aggregations
    // =========================================================================

    TEST(Phase12ComposableQueryTest, ChainedAndOr_RichPredicates_CountAndAny)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());

        struct PersonQueryDbContext : public DAL::DbContextBase<DAL::SQLiteConnection>
        {
            Set<Core::Person> people{ this };
            explicit PersonQueryDbContext(std::shared_ptr<DAL::SQLiteConnection> c)
                : DAL::DbContextBase<DAL::SQLiteConnection>(std::move(c))
            {}
        };

        PersonQueryDbContext db(connRes.value());
        ASSERT_TRUE(db.ensure_created().has_value());

        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 1 }, "Charlie", "Smith")).has_value());
        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 2 }, "Alice", "Smith")).has_value());
        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 3 }, "Bob", "Smith")).has_value());
        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 4 }, "Diana", "Jones")).has_value());
        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 5 }, "Alicia", "Smith")).has_value());

        // Chained AND + > + LIKE
        auto filtered = db.people
            .where_equal<"last_name">("Smith")
            .where_gt<"id">(1)
            .where_like<"first_name">("%li%")
            .order_by<"id">(false)
            .to_vector();
        ASSERT_EQ(filtered.size(), 2u);
        EXPECT_EQ(filtered[0].getFirstName(), "Alice");
        EXPECT_EQ(filtered[1].getFirstName(), "Alicia");

        // OR chaining
        auto orQuery = db.people
            .where_equal<"first_name">("Bob")
            .or_where_equal<"first_name">("Diana");
        EXPECT_EQ(orQuery.count(), 2u);
        EXPECT_TRUE(orQuery.any());

        // where_in + count() + any()
        auto inQuery = db.people.where_in<"id">(1, 3, 5);
        auto inCount = inQuery.try_count();
        ASSERT_TRUE(inCount.has_value());
        EXPECT_EQ(inCount.value(), 3u);
        EXPECT_TRUE(inQuery.any());
        EXPECT_FALSE(db.people.where_equal<"last_name">("Unknown").any());

        // where_le / where_not_equal
        auto leNotEq = db.people
            .where_le<"id">(3)
            .where_not_equal<"first_name">("Bob")
            .to_vector();
        EXPECT_EQ(leNotEq.size(), 2u);
    }

    // =========================================================================
    // Phase 13: Atomic Upsert & Bulk Server-Side remove_where
    // =========================================================================

    struct PocoSetting
    {
        static constexpr auto tablename = "PocoSettings";
        sqlgen::PrimaryKey<std::int32_t, false> key_id{ 0 };
        std::string val;
    };

    struct SettingDbContext : public DAL::DbContextBase<DAL::SQLiteConnection>
    {
        Set<PocoSetting> settings{ this };
        explicit SettingDbContext(std::shared_ptr<DAL::SQLiteConnection> c)
            : DAL::DbContextBase<DAL::SQLiteConnection>(std::move(c))
        {}
    };

    TEST(Phase13UpsertAndBulkDeleteTest, UpsertImmediateAndDeferred_And_RemoveWhere)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());

        struct PersonUpsertDbContext : public DAL::DbContextBase<DAL::SQLiteConnection>
        {
            Set<Core::Person> people{ this };
            explicit PersonUpsertDbContext(std::shared_ptr<DAL::SQLiteConnection> c)
                : DAL::DbContextBase<DAL::SQLiteConnection>(std::move(c))
            {}
        };

        PersonUpsertDbContext db(connRes.value());
        ASSERT_TRUE(db.ensure_created().has_value());

        // Immediate upsert (insert)
        auto upsert1 = db.people.upsert(Core::Person(Core::PersonId{ 1 }, "Alice", "Smith"));
        ASSERT_TRUE(upsert1.has_value()) << upsert1.error().to_string();
        EXPECT_EQ(db.people.count(), 1u);

        // Immediate upsert (replace existing)
        auto upsert2 = db.people.upsert(Core::Person(Core::PersonId{ 1 }, "AliceV2", "Smith"));
        ASSERT_TRUE(upsert2.has_value()) << upsert2.error().to_string();
        EXPECT_EQ(db.people.count(), 1u);
        EXPECT_EQ(db.people.find(1)->getFirstName(), "AliceV2");

        // Deferred upsert inside save_changes()
        db.defer_changes(true);
        ASSERT_TRUE(db.people.upsert(Core::Person(Core::PersonId{ 2 }, "Bob", "Smith")).has_value());
        ASSERT_TRUE(db.people.upsert(Core::Person(Core::PersonId{ 3 }, "Diana", "Jones")).has_value());
        EXPECT_EQ(db.people.count(), 1u);
        ASSERT_TRUE(db.save_changes().has_value());
        db.defer_changes(false);
        EXPECT_EQ(db.people.count(), 3u);

        // Bulk server-side remove_where()
        ASSERT_TRUE(db.people.where_equal<"last_name">("Smith").remove_where().has_value());
        EXPECT_EQ(db.people.count(), 1u);
        EXPECT_EQ(db.people.find(3)->getFirstName(), "Diana");

        // Also test native sqlgen::insert_or_replace path on a non-auto-increment PK entity
        SettingDbContext sdb(connRes.value());
        ASSERT_TRUE(sdb.ensure_created().has_value());
        ASSERT_TRUE(sdb.settings.upsert(PocoSetting{ .key_id = 42, .val = "v1" }).has_value());
        ASSERT_TRUE(sdb.settings.upsert(PocoSetting{ .key_id = 42, .val = "v2" }).has_value());
        EXPECT_EQ(sdb.settings.count(), 1u);
        EXPECT_EQ(sdb.settings.find(42)->val, "v2");
    }

    // =========================================================================
    // Phase 14: Compile-Time Foreign-Key Relationship Navigation
    // =========================================================================

    TEST(Phase14ForeignKeyNavigationTest, QueryChildren_QueryParent_QueryManyToMany_EncapsulatedAndPoco)
    {
        // 1. Encapsulated domain entities (Core::Person, Core::Something, Core::Person_Something)
        {
            auto connRes = DAL::SQLiteConnection::connect(":memory:");
            ASSERT_TRUE(connRes.has_value());

            struct NavDbContext : public DAL::DbContextBase<DAL::SQLiteConnection>
            {
                Set<Core::Person> people{ this };
                Set<Core::Something> somethings{ this };
                Set<Core::Person_Something> personSomethings{ this };

                explicit NavDbContext(std::shared_ptr<DAL::SQLiteConnection> c)
                    : DAL::DbContextBase<DAL::SQLiteConnection>(std::move(c))
                {}
            };

            NavDbContext db(connRes.value());
            ASSERT_TRUE(db.ensure_created().has_value());

            Core::Person alice(Core::PersonId{ 1 }, "Alice", "Smith");
            Core::Something laptop(Core::SomethingId{ 1 }, "Laptop", "Electronics", "Workstation");
            Core::Something coffee(Core::SomethingId{ 2 }, "Coffee", "Appliances", "Espresso");
            Core::Person_Something link1(Core::PersonId{ 1 }, Core::SomethingId{ 1 }, "Owner");
            Core::Person_Something link2(Core::PersonId{ 1 }, Core::SomethingId{ 2 }, "User");

            ASSERT_TRUE(db.people.add(alice).has_value());
            ASSERT_TRUE(db.somethings.add(laptop).has_value());
            ASSERT_TRUE(db.somethings.add(coffee).has_value());
            ASSERT_TRUE(db.personSomethings.add(link1).has_value());
            ASSERT_TRUE(db.personSomethings.add(link2).has_value());

            auto childrenRes = db.query_children<Core::Person_Something>(alice);
            ASSERT_TRUE(childrenRes.has_value()) << childrenRes.error().to_string();
            EXPECT_EQ(childrenRes->size(), 2u);

            auto parentRes = db.query_parent<Core::Person>(link1);
            ASSERT_TRUE(parentRes.has_value()) << parentRes.error().to_string();
            ASSERT_TRUE(parentRes->has_value());
            EXPECT_EQ((*parentRes)->getFirstName(), "Alice");

            auto m2mRes = db.query_many_to_many<Core::Something, Core::Person_Something>(alice);
            ASSERT_TRUE(m2mRes.has_value()) << m2mRes.error().to_string();
            ASSERT_EQ(m2mRes->size(), 2u);
        }

        // 2. Zero-DTO POCO entities (PocoUser, PocoRole, PocoUserRole)
        {
            auto connRes = DAL::SQLiteConnection::connect(":memory:");
            ASSERT_TRUE(connRes.has_value());

            PocoDbContext db(connRes.value());
            ASSERT_TRUE(db.ensure_created().has_value());

            ASSERT_TRUE(db.users.add(PocoUser{ .username = "alice" }).has_value());
            ASSERT_TRUE(db.roles.add(PocoRole{ .role_name = "Admin" }).has_value());
            ASSERT_TRUE(db.roles.add(PocoRole{ .role_name = "Reviewer" }).has_value());
            ASSERT_TRUE(db.userRoles.add(PocoUserRole{ .user_id = 1, .role_id = 1, .assigned_by = "sys" }).has_value());
            ASSERT_TRUE(db.userRoles.add(PocoUserRole{ .user_id = 1, .role_id = 2, .assigned_by = "sys" }).has_value());

            auto alice = db.users.find(1).value();
            auto links = db.query_children<PocoUserRole>(alice);
            ASSERT_TRUE(links.has_value());
            EXPECT_EQ(links->size(), 2u);

            auto parentUser = db.query_parent<PocoUser>(links->front());
            ASSERT_TRUE(parentUser.has_value());
            ASSERT_TRUE(parentUser->has_value());
            EXPECT_EQ((*parentUser)->username, "alice");

            auto roles = db.query_many_to_many<PocoRole, PocoUserRole>(alice);
            ASSERT_TRUE(roles.has_value());
            ASSERT_EQ(roles->size(), 2u);
        }
    }

    // =========================================================================
    // Phase 15: Universal STL & Strong-ID Primary Keys
    // =========================================================================

    struct PocoDocument
    {
        static constexpr auto tablename = "PocoDocuments";
        sqlgen::PrimaryKey<std::string> doc_slug;
        std::string title;
    };

    struct PocoEvent
    {
        static constexpr auto tablename = "PocoEvents";
        sqlgen::PrimaryKey<std::int64_t, false> event_id{ 0 };
        std::string name;
    };

    struct PocoTenantMetric
    {
        static constexpr auto tablename = "PocoTenantMetrics";
        sqlgen::ForeignKey<std::string, PocoDocument, "doc_slug"> tenant;
        sqlgen::ForeignKey<std::int64_t, PocoEvent, "event_id"> metric_id{ 0 };
        std::string label;
    };

    struct UniversalKeyDbContext : public DAL::DbContextBase<DAL::SQLiteConnection>
    {
        Set<PocoDocument> documents{ this };
        Set<PocoEvent> events{ this };
        Set<PocoTenantMetric> metrics{ this };

        explicit UniversalKeyDbContext(std::shared_ptr<DAL::SQLiteConnection> c)
            : DAL::DbContextBase<DAL::SQLiteConnection>(std::move(c))
        {}
    };

    TEST(Phase15UniversalPrimaryKeysTest, String_Int64_StrongId_And_HeterogeneousCompositeKeys)
    {
        using DocId = Core::Id<struct DocTag, std::string>;
        DocId strongDocId("doc-alpha");
        EXPECT_EQ(strongDocId.get(), "doc-alpha");
        EXPECT_EQ(std::format("{}", strongDocId), "doc-alpha");

        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());

        UniversalKeyDbContext db(connRes.value());
        auto ensureRes = db.ensure_created();
        ASSERT_TRUE(ensureRes.has_value()) << ensureRes.error().to_string();
        db.enable_change_tracking(true);

        // 1. std::string PrimaryKey
        ASSERT_TRUE(db.documents.add(PocoDocument{ .doc_slug = "readme", .title = "Intro" }).has_value());
        EXPECT_TRUE(db.documents.contains(std::string("readme")));
        auto docFind = db.documents.try_find(std::string("readme"));
        ASSERT_TRUE(docFind.has_value());
        ASSERT_TRUE(docFind->has_value());
        EXPECT_EQ((*docFind)->title, "Intro");

        auto trackedDoc = db.documents.find_tracked(std::string("readme"));
        ASSERT_NE(trackedDoc, nullptr);
        trackedDoc->title = "Intro V2";
        ASSERT_TRUE(db.save_changes().has_value());
        EXPECT_EQ(db.documents.find(std::string("readme"))->title, "Intro V2");

        ASSERT_TRUE(db.documents.upsert(PocoDocument{ .doc_slug = "readme", .title = "Intro V3" }).has_value());
        EXPECT_EQ(db.documents.find(std::string("readme"))->title, "Intro V3");

        ASSERT_TRUE(db.documents.remove(std::string("readme")).has_value());
        EXPECT_FALSE(db.documents.contains(std::string("readme")));

        // 2. std::int64_t PrimaryKey (large 64-bit value beyond int32_t range)
        const std::int64_t bigId = 5'000'000'123LL;
        ASSERT_TRUE(db.events.add(PocoEvent{ .event_id = bigId, .name = "Launch" }).has_value());
        EXPECT_TRUE(db.events.contains(bigId));
        auto ev = db.events.find(bigId);
        ASSERT_TRUE(ev.has_value());
        EXPECT_EQ(ev->name, "Launch");

        // 3. Heterogeneous CompositeKey<std::string, std::int64_t>
        ASSERT_TRUE(db.documents.add(PocoDocument{ .doc_slug = "acme", .title = "Acme" }).has_value());
        ASSERT_TRUE(db.metrics.add(PocoTenantMetric{
            .tenant = "acme",
            .metric_id = bigId,
            .label = "requests"
        }).has_value());
        EXPECT_TRUE(db.metrics.contains(std::string("acme"), bigId));
        auto metric = db.metrics.find(std::string("acme"), bigId);
        ASSERT_TRUE(metric.has_value());
        EXPECT_EQ(metric->label, "requests");
        ASSERT_TRUE(db.metrics.remove(std::string("acme"), bigId).has_value());
        EXPECT_FALSE(db.metrics.contains(std::string("acme"), bigId));
    }

    // =========================================================================
    // Phase 16: Compile-Time Domain-to-DTO Column Resolution & Validation
    // =========================================================================

    TEST(Phase16DomainColumnResolutionTest, ValidQueryColumnConceptAndDomainFieldQueryPushdown)
    {
        static_assert(DAL::Mappers::ValidQueryColumn<Core::Person, DAL::Schema::PersonDTO, "first_name">);
        static_assert(DAL::Mappers::ValidQueryColumn<Core::Person, DAL::Schema::PersonDTO, "firstName">);
        static_assert(DAL::Mappers::ValidQueryColumn<Core::Person, DAL::Schema::PersonDTO, "firstName_">);
        static_assert(!DAL::Mappers::ValidQueryColumn<Core::Person, DAL::Schema::PersonDTO, "nonExistentCol">);

        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());

        struct PersonColDbContext : public DAL::DbContextBase<DAL::SQLiteConnection>
        {
            Set<Core::Person> people{ this };
            explicit PersonColDbContext(std::shared_ptr<DAL::SQLiteConnection> c)
                : DAL::DbContextBase<DAL::SQLiteConnection>(std::move(c))
            {}
        };

        PersonColDbContext db(connRes.value());
        ASSERT_TRUE(db.ensure_created().has_value());
        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 1 }, "Charlie", "Smith")).has_value());
        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 2 }, "Alice", "Smith")).has_value());
        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 3 }, "Diana", "Jones")).has_value());

        auto smiths = db.people
            .where_equal<"lastName">("Smith")
            .order_by<"firstName">(false)
            .to_vector();
        ASSERT_EQ(smiths.size(), 2u);
        EXPECT_EQ(smiths[0].getFirstName(), "Alice");
        EXPECT_EQ(smiths[1].getFirstName(), "Charlie");
    }

    // =========================================================================
    // Phase 17: Batched WHERE pk IN (vector) & remove_where Tracker Sync
    // =========================================================================

    TEST(Phase17BatchedManyToManyAndRemoveWhereSyncTest, WhereInVec_BatchedManyToMany_And_TrackerEviction)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());

        struct NavBatchDbContext : public DAL::DbContextBase<DAL::SQLiteConnection>
        {
            Set<Core::Person> people{ this };
            Set<Core::Something> somethings{ this };
            Set<Core::Person_Something> personSomethings{ this };

            explicit NavBatchDbContext(std::shared_ptr<DAL::SQLiteConnection> c)
                : DAL::DbContextBase<DAL::SQLiteConnection>(std::move(c))
            {}
        };

        NavBatchDbContext db(connRes.value());
        ASSERT_TRUE(db.ensure_created().has_value());
        db.enable_change_tracking(true);

        Core::Person p1(Core::PersonId{ 1 }, "Alice", "Smith");
        Core::Person p2(Core::PersonId{ 2 }, "Bob", "Smith");
        Core::Person p3(Core::PersonId{ 3 }, "Charlie", "Jones");
        Core::Person p4(Core::PersonId{ 4 }, "Diana", "Jones");
        Core::Person p5(Core::PersonId{ 5 }, "Eve", "Brown");
        ASSERT_TRUE(db.people.add_range({ p1, p2, p3, p4, p5 }).has_value());

        // where_in_vec with raw ints and Core::PersonId
        auto byInts = db.people.where_in_vec<"id">(std::vector<int>{ 1, 3, 5 }).order_by<"id">(false).to_vector();
        ASSERT_EQ(byInts.size(), 3u);
        EXPECT_EQ(byInts[0].getFirstName(), "Alice");
        EXPECT_EQ(byInts[1].getFirstName(), "Charlie");
        EXPECT_EQ(byInts[2].getFirstName(), "Eve");

        auto byIds = db.people.where_in_vec<"id">(std::vector<Core::PersonId>{ Core::PersonId{ 2 }, Core::PersonId{ 4 } }).to_vector();
        ASSERT_EQ(byIds.size(), 2u);

        // Batched query_many_to_many with 0, 1, and 2 targets
        Core::Something s1(Core::SomethingId{ 1 }, "Laptop", "Electronics", "Work");
        Core::Something s2(Core::SomethingId{ 2 }, "Desk", "Furniture", "Office");
        ASSERT_TRUE(db.somethings.add_range({ s1, s2 }).has_value());
        ASSERT_TRUE(db.personSomethings.add(Core::Person_Something(Core::PersonId{ 1 }, Core::SomethingId{ 1 }, "Owner")).has_value());
        ASSERT_TRUE(db.personSomethings.add(Core::Person_Something(Core::PersonId{ 1 }, Core::SomethingId{ 2 }, "Owner")).has_value());
        ASSERT_TRUE(db.personSomethings.add(Core::Person_Something(Core::PersonId{ 2 }, Core::SomethingId{ 1 }, "User")).has_value());

        EXPECT_EQ((db.query_many_to_many<Core::Something, Core::Person_Something>(p1)->size()), 2u);
        EXPECT_EQ((db.query_many_to_many<Core::Something, Core::Person_Something>(p2)->size()), 1u);
        EXPECT_EQ((db.query_many_to_many<Core::Something, Core::Person_Something>(p3)->size()), 0u);

        // remove_where() invalidates tracked entries so save_changes() does not resurrect them
        auto trackedP5 = db.people.find_tracked(5);
        ASSERT_NE(trackedP5, nullptr);
        trackedP5->setFirstName("EveMutated");
        auto remRes = db.people.where_equal<"lastName">("Brown").remove_where();
        ASSERT_TRUE(remRes.has_value()) << remRes.error().to_string();
        ASSERT_TRUE(db.save_changes().has_value());
        EXPECT_FALSE(db.people.find(5).has_value());
        EXPECT_EQ(db.people.count(), 4u);
    }

    // =========================================================================
    // Phase 18: Repository Modernization & SQLitePersonRepo Pushdown Fix
    // =========================================================================

    TEST(Phase18RepositoryModernizationTest, SQLitePersonRepo_FindByLastName_WithLiveSQLite)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());
        ASSERT_TRUE(connRes.value()->ensure_table_created<DAL::Schema::PersonDTO>().has_value());

        DAL::Repositories::SQLitePersonRepo<DAL::SQLiteConnection> repo(connRes.value());
        ASSERT_TRUE(repo.insert_one(Core::Person(Core::PersonId{ 1 }, "Alice", "Smith")).has_value());
        ASSERT_TRUE(repo.insert_one(Core::Person(Core::PersonId{ 2 }, "Bob", "Jones")).has_value());
        ASSERT_TRUE(repo.insert_one(Core::Person(Core::PersonId{ 3 }, "Charlie", "Smith")).has_value());

        auto smiths = repo.find_by_last_name("Smith");
        ASSERT_EQ(smiths.size(), 2u);
        EXPECT_EQ(smiths[0].getFirstName(), "Alice");
        EXPECT_EQ(smiths[1].getFirstName(), "Charlie");
    }

    // =========================================================================
    // Phase 20: Case-Insensitive Column Resolution
    // =========================================================================

    TEST(Phase20CaseInsensitiveColumnResolutionTest, ResolvesSnakeCamelPascalAndTrailingUnderscore)
    {
        static_assert(DAL::Mappers::ValidQueryColumn<Core::Person, DAL::Schema::PersonDTO, "first_name">);
        static_assert(DAL::Mappers::ValidQueryColumn<Core::Person, DAL::Schema::PersonDTO, "firstName">);
        static_assert(DAL::Mappers::ValidQueryColumn<Core::Person, DAL::Schema::PersonDTO, "FirstName">);
        static_assert(DAL::Mappers::ValidQueryColumn<Core::Person, DAL::Schema::PersonDTO, "firstName_">);
        static_assert(!DAL::Mappers::ValidQueryColumn<Core::Person, DAL::Schema::PersonDTO, "non_existent_col">);

        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());
        ASSERT_TRUE(connRes.value()->ensure_table_created<DAL::Schema::PersonDTO>().has_value());
        DAL::DbSet<Core::Person, DAL::Schema::PersonDTO, DAL::SQLiteConnection> people(connRes.value());
        ASSERT_TRUE(people.add(Core::Person(Core::PersonId{ 1 }, "Alice", "Smith")).has_value());

        EXPECT_EQ(people.where_equal<"FirstName">("Alice").count(), 1u);
        EXPECT_EQ(people.where_equal<"firstName">("Alice").count(), 1u);
        EXPECT_EQ(people.where_equal<"firstName_">("Alice").count(), 1u);
        EXPECT_EQ(people.where_equal<"first_name">("Alice").count(), 1u);
    }

    // =========================================================================
    // Phase 21: Nullable/Pattern Predicates & SQL Aggregations
    // =========================================================================

    struct Phase21Product
    {
        static constexpr const char* tablename = "phase21_products";
        sqlgen::PrimaryKey<std::int32_t, true> product_id{ 0 };
        std::string name{};
        std::optional<std::string> notes{};
        int score{ 0 };
        double price{ 0.0 };
    };

    TEST(Phase21NullPatternAndAggregationTest, NullablePredicatesAndAggregations)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());
        auto conn = connRes.value();
        ASSERT_TRUE(conn->ensure_table_created<Phase21Product>().has_value());

        DAL::DbSet<Phase21Product, Phase21Product, DAL::SQLiteConnection> products(conn);

        // Empty table returns std::nullopt for aggregations
        EXPECT_FALSE(products.min<"score">().has_value());
        EXPECT_FALSE(products.max<"score">().has_value());
        EXPECT_FALSE(products.sum<"score">().has_value());
        EXPECT_FALSE(products.avg<"price">().has_value());

        ASSERT_TRUE(products.add(Phase21Product{ .name = "Alpha", .notes = "Featured", .score = 10, .price = 10.0 }).has_value());
        ASSERT_TRUE(products.add(Phase21Product{ .name = "Beta",  .notes = std::nullopt, .score = 20, .price = 25.0 }).has_value());
        ASSERT_TRUE(products.add(Phase21Product{ .name = "Gamma", .notes = "Clearance", .score = 30, .price = 55.0 }).has_value());

        EXPECT_EQ(products.where_null<"notes">().count(), 1u);
        EXPECT_EQ(products.where_not_null<"notes">().count(), 2u);
        EXPECT_EQ(products.where_not_like<"name">("A%").count(), 2u);
        EXPECT_EQ(products.where_not_in<"score">(10, 30).count(), 1u);

        EXPECT_EQ(products.min<"score">(), std::optional<int>(10));
        EXPECT_EQ(products.max<"score">(), std::optional<int>(30));
        EXPECT_EQ(products.sum<"score">(), std::optional<int>(60));
        ASSERT_TRUE(products.avg<"price">().has_value());
        EXPECT_NEAR(*products.avg<"price">(), 30.0, 1e-6);

        // Filtered aggregation
        EXPECT_EQ(products.where_not_null<"notes">().sum<"score">(), std::optional<int>(40));
        EXPECT_EQ(products.where_not_null<"notes">().min<"score">(), std::optional<int>(10));
        EXPECT_EQ(products.where_not_null<"notes">().max<"score">(), std::optional<int>(30));
    }

    // =========================================================================
    // Phase 22: Auto-Increment ID Retrieval (add_returning)
    // =========================================================================

    struct Phase22AutoUser
    {
        static constexpr const char* tablename = "phase22_users";
        sqlgen::PrimaryKey<std::int32_t, true> user_id{ 0 };
        std::string username{};
    };

    TEST(Phase22AddReturningTest, PopulatesAutoIncrementPrimaryKey)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());
        auto conn = connRes.value();
        ASSERT_TRUE(conn->ensure_table_created<Phase22AutoUser>().has_value());

        DAL::DbSet<Phase22AutoUser, Phase22AutoUser, DAL::SQLiteConnection> users(conn);

        auto aliceRes = users.add_returning(Phase22AutoUser{ .username = "alice" });
        ASSERT_TRUE(aliceRes.has_value()) << aliceRes.error().to_string();
        EXPECT_EQ(aliceRes->user_id.value(), 1);
        EXPECT_EQ(aliceRes->username, "alice");

        auto bobRes = users.add_returning(Phase22AutoUser{ .username = "bob" });
        ASSERT_TRUE(bobRes.has_value()) << bobRes.error().to_string();
        EXPECT_EQ(bobRes->user_id.value(), 2);
        EXPECT_EQ(bobRes->username, "bob");

        // Also test encapsulated Core::Person via DbSet
        ASSERT_TRUE(conn->ensure_table_created<DAL::Schema::PersonDTO>().has_value());
        DAL::DbSet<Core::Person, DAL::Schema::PersonDTO, DAL::SQLiteConnection> people(conn);
        auto pRes = people.add_returning(Core::Person(Core::PersonId{ 0 }, "Jane", "Doe"));
        ASSERT_TRUE(pRes.has_value()) << pRes.error().to_string();
        EXPECT_EQ(pRes->getId().value, 1);
        EXPECT_EQ(pRes->getFirstName(), "Jane");
    }

} // namespace
