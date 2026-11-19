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
        EXPECT_TRUE(conn->enable_wal_mode().has_value());

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

} // namespace
