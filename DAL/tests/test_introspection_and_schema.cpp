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
        const auto dbPath = make_unique_temp_db_path("codefirst_phase43_fk_reopen");
        const ScopedTempDbCleanup cleanup{ dbPath };

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
    }

    // =========================================================================
    // Phase 50 (P1): Transactional ensure_created() & PRAGMA user_version Hooks
    // =========================================================================

    struct FirstGoodSchemaDTO
    {
        static constexpr auto tablename = "FirstGoodSchema";
        sqlgen::PrimaryKey<std::int32_t, true> id{ 0 };
        std::string name;
    };

    struct SecondBadSchemaDTO
    {
        // Deliberately invalid quoted table name to force DDL failure on second table.
        static constexpr auto tablename = "Bad\"Table";
        sqlgen::PrimaryKey<std::int32_t, true> id{ 0 };
        std::string name;
    };

    struct SecondReservedSqliteSchemaDTO
    {
        // Valid identifier that passes client-side validation and triggers a genuine SQLite DDL error
        // ("object name reserved for internal use: sqlite_reserved_ddl_fail").
        static constexpr auto tablename = "sqlite_reserved_ddl_fail";
        sqlgen::PrimaryKey<std::int32_t, true> id{ 0 };
        std::string name;
    };

    struct PartialFailSchemaContext : DAL::DbContextBase<DAL::SQLiteConnection>
    {
        Set<FirstGoodSchemaDTO> first{ this };
        Set<SecondBadSchemaDTO> second{ this };

        explicit PartialFailSchemaContext(std::shared_ptr<DAL::SQLiteConnection> c)
            : DAL::DbContextBase<DAL::SQLiteConnection>(std::move(c))
        {
        }
    };

    struct SqliteLevelFailSchemaContext : DAL::DbContextBase<DAL::SQLiteConnection>
    {
        Set<FirstGoodSchemaDTO> first{ this };
        Set<SecondReservedSqliteSchemaDTO> second{ this };

        explicit SqliteLevelFailSchemaContext(std::shared_ptr<DAL::SQLiteConnection> c)
            : DAL::DbContextBase<DAL::SQLiteConnection>(std::move(c))
        {
        }
    };

    TEST(Phase50SchemaVersioningTest, EnsureCreatedIsTransactionalAndRollsBackEarlierTablesOnFailure)
    {
        // 1. Client-side identifier validation failure rolls back earlier tables.
        {
            auto connRes = DAL::SQLiteConnection::connect(":memory:");
            ASSERT_TRUE(connRes.has_value());
            PartialFailSchemaContext db(connRes.value());

            auto res = db.ensure_created();
            ASSERT_FALSE(res.has_value());
            EXPECT_FALSE(db.in_transaction());

            auto tablesRes = db.list_tables();
            ASSERT_TRUE(tablesRes.has_value());
            EXPECT_FALSE(std::ranges::contains(*tablesRes, std::string_view{ "FirstGoodSchema" }));
        }

        // 2. Genuine SQLite-level DDL execution failure also rolls back earlier created tables.
        {
            auto connRes = DAL::SQLiteConnection::connect(":memory:");
            ASSERT_TRUE(connRes.has_value());
            SqliteLevelFailSchemaContext db(connRes.value());

            auto res = db.ensure_created();
            ASSERT_FALSE(res.has_value());
            EXPECT_FALSE(db.in_transaction());

            auto tablesRes = db.list_tables();
            ASSERT_TRUE(tablesRes.has_value());
            EXPECT_FALSE(std::ranges::contains(*tablesRes, std::string_view{ "FirstGoodSchema" }));
        }
    }

    TEST(Phase50SchemaVersioningTest, UserVersionAndTransactionalMigrateToHook)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());
        Phase30DbContext db(connRes.value());

        auto v0 = db.try_user_version();
        ASSERT_TRUE(v0.has_value());
        EXPECT_EQ(*v0, 0);

        // Upgrade 0 -> 1: creates base tables + adds an extra column via migration callback.
        int calls = 0;
        auto m1 = db.migrate_to(
            1,
            [&](std::int32_t from_v, std::int32_t to_v) -> std::expected<void, Core::DbError>
            {
                ++calls;
                EXPECT_EQ(from_v, 0);
                EXPECT_EQ(to_v, 1);
                return db.execute_raw("ALTER TABLE Person ADD COLUMN nickname TEXT;");
            });
        ASSERT_TRUE(m1.has_value()) << m1.error().to_string();
        EXPECT_EQ(calls, 1);
        EXPECT_EQ(db.try_user_version().value(), 1);

        // Idempotent re-run at version 1 must not re-invoke migration callback.
        auto m1Again = db.migrate_to(
            1,
            [&](std::int32_t, std::int32_t) -> std::expected<void, Core::DbError>
            {
                ++calls;
                return {};
            });
        ASSERT_TRUE(m1Again.has_value());
        EXPECT_EQ(calls, 1);

        // Failed migration 1 -> 2 must roll back both DDL changes and user_version.
        auto m2Fail = db.migrate_to(
            2,
            [&](std::int32_t from_v, std::int32_t to_v) -> std::expected<void, Core::DbError>
            {
                EXPECT_EQ(from_v, 1);
                EXPECT_EQ(to_v, 2);
                auto ok = db.execute_raw("CREATE TABLE TempV2 (id INTEGER PRIMARY KEY);");
                if (!ok)
                {
                    return ok;
                }
                return std::unexpected<Core::DbError>{ Core::DbError::unknown("abort v2 migration") };
            });
        ASSERT_FALSE(m2Fail.has_value());
        EXPECT_EQ(db.try_user_version().value(), 1);
        auto tablesAfterFail = db.list_tables();
        ASSERT_TRUE(tablesAfterFail.has_value());
        EXPECT_FALSE(std::ranges::contains(*tablesAfterFail, std::string_view{ "TempV2" }));

        // Downgrade 1 -> 0 must be rejected cleanly.
        auto downgrade = db.migrate_to(
            0,
            [](std::int32_t, std::int32_t) -> std::expected<void, Core::DbError>
            {
                return {};
            });
        ASSERT_FALSE(downgrade.has_value());
        EXPECT_EQ(downgrade.error().code, Core::DbErrorCode::ConstraintViolation);
        EXPECT_EQ(db.try_user_version().value(), 1);
    }

    // =========================================================================
    // Phase 54 (P1/P2): SQLite Error Fidelity, Float Precision & Escaping
    // =========================================================================

    struct ExactFloatDTO
    {
        static constexpr auto tablename = "ExactFloats";
        sqlgen::PrimaryKey<std::int32_t, false> id{ 0 };
        double val = 0.0;
    };

    struct ApostropheTableDTO
    {
        static constexpr auto tablename = "User'sNotes";
        sqlgen::PrimaryKey<std::int32_t, false> id{ 0 };
        std::string body;
    };

    struct Phase54DriverContext : DAL::DbContextBase<DAL::SQLiteConnection>
    {
        Set<ExactFloatDTO> floats{ this };
        Set<ApostropheTableDTO> notes{ this };

        explicit Phase54DriverContext(std::shared_ptr<DAL::SQLiteConnection> c)
            : DAL::DbContextBase<DAL::SQLiteConnection>(std::move(c))
        {
        }
    };

    TEST(Phase54SqliteDriverFidelityTest, TriggerRaiseMapsToConstraintViolationAndClientErrorIsNotStale)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());
        auto conn = connRes.value();
        Phase30DbContext db(conn);
        ASSERT_TRUE(db.ensure_created().has_value());

        // 1. Trigger RAISE(ABORT, 'custom rule') has SQLITE_CONSTRAINT_TRIGGER -> MUST map to ConstraintViolation.
        ASSERT_TRUE(conn->execute_raw(
                            "CREATE TRIGGER trg_reject_custom BEFORE INSERT ON Person WHEN NEW.first_name = 'Forbidden' "
                            "BEGIN SELECT RAISE(ABORT, 'custom business rule'); END;")
                        .has_value());
        auto trigRes = db.people.add(Core::Person(Core::PersonId{ 1 }, "Forbidden", "User"));
        ASSERT_FALSE(trigRes.has_value());
        EXPECT_EQ(trigRes.error().code, Core::DbErrorCode::ConstraintViolation);
        EXPECT_NE(trigRes.error().native_code, 0);

        // 2. Force a DuplicateKey error, then trigger client-side validation errors; stale SQLite errcode MUST NOT leak.
        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 10 }, "Alice", "One")).has_value());
        auto dupRes = db.people.add(Core::Person(Core::PersonId{ 10 }, "Alice", "Dup"));
        ASSERT_FALSE(dupRes.has_value());
        EXPECT_EQ(dupRes.error().code, Core::DbErrorCode::DuplicateKey);

        auto negTimeout = conn->set_busy_timeout(std::chrono::milliseconds{ -5 });
        ASSERT_FALSE(negTimeout.has_value());
        EXPECT_EQ(negTimeout.error().code, Core::DbErrorCode::ConstraintViolation);

        auto badDtoRes = conn->try_insert(SecondBadSchemaDTO{ .id = 1, .name = "x" });
        ASSERT_FALSE(badDtoRes.has_value());
        EXPECT_EQ(badDtoRes.error().code, Core::DbErrorCode::ConstraintViolation);
        EXPECT_EQ(badDtoRes.error().native_code, 0);

        // 3. Missing-row update directly returns NotFound without relying on sticky SQLite errcode.
        auto missingUpd = db.people.update(Core::Person(Core::PersonId{ 999 }, "Ghost", "User"));
        ASSERT_FALSE(missingUpd.has_value());
        EXPECT_EQ(missingUpd.error().code, Core::DbErrorCode::NotFound);
    }

    TEST(Phase54SqliteDriverFidelityTest, FloatDoubleRoundTripPreservesExactIeee754Precision)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());
        Phase54DriverContext db(connRes.value());
        ASSERT_TRUE(db.ensure_created().has_value());

        const std::vector<double> values{
            3.1415926535897931, 1.2345678901234567e-12, -9.8765432109876543e15, 0.1 + 0.2, 2.2250738585072014e-308
        };
        for (std::size_t i = 0; i < values.size(); ++i)
        {
            ASSERT_TRUE(db.floats.add(ExactFloatDTO{ .id = static_cast<std::int32_t>(i + 1), .val = values[i] }).has_value());
        }

        for (std::size_t i = 0; i < values.size(); ++i)
        {
            auto found = db.floats.find(static_cast<std::int32_t>(i + 1));
            ASSERT_TRUE(found.has_value());
            EXPECT_EQ(found->val, values[i]) << "Precision lost at index " << i;
        }
    }

    TEST(Phase54SqliteDriverFidelityTest, ApostropheQuotedTableWithEmbeddedNulWhereLiteralWorks)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());
        Phase54DriverContext db(connRes.value());
        ASSERT_TRUE(db.ensure_created().has_value());

        const std::string withNul("alpha\0beta", 10);
        ASSERT_TRUE(db.notes.add(ApostropheTableDTO{ .id = 1, .body = withNul }).has_value());
        ASSERT_TRUE(db.notes.add(ApostropheTableDTO{ .id = 2, .body = "alpha" }).has_value());

        auto matched = db.notes.where_equal<"body">(withNul).try_to_vector();
        ASSERT_TRUE(matched.has_value()) << matched.error().to_string();
        ASSERT_EQ(matched->size(), 1u);
        EXPECT_EQ(matched->front().id.value(), 1);
        EXPECT_EQ(matched->front().body, withNul);
    }

    // =========================================================================
    // Phase 56 (P0/P1): Strict Numeric Storage, Streaming Decoder & Wide Sum
    // =========================================================================

    struct StrictNumericDTO
    {
        static constexpr auto tablename = "StrictNumerics";
        sqlgen::PrimaryKey<std::int32_t, false> id{ 0 };
        std::int8_t small_score = 0;
        std::uint64_t big_counter = 0;
    };

    struct Phase56NumericContext : DAL::DbContextBase<DAL::SQLiteConnection>
    {
        Set<StrictNumericDTO> items{ this };

        explicit Phase56NumericContext(std::shared_ptr<DAL::SQLiteConnection> c)
            : DAL::DbContextBase<DAL::SQLiteConnection>(std::move(c))
        {
        }
    };

    TEST(Phase56StrictNumericTest, RejectsOversizedUint64AndCorruptOrOutOfRangeReadsAndWidensSum)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());
        auto conn = connRes.value();
        EXPECT_NE(conn->native_handle(), nullptr);

        Phase56NumericContext db(conn);
        ASSERT_TRUE(db.ensure_created().has_value());

        // 1. Oversized uint64_t (>= 2^63) MUST be rejected before storage (never coerced to lossy REAL).
        const std::uint64_t twoPow63 = 1ULL << 63;
        auto ovfSingle = db.items.add(StrictNumericDTO{ .id = 1, .small_score = 10, .big_counter = twoPow63 });
        ASSERT_FALSE(ovfSingle.has_value());
        EXPECT_EQ(ovfSingle.error().code, Core::DbErrorCode::ConstraintViolation);
        EXPECT_EQ(db.items.count(), 0u);

        std::vector<StrictNumericDTO> batch{
            StrictNumericDTO{ .id = 1, .small_score = 100, .big_counter = 42 },
            StrictNumericDTO{ .id = 2, .small_score = 100, .big_counter = twoPow63 + 1ULL },
        };
        auto ovfBatch = db.items.add_range(batch);
        ASSERT_FALSE(ovfBatch.has_value());
        EXPECT_EQ(ovfBatch.error().code, Core::DbErrorCode::ConstraintViolation);
        EXPECT_EQ(db.items.count(), 0u);

        // 2. Valid narrow int8_t rows summing past 127 MUST widen to int64_t (200, not -56).
        ASSERT_TRUE(db.items.add(StrictNumericDTO{ .id = 1, .small_score = 100, .big_counter = 10 }).has_value());
        ASSERT_TRUE(db.items.add(StrictNumericDTO{ .id = 2, .small_score = 100, .big_counter = 20 }).has_value());
        auto sumRes = db.items.try_sum<"small_score">();
        ASSERT_TRUE(sumRes.has_value()) << sumRes.error().to_string();
        EXPECT_EQ(*sumRes, std::optional<std::int64_t>{ 200 });

        // 3. Trailing-garbage text ("17ms") and out-of-range integer (128) MUST fail strictly across
        //    entity reads, single-column projections, and tuple projections.
        ASSERT_TRUE(conn->execute_raw("INSERT INTO StrictNumerics (id, small_score, big_counter) VALUES (3, '17ms', 30);").has_value());
        auto badTextRead = db.items.try_find(3);
        ASSERT_FALSE(badTextRead.has_value());
        EXPECT_EQ(badTextRead.error().code, Core::DbErrorCode::ConstraintViolation);

        auto badTextProj = db.items.where_equal<"id">(3).try_select<"small_score">();
        ASSERT_FALSE(badTextProj.has_value());
        EXPECT_EQ(badTextProj.error().code, Core::DbErrorCode::ConstraintViolation);

        auto badTextTuple = db.items.where_equal<"id">(3).try_select_columns<"id", "small_score">();
        ASSERT_FALSE(badTextTuple.has_value());
        EXPECT_EQ(badTextTuple.error().code, Core::DbErrorCode::ConstraintViolation);

        ASSERT_TRUE(conn->execute_raw("INSERT INTO StrictNumerics (id, small_score, big_counter) VALUES (4, 128, 40);").has_value());
        auto badRangeRead = db.items.try_find(4);
        ASSERT_FALSE(badRangeRead.has_value());
        EXPECT_EQ(badRangeRead.error().code, Core::DbErrorCode::ConstraintViolation);

        auto badRangeProj = db.items.where_equal<"id">(4).try_select<"small_score">();
        ASSERT_FALSE(badRangeProj.has_value());
        EXPECT_EQ(badRangeProj.error().code, Core::DbErrorCode::ConstraintViolation);

        auto badRangeAgg = db.items.where_equal<"id">(4).try_max<"small_score">();
        ASSERT_FALSE(badRangeAgg.has_value());
        EXPECT_EQ(badRangeAgg.error().code, Core::DbErrorCode::ConstraintViolation);
    }

    // =========================================================================
    // Phase 58: Concurrent BEGIN IMMEDIATE Migrations & Declared FK Target Col
    // =========================================================================

    struct CompositeRefParentDTO
    {
        static constexpr auto tablename = "CompositeRefParents";
        static constexpr std::array<bool, 3> unique_field_flags{ false, true, false };
        sqlgen::PrimaryKey<std::int32_t> tenant_id{ 0 };
        sqlgen::PrimaryKey<std::int32_t> code_id{ 0 };
        std::string label;
    };

    struct CompositeRefChildDTO
    {
        static constexpr auto tablename = "CompositeRefChildren";
        sqlgen::PrimaryKey<std::int32_t> child_id{ 0 };
        sqlgen::ForeignKey<std::int32_t, CompositeRefParentDTO, "code_id"> parent_code{ 0 };
        std::string note;
    };

    struct Phase58FkNavContext : public DAL::DbContextBase<DAL::SQLiteConnection>
    {
        explicit Phase58FkNavContext(std::shared_ptr<DAL::SQLiteConnection> conn)
            : DbContextBase(std::move(conn))
        {
        }

        Set<CompositeRefParentDTO> parents{ this };
        Set<CompositeRefChildDTO> children{ this };
    };

    TEST(Phase58SnapshotAndFkTargetTest, ConcurrentMigrateToSerializesWithBeginImmediateAndPreventsDowngrade)
    {
        const auto dbPath = make_unique_temp_db_path("codefirst_phase58_migrate_race");
        const ScopedTempDbCleanup cleanup{ dbPath };

        DAL::SQLiteConnectionOptions opts;
        opts.foreign_keys = true;
        opts.busy_timeout = std::chrono::milliseconds(5000);
        opts.wal_mode = true;

        auto connARes = DAL::SQLiteConnection::connect(dbPath, opts);
        ASSERT_TRUE(connARes.has_value()) << connARes.error().to_string();
        auto connBRes = DAL::SQLiteConnection::connect(dbPath, opts);
        ASSERT_TRUE(connBRes.has_value()) << connBRes.error().to_string();

        auto connA = connARes.value();
        auto connB = connBRes.value();

        std::promise<void> enteredMigration;
        auto enteredFuture = enteredMigration.get_future();

        // Thread B attempts to migrate to version 2 while Thread A holds BEGIN IMMEDIATE migrating 0 -> 3.
        std::expected<void, Core::DbError> resB{};
        std::thread threadB(
            [&]()
            {
                enteredFuture.wait();
                Phase30DbContext dbB(connB);
                resB = dbB.migrate_to(
                    2,
                    [](std::int32_t, std::int32_t) -> std::expected<void, Core::DbError>
                    {
                        return {};
                    });
            });

        Phase30DbContext dbA(connA);
        auto resA = dbA.migrate_to(
            3,
            [&](std::int32_t from, std::int32_t to) -> std::expected<void, Core::DbError>
            {
                EXPECT_EQ(from, 0);
                EXPECT_EQ(to, 3);
                enteredMigration.set_value();
                std::this_thread::sleep_for(std::chrono::milliseconds(80));
                return connA->execute_raw("CREATE TABLE IF NOT EXISTS migrate_v3_marker (v INTEGER NOT NULL);");
            });

        threadB.join();

        ASSERT_TRUE(resA.has_value()) << resA.error().to_string();
        // Thread B MUST see version 3 inside its own BEGIN IMMEDIATE snapshot and reject downgrade 3 -> 2.
        ASSERT_FALSE(resB.has_value());
        EXPECT_EQ(resB.error().code, Core::DbErrorCode::ConstraintViolation);
        auto verRes = dbA.try_user_version();
        ASSERT_TRUE(verRes.has_value());
        EXPECT_EQ(*verRes, 3);

        // Concurrent idempotent race to version 4: callback MUST run exactly once across both connections.
        std::atomic<int> v4CallbackCount{ 0 };
        std::promise<void> v4Entered;
        auto v4Future = v4Entered.get_future();
        std::expected<void, Core::DbError> resB4{};
        std::thread threadB4(
            [&]()
            {
                v4Future.wait();
                Phase30DbContext dbB4(connB);
                resB4 = dbB4.migrate_to(
                    4,
                    [&](std::int32_t, std::int32_t) -> std::expected<void, Core::DbError>
                    {
                        ++v4CallbackCount;
                        return {};
                    });
            });
        auto resA4 = dbA.migrate_to(
            4,
            [&](std::int32_t from, std::int32_t to) -> std::expected<void, Core::DbError>
            {
                EXPECT_EQ(from, 3);
                EXPECT_EQ(to, 4);
                ++v4CallbackCount;
                v4Entered.set_value();
                std::this_thread::sleep_for(std::chrono::milliseconds(80));
                return {};
            });
        threadB4.join();

        ASSERT_TRUE(resA4.has_value()) << resA4.error().to_string();
        ASSERT_TRUE(resB4.has_value()) << resB4.error().to_string();
        EXPECT_EQ(v4CallbackCount.load(), 1);
        EXPECT_EQ(dbA.try_user_version().value_or(-1), 4);

        connA->close();
        connB->close();
    }

    TEST(Phase58SnapshotAndFkTargetTest, ForeignKeyNavigationHonorsDeclaredReferencedColumnOnCompositeParent)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());
        Phase58FkNavContext db(connRes.value());
        ASSERT_TRUE(db.ensure_created().has_value());

        // Overlapping values between tenant_id and code_id:
        // p1 has tenant_id=10, code_id=20; p2 has tenant_id=20, code_id=10.
        // Child FK explicitly references CompositeRefParents("code_id"), NOT tenant_id.
        const CompositeRefParentDTO p1{ .tenant_id = 10, .code_id = 20, .label = "ParentCode20" };
        const CompositeRefParentDTO p2{ .tenant_id = 20, .code_id = 10, .label = "ParentCode10" };
        ASSERT_TRUE(db.parents.add(p1).has_value());
        ASSERT_TRUE(db.parents.add(p2).has_value());

        const CompositeRefChildDTO c1{ .child_id = 1, .parent_code = 20, .note = "BelongsToP1" };
        const CompositeRefChildDTO c2{ .child_id = 2, .parent_code = 10, .note = "BelongsToP2" };
        ASSERT_TRUE(db.children.add(c1).has_value());
        ASSERT_TRUE(db.children.add(c2).has_value());

        auto childrenOfP1 = db.query_children<CompositeRefChildDTO>(p1);
        ASSERT_TRUE(childrenOfP1.has_value()) << childrenOfP1.error().to_string();
        ASSERT_EQ(childrenOfP1->size(), 1u);
        EXPECT_EQ(childrenOfP1->front().note, "BelongsToP1");

        auto parentOfC1 = db.query_parent<CompositeRefParentDTO>(c1);
        ASSERT_TRUE(parentOfC1.has_value()) << parentOfC1.error().to_string();
        ASSERT_TRUE(parentOfC1->has_value());
        EXPECT_EQ((*parentOfC1)->label, "ParentCode20");

        auto batchParents = db.query_parents_batch<CompositeRefParentDTO>(std::vector<CompositeRefChildDTO>{ c1 });
        ASSERT_TRUE(batchParents.has_value()) << batchParents.error().to_string();
        ASSERT_EQ(batchParents->size(), 1u);
        EXPECT_EQ(batchParents->front().label, "ParentCode20");

        auto grouped = db.query_children_grouped<CompositeRefChildDTO>(std::vector<CompositeRefParentDTO>{ p1, p2 });
        ASSERT_TRUE(grouped.has_value()) << grouped.error().to_string();
        ASSERT_EQ(grouped->size(), 2u);
        ASSERT_EQ((*grouped)[0].second.size(), 1u);
        EXPECT_EQ((*grouped)[0].second.front().note, "BelongsToP1");
        ASSERT_EQ((*grouped)[1].second.size(), 1u);
        EXPECT_EQ((*grouped)[1].second.front().note, "BelongsToP2");
    }
} // namespace
