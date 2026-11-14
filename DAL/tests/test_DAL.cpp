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

} // namespace
