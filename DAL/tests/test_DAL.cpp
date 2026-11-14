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
                    if (item.id.value() == id)
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
                    if (elem.id.value() == item.id.value())
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

        sqlgen::Result<sqlgen::Nothing> delete_by_composite_id(std::int32_t personId, std::int32_t somethingId)
        {
            auto& storage = person_somethings_;
            std::erase_if(
                storage,
                [personId, somethingId](const DAL::Schema::Person_SomethingDTO& item)
                {
                    return item.person_id.value() == personId && item.something_id.value() == somethingId;
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
        std::vector<DAL::Schema::PersonDTO> persons_;
        std::vector<DAL::Schema::SomethingDTO> somethings_;
        std::vector<DAL::Schema::Person_SomethingDTO> person_somethings_;
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

} // namespace
