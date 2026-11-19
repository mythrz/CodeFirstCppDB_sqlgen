#include <gtest/gtest.h>
#include <sqlgen.hpp>
#include <sqlgen/sqlite.hpp>

import std;
import core;
import dal;
import samples;

#include "TestSupport.hpp"

namespace
{
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

    struct Phase31Member
    {
        static constexpr const char* tablename = "phase31_members";
        sqlgen::PrimaryKey<std::int32_t, true> id{ 0 };
        std::string email{};
        std::optional<std::string> nickname{};
    };

    TEST(Phase31IndexAndPartialIndexDdlTest, CompositeUniqueAndPartialIndexes)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());
        auto conn = connRes.value();
        ASSERT_TRUE(conn->ensure_table_created<DAL::Schema::PersonDTO>().has_value());
        ASSERT_TRUE(conn->ensure_table_created<Phase31Member>().has_value());

        DAL::DbSet<Core::Person, DAL::Schema::PersonDTO, DAL::SQLiteConnection> people(conn);
        ASSERT_TRUE((people.ensure_index_created<"idx_person_name", "lastName", "firstName">().has_value()));
        ASSERT_TRUE((people.ensure_unique_index_created<"idx_person_unique_name", "firstName", "lastName">().has_value()));

        ASSERT_TRUE(people.add(Core::Person(Core::PersonId{ 1 }, "Ada", "Lovelace")).has_value());
        auto dupPerson = people.add(Core::Person(Core::PersonId{ 2 }, "Ada", "Lovelace"));
        ASSERT_FALSE(dupPerson.has_value());
        EXPECT_EQ(dupPerson.error().code, Core::DbErrorCode::DuplicateKey);

        // Partial unique index: multiple NULL nicknames allowed, duplicate non-NULL fails
        DAL::DbSet<Phase31Member, Phase31Member, DAL::SQLiteConnection> members(conn);
        ASSERT_TRUE((members.where_not_null<"nickname">().ensure_unique_index_created<"idx_unique_nickname", "nickname">().has_value()));

        ASSERT_TRUE(members.add(Phase31Member{ .email = "a@example.com", .nickname = std::nullopt }).has_value());
        ASSERT_TRUE(members.add(Phase31Member{ .email = "b@example.com", .nickname = std::nullopt }).has_value());
        ASSERT_TRUE(members.add(Phase31Member{ .email = "c@example.com", .nickname = "ada" }).has_value());
        auto dupNick = members.add(Phase31Member{ .email = "d@example.com", .nickname = "ada" });
        ASSERT_FALSE(dupNick.has_value());
        EXPECT_EQ(dupNick.error().code, Core::DbErrorCode::DuplicateKey);
    }

    // =========================================================================
    // Phase 32: Multi-Column Server-Side Tuple Projections
    // =========================================================================

    TEST(Phase37SqliteHardeningTest, CompositePkUniqueAndFkEnforcementOnFreshConnect)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());
        Phase30DbContext db(connRes.value());
        ASSERT_TRUE(db.ensure_created().has_value());

        // Foreign keys must be enforced automatically on fresh connect()
        auto badFk = db.personSomethings.add(Core::Person_Something(Core::PersonId{ 999 }, Core::SomethingId{ 999 }, "Invalid"));
        ASSERT_FALSE(badFk.has_value());

        auto pRes = db.people.add(Core::Person(Core::PersonId{ 0 }, "Ada", "Lovelace"));
        ASSERT_TRUE(pRes.has_value()) << pRes.error().to_string();
        auto sRes = db.somethings.add(Core::Something(Core::SomethingId{ 0 }, "Engine", "Tech"));
        ASSERT_TRUE(sRes.has_value()) << sRes.error().to_string();

        const auto pid = Core::PersonId{ 1 };
        const auto sid = Core::SomethingId{ 1 };

        auto okAssoc = db.personSomethings.add(Core::Person_Something(pid, sid, "Creator"));
        ASSERT_TRUE(okAssoc.has_value()) << okAssoc.error().to_string();

        // Duplicate composite key must fail with DuplicateKey
        auto dupAssoc = db.personSomethings.add(Core::Person_Something(pid, sid, "Duplicate"));
        ASSERT_FALSE(dupAssoc.has_value());
        EXPECT_EQ(dupAssoc.error().code, Core::DbErrorCode::DuplicateKey);

        // Update existing composite-key row in-place
        auto updateAssoc = db.personSomethings.update(Core::Person_Something(pid, sid, "Maintainer"));
        ASSERT_TRUE(updateAssoc.has_value()) << updateAssoc.error().to_string();

        auto all = db.personSomethings.to_vector();
        ASSERT_EQ(all.size(), 1u);
        EXPECT_EQ(all[0].getAssociationType(), "Maintainer");
    }

    // =========================================================================
    // Phase 38: Runtime SQLite Schema & Table Introspection API
    // =========================================================================

    struct CompositePkPairDTO
    {
        static constexpr const char* tablename = "CompositePkPairs";
        sqlgen::PrimaryKey<std::string, false> first{};
        sqlgen::PrimaryKey<std::string, false> second{};
        std::string value{};
    };

    TEST(Phase38SchemaIntrospectionTest, ListTablesTableInfoAndExecuteRaw)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());
        Phase30DbContext db(connRes.value());
        ASSERT_TRUE(db.ensure_created().has_value());

        auto tablesRes = db.list_tables();
        ASSERT_TRUE(tablesRes.has_value()) << tablesRes.error().to_string();
        const auto& tables = *tablesRes;
        EXPECT_NE(std::ranges::find(tables, std::string_view("Person")), tables.end());
        EXPECT_NE(std::ranges::find(tables, std::string_view("Something")), tables.end());
        EXPECT_NE(std::ranges::find(tables, std::string_view("Person_Something")), tables.end());

        auto personColsRes = db.table_info("Person");
        ASSERT_TRUE(personColsRes.has_value()) << personColsRes.error().to_string();
        ASSERT_EQ(personColsRes->size(), 3u);
        EXPECT_EQ((*personColsRes)[0].name, "id");
        EXPECT_EQ((*personColsRes)[0].pk_index, 1);
        EXPECT_EQ((*personColsRes)[1].name, "first_name");
        EXPECT_EQ((*personColsRes)[2].name, "last_name");

        auto somethingColsRes = db.table_info("Something");
        ASSERT_TRUE(somethingColsRes.has_value()) << somethingColsRes.error().to_string();
        ASSERT_EQ(somethingColsRes->size(), 4u);
        EXPECT_EQ((*somethingColsRes)[3].name, "description");
        EXPECT_FALSE((*somethingColsRes)[3].not_null);

        auto psColsRes = db.table_info("Person_Something");
        ASSERT_TRUE(psColsRes.has_value()) << psColsRes.error().to_string();
        ASSERT_EQ(psColsRes->size(), 3u);
        EXPECT_EQ((*psColsRes)[0].name, "person_id");
        EXPECT_EQ((*psColsRes)[0].pk_index, 1);
        EXPECT_EQ((*psColsRes)[1].name, "something_id");
        EXPECT_EQ((*psColsRes)[1].pk_index, 2);
        EXPECT_EQ((*psColsRes)[2].name, "association_type");
        EXPECT_EQ((*psColsRes)[2].pk_index, 0);

        ASSERT_TRUE(connRes.value()->ensure_table_created<CompositePkPairDTO>().has_value());
        auto pairColsRes = db.table_info("CompositePkPairs");
        ASSERT_TRUE(pairColsRes.has_value()) << pairColsRes.error().to_string();
        ASSERT_EQ(pairColsRes->size(), 3u);
        EXPECT_EQ((*pairColsRes)[0].name, "first");
        EXPECT_EQ((*pairColsRes)[0].pk_index, 1);
        EXPECT_EQ((*pairColsRes)[1].name, "second");
        EXPECT_EQ((*pairColsRes)[1].pk_index, 2);
        EXPECT_EQ((*pairColsRes)[2].name, "value");
        EXPECT_EQ((*pairColsRes)[2].pk_index, 0);
        DAL::DbSet<CompositePkPairDTO, CompositePkPairDTO, DAL::SQLiteConnection> pairSet(connRes.value());
        EXPECT_TRUE(pairSet.add(CompositePkPairDTO{ .first = "a", .second = "b", .value = "v1" }).has_value());
        EXPECT_TRUE(pairSet.add(CompositePkPairDTO{ .first = "a", .second = "c", .value = "v2" }).has_value());
        auto dupPair = pairSet.add(CompositePkPairDTO{ .first = "a", .second = "b", .value = "dup" });
        ASSERT_FALSE(dupPair.has_value());
        EXPECT_EQ(dupPair.error().code, Core::DbErrorCode::DuplicateKey);

        EXPECT_TRUE(db.execute_raw("CREATE INDEX IF NOT EXISTS idx_person_last ON Person (last_name);").has_value());
        EXPECT_FALSE(db.execute_raw("").has_value());
    }

    // =========================================================================
    // Phase 39: Transactional Correctness, Strong-ID Safety & Lifecycle Hardening
    // =========================================================================

    TEST(Phase40ErrorCodesAndOptionsTest, ExtendedNativeCodesAndConnectionOptions)
    {
        DAL::SQLiteConnectionOptions opts{ .foreign_keys = true, .busy_timeout = std::chrono::milliseconds{ 2500 }, .wal_mode = false };
        auto connRes = DAL::SQLiteConnection::connect(":memory:", opts);
        ASSERT_TRUE(connRes.has_value());
        Phase30DbContext db(connRes.value());
        ASSERT_TRUE(db.ensure_created().has_value());

        // Trigger foreign key violation and verify native_code is populated (SQLITE_CONSTRAINT_FOREIGNKEY = 787)
        auto fkErr = db.personSomethings.add(Core::Person_Something(Core::PersonId{ 100 }, Core::SomethingId{ 200 }, "BadFK"));
        ASSERT_FALSE(fkErr.has_value());
        EXPECT_EQ(fkErr.error().code, Core::DbErrorCode::ConstraintViolation);
        EXPECT_NE(fkErr.error().native_code, 0);
    }

    // =========================================================================
    // Phase 41: Parameterized Dynamic Row Query API (query_rows / execute_prepared)
    // =========================================================================

    TEST(Phase41DynamicQueryTest, QueryRowsAndExecutePreparedWithAllCellTypes)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());
        Phase30DbContext db(connRes.value());

        ASSERT_TRUE(
            db.execute_raw("CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT, price REAL, note TEXT, payload BLOB);").has_value());

        const std::vector<std::byte> blobData{ std::byte{ 0xDE }, std::byte{ 0xAD }, std::byte{ 0xBE }, std::byte{ 0xEF } };
        const std::array<DAL::DynamicCell, 5> insertParams{ std::int64_t{ 1 }, std::string{ "Widget" }, 19.99, std::monostate{}, blobData };
        ASSERT_TRUE(db.execute_prepared(
                          "INSERT INTO items (id, name, price, note, payload) VALUES (?, ?, ?, ?, ?);",
                          std::span<const DAL::DynamicCell>{ insertParams })
                        .has_value());

        const std::array<DAL::DynamicCell, 1> queryParams{ std::string{ "Widget" } };
        auto res = db.query_rows(
            "SELECT id, name, price, note, payload FROM items WHERE name = ?;", std::span<const DAL::DynamicCell>{ queryParams });
        ASSERT_TRUE(res.has_value()) << res.error().to_string();
        ASSERT_EQ(res->columns.size(), 5u);
        EXPECT_EQ(res->columns[1], "name");
        ASSERT_EQ(res->rows.size(), 1u);

        const auto& cells = res->rows[0].cells;
        EXPECT_EQ(std::get<std::int64_t>(cells[0]), 1);
        EXPECT_EQ(std::get<std::string>(cells[1]), "Widget");
        EXPECT_DOUBLE_EQ(std::get<double>(cells[2]), 19.99);
        EXPECT_TRUE(std::holds_alternative<std::monostate>(cells[3]));
        EXPECT_EQ(std::get<std::vector<std::byte>>(cells[4]), blobData);
    }

    TEST(Phase43ReopenForeignKeyTest, ReopeningExistingDbWithoutEnsureCreatedEnforcesForeignKeys)
    {
        const auto tempDir = std::filesystem::temp_directory_path();
        const auto dbPath = (tempDir / "codefirst_phase43_fk_reopen.db").string();
        std::filesystem::remove(dbPath);

        {
            auto connRes = DAL::SQLiteConnection::connect(dbPath);
            ASSERT_TRUE(connRes.has_value());
            auto& conn = *connRes.value();
            ASSERT_TRUE(conn.execute_raw("CREATE TABLE Parent (id INTEGER PRIMARY KEY);").has_value());
            ASSERT_TRUE(conn.execute_raw("CREATE TABLE Child (id INTEGER PRIMARY KEY, parent_id INTEGER NOT NULL REFERENCES Parent(id));")
                            .has_value());
            ASSERT_TRUE(conn.execute_raw("INSERT INTO Parent (id) VALUES (1);").has_value());
            conn.close();
        }

        {
            // Reopen without calling ensure_created() or any schema DDL
            auto reopenedRes = DAL::SQLiteConnection::connect(dbPath);
            ASSERT_TRUE(reopenedRes.has_value());
            auto& reopened = *reopenedRes.value();

            auto fkRes = reopened.query_rows("PRAGMA foreign_keys;");
            ASSERT_TRUE(fkRes.has_value());
            ASSERT_EQ(fkRes->rows.size(), 1u);
            ASSERT_EQ(std::get<std::int64_t>(fkRes->rows[0].cells[0]), 1);

            // Inserting a child referencing a missing parent must fail with ConstraintViolation
            auto badInsert = reopened.execute_raw("INSERT INTO Child (id, parent_id) VALUES (10, 999);");
            ASSERT_FALSE(badInsert.has_value());
            EXPECT_EQ(badInsert.error().code, Core::DbErrorCode::ConstraintViolation);

            reopened.close();
        }

        std::filesystem::remove(dbPath);
    }
} // namespace
