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
        auto fkErr = db.personSomethings.add(Core::Person_Something(Core::PersonId{ 99 }, Core::SomethingId{ 88 }, "Orphan"));
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
        ASSERT_TRUE(db.personSomethings.add(Core::Person_Something(Core::PersonId{ 1 }, Core::SomethingId{ 999 }, "Broken")).has_value());

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
        auto txOk = db.transaction(
            [&]() -> std::expected<void, Core::DbError>
            {
                auto r1 = db.people.add(Core::Person(Core::PersonId{ 1 }, "Linus", "Torvalds"));
                if (!r1)
                    return r1;
                return db.somethings.add(Core::Something(Core::SomethingId{ 1 }, "Git", "VCS"));
            });
        ASSERT_TRUE(txOk.has_value());
        EXPECT_FALSE(db.in_transaction());
        EXPECT_EQ(db.people.count(), 1u);
        EXPECT_EQ(db.somethings.count(), 1u);

        // 2. Failing scoped transaction rolls back
        auto txFail = db.transaction(
            [&]() -> std::expected<void, Core::DbError>
            {
                auto r = db.people.add(Core::Person(Core::PersonId{ 2 }, "Temp", "User"));
                if (!r)
                    return r;
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

        // 2. Scope with explicit commit() -> persists changes with explicit non-zero PK
        {
            DAL::TransactionScope scope(db);
            ASSERT_TRUE(scope.begin_result().has_value());
            ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 2 }, "Committed", "User")).has_value());
            ASSERT_TRUE(scope.commit().has_value());
            EXPECT_FALSE(scope.is_active());
        }
        EXPECT_FALSE(db.in_transaction());
        EXPECT_EQ(db.people.count(), 1u);
        auto persisted = db.people.find(2);
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
        ASSERT_TRUE(db.personSomethings.add(Core::Person_Something(Core::PersonId{ 1 }, Core::SomethingId{ 1 }, "Reader")).has_value());

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
        [[maybe_unused]] auto trackedBob = db.users.track_copy(*bobLocal);

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
        {
        }
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
            {
            }
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
                {
                }
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

    // =========================================================================
    // Phase 23: Read-Only Temp-View try_last_insert_rowid & add_range_returning
    // =========================================================================

    TEST(Phase23TempViewAndAddRangeReturningTest, IdempotentRowIdAndAddRangeReturning)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());
        auto conn = connRes.value();
        ASSERT_TRUE(conn->ensure_table_created<DAL::Schema::PersonDTO>().has_value());

        DAL::DbSet<Core::Person, DAL::Schema::PersonDTO, DAL::SQLiteConnection> people(conn);

        auto one = people.add_returning(Core::Person(Core::PersonId{ 0 }, "First", "User"));
        ASSERT_TRUE(one.has_value()) << one.error().to_string();
        EXPECT_EQ(one->getId().value, 1);

        // Calling try_last_insert_rowid() twice in a row must return the exact same rowid
        auto r1 = conn->try_last_insert_rowid();
        auto r2 = conn->try_last_insert_rowid();
        ASSERT_TRUE(r1.has_value());
        ASSERT_TRUE(r2.has_value());
        EXPECT_EQ(*r1, 1);
        EXPECT_EQ(*r2, 1);

        // add_range_returning populates sequential IDs atomically
        std::vector<Core::Person> batch{ Core::Person(Core::PersonId{ 0 }, "Alice", "Smith"),
                                         Core::Person(Core::PersonId{ 0 }, "Bob", "Jones"),
                                         Core::Person(Core::PersonId{ 0 }, "Charlie", "Brown") };
        auto batchRes = people.add_range_returning(batch);
        ASSERT_TRUE(batchRes.has_value()) << batchRes.error().to_string();
        ASSERT_EQ(batchRes->size(), 3u);
        EXPECT_EQ((*batchRes)[0].getId().value, 2);
        EXPECT_EQ((*batchRes)[1].getId().value, 3);
        EXPECT_EQ((*batchRes)[2].getId().value, 4);
    }

    // =========================================================================
    // Phase 24: Multi-Column then_by, select, where_between, where_not_in_vec
    // =========================================================================

    TEST(Phase26NestedSavepointAndPragmasTest, NestedSavepointRollbackAndPragmas)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());
        auto conn = connRes.value();

        EXPECT_TRUE(conn->set_busy_timeout(std::chrono::milliseconds(5000)).has_value());
        // SQLite cannot enable WAL on :memory: databases (effective journal_mode stays "memory").
        EXPECT_FALSE(conn->enable_wal_mode().has_value());

        Phase25Context db(conn);
        ASSERT_TRUE(db.ensure_created().has_value());

        // Outer transaction: inserts a1, fails inner transaction (inserts a2 then fails), then inserts a3
        auto outerRes = db.transaction(
            [&]() -> std::expected<void, Core::DbError>
            {
                auto r1 = db.authors.add(Phase25Author{ .author_id = 1, .name = "OuterBefore" });
                if (!r1)
                    return r1;

                auto innerFail = db.transaction(
                    [&]() -> std::expected<void, Core::DbError>
                    {
                        auto r2 = db.authors.add(Phase25Author{ .author_id = 2, .name = "InnerRolledBack" });
                        if (!r2)
                            return r2;
                        return std::unexpected<Core::DbError>{ Core::DbError::unknown("Intentional inner failure") };
                    });
                EXPECT_FALSE(innerFail.has_value());

                auto innerOk = db.transaction(
                    [&]() -> std::expected<void, Core::DbError>
                    {
                        return db.authors.add(Phase25Author{ .author_id = 3, .name = "InnerCommitted" });
                    });
                if (!innerOk)
                    return innerOk;

                return db.authors.add(Phase25Author{ .author_id = 4, .name = "OuterAfter" });
            });
        ASSERT_TRUE(outerRes.has_value()) << outerRes.error().to_string();

        EXPECT_EQ(db.authors.count(), 3u);
        EXPECT_EQ(db.authors.where_equal<"name">("OuterBefore").count(), 1u);
        EXPECT_EQ(db.authors.where_equal<"name">("InnerRolledBack").count(), 0u);
        EXPECT_EQ(db.authors.where_equal<"name">("InnerCommitted").count(), 1u);
        EXPECT_EQ(db.authors.where_equal<"name">("OuterAfter").count(), 1u);
    }

    // =========================================================================
    // Phase 27: Nested TransactionScope Savepoints & Deferred-Op Rollback Safety
    // =========================================================================

    TEST(Phase27NestedTransactionScopeTest, NestedScopesAndDeferredOpsRollbackSafety)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());
        Phase25Context db(connRes.value());
        ASSERT_TRUE(db.ensure_created().has_value());

        // 1. Nested TransactionScope inside outer TransactionScope
        {
            DAL::TransactionScope<DAL::SQLiteConnection> outer(db);
            ASSERT_TRUE(outer.is_active());
            ASSERT_TRUE(db.authors.add(Phase25Author{ .name = "ScopeOuter1" }).has_value());

            {
                DAL::TransactionScope<DAL::SQLiteConnection> innerAbort(db);
                ASSERT_TRUE(innerAbort.is_active());
                ASSERT_TRUE(db.authors.add(Phase25Author{ .name = "ScopeInnerAborted" }).has_value());
                // Destructor rolls back savepoint without rolling back outer
            }

            {
                DAL::TransactionScope<DAL::SQLiteConnection> innerOk(db);
                ASSERT_TRUE(innerOk.is_active());
                ASSERT_TRUE(db.authors.add(Phase25Author{ .name = "ScopeInnerCommitted" }).has_value());
                ASSERT_TRUE(innerOk.commit().has_value());
            }

            ASSERT_TRUE(outer.commit().has_value());
        }

        EXPECT_EQ(db.authors.where_equal<"name">("ScopeOuter1").count(), 1u);
        EXPECT_EQ(db.authors.where_equal<"name">("ScopeInnerAborted").count(), 0u);
        EXPECT_EQ(db.authors.where_equal<"name">("ScopeInnerCommitted").count(), 1u);

        // 2. Deferred Unit-of-Work ops staged inside a failing nested transaction are truncated
        db.defer_changes(true);
        auto txRes = db.transaction(
            [&]() -> std::expected<void, Core::DbError>
            {
                auto s1 = db.authors.add(Phase25Author{ .name = "DeferredOuter" });
                if (!s1)
                    return s1;

                auto innerFail = db.transaction(
                    [&]() -> std::expected<void, Core::DbError>
                    {
                        auto s2 = db.authors.add(Phase25Author{ .name = "DeferredInnerAborted" });
                        if (!s2)
                            return s2;
                        return std::unexpected<Core::DbError>{ Core::DbError::unknown("Abort inner staged op") };
                    });
                EXPECT_FALSE(innerFail.has_value());
                return {};
            });
        ASSERT_TRUE(txRes.has_value()) << txRes.error().to_string();
        db.defer_changes(false);

        EXPECT_EQ(db.authors.where_equal<"name">("DeferredOuter").count(), 1u);
        EXPECT_EQ(db.authors.where_equal<"name">("DeferredInnerAborted").count(), 0u);
    }

    // =========================================================================
    // Phase 28: Non-Tracking any/exists, try_first_or_default, & Multi-Col update_where
    // =========================================================================

    TEST(Phase34SavepointDirtyFlushAndUpdateWhereEqualTest, NestedSavepointFlushesDirtyAndUpdateWhereEqual)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());
        Phase30DbContext db(connRes.value());
        ASSERT_TRUE(db.ensure_created().has_value());
        db.enable_change_tracking(true);

        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 1 }, "Ada", "Lovelace")).has_value());

        {
            DAL::TransactionScope<DAL::SQLiteConnection> outer(db);
            ASSERT_TRUE(outer.is_active());

            {
                DAL::TransactionScope<DAL::SQLiteConnection> inner(db);
                ASSERT_TRUE(inner.is_active());

                auto tracked = db.people.find_tracked(1);
                ASSERT_NE(tracked, nullptr);
                tracked->setFirstName("LadyAda");

                ASSERT_TRUE(inner.commit().has_value());
            }

            // Persisted immediately into the SQLite transaction upon inner.commit()
            auto midRead = db.people.where_equal<"id">(1).select<"firstName">();
            ASSERT_EQ(midRead.size(), 1u);
            EXPECT_EQ(midRead[0], "LadyAda");

            ASSERT_TRUE(outer.commit().has_value());
        }

        // Shorthand update_where_equal
        auto updRes = db.people.update_where_equal<"lastName", "firstName">("Lovelace", "Augusta");
        ASSERT_TRUE(updRes.has_value()) << updRes.error().to_string();
        auto afterRead = db.people.find(1);
        ASSERT_TRUE(afterRead.has_value());
        EXPECT_EQ(afterRead->getFirstName(), "Augusta");
    }

    TEST(Phase39HardeningTest, SaveChangesInsideOuterTransactionUsesSavepointAndDoesNotCommitOuter)
    {
        static_assert(!std::is_convertible_v<Core::PersonId, int>);
        static_assert(!std::is_convertible_v<Core::PersonId, Core::SomethingId>);
        static_assert(static_cast<int>(Core::PersonId{ 42 }) == 42);

        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());
        Phase30DbContext db(connRes.value());
        ASSERT_TRUE(db.ensure_created().has_value());

        ASSERT_TRUE(db.begin_transaction().has_value());
        db.defer_changes(true);
        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 0 }, "Outer", "Person")).has_value());
        ASSERT_TRUE(db.save_changes().has_value());

        // Outer transaction must STILL be active!
        EXPECT_TRUE(db.in_transaction());
        EXPECT_EQ(db.people.count(), 1u);

        // Rolling back outer transaction must undo the save_changes()-flushed row!
        ASSERT_TRUE(db.rollback().has_value());
        EXPECT_FALSE(db.in_transaction());
        EXPECT_EQ(db.people.count(), 0u);
    }

    TEST(Phase39HardeningTest, TrackCopyAndAtomicUpdateAndClose)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());
        auto conn = connRes.value();
        Phase30DbContext db(conn);
        ASSERT_TRUE(db.ensure_created().has_value());
        db.enable_change_tracking(true);

        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 0 }, "Grace", "Hopper")).has_value());
        auto tracked = db.people.track_copy(Core::Person(Core::PersonId{ 1 }, "Grace", "Hopper"));
        ASSERT_NE(tracked, nullptr);
        tracked->setLastName("Murray");
        ASSERT_TRUE(db.save_changes().has_value());

        auto loaded = db.people.find(1);
        ASSERT_TRUE(loaded.has_value());
        EXPECT_EQ(loaded->getLastName(), "Murray");

        // Atomic update on non-existent row must fail
        auto missingUpd = db.people.update(Core::Person(Core::PersonId{ 999 }, "No", "Body"));
        EXPECT_FALSE(missingUpd.has_value());

        conn->close();
        EXPECT_FALSE(conn->is_open());
    }

    TEST(Phase13UpsertAndBulkDeleteTest, CompositeKeyUpsertReplacesSingleRowCleanly)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());
        Phase30DbContext db(connRes.value());
        ASSERT_TRUE(db.ensure_created().has_value());

        auto p1Res = db.people.add_returning(Core::Person(Core::PersonId{ 0 }, "Ada", "Lovelace"));
        auto s1Res = db.somethings.add_returning(Core::Something(Core::SomethingId{ 0 }, "Item10", "Cat"));
        auto s2Res = db.somethings.add_returning(Core::Something(Core::SomethingId{ 0 }, "Item20", "Cat"));
        ASSERT_TRUE(p1Res.has_value() && s1Res.has_value() && s2Res.has_value());
        const auto pid = p1Res->getId();
        const auto sid1 = s1Res->getId();
        const auto sid2 = s2Res->getId();

        ASSERT_TRUE(db.personSomethings.add(Core::Person_Something(pid, sid1, "Owner")).has_value());
        ASSERT_TRUE(db.personSomethings.add(Core::Person_Something(pid, sid2, "Viewer")).has_value());

        // Immediate composite-key upsert should replace only (pid, sid1)
        ASSERT_TRUE(db.personSomethings.upsert(Core::Person_Something(pid, sid1, "Admin")).has_value());

        EXPECT_EQ(db.personSomethings.count(), 2u);
        auto row1 = db.personSomethings.find(pid, sid1);
        ASSERT_TRUE(row1.has_value());
        EXPECT_EQ(row1->getAssociationType(), "Admin");

        auto row2 = db.personSomethings.find(pid, sid2);
        ASSERT_TRUE(row2.has_value());
        EXPECT_EQ(row2->getAssociationType(), "Viewer");

        // Deferred stage_upsert + save_changes() on composite key
        db.defer_changes(true);
        ASSERT_TRUE(db.personSomethings.stage_upsert(Core::Person_Something(pid, sid1, "Maintainer")).has_value());
        ASSERT_TRUE(db.save_changes().has_value());
        EXPECT_EQ(db.personSomethings.count(), 2u);
        auto updated1 = db.personSomethings.find(pid, sid1);
        ASSERT_TRUE(updated1.has_value());
        EXPECT_EQ(updated1->getAssociationType(), "Maintainer");
    }

    // =========================================================================
    // Review 6 & Phase 47 Reliability, Ownership & SQL Boundary Regressions
    // =========================================================================

    struct StringPartKeyDTO
    {
        static constexpr const char* tablename = "StringPartKeys";
        sqlgen::PrimaryKey<std::string, false> val{};
    };

    struct StringPairRow
    {
        std::string part_a{};
        std::string part_b{};
        std::string payload{};
    };

    struct StringPairRowDTO
    {
        static constexpr const char* tablename = "StringPairRows";
        static constexpr bool composite_primary_key = true;
        sqlgen::ForeignKey<std::string, StringPartKeyDTO, "val"> part_a{};
        sqlgen::ForeignKey<std::string, StringPartKeyDTO, "val"> part_b{};
        std::string payload{};
    };

    TEST(Review6ReliabilityRegressionsTest, TrackedTransactionRollbackRetryAndFailedBatchRetry)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());
        auto conn = connRes.value();
        Phase30DbContext db(conn);
        ASSERT_TRUE(db.ensure_created().has_value());
        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 0 }, "Original", "Person")).has_value());
        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 0 }, "Second", "Person")).has_value());

        db.enable_change_tracking();
        auto p1 = db.people.find_tracked(1);
        auto p2 = db.people.find_tracked(2);
        ASSERT_NE(p1, nullptr);
        ASSERT_NE(p2, nullptr);

        // R01: Tracked-only edit inside db.transaction() must be detected & committed
        ASSERT_TRUE(db.transaction(
                          [&]()
                          {
                              p1->setFirstName("Changed");
                          })
                        .has_value());
        EXPECT_EQ(db.people.find(1)->getFirstName(), "Changed");

        // R02: Explicit rollback restores baseline so a subsequent save_changes() retries the edit
        ASSERT_TRUE(db.begin_transaction().has_value());
        p1->setFirstName("ChangedAgain");
        ASSERT_TRUE(db.save_changes().has_value());
        ASSERT_TRUE(db.rollback().has_value());
        EXPECT_EQ(db.people.find(1)->getFirstName(), "Changed");
        ASSERT_TRUE(db.save_changes().has_value());
        EXPECT_EQ(db.people.find(1)->getFirstName(), "ChangedAgain");

        // R03: Mid-batch update failure restores earlier baselines so retry persists all edits
        ASSERT_TRUE(conn->execute_raw(
                            "CREATE TRIGGER fail_second_upd BEFORE UPDATE ON Person WHEN NEW.id = 2 "
                            "BEGIN SELECT RAISE(ABORT, 'injected failure'); END;")
                        .has_value());
        p1->setFirstName("BatchFirst");
        p2->setFirstName("BatchSecond");
        EXPECT_FALSE(db.save_changes().has_value());
        EXPECT_EQ(db.people.find(1)->getFirstName(), "ChangedAgain");

        ASSERT_TRUE(conn->execute_raw("DROP TRIGGER fail_second_upd").has_value());
        ASSERT_TRUE(db.save_changes().has_value());
        EXPECT_EQ(db.people.find(1)->getFirstName(), "BatchFirst");
        EXPECT_EQ(db.people.find(2)->getFirstName(), "BatchSecond");
    }

    TEST(Review6ReliabilityRegressionsTest, AbandonedOuterScopeTruncatesStagedOperations)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());
        Phase30DbContext db(connRes.value());
        ASSERT_TRUE(db.ensure_created().has_value());
        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 0 }, "Keep", "Person")).has_value());

        db.defer_changes(true);
        {
            DAL::TransactionScope<DAL::SQLiteConnection> scope(db);
            ASSERT_TRUE(scope.is_active());
            ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 0 }, "Canceled", "Person")).has_value());
        }
        // R04: Abandoned outer TransactionScope must not resurrect canceled deferred operations
        ASSERT_TRUE(db.save_changes().has_value());
        EXPECT_EQ(db.people.count(), 1u);
    }

    TEST(Review6ReliabilityRegressionsTest, PreparedContractsSavepointValidationAndRawTransaction)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());
        auto conn = connRes.value();

        // R05: Missing parameter binding must be rejected instead of silently binding NULL
        auto missingRes = conn->query_rows("SELECT ? AS required_val");
        EXPECT_FALSE(missingRes.has_value());

        // R06: Empty vector<std::byte> must bind as empty BLOB (length 0), not SQL NULL
        const std::array<DAL::DynamicCell, 2> blobs{ std::vector<std::byte>{}, std::vector<std::byte>{} };
        auto blobRes = conn->query_rows("SELECT typeof(?), length(?)", std::span<const DAL::DynamicCell>{ blobs });
        ASSERT_TRUE(blobRes.has_value()) << blobRes.error().to_string();
        ASSERT_EQ(blobRes->rows.size(), 1u);
        EXPECT_EQ(std::get<std::string>(blobRes->rows[0].cells[0]), "blob");
        EXPECT_EQ(std::get<std::int64_t>(blobRes->rows[0].cells[1]), 0);

        // R07: Trailing SQL statements in prepared calls must be rejected rather than silently ignored
        EXPECT_FALSE(conn->execute_prepared("CREATE TABLE first_t(id INTEGER); CREATE TABLE second_t(id INTEGER);").has_value());
        EXPECT_FALSE(conn->query_rows("SELECT 1; SELECT 2;").has_value());
        auto tablesRes = conn->list_tables();
        ASSERT_TRUE(tablesRes.has_value());
        EXPECT_FALSE(std::ranges::contains(*tablesRes, std::string_view{ "second_t" }));

        // R08: Raw BEGIN must be reflected by engine-derived sqlite3_get_autocommit state
        ASSERT_TRUE(conn->execute_raw("BEGIN").has_value());
        EXPECT_TRUE(conn->in_transaction());
        ASSERT_TRUE(conn->rollback().has_value());
        EXPECT_FALSE(conn->in_transaction());

        // R09: Malicious savepoint identifier with quotes/semicolon must be rejected
        EXPECT_FALSE(conn->savepoint("safe\"; CREATE TABLE injected(id INTEGER); --").has_value());
        auto postTables = conn->list_tables();
        ASSERT_TRUE(postTables.has_value());
        EXPECT_FALSE(std::ranges::contains(*postTables, std::string_view{ "injected" }));
    }

    TEST(Review6ReliabilityRegressionsTest, LimitedDeleteAndMockAdapterCapabilitiesAndRowidOverflow)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());
        auto conn = connRes.value();
        Phase30DbContext db(conn);
        ASSERT_TRUE(db.ensure_created().has_value());

        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 0 }, "One", "Person")).has_value());
        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 0 }, "Two", "Person")).has_value());
        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 0 }, "Three", "Person")).has_value());

        // R10: limit(1).remove_where() deletes only 1 matching row, and limited update_where() rejects cleanly
        EXPECT_FALSE(db.people.where_equal<"lastName">("Person").limit(1).update_where<"firstName">("X").has_value());
        auto delLimited = db.people.where_equal<"lastName">("Person").limit(1).remove_where();
        ASSERT_TRUE(delLimited.has_value()) << delLimited.error().to_string();
        EXPECT_EQ(db.people.count(), 2u);

        // R13: SQLite 64-bit rowid exceeding int32 max is rejected without leaving an inaccessible row
        ASSERT_TRUE(conn->execute_raw("UPDATE sqlite_sequence SET seq = 2147483647 WHERE name = 'Person'").has_value());
        auto overflowRes = db.people.add_returning(Core::Person(Core::PersonId{ 0 }, "Overflow", "Person"));
        EXPECT_FALSE(overflowRes.has_value());
        EXPECT_EQ(db.people.count(), 2u);

        // R11 & R12: Connection adapters lacking WHERE execution must fail rather than returning/deleting unfiltered rows
        auto mockConn = std::make_shared<TestDatabaseConnection>();
        Phase6MockDbContext mockDb(mockConn);
        ASSERT_TRUE(mockDb.ensure_created().has_value());
        ASSERT_TRUE(mockDb.people.add(Core::Person(Core::PersonId{ 1 }, "Mock1", "Person")).has_value());
        ASSERT_TRUE(mockDb.people.add(Core::Person(Core::PersonId{ 2 }, "Mock2", "Person")).has_value());

        EXPECT_FALSE(mockDb.people.where_equal<"id">(1).try_to_vector().has_value());
        EXPECT_FALSE(mockDb.people.where_equal<"id">(1).remove_where().has_value());
        EXPECT_EQ(mockDb.people.count(), 2u);
    }

    TEST(Review6ReliabilityRegressionsTest, CompositeKeyEncodingAndIdentityMapAvoidsStringCollisions)
    {
        // R14: Distinct string tuples containing delimiters never collide
        const Core::CompositeKey<std::string, std::string> k1{ "a, b", "c" };
        const Core::CompositeKey<std::string, std::string> k2{ "a", "b, c" };
        EXPECT_NE(std::format("{}", k1), std::format("{}", k2));
        EXPECT_NE(Core::encode_key(k1), Core::encode_key(k2));

        // Verify live two-row SQLite identity-map separation with delimiter-containing string keys
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());
        auto conn = connRes.value();

        struct PairContext : DAL::DbContextBase<DAL::SQLiteConnection>
        {
            Set<StringPartKeyDTO> parts{ this };
            Set<StringPairRow, StringPairRowDTO> pairs{ this };
            explicit PairContext(std::shared_ptr<DAL::SQLiteConnection> c)
                : DAL::DbContextBase<DAL::SQLiteConnection>(std::move(c))
            {
            }
        };

        PairContext db(conn);
        ASSERT_TRUE(db.ensure_created().has_value());
        ASSERT_TRUE(db.parts.add(StringPartKeyDTO{ "a, b" }).has_value());
        ASSERT_TRUE(db.parts.add(StringPartKeyDTO{ "c" }).has_value());
        ASSERT_TRUE(db.parts.add(StringPartKeyDTO{ "a" }).has_value());
        ASSERT_TRUE(db.parts.add(StringPartKeyDTO{ "b, c" }).has_value());

        db.enable_change_tracking();
        ASSERT_TRUE(db.pairs.add(StringPairRow{ "a, b", "c", "first_payload" }).has_value());
        ASSERT_TRUE(db.pairs.add(StringPairRow{ "a", "b, c", "second_payload" }).has_value());

        auto t1 = db.pairs.find_tracked(std::string("a, b"), std::string("c"));
        auto t2 = db.pairs.find_tracked(std::string("a"), std::string("b, c"));
        ASSERT_NE(t1, nullptr);
        ASSERT_NE(t2, nullptr);
        EXPECT_NE(t1.get(), t2.get());
        EXPECT_EQ(t1->payload, "first_payload");
        EXPECT_EQ(t2->payload, "second_payload");
    }

    TEST(Review6ReliabilityRegressionsTest, StandaloneTrackedRefEscapedQueryAndPhase47ErrorParity)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());
        auto conn = connRes.value();
        Phase30DbContext db(conn);
        ASSERT_TRUE(db.ensure_created().has_value());
        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 0 }, "Original", "Person")).has_value());

        // R15 & Task 47.2: standalone DbSet owns its tracked instances & accepts raw integral IDs
        auto standalone = db.create_set<Core::Person>();
        auto refByRawId = standalone.find_tracked_ref(1);
        ASSERT_TRUE(refByRawId.has_value());
        EXPECT_EQ(refByRawId->getFirstName(), "Original");

        auto standaloneByConn = DAL::DbSet<Core::Person>(conn);
        auto refByKey = standaloneByConn.find_tracked_ref(Core::Person::Key{ Core::PersonId{ 1 } });
        ASSERT_TRUE(refByKey.has_value());
        EXPECT_EQ(refByKey->getFirstName(), "Original");

        // R16: Query escaping a destroyed local DbContext safely avoids dangling pointer dereference
        auto escapedQuery = [&]()
        {
            Phase30DbContext local(conn);
            local.enable_change_tracking();
            return local.people.where_equal<"id">(1);
        }();
        auto escapedRes = escapedQuery.try_to_vector();
        ASSERT_TRUE(escapedRes.has_value()) << escapedRes.error().to_string();
        ASSERT_EQ(escapedRes->size(), 1u);
        EXPECT_EQ(escapedRes->front().getFirstName(), "Original");

        // Task 47.3: map_from_message captures Debug stacktraces
        auto mappedErr = Core::DbError::map_from_message("UNIQUE constraint failed");
        EXPECT_EQ(mappedErr.code, Core::DbErrorCode::DuplicateKey);
#ifndef NDEBUG
        EXPECT_FALSE(mappedErr.trace.empty());
#endif
    }

    // =========================================================================
    // Phase 49 (P0): Direct Prepared-Statement Typed SQLite Reader Regressions
    // =========================================================================

    struct OverflowViewRowDTO
    {
        static constexpr const char* tablename = "OverflowView";
        sqlgen::PrimaryKey<std::int64_t, false> id{ 0 };
        std::int64_t computed{ 0 };
    };

    TEST(TypedSqliteReaderHardeningTest, PreservesEmbeddedNulTextBytesAcrossTypedReads)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());
        Phase30DbContext db(connRes.value());
        ASSERT_TRUE(db.ensure_created().has_value());

        const std::string nulText("A\0B", 3);
        const std::string lastText("X\0\0Y", 4);
        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 0 }, nulText, lastText)).has_value());

        auto byId = db.people.try_find(1);
        ASSERT_TRUE(byId.has_value()) << byId.error().to_string();
        ASSERT_TRUE(byId->has_value());
        EXPECT_EQ((*byId)->getFirstName().size(), 3u);
        EXPECT_EQ((*byId)->getFirstName(), nulText);
        EXPECT_EQ((*byId)->getLastName().size(), 4u);
        EXPECT_EQ((*byId)->getLastName(), lastText);

        auto all = db.people.try_to_vector();
        ASSERT_TRUE(all.has_value()) << all.error().to_string();
        ASSERT_EQ(all->size(), 1u);
        EXPECT_EQ(all->front().getFirstName(), nulText);

        auto colProj = db.people.query().try_select<"firstName">();
        ASSERT_TRUE(colProj.has_value()) << colProj.error().to_string();
        ASSERT_EQ(colProj->size(), 1u);
        EXPECT_EQ(colProj->front(), nulText);

        auto tupleProj = db.people.query().try_select_columns<"firstName", "lastName">();
        ASSERT_TRUE(tupleProj.has_value()) << tupleProj.error().to_string();
        ASSERT_EQ(tupleProj->size(), 1u);
        EXPECT_EQ(std::get<0>(tupleProj->front()), nulText);
        EXPECT_EQ(std::get<1>(tupleProj->front()), lastText);
    }

    TEST(TypedSqliteReaderHardeningTest, PropagatesMidQuerySqliteStepErrorInsteadOfTruncatingAsEof)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());
        auto conn = connRes.value();

        ASSERT_TRUE(conn->execute_raw(
                            "CREATE TABLE OverflowSrc (id INTEGER PRIMARY KEY, val INTEGER NOT NULL);"
                            "INSERT INTO OverflowSrc (id, val) VALUES (1, 10);"
                            "INSERT INTO OverflowSrc (id, val) VALUES (2, -9223372036854775807 - 1);"
                            "CREATE VIEW OverflowView AS SELECT id, abs(val) AS computed FROM OverflowSrc ORDER BY id;")
                        .has_value());

        // Row 1 succeeds, row 2 triggers SQLite integer overflow on sqlite3_step.
        // Typed reads MUST return an error rather than a partial 1-row batch.
        auto directRes = conn->try_fetch_all<OverflowViewRowDTO>();
        ASSERT_FALSE(directRes.has_value());
        EXPECT_NE(directRes.error().message.find("overflow"), std::string::npos);

        DAL::DbSet<OverflowViewRowDTO, OverflowViewRowDTO, DAL::SQLiteConnection> viewSet(conn);
        auto setRes = viewSet.try_to_vector();
        ASSERT_FALSE(setRes.has_value());

        auto tupleRes = viewSet.query().try_select_columns<"id", "computed">();
        ASSERT_FALSE(tupleRes.has_value());
    }

    TEST(TypedSqliteReaderHardeningTest, FinalizesStatementsImmediatelyAllowingCleanCloseAfterEscapedReads)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());
        auto conn = connRes.value();
        {
            Phase30DbContext db(conn);
            ASSERT_TRUE(db.ensure_created().has_value());
            ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 0 }, "Ada", "Lovelace")).has_value());
            ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 0 }, "Grace", "Hopper")).has_value());
        }

        // Run typed reads and escaped query, then close connection immediately.
        auto rowsRes = conn->try_fetch_all<DAL::Schema::PersonDTO>();
        ASSERT_TRUE(rowsRes.has_value());
        EXPECT_EQ(rowsRes->size(), 2u);

        auto cntRes = conn->try_count<DAL::Schema::PersonDTO>();
        ASSERT_TRUE(cntRes.has_value());
        EXPECT_EQ(*cntRes, 2u);

        conn->close();
        EXPECT_FALSE(conn->is_open());
        EXPECT_FALSE(conn->try_fetch_all<DAL::Schema::PersonDTO>().has_value());
    }

    // =========================================================================
    // Phase 49 (P0): Write & Batch Atomicity Regressions
    // =========================================================================

    struct AssignedUniqueRowDTO
    {
        static constexpr const char* tablename = "AssignedUniqueRows";
        sqlgen::PrimaryKey<std::int32_t, false> id{ 0 };
        sqlgen::Unique<std::string> slug{};
    };

    struct ThrowingCopyEntity
    {
        static inline int copies_before_throw = -1;
        std::int32_t id = 0;
        std::string label;

        ThrowingCopyEntity() = default;
        ThrowingCopyEntity(std::int32_t i, std::string l)
            : id(i)
            , label(std::move(l))
        {
        }
        ThrowingCopyEntity(const ThrowingCopyEntity& other)
            : id(other.id)
            , label(other.label)
        {
            if (copies_before_throw == 0)
            {
                throw std::runtime_error("injected copy failure");
            }
            if (copies_before_throw > 0)
            {
                --copies_before_throw;
            }
        }
        ThrowingCopyEntity(ThrowingCopyEntity&&) noexcept = default;
        ThrowingCopyEntity& operator=(const ThrowingCopyEntity&) = default;
        ThrowingCopyEntity& operator=(ThrowingCopyEntity&&) noexcept = default;
    };

    struct ThrowingCopyRowDTO
    {
        static constexpr const char* tablename = "ThrowingCopyRows";
        sqlgen::PrimaryKey<std::int32_t, true> id{ 0 };
        std::string label{};
    };

    TEST(WriteBatchAtomicityHardeningTest, AddReturningRollsBackInsertAndTriggerSideEffectsOnOverflow)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());
        auto conn = connRes.value();
        Phase30DbContext db(conn);
        ASSERT_TRUE(db.ensure_created().has_value());
        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 0 }, "Seed", "Person")).has_value());

        ASSERT_TRUE(conn->execute_raw(
                            "CREATE TABLE PersonAudit (id INTEGER PRIMARY KEY AUTOINCREMENT, inserted_name TEXT NOT NULL);"
                            "CREATE TRIGGER trg_person_audit AFTER INSERT ON Person "
                            "BEGIN INSERT INTO PersonAudit (inserted_name) VALUES (NEW.first_name); END;")
                        .has_value());
        ASSERT_TRUE(conn->execute_raw("UPDATE sqlite_sequence SET seq = 2147483647 WHERE name = 'Person';").has_value());

        auto res = db.people.add_returning(Core::Person(Core::PersonId{ 0 }, "OverflowTrigger", "Person"));
        ASSERT_FALSE(res.has_value());
        EXPECT_EQ(db.people.count(), 1u);

        auto auditRows = conn->query_rows("SELECT COUNT(*) FROM PersonAudit;");
        ASSERT_TRUE(auditRows.has_value());
        EXPECT_EQ(std::get<std::int64_t>(auditRows->rows[0].cells[0]), 0);
    }

    TEST(WriteBatchAtomicityHardeningTest, AddRangeReturningRollsBackOnMidBatchException)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());
        auto conn = connRes.value();
        ASSERT_TRUE(conn->ensure_table_created<ThrowingCopyRowDTO>().has_value());

        DAL::DbSet<ThrowingCopyEntity, ThrowingCopyRowDTO, DAL::SQLiteConnection> set(conn);
        std::vector<ThrowingCopyEntity> batch;
        batch.emplace_back(0, "First");
        batch.emplace_back(0, "Second");

        // Allow copies for first item (add_returning_impl copies item -> result), then throw on second item.
        ThrowingCopyEntity::copies_before_throw = 1;
        EXPECT_THROW((void)set.add_range_returning(batch), std::runtime_error);
        ThrowingCopyEntity::copies_before_throw = -1;

        EXPECT_FALSE(conn->in_transaction());
        auto cnt = conn->try_count<ThrowingCopyRowDTO>();
        ASSERT_TRUE(cnt.has_value());
        EXPECT_EQ(*cnt, 0u);
    }

    TEST(WriteBatchAtomicityHardeningTest, AssignedKeyUpsertRangeRollsBackAllRowsOnPartialFailure)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());
        auto conn = connRes.value();

        struct UniqueCtx : DAL::DbContextBase<DAL::SQLiteConnection>
        {
            Set<AssignedUniqueRowDTO> rows{ this };
            explicit UniqueCtx(std::shared_ptr<DAL::SQLiteConnection> c)
                : DAL::DbContextBase<DAL::SQLiteConnection>(std::move(c))
            {
            }
        };

        UniqueCtx db(conn);
        ASSERT_TRUE(db.ensure_created().has_value());
        ASSERT_TRUE(db.rows.add(AssignedUniqueRowDTO{ .id = 100, .slug = "taken" }).has_value());

        // Positive upsert & upsert_range on a table with BOTH PrimaryKey and Unique constraints
        // must target ON CONFLICT ("id") cleanly (insert new + update existing).
        ASSERT_TRUE(db.rows.upsert(AssignedUniqueRowDTO{ .id = 100, .slug = "taken" }).has_value());
        ASSERT_TRUE(db.rows.upsert(AssignedUniqueRowDTO{ .id = 101, .slug = "seed-101" }).has_value());
        ASSERT_TRUE(db.rows.upsert(AssignedUniqueRowDTO{ .id = 101, .slug = "updated-101" }).has_value());
        auto updated101 = db.rows.find(101);
        ASSERT_TRUE(updated101.has_value());
        EXPECT_EQ(updated101->slug.value(), "updated-101");
        ASSERT_TRUE(db.rows.remove(101).has_value());

        // Attach an AFTER INSERT trigger to prove row 1 genuinely executed and was rolled back
        // (not rejected at sqlite3_prepare_v2 before row 1 ran).
        ASSERT_TRUE(conn->execute_raw(
                            "CREATE TABLE UniqueAudit (inserted_id INTEGER NOT NULL);"
                            "CREATE TRIGGER trg_unique_audit AFTER INSERT ON AssignedUniqueRows "
                            "BEGIN INSERT INTO UniqueAudit (inserted_id) VALUES (NEW.id); END;")
                        .has_value());

        db.enable_change_tracking();

        // Row 1 is valid and triggers AFTER INSERT, row 2 violates UNIQUE(slug) with a different PK.
        std::vector<AssignedUniqueRowDTO> batch{ AssignedUniqueRowDTO{ .id = 1, .slug = "fresh" },
                                                 AssignedUniqueRowDTO{ .id = 2, .slug = "taken" } };
        auto res = db.rows.upsert_range(batch);
        ASSERT_FALSE(res.has_value());
        EXPECT_EQ(res.error().code, Core::DbErrorCode::DuplicateKey);
        EXPECT_FALSE(conn->in_transaction());

        // Row 1 and its trigger audit row MUST have been rolled back and MUST NOT be tracked.
        EXPECT_EQ(db.tracked_entities_count(), 0u);
        auto auditCnt = conn->query_rows("SELECT COUNT(*) FROM UniqueAudit;");
        ASSERT_TRUE(auditCnt.has_value());
        EXPECT_EQ(std::get<std::int64_t>(auditCnt->rows[0].cells[0]), 0);
        auto all = db.rows.try_to_vector();
        ASSERT_TRUE(all.has_value());
        ASSERT_EQ(all->size(), 1u);
        EXPECT_EQ(all->front().id.value(), 100);
    }

    TEST(WriteBatchAtomicityHardeningTest, ExplicitAndAutoIncrementKeysInAddUpsertAndUpsertRange)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());
        Phase30DbContext db(connRes.value());
        ASSERT_TRUE(db.ensure_created().has_value());
        db.enable_change_tracking();

        // Explicit non-zero PK on auto_incr table is honored on add() and upsert().
        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 50 }, "Explicit", "Id")).has_value());
        auto found50 = db.people.find(50);
        ASSERT_TRUE(found50.has_value());
        EXPECT_EQ(found50->getFirstName(), "Explicit");

        ASSERT_TRUE(db.people.upsert(Core::Person(Core::PersonId{ 42 }, "FortyTwo", "V1")).has_value());
        ASSERT_TRUE(db.people.upsert(Core::Person(Core::PersonId{ 42 }, "FortyTwo", "V2")).has_value());
        EXPECT_EQ(db.people.count(), 2u);
        auto found42 = db.people.find(42);
        ASSERT_TRUE(found42.has_value());
        EXPECT_EQ(found42->getLastName(), "V2");

        // Auto-increment placeholder (id = 0) in upsert_range hydrates generated IDs in ChangeTracker.
        db.clear_change_tracker();
        std::vector<Core::Person> autoBatch{ Core::Person(Core::PersonId{ 0 }, "AutoA", "Gen"),
                                             Core::Person(Core::PersonId{ 0 }, "AutoB", "Gen") };
        ASSERT_TRUE(db.people.upsert_range(autoBatch).has_value());
        EXPECT_EQ(db.people.count(), 4u);
        EXPECT_EQ(db.tracked_entities_count(), 2u);
        auto found51 = db.people.find_tracked(51);
        auto found52 = db.people.find_tracked(52);
        ASSERT_NE(found51, nullptr);
        ASSERT_NE(found52, nullptr);
        EXPECT_EQ(found51->getFirstName(), "AutoA");
        EXPECT_EQ(found52->getFirstName(), "AutoB");
    }

    struct NulKeyDoc
    {
        static constexpr auto tablename = "NulKeyDocs";
        sqlgen::PrimaryKey<std::string, false> slug{};
        std::string body{};
    };

    struct NulDocCtx : DAL::DbContextBase<DAL::SQLiteConnection>
    {
        Set<NulKeyDoc> docs{ this };
        explicit NulDocCtx(std::shared_ptr<DAL::SQLiteConnection> c)
            : DAL::DbContextBase<DAL::SQLiteConnection>(std::move(c))
        {
        }
    };

    TEST(TypedSqliteReaderHardeningTest, EmbeddedNulRoundTripsAcrossUpdateWhereDeleteAndRejectsRawSql)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());
        auto conn = connRes.value();

        NulDocCtx db(conn);
        ASSERT_TRUE(db.ensure_created().has_value());

        const std::string keyWithNul("k\0a", 3);
        const std::string bodyV1("v1\0tail", 7);
        const std::string bodyV2("v2\0\0end", 7);

        ASSERT_TRUE(db.docs.add(NulKeyDoc{ .slug = keyWithNul, .body = bodyV1 }).has_value());
        auto found1 = db.docs.try_find(keyWithNul);
        ASSERT_TRUE(found1.has_value() && found1->has_value());
        EXPECT_EQ((*found1)->body, bodyV1);

        // Parameterized update with embedded NUL in both PK and updated column
        ASSERT_TRUE(db.docs.update(NulKeyDoc{ .slug = keyWithNul, .body = bodyV2 }).has_value());
        auto found2 = db.docs.try_find(keyWithNul);
        ASSERT_TRUE(found2.has_value() && found2->has_value());
        EXPECT_EQ((*found2)->body, bodyV2);

        // DbQuery where_equal with embedded NUL string literal
        auto byBody = db.docs.where_equal<"body">(bodyV2).try_to_vector();
        ASSERT_TRUE(byBody.has_value()) << byBody.error().to_string();
        ASSERT_EQ(byBody->size(), 1u);
        EXPECT_EQ(byBody->front().slug.value(), keyWithNul);

        // Parameterized remove with embedded NUL in string PK
        ASSERT_TRUE(db.docs.remove(keyWithNul).has_value());
        EXPECT_EQ(db.docs.count(), 0u);

        // Raw execute_raw must reject embedded NUL instead of silently truncating at .c_str()
        const std::string rawWithNul("SELECT 1;\0DROP TABLE NulKeyDocs;", 32);
        auto rawRes = conn->execute_raw(rawWithNul);
        ASSERT_FALSE(rawRes.has_value());
        EXPECT_EQ(rawRes.error().code, Core::DbErrorCode::ConstraintViolation);
    }

    TEST(WriteBatchAtomicityHardeningTest, AutoIncrementUpsertRangeRollsBackAllRowsOnPartialFailure)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());
        auto conn = connRes.value();
        Phase30DbContext db(conn);
        ASSERT_TRUE(db.ensure_created().has_value());
        db.enable_change_tracking();

        ASSERT_TRUE(conn->execute_raw(
                            "CREATE TRIGGER trg_fail_boom BEFORE INSERT ON Person WHEN NEW.first_name = 'Boom' "
                            "BEGIN SELECT RAISE(ABORT, 'injected boom'); END;")
                        .has_value());

        std::vector<Core::Person> batch{ Core::Person(Core::PersonId{ 0 }, "ValidFirst", "User"),
                                         Core::Person(Core::PersonId{ 0 }, "Boom", "User") };
        auto res = db.people.upsert_range(batch);
        ASSERT_FALSE(res.has_value());
        EXPECT_FALSE(conn->in_transaction());

        auto all = db.people.try_to_vector();
        ASSERT_TRUE(all.has_value());
        EXPECT_TRUE(all->empty());
        EXPECT_EQ(db.tracked_entities_count(), 0u);
    }

    // =========================================================================
    // Phase 49 (P0): Ownership, Lifetimes, Deferred Eviction, Nested Tx & WAL
    // =========================================================================

    TEST(OwnershipAndLifetimeHardeningTest, SqliteConnectionIsNonCopyableAndWalVerifiesEffectiveMode)
    {
        static_assert(!std::is_copy_constructible_v<DAL::SQLiteConnection>);
        static_assert(!std::is_copy_assignable_v<DAL::SQLiteConnection>);
        static_assert(!std::is_move_constructible_v<DAL::SQLiteConnection>);
        static_assert(!std::is_move_assignable_v<DAL::SQLiteConnection>);

        // WAL on :memory: must fail because SQLite keeps journal_mode = memory.
        auto memRes = DAL::SQLiteConnection::connect(":memory:", DAL::SQLiteConnectionOptions{ .wal_mode = true });
        EXPECT_FALSE(memRes.has_value());

        // WAL on an on-disk database must succeed and read back "wal".
        const auto dbPath = make_unique_temp_db_path("codefirst_phase49_wal_check");
        const ScopedTempDbCleanup cleanup{ dbPath };

        {
            auto diskRes = DAL::SQLiteConnection::connect(dbPath, DAL::SQLiteConnectionOptions{ .wal_mode = true });
            ASSERT_TRUE(diskRes.has_value()) << diskRes.error().to_string();
            auto jmRes = (*diskRes)->query_rows("PRAGMA journal_mode;");
            ASSERT_TRUE(jmRes.has_value());
            ASSERT_EQ(jmRes->rows.size(), 1u);
            EXPECT_EQ(std::get<std::string>(jmRes->rows[0].cells[0]), "wal");
            (*diskRes)->close();
        }
    }

    TEST(OwnershipAndLifetimeHardeningTest, EscapedDbSetFindTrackedRefReturnsNulloptWhenContextExpired)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());
        auto conn = connRes.value();

        auto escapedSet = [&]()
        {
            Phase30DbContext local(conn);
            EXPECT_TRUE(local.ensure_created().has_value());
            EXPECT_TRUE(local.people.add(Core::Person(Core::PersonId{ 0 }, "Ada", "Lovelace")).has_value());
            return local.people;
        }();

        // Context has expired: find_tracked_ref MUST return std::nullopt instead of a dangling reference.
        auto expiredRef = escapedSet.find_tracked_ref(1);
        EXPECT_FALSE(expiredRef.has_value());
    }

    TEST(OwnershipAndLifetimeHardeningTest, RemoveDefersTrackerEvictionUntilDeleteConfirmedAndRestoresOnRollback)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());
        auto conn = connRes.value();
        Phase30DbContext db(conn);
        ASSERT_TRUE(db.ensure_created().has_value());
        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 0 }, "Alice", "Smith")).has_value());
        ASSERT_TRUE(db.somethings.add(Core::Something(Core::SomethingId{ 0 }, "Widget", "Tool")).has_value());
        ASSERT_TRUE(db.personSomethings.add(Core::Person_Something(Core::PersonId{ 1 }, Core::SomethingId{ 1 }, "Owner")).has_value());

        db.enable_change_tracking();
        auto tracked = db.people.find_tracked(1);
        ASSERT_NE(tracked, nullptr);
        EXPECT_EQ(db.tracked_entities_count(), 1u);

        // 1. Immediate remove rejected by FK constraint MUST keep entity tracked!
        auto failedDel = db.people.remove(Core::Person::Key{ Core::PersonId{ 1 } });
        ASSERT_FALSE(failedDel.has_value());
        EXPECT_EQ(db.tracked_entities_count(), 1u);
        tracked->setFirstName("StillTracked");
        ASSERT_TRUE(db.save_changes().has_value());
        EXPECT_EQ(db.people.find(1)->getFirstName(), "StillTracked");

        // 2. Remove child link, then delete parent inside an outer transaction that rolls back:
        //    tracker membership AND baseline must be restored!
        ASSERT_TRUE(db.personSomethings.remove(Core::Person_Something::Key{ Core::PersonId{ 1 }, Core::SomethingId{ 1 } }).has_value());
        ASSERT_TRUE(db.begin_transaction().has_value());
        ASSERT_TRUE(db.people.remove(Core::Person::Key{ Core::PersonId{ 1 } }).has_value());
        EXPECT_EQ(db.tracked_entities_count(), 0u);
        ASSERT_TRUE(db.rollback().has_value());
        EXPECT_EQ(db.tracked_entities_count(), 1u);
        tracked->setFirstName("RestoredAfterRollback");
        ASSERT_TRUE(db.save_changes().has_value());
        EXPECT_EQ(db.people.find(1)->getFirstName(), "RestoredAfterRollback");
    }

    TEST(OwnershipAndLifetimeHardeningTest, NestedTransactionWithInnerSaveChangesAndRollbackPreservesOuterQueue)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());
        auto conn = connRes.value();
        Phase30DbContext db(conn);
        ASSERT_TRUE(db.ensure_created().has_value());
        db.defer_changes(true);

        auto outerRes = db.transaction(
            [&]() -> std::expected<void, Core::DbError>
            {
                // Stage an outer operation before entering nested transaction.
                auto r1 = db.people.add(Core::Person(Core::PersonId{ 0 }, "OuterKept", "User"));
                if (!r1)
                {
                    return r1;
                }

                // Nested transaction stages an inner op, calls save_changes(), then returns error.
                auto innerRes = db.transaction(
                    [&]() -> std::expected<void, Core::DbError>
                    {
                        auto r2 = db.people.add(Core::Person(Core::PersonId{ 0 }, "InnerRolledBack", "User"));
                        if (!r2)
                        {
                            return r2;
                        }
                        auto sc = db.save_changes();
                        if (!sc)
                        {
                            return sc;
                        }
                        return std::unexpected<Core::DbError>{ Core::DbError::unknown("abort inner") };
                    });
                EXPECT_FALSE(innerRes.has_value());

                // Outer staged operation MUST still be intact (no empty std::move_only_function slots).
                return {};
            });
        ASSERT_TRUE(outerRes.has_value()) << outerRes.error().to_string();

        auto all = db.people.try_to_vector();
        ASSERT_TRUE(all.has_value());
        ASSERT_EQ(all->size(), 1u);
        EXPECT_EQ(all->front().getFirstName(), "OuterKept");
    }

    // =========================================================================
    // Phase 52 (P1): UoW Ghost Eviction, Cross-Context Savepoints & Baseline Sync
    // =========================================================================

    TEST(Phase52UoWAndSavepointHardeningTest, RollbackEvictsGhostEntitiesCreatedAfterCheckpoint)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());
        auto conn = connRes.value();
        Phase30DbContext db(conn);
        ASSERT_TRUE(db.ensure_created().has_value());
        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 0 }, "Original", "User")).has_value());

        db.enable_change_tracking();
        auto existing = db.people.find_tracked(1);
        ASSERT_NE(existing, nullptr);
        EXPECT_EQ(db.tracked_entities_count(), 1u);

        auto txRes = db.transaction(
            [&]() -> std::expected<void, Core::DbError>
            {
                auto created = db.people.add_returning(Core::Person(Core::PersonId{ 0 }, "Ghost", "Entity"));
                if (!created)
                {
                    return std::unexpected<Core::DbError>{ created.error() };
                }
                auto ghost = db.people.find_tracked(created->getId().get());
                EXPECT_NE(ghost, nullptr);
                EXPECT_EQ(db.tracked_entities_count(), 2u);
                return std::unexpected<Core::DbError>{ Core::DbError::unknown("abort transaction") };
            });
        ASSERT_FALSE(txRes.has_value());

        // Ghost entity created inside the rolled-back transaction MUST be evicted from the tracker.
        EXPECT_EQ(db.tracked_entities_count(), 1u);
        EXPECT_EQ(db.people.find_tracked(2), nullptr);
        ASSERT_TRUE(db.save_changes().has_value());

        // Existing entity tracked prior to the transaction remains tracked and updatable.
        existing->setFirstName("UpdatedOriginal");
        ASSERT_TRUE(db.save_changes().has_value());
        auto reloaded = db.people.find(1);
        ASSERT_TRUE(reloaded.has_value());
        EXPECT_EQ(reloaded->getFirstName(), "UpdatedOriginal");
    }

    TEST(Phase52UoWAndSavepointHardeningTest, SharedConnectionSavepointsAcrossTwoDbContextsAreIsolated)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());
        auto conn = connRes.value();
        Phase30DbContext dbA(conn);
        Phase30DbContext dbB(conn);
        ASSERT_TRUE(dbA.ensure_created().has_value());

        ASSERT_TRUE(dbA.begin_transaction().has_value());

        auto outerRes = dbA.transaction(
            [&]() -> std::expected<void, Core::DbError>
            {
                auto rA = dbA.people.add(Core::Person(Core::PersonId{ 0 }, "FromA", "User"));
                if (!rA)
                {
                    return rA;
                }

                // dbB shares the same SQLiteConnection; its savepoint name MUST NOT collide with dbA's.
                auto innerRes = dbB.transaction(
                    [&]() -> std::expected<void, Core::DbError>
                    {
                        auto rB = dbB.people.add(Core::Person(Core::PersonId{ 0 }, "FromB", "User"));
                        if (!rB)
                        {
                            return rB;
                        }
                        return std::unexpected<Core::DbError>{ Core::DbError::unknown("rollback dbB only") };
                    });
                EXPECT_FALSE(innerRes.has_value());
                return {};
            });
        ASSERT_TRUE(outerRes.has_value()) << outerRes.error().to_string();
        ASSERT_TRUE(dbA.commit().has_value());

        auto all = dbA.people.try_to_vector();
        ASSERT_TRUE(all.has_value());
        ASSERT_EQ(all->size(), 1u);
        EXPECT_EQ(all->front().getFirstName(), "FromA");
    }

    TEST(Phase52UoWAndSavepointHardeningTest, DeferredStageUpdateRefreshesTrackerBaseline)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());
        auto conn = connRes.value();
        Phase30DbContext db(conn);
        ASSERT_TRUE(db.ensure_created().has_value());
        ASSERT_TRUE(conn->execute_raw("CREATE TABLE update_audit (n INTEGER NOT NULL);").has_value());
        ASSERT_TRUE(conn->execute_raw(
                            "CREATE TRIGGER trg_count_person_updates AFTER UPDATE ON Person "
                            "BEGIN INSERT INTO update_audit(n) VALUES (1); END;")
                        .has_value());

        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 0 }, "Alice", "Initial")).has_value());

        db.enable_change_tracking();
        db.defer_changes(true);

        auto tracked = db.people.find_tracked(1);
        ASSERT_NE(tracked, nullptr);
        tracked->setFirstName("AliceUpdated");

        // Explicitly stage the update and flush via save_changes().
        ASSERT_TRUE(db.people.update(*tracked).has_value());
        ASSERT_TRUE(db.save_changes().has_value());

        auto countUpdates = [&]() -> std::int64_t
        {
            auto rows = conn->query_rows("SELECT COUNT(*) FROM update_audit;");
            EXPECT_TRUE(rows.has_value());
            return std::get<std::int64_t>(rows->rows[0].cells[0]);
        };
        const std::int64_t updatesAfterFirstFlush = countUpdates();
        EXPECT_EQ(updatesAfterFirstFlush, 1);

        // A second save_changes() MUST see refreshed baseline and emit 0 redundant UPDATEs.
        ASSERT_TRUE(db.save_changes().has_value());
        EXPECT_EQ(countUpdates(), 1);
    }

    // =========================================================================
    // Phase 53 (P1): Tracker Coherence, Nested TransactionScope & Atomic Drop
    // =========================================================================

    TEST(Phase53UoWAndTrackerCoherenceTest, ExplicitUpdateAndUpsertSyncTrackedInstanceWithoutReverting)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());
        Phase30DbContext db(connRes.value());
        ASSERT_TRUE(db.ensure_created().has_value());
        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 1 }, "Alice", "Initial")).has_value());

        db.enable_change_tracking();
        auto originalPtr = db.people.find_tracked(1);
        ASSERT_NE(originalPtr, nullptr);
        EXPECT_EQ(originalPtr->getFirstName(), "Alice");

        // 1. Explicit value update with a separate Person object MUST NOT be reverted on save_changes().
        ASSERT_TRUE(db.people.update(Core::Person(Core::PersonId{ 1 }, "Bob", "Updated")).has_value());
        ASSERT_TRUE(db.save_changes().has_value());
        auto afterSave = db.people.find(1);
        ASSERT_TRUE(afterSave.has_value());
        EXPECT_EQ(afterSave->getFirstName(), "Bob");

        auto trackedNow = db.people.find_tracked(1);
        ASSERT_NE(trackedNow, nullptr);
        EXPECT_EQ(trackedNow->getFirstName(), "Bob");

        // 2. Mutating the active tracked pointer and calling update(*trackedNow) preserves pointer tracking.
        trackedNow->setFirstName("Carol");
        ASSERT_TRUE(db.people.update(*trackedNow).has_value());
        trackedNow->setFirstName("CarolV2");
        ASSERT_TRUE(db.save_changes().has_value());
        EXPECT_EQ(db.people.find(1)->getFirstName(), "CarolV2");

        // 3. Explicit update inside a rolled-back transaction restores the prior tracked instance & baseline.
        auto txRes = db.transaction(
            [&]() -> std::expected<void, Core::DbError>
            {
                auto r = db.people.update(Core::Person(Core::PersonId{ 1 }, "Transient", "RolledBack"));
                if (!r)
                {
                    return r;
                }
                EXPECT_EQ(db.people.find_tracked(1)->getFirstName(), "Transient");
                return std::unexpected<Core::DbError>{ Core::DbError::unknown("abort update") };
            });
        ASSERT_FALSE(txRes.has_value());
        EXPECT_EQ(db.people.find_tracked(1).get(), trackedNow.get());
        EXPECT_EQ(db.people.find(1)->getFirstName(), "CarolV2");
        ASSERT_TRUE(db.save_changes().has_value());
        EXPECT_EQ(db.people.find(1)->getFirstName(), "CarolV2");
    }

    TEST(Phase53UoWAndTrackerCoherenceTest, NestedTransactionScopeFlushesStagedOpsBeforeDetectChanges)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());
        auto conn = connRes.value();
        Phase30DbContext db(conn);
        ASSERT_TRUE(db.ensure_created().has_value());
        ASSERT_TRUE(conn->execute_raw("CREATE TABLE txscope_updates (n INTEGER NOT NULL);").has_value());
        ASSERT_TRUE(conn->execute_raw(
                            "CREATE TRIGGER trg_txscope_updates AFTER UPDATE ON Person "
                            "BEGIN INSERT INTO txscope_updates(n) VALUES (1); END;")
                        .has_value());
        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 1 }, "Ada", "Initial")).has_value());

        db.enable_change_tracking();
        db.defer_changes(true);
        auto tracked = db.people.find_tracked(1);
        ASSERT_NE(tracked, nullptr);

        {
            DAL::TransactionScope outer(db);
            ASSERT_TRUE(outer.begin_result().has_value());
            {
                DAL::TransactionScope inner(db);
                ASSERT_TRUE(inner.begin_result().has_value());
                tracked->setFirstName("AdaScoped");
                ASSERT_TRUE(db.people.update(*tracked).has_value());
                ASSERT_TRUE(inner.commit().has_value());
            }
            ASSERT_TRUE(outer.commit().has_value());
        }

        auto rows = conn->query_rows("SELECT COUNT(*) FROM txscope_updates;");
        ASSERT_TRUE(rows.has_value());
        EXPECT_EQ(std::get<std::int64_t>(rows->rows[0].cells[0]), 1);
        EXPECT_EQ(db.people.find(1)->getFirstName(), "AdaScoped");
    }

    struct AtomicDropGoodDTO
    {
        static constexpr auto tablename = "AtomicDropGood";
        sqlgen::PrimaryKey<std::int32_t, true> id{ 0 };
        std::string label;
    };

    struct AtomicDropBadDTO
    {
        static constexpr auto tablename = "Bad\"DropTable";
        sqlgen::PrimaryKey<std::int32_t, true> id{ 0 };
        std::string label;
    };

    struct AtomicDropContext : DAL::DbContextBase<DAL::SQLiteConnection>
    {
        Set<AtomicDropGoodDTO> good{ this };
        Set<AtomicDropBadDTO> bad{ this };

        explicit AtomicDropContext(std::shared_ptr<DAL::SQLiteConnection> c)
            : DAL::DbContextBase<DAL::SQLiteConnection>(std::move(c))
        {
        }
    };

    TEST(Phase53UoWAndTrackerCoherenceTest, EnsureDeletedIsTransactionalAndRollsBackOnFailure)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());
        auto conn = connRes.value();
        ASSERT_TRUE(conn->ensure_table_created<AtomicDropGoodDTO>().has_value());

        AtomicDropContext db(conn);
        auto dropRes = db.ensure_deleted();
        ASSERT_FALSE(dropRes.has_value());
        EXPECT_FALSE(db.in_transaction());

        // AtomicDropGood MUST still exist because ensure_deleted() rolled back when AtomicDropBad failed.
        auto tablesRes = db.list_tables();
        ASSERT_TRUE(tablesRes.has_value());
        EXPECT_TRUE(std::ranges::contains(*tablesRes, std::string_view{ "AtomicDropGood" }));
    }

    // =========================================================================
    // Phase 55 (P0): Row Identity & Key Safety (Range Checks & Immutable Tracked PK)
    // =========================================================================

    struct MutableCompositeLinkDTO
    {
        static constexpr auto tablename = "MutableCompositeLinks";
        static constexpr bool composite_primary_key = true;
        sqlgen::PrimaryKey<std::int32_t, false> left_id{ 0 };
        sqlgen::PrimaryKey<std::int32_t, false> right_id{ 0 };
        std::string tag;
    };

    struct Phase55IdentityContext : DAL::DbContextBase<DAL::SQLiteConnection>
    {
        Set<PocoUser> users{ this };
        Set<MutableCompositeLinkDTO> links{ this };

        explicit Phase55IdentityContext(std::shared_ptr<DAL::SQLiteConnection> c)
            : DAL::DbContextBase<DAL::SQLiteConnection>(std::move(c))
        {
        }
    };

    TEST(Phase55RowIdentityTest, OversizedRawKeyRejectedAcrossAllDbSetOverloadsAndReopenIntact)
    {
        const auto dbPath = make_unique_temp_db_path("codefirst_phase55_raw_key");
        const ScopedTempDbCleanup cleanup{ dbPath };

        const std::int64_t wrappedOne = (1LL << 32) + 1LL; // 4,294,967,297 -> would truncate to int32_t(1)

        {
            auto connRes = DAL::SQLiteConnection::connect(dbPath);
            ASSERT_TRUE(connRes.has_value());
            Phase55IdentityContext db(connRes.value());
            ASSERT_TRUE(db.ensure_created().has_value());
            db.enable_change_tracking();

            ASSERT_TRUE(db.users.add(PocoUser{ .user_id = 1, .username = "row1" }).has_value());
            ASSERT_TRUE(db.users.add(PocoUser{ .user_id = 2, .username = "row2" }).has_value());
            ASSERT_TRUE(db.links.add(MutableCompositeLinkDTO{ .left_id = 1, .right_id = 1, .tag = "L11" }).has_value());
            ASSERT_TRUE(db.links.add(MutableCompositeLinkDTO{ .left_id = 1, .right_id = 2, .tag = "L12" }).has_value());

            // Single-key oversized integer must NOT silently target row 1.
            EXPECT_FALSE(db.users.find(wrappedOne).has_value());
            EXPECT_FALSE(db.users.contains(wrappedOne));
            EXPECT_EQ(db.users.find_tracked(wrappedOne), nullptr);
            EXPECT_FALSE(db.users.find_tracked_ref(wrappedOne).has_value());

            auto tf = db.users.try_find(wrappedOne);
            ASSERT_FALSE(tf.has_value());
            EXPECT_EQ(tf.error().code, Core::DbErrorCode::ConstraintViolation);

            auto rm = db.users.remove(wrappedOne);
            ASSERT_FALSE(rm.has_value());
            EXPECT_EQ(rm.error().code, Core::DbErrorCode::ConstraintViolation);

            db.defer_changes(true);
            auto srm = db.users.stage_remove(wrappedOne);
            ASSERT_FALSE(srm.has_value());
            EXPECT_EQ(srm.error().code, Core::DbErrorCode::ConstraintViolation);
            EXPECT_EQ(db.pending_changes_count(), 0u);
            db.defer_changes(false);

            // Composite-key oversized integer must NOT silently target (1, 1).
            EXPECT_FALSE(db.links.find(wrappedOne, std::int64_t{ 1 }).has_value());
            EXPECT_FALSE(db.links.contains(std::int64_t{ 1 }, wrappedOne));
            EXPECT_EQ(db.links.find_tracked(wrappedOne, std::int64_t{ 1 }), nullptr);
            auto ctf = db.links.try_find(wrappedOne, std::int64_t{ 1 });
            ASSERT_FALSE(ctf.has_value());
            EXPECT_EQ(ctf.error().code, Core::DbErrorCode::ConstraintViolation);
            auto crm = db.links.remove(wrappedOne, std::int64_t{ 1 });
            ASSERT_FALSE(crm.has_value());
            EXPECT_EQ(crm.error().code, Core::DbErrorCode::ConstraintViolation);

            connRes.value()->close();
        }

        // Reopen database and verify all rows remain completely untouched.
        {
            auto connRes = DAL::SQLiteConnection::connect(dbPath);
            ASSERT_TRUE(connRes.has_value());
            Phase55IdentityContext db(connRes.value());
            ASSERT_EQ(db.users.count(), 2u);
            EXPECT_EQ(db.users.find(1)->username, "row1");
            EXPECT_EQ(db.users.find(2)->username, "row2");
            ASSERT_EQ(db.links.count(), 2u);
            EXPECT_EQ(db.links.find(1, 1)->tag, "L11");
            EXPECT_EQ(db.links.find(1, 2)->tag, "L12");
            connRes.value()->close();
        }
    }

    TEST(Phase55RowIdentityTest, TrackedPrimaryKeyMutationRejectedForSingleAndCompositeKeys)
    {
        const auto dbPath = make_unique_temp_db_path("codefirst_phase55_rekey_reject");
        const ScopedTempDbCleanup cleanup{ dbPath };

        {
            auto connRes = DAL::SQLiteConnection::connect(dbPath);
            ASSERT_TRUE(connRes.has_value());
            Phase55IdentityContext db(connRes.value());
            ASSERT_TRUE(db.ensure_created().has_value());
            db.enable_change_tracking();

            ASSERT_TRUE(db.users.add(PocoUser{ .user_id = 1, .username = "alice" }).has_value());
            ASSERT_TRUE(db.users.add(PocoUser{ .user_id = 2, .username = "bob" }).has_value());
            ASSERT_TRUE(db.links.add(MutableCompositeLinkDTO{ .left_id = 1, .right_id = 1, .tag = "orig11" }).has_value());
            ASSERT_TRUE(db.links.add(MutableCompositeLinkDTO{ .left_id = 1, .right_id = 2, .tag = "orig12" }).has_value());

            // 1. Mutate single-key tracked entity's PK from 1 -> 2: save_changes MUST fail and NOT overwrite row 2.
            auto u1 = db.users.find_tracked(1);
            ASSERT_NE(u1, nullptr);
            u1->user_id = 2;
            u1->username = "hijacked";
            auto saveSingle = db.save_changes();
            ASSERT_FALSE(saveSingle.has_value());
            EXPECT_EQ(saveSingle.error().code, Core::DbErrorCode::ConstraintViolation);

            // Restore u1's PK so we can test composite-key mutation cleanly.
            u1->user_id = 1;
            u1->username = "alice";
            ASSERT_TRUE(db.save_changes().has_value());

            // 2. Mutate composite-key tracked entity's PK from (1, 1) -> (1, 2): MUST fail and NOT overwrite (1, 2).
            auto l11 = db.links.find_tracked(1, 1);
            ASSERT_NE(l11, nullptr);
            l11->right_id = 2;
            l11->tag = "hijacked_link";
            auto saveComp = db.save_changes();
            ASSERT_FALSE(saveComp.has_value());
            EXPECT_EQ(saveComp.error().code, Core::DbErrorCode::ConstraintViolation);

            connRes.value()->close();
        }

        // Reopen database and verify rows 1, 2, (1,1), (1,2) were never overwritten.
        {
            auto connRes = DAL::SQLiteConnection::connect(dbPath);
            ASSERT_TRUE(connRes.has_value());
            Phase55IdentityContext db(connRes.value());
            EXPECT_EQ(db.users.find(1)->username, "alice");
            EXPECT_EQ(db.users.find(2)->username, "bob");
            EXPECT_EQ(db.links.find(1, 1)->tag, "orig11");
            EXPECT_EQ(db.links.find(1, 2)->tag, "orig12");
            connRes.value()->close();
        }
    }

    // =========================================================================
    // Phase 57: Tracker Ownership, Exact Bulk Invalidation, Expired Sets,
    //           Statement-Local INSERT RETURNING & Connection Batch Atomicity
    // =========================================================================

    TEST(Phase57TrackerAndAtomicityTest, InsertTrackRemoveRollbackDoesNotResurrectGhost)
    {
        const auto dbPath = make_unique_temp_db_path("codefirst_phase57_no_ghost");
        const ScopedTempDbCleanup cleanup{ dbPath };

        {
            auto connRes = DAL::SQLiteConnection::connect(dbPath);
            ASSERT_TRUE(connRes.has_value());
            Phase30DbContext db(connRes.value());
            ASSERT_TRUE(db.ensure_created().has_value());
            db.enable_change_tracking();

            ASSERT_TRUE(db.begin_transaction().has_value());
            auto inserted = db.people.add_returning(Core::Person(Core::PersonId{ 0 }, "Transient", "Ghost"));
            ASSERT_TRUE(inserted.has_value()) << inserted.error().to_string();
            auto tracked = db.people.find_tracked(inserted->getId().get());
            ASSERT_NE(tracked, nullptr);
            EXPECT_EQ(db.tracked_entities_count(), 1u);

            ASSERT_TRUE(db.people.remove(inserted->getId().get()).has_value());
            EXPECT_EQ(db.tracked_entities_count(), 0u);

            ASSERT_TRUE(db.rollback().has_value());
            // Rolling back must NOT resurrect the deleted row that never existed before begin_transaction().
            EXPECT_EQ(db.tracked_entities_count(), 0u);
            EXPECT_EQ(db.people.find_tracked(inserted->getId().get()), nullptr);
            EXPECT_EQ(db.people.count(), 0u);
            ASSERT_TRUE(db.save_changes().has_value());
            connRes.value()->close();
        }

        // Reopen and verify row is absent.
        {
            auto connRes = DAL::SQLiteConnection::connect(dbPath);
            ASSERT_TRUE(connRes.has_value());
            Phase30DbContext db(connRes.value());
            EXPECT_EQ(db.people.count(), 0u);
            connRes.value()->close();
        }
    }

    TEST(Phase57TrackerAndAtomicityTest, BulkUpdateWhereAndRemoveWherePreserveUnrelatedDirtyTrackedEntities)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());
        Phase30DbContext db(connRes.value());
        ASSERT_TRUE(db.ensure_created().has_value());

        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 1 }, "Alice", "Keep")).has_value());
        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 2 }, "Bob", "Target")).has_value());
        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 3 }, "Carol", "DeleteMe")).has_value());

        db.enable_change_tracking();
        auto alice = db.people.find_tracked(1);
        auto bob = db.people.find_tracked(2);
        auto carol = db.people.find_tracked(3);
        ASSERT_NE(alice, nullptr);
        ASSERT_NE(bob, nullptr);
        ASSERT_NE(carol, nullptr);
        EXPECT_EQ(db.tracked_entities_count(), 3u);

        // Mutate Alice in memory (unsaved dirty edit).
        alice->setFirstName("AliceEdited");

        // 1. 0-row update_where must NOT evict Alice, Bob, or Carol.
        ASSERT_TRUE(db.people.where_equal<"lastName">("NoSuchMatch").template update_where<"firstName">("Nobody").has_value());
        EXPECT_EQ(db.tracked_entities_count(), 3u);

        // 2. Partial update_where matching only Bob evicts ONLY Bob, keeping Alice (dirty) and Carol tracked.
        ASSERT_TRUE(db.people.where_equal<"lastName">("Target").template update_where<"firstName">("BobBulk").has_value());
        EXPECT_EQ(db.tracked_entities_count(), 2u);

        // 3. 0-row remove_where must NOT evict Alice or Carol.
        ASSERT_TRUE(db.people.where_equal<"lastName">("StillNoMatch").remove_where().has_value());
        EXPECT_EQ(db.tracked_entities_count(), 2u);

        // 4. Unpaginated remove_where matching only Carol evicts ONLY Carol, keeping Alice's dirty edit intact.
        ASSERT_TRUE(db.people.where_equal<"lastName">("DeleteMe").remove_where().has_value());
        EXPECT_EQ(db.tracked_entities_count(), 1u);

        // 5. save_changes() must persist Alice's unsaved edit without touching Bob's bulk update.
        ASSERT_TRUE(db.save_changes().has_value());
        EXPECT_EQ(db.people.count(), 2u);
        EXPECT_EQ(db.people.find(1)->getFirstName(), "AliceEdited");
        EXPECT_EQ(db.people.find(2)->getFirstName(), "BobBulk");
        EXPECT_FALSE(db.people.find(3).has_value());
    }

    TEST(Phase57TrackerAndAtomicityTest, ExpiredContextBoundDbSetRejectsAllMutationsAndWritesNothing)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());
        auto conn = connRes.value();

        std::optional<decltype(Phase30DbContext::people)> escapedSet;
        {
            Phase30DbContext db(conn);
            ASSERT_TRUE(db.ensure_created().has_value());
            ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 1 }, "Existing", "Row")).has_value());
            db.defer_changes(true);
            escapedSet.emplace(db.people);
        }

        // Owning DbContext is now destroyed; every mutating call on escapedSet MUST fail with ConnectionError.
        auto rAdd = escapedSet->add(Core::Person(Core::PersonId{ 2 }, "Ghost", "Add"));
        ASSERT_FALSE(rAdd.has_value());
        EXPECT_EQ(rAdd.error().code, Core::DbErrorCode::ConnectionError);

        auto rAddRet = escapedSet->add_returning(Core::Person(Core::PersonId{ 0 }, "Ghost", "AddRet"));
        ASSERT_FALSE(rAddRet.has_value());
        EXPECT_EQ(rAddRet.error().code, Core::DbErrorCode::ConnectionError);

        auto rAddRange = escapedSet->add_range({ Core::Person(Core::PersonId{ 3 }, "Ghost", "Range") });
        ASSERT_FALSE(rAddRange.has_value());
        EXPECT_EQ(rAddRange.error().code, Core::DbErrorCode::ConnectionError);

        auto rAddRangeRet = escapedSet->add_range_returning({ Core::Person(Core::PersonId{ 0 }, "Ghost", "RangeRet") });
        ASSERT_FALSE(rAddRangeRet.has_value());
        EXPECT_EQ(rAddRangeRet.error().code, Core::DbErrorCode::ConnectionError);

        auto rUpd = escapedSet->update(Core::Person(Core::PersonId{ 1 }, "Hijacked", "Row"));
        ASSERT_FALSE(rUpd.has_value());
        EXPECT_EQ(rUpd.error().code, Core::DbErrorCode::ConnectionError);

        auto rRem = escapedSet->remove(1);
        ASSERT_FALSE(rRem.has_value());
        EXPECT_EQ(rRem.error().code, Core::DbErrorCode::ConnectionError);

        auto rUps = escapedSet->upsert(Core::Person(Core::PersonId{ 1 }, "HijackedUpsert", "Row"));
        ASSERT_FALSE(rUps.has_value());
        EXPECT_EQ(rUps.error().code, Core::DbErrorCode::ConnectionError);

        auto rUpsRange = escapedSet->upsert_range({ Core::Person(Core::PersonId{ 4 }, "Ghost", "UpsertRange") });
        ASSERT_FALSE(rUpsRange.has_value());
        EXPECT_EQ(rUpsRange.error().code, Core::DbErrorCode::ConnectionError);

        auto rBulkDel = escapedSet->where_equal<"firstName">("Existing").remove_where();
        ASSERT_FALSE(rBulkDel.has_value());
        EXPECT_EQ(rBulkDel.error().code, Core::DbErrorCode::ConnectionError);

        auto rBulkUpd = escapedSet->where_equal<"firstName">("Existing").template update_where<"firstName">("HijackedBulk");
        ASSERT_FALSE(rBulkUpd.has_value());
        EXPECT_EQ(rBulkUpd.error().code, Core::DbErrorCode::ConnectionError);

        // Verify the database was completely untouched.
        Phase30DbContext verifyDb(conn);
        EXPECT_EQ(verifyDb.people.count(), 1u);
        auto row1 = verifyDb.people.find(1);
        ASSERT_TRUE(row1.has_value());
        EXPECT_EQ(row1->getFirstName(), "Existing");
    }

    TEST(Phase57TrackerAndAtomicityTest, TriggerRaiseIgnoreFailsAddReturningInsteadOfReturningStaleRowId)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());
        auto conn = connRes.value();
        Phase30DbContext db(conn);
        ASSERT_TRUE(db.ensure_created().has_value());

        // Seed row with explicit id=27 so sqlite3_last_insert_rowid() becomes 27.
        auto seed = db.people.add_returning(Core::Person(Core::PersonId{ 27 }, "Seed", "Row"));
        ASSERT_TRUE(seed.has_value());
        EXPECT_EQ(seed->getId().get(), 27);

        // Install a BEFORE INSERT trigger that silently ignores rows whose first_name is "IGNORED".
        ASSERT_TRUE(conn->execute_raw(
                            "CREATE TRIGGER trg_ignore_person BEFORE INSERT ON Person "
                            "WHEN NEW.first_name = 'IGNORED' "
                            "BEGIN SELECT RAISE(IGNORE); END;")
                        .has_value());

        // Single add_returning MUST fail with ConstraintViolation rather than returning stale id 27.
        auto ignoredRes = db.people.add_returning(Core::Person(Core::PersonId{ 0 }, "IGNORED", "Row"));
        ASSERT_FALSE(ignoredRes.has_value());
        EXPECT_EQ(ignoredRes.error().code, Core::DbErrorCode::ConstraintViolation) << ignoredRes.error().to_string();

        // Batch add_range_returning with an ignored second row MUST fail and roll back the first row too.
        auto rangeRes = db.people.add_range_returning(
            {
                Core::Person(Core::PersonId{ 0 }, "ValidBeforeIgnored", "Row"),
                Core::Person(Core::PersonId{ 0 }, "IGNORED", "Row"),
            });
        ASSERT_FALSE(rangeRes.has_value());
        EXPECT_EQ(rangeRes.error().code, Core::DbErrorCode::ConstraintViolation) << rangeRes.error().to_string();
        EXPECT_EQ(db.people.count(), 1u);
    }

    TEST(Phase57TrackerAndAtomicityTest, DirectConnectionAndRepositoryInsertAndUpsertManyAreAtomic)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());
        auto conn = connRes.value();
        ASSERT_TRUE(conn->ensure_table_created<PocoUser>().has_value());
        ASSERT_TRUE(conn->execute_raw("CREATE UNIQUE INDEX ux_poco_user_name ON PocoUsers(username);").has_value());

        // 1. Direct SQLiteConnection::try_insert_many fails on 2nd row (duplicate username): 1st row MUST roll back.
        auto insRes = conn->try_insert_many<PocoUser>({
            PocoUser{ .user_id = 1, .username = "unique_name" },
            PocoUser{ .user_id = 2, .username = "unique_name" },
        });
        ASSERT_FALSE(insRes.has_value());
        EXPECT_EQ(insRes.error().code, Core::DbErrorCode::DuplicateKey) << insRes.error().to_string();
        EXPECT_EQ(conn->count<PocoUser>(), 0u);

        // 2. Direct SQLiteConnection::try_upsert_many fails on 2nd row (unique username collision on different PK): 1st row MUST roll back.
        auto upsRes = conn->try_upsert_many<PocoUser>({
            PocoUser{ .user_id = 10, .username = "dup_upsert" },
            PocoUser{ .user_id = 11, .username = "dup_upsert" },
        });
        ASSERT_FALSE(upsRes.has_value());
        EXPECT_EQ(upsRes.error().code, Core::DbErrorCode::DuplicateKey) << upsRes.error().to_string();
        EXPECT_EQ(conn->count<PocoUser>(), 0u);

        // 3. Direct GenericRepository::insert_many fails on 2nd row: 1st row MUST roll back.
        DAL::Repositories::GenericRepository<PocoUser, PocoUser, DAL::SQLiteConnection> repo(conn);
        auto repoRes = repo.insert_many(
            {
                PocoUser{ .user_id = 20, .username = "repo_dup" },
                PocoUser{ .user_id = 21, .username = "repo_dup" },
            });
        ASSERT_FALSE(repoRes.has_value());
        EXPECT_EQ(repoRes.error().code, Core::DbErrorCode::DuplicateKey) << repoRes.error().to_string();
        EXPECT_EQ(conn->count<PocoUser>(), 0u);

        // 4. Nested inside an active transaction, try_insert_many uses a savepoint so outer row survives while failed batch rolls back.
        ASSERT_TRUE(conn->begin_transaction().has_value());
        ASSERT_TRUE(conn->try_insert(PocoUser{ .user_id = 100, .username = "outer_row" }).has_value());
        auto nestedRes = conn->try_insert_many<PocoUser>({
            PocoUser{ .user_id = 101, .username = "nested_dup" },
            PocoUser{ .user_id = 102, .username = "nested_dup" },
        });
        ASSERT_FALSE(nestedRes.has_value());
        ASSERT_TRUE(conn->commit().has_value());
        EXPECT_EQ(conn->count<PocoUser>(), 1u);
        EXPECT_TRUE(conn->exists<PocoUser>(100));
        EXPECT_FALSE(conn->exists<PocoUser>(101));
    }

} // namespace
