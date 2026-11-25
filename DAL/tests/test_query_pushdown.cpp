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

    TEST(Phase10QueryBuilderAndChangeTrackerTest, ServerSideQueryPushdown_WhereOrderByAndPagination)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());

        struct PersonQueryDbContext : public DAL::DbContextBase<DAL::SQLiteConnection>
        {
            Set<Core::Person> people{ this };
            explicit PersonQueryDbContext(std::shared_ptr<DAL::SQLiteConnection> c)
                : DAL::DbContextBase<DAL::SQLiteConnection>(std::move(c))
            {
            }
        };

        PersonQueryDbContext db(connRes.value());
        ASSERT_TRUE(db.ensure_created().has_value());

        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 1 }, "Charlie", "Smith")).has_value());
        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 2 }, "Alice", "Smith")).has_value());
        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 3 }, "Bob", "Smith")).has_value());
        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 4 }, "Diana", "Jones")).has_value());
        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 5 }, "Eve", "Smith")).has_value());

        // where_equal + ascending order_by
        auto smithAsc = db.people.where_equal<"last_name">("Smith").order_by<"first_name">(false).to_vector();
        ASSERT_EQ(smithAsc.size(), 4u);
        EXPECT_EQ(smithAsc[0].getFirstName(), "Alice");
        EXPECT_EQ(smithAsc[1].getFirstName(), "Bob");
        EXPECT_EQ(smithAsc[2].getFirstName(), "Charlie");
        EXPECT_EQ(smithAsc[3].getFirstName(), "Eve");

        // where_equal + descending order_by + page(limit=2, offset=1)
        auto smithDescPage = db.people.where_equal<"last_name">("Smith").order_by<"first_name">(true).page(2, 1).to_vector();
        ASSERT_EQ(smithDescPage.size(), 2u);
        EXPECT_EQ(smithDescPage[0].getFirstName(), "Charlie");
        EXPECT_EQ(smithDescPage[1].getFirstName(), "Bob");

        // where_sql with raw sqlgen condition + first_or_default()
        auto firstJones = db.people.where_sql(sqlgen::Col<"last_name", "", false>{} == std::string("Jones")).first_or_default();
        ASSERT_TRUE(firstJones.has_value());
        EXPECT_EQ(firstJones->getFirstName(), "Diana");

        // Chained order_by + page regardless of filtering
        auto pagedAll = db.people.order_by<"id">(true).page(2, 2).to_vector();
        ASSERT_EQ(pagedAll.size(), 2u);
        EXPECT_EQ(pagedAll[0].getFirstName(), "Bob");
        EXPECT_EQ(pagedAll[1].getFirstName(), "Alice");
    }

    TEST(Phase12ComposableQueryTest, ChainedAndOr_RichPredicates_CountAndAny)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());

        struct PersonQueryDbContext : public DAL::DbContextBase<DAL::SQLiteConnection>
        {
            Set<Core::Person> people{ this };
            explicit PersonQueryDbContext(std::shared_ptr<DAL::SQLiteConnection> c)
                : DAL::DbContextBase<DAL::SQLiteConnection>(std::move(c))
            {
            }
        };

        PersonQueryDbContext db(connRes.value());
        ASSERT_TRUE(db.ensure_created().has_value());

        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 1 }, "Charlie", "Smith")).has_value());
        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 2 }, "Alice", "Smith")).has_value());
        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 3 }, "Bob", "Smith")).has_value());
        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 4 }, "Diana", "Jones")).has_value());
        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 5 }, "Alicia", "Smith")).has_value());

        // Chained AND + > + LIKE
        auto filtered = db.people.where_equal<"last_name">("Smith")
                            .where_gt<"id">(1)
                            .where_like<"first_name">("%li%")
                            .order_by<"id">(false)
                            .to_vector();
        ASSERT_EQ(filtered.size(), 2u);
        EXPECT_EQ(filtered[0].getFirstName(), "Alice");
        EXPECT_EQ(filtered[1].getFirstName(), "Alicia");

        // OR chaining
        auto orQuery = db.people.where_equal<"first_name">("Bob").or_where_equal<"first_name">("Diana");
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
        auto leNotEq = db.people.where_le<"id">(3).where_not_equal<"first_name">("Bob").to_vector();
        EXPECT_EQ(leNotEq.size(), 2u);
    }

    // =========================================================================
    // Phase 13: Atomic Upsert & Bulk Server-Side remove_where
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
            {
            }
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
        ASSERT_TRUE(products.add(Phase21Product{ .name = "Beta", .notes = std::nullopt, .score = 20, .price = 25.0 }).has_value());
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

    TEST(Phase24SortingProjectionAndBetweenTest, MultiColumnSortProjectionAndPredicates)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());
        auto conn = connRes.value();
        ASSERT_TRUE(conn->ensure_table_created<DAL::Schema::PersonDTO>().has_value());

        DAL::DbSet<Core::Person, DAL::Schema::PersonDTO, DAL::SQLiteConnection> people(conn);
        ASSERT_TRUE(people.add(Core::Person(Core::PersonId{ 1 }, "Alice", "Smith")).has_value());
        ASSERT_TRUE(people.add(Core::Person(Core::PersonId{ 2 }, "Zack", "Smith")).has_value());
        ASSERT_TRUE(people.add(Core::Person(Core::PersonId{ 3 }, "Bob", "Adams")).has_value());
        ASSERT_TRUE(people.add(Core::Person(Core::PersonId{ 4 }, "Charlie", "Adams")).has_value());

        // Order by lastName ASC, then firstName DESC -> Charlie Adams, Bob Adams, Zack Smith, Alice Smith
        auto sortedNames = people.order_by<"lastName">(false).then_by<"firstName">(true).select<"firstName">();
        ASSERT_EQ(sortedNames.size(), 4u);
        EXPECT_EQ(sortedNames[0], "Charlie");
        EXPECT_EQ(sortedNames[1], "Bob");
        EXPECT_EQ(sortedNames[2], "Zack");
        EXPECT_EQ(sortedNames[3], "Alice");

        // Filtered projection
        auto smithNames = people.where_equal<"lastName">("Smith").order_by<"firstName">(false).select<"firstName">();
        ASSERT_EQ(smithNames.size(), 2u);
        EXPECT_EQ(smithNames[0], "Alice");
        EXPECT_EQ(smithNames[1], "Zack");

        // where_between & where_not_in_vec
        auto midIds = people.where_between<"id">(2, 3).order_by<"id">(false).select<"id">();
        ASSERT_EQ(midIds.size(), 2u);
        EXPECT_EQ(midIds[0], 2);
        EXPECT_EQ(midIds[1], 3);

        auto excluded = people.where_not_in_vec<"id">(std::vector<int>{ 1, 4 }).order_by<"id">(false).select<"id">();
        ASSERT_EQ(excluded.size(), 2u);
        EXPECT_EQ(excluded[0], 2);
        EXPECT_EQ(excluded[1], 3);
    }

    // =========================================================================
    // Phase 25: Bulk update_where & Batched query_children_batch / query_parents_batch
    // =========================================================================

    TEST(Phase25BulkUpdateAndBatchedNavTest, UpdateWhereAndBatchedNavigation)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());
        Phase25Context db(connRes.value());
        ASSERT_TRUE(db.ensure_created().has_value());
        db.enable_change_tracking(true);

        auto a1 = db.authors.add_returning(Phase25Author{ .name = "Author One" }).value();
        auto a2 = db.authors.add_returning(Phase25Author{ .name = "Author Two" }).value();

        ASSERT_TRUE(
            db.books.add(Phase25Book{ .book_id = 1, .author_id = a1.author_id.value(), .title = "Book A", .genre = "SciFi" }).has_value());
        ASSERT_TRUE(
            db.books.add(Phase25Book{ .book_id = 2, .author_id = a1.author_id.value(), .title = "Book B", .genre = "SciFi" }).has_value());
        ASSERT_TRUE(
            db.books.add(Phase25Book{ .book_id = 3, .author_id = a2.author_id.value(), .title = "Book C", .genre = "Drama" }).has_value());

        // Track a book and mutate it, then run update_where which should evict tracked entries
        auto trackedBook = db.books.find_tracked(1);
        ASSERT_NE(trackedBook, nullptr);
        trackedBook->title = "StaleMutatedTitle";

        auto updRes = db.books.where_equal<"genre">("SciFi").update_where<"genre">("Speculative");
        ASSERT_TRUE(updRes.has_value()) << updRes.error().to_string();
        ASSERT_TRUE(db.save_changes().has_value());

        auto reloaded = db.books.find(1);
        ASSERT_TRUE(reloaded.has_value());
        EXPECT_EQ(reloaded->genre, "Speculative");
        EXPECT_EQ(reloaded->title, "Book A");

        // Batched 1:N and N:1 loading
        auto allChildren = db.query_children_batch<Phase25Book>(std::vector<Phase25Author>{ a1, a2 });
        ASSERT_TRUE(allChildren.has_value()) << allChildren.error().to_string();
        EXPECT_EQ(allChildren->size(), 3u);

        auto allParents = db.query_parents_batch<Phase25Author>(*allChildren);
        ASSERT_TRUE(allParents.has_value()) << allParents.error().to_string();
        EXPECT_EQ(allParents->size(), 2u);
    }

    TEST(Phase28NonTrackingAnyAndMultiColUpdateTest, NonTrackingAnyAndVariadicUpdateWhere)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());
        Phase25Context db(connRes.value());
        ASSERT_TRUE(db.ensure_created().has_value());
        db.enable_change_tracking(true);

        auto a1 = db.authors.add_returning(Phase25Author{ .name = "Author1" }).value();
        ASSERT_TRUE(db.books.add(Phase25Book{ .book_id = 1, .author_id = a1.author_id.value(), .title = "Old Title", .genre = "SciFi" })
                        .has_value());
        ASSERT_TRUE(
            db.books.add(Phase25Book{ .book_id = 2, .author_id = a1.author_id.value(), .title = "Other", .genre = "Drama" }).has_value());

        // Clear any tracked snapshot from add_returning
        db.clear_change_tracker();
        EXPECT_EQ(db.tracked_entities_count(), 0u);

        // any() / try_any() must NOT pollute ChangeTracker
        EXPECT_TRUE(db.books.where_equal<"genre">("SciFi").any());
        auto anyRes = db.books.where_equal<"genre">("Drama").try_any();
        ASSERT_TRUE(anyRes.has_value());
        EXPECT_TRUE(*anyRes);
        EXPECT_EQ(db.tracked_entities_count(), 0u);

        // try_first_or_default()
        auto firstRes = db.books.where_equal<"genre">("SciFi").try_first_or_default();
        ASSERT_TRUE(firstRes.has_value());
        ASSERT_TRUE(firstRes->has_value());
        EXPECT_EQ((*firstRes)->title, "Old Title");

        // Variadic multi-column update_where
        auto updRes = db.books.where_equal<"genre">("SciFi").update_where<"genre", "title">("Classic", "Updated Title");
        ASSERT_TRUE(updRes.has_value()) << updRes.error().to_string();

        auto updated = db.books.where_equal<"genre">("Classic").try_first_or_default();
        ASSERT_TRUE(updated.has_value() && updated->has_value());
        EXPECT_EQ((*updated)->title, "Updated Title");
    }

    // =========================================================================
    // Phase 29: Server-Side select_distinct, count_distinct, & Full or_where_* Parity
    // =========================================================================

    TEST(Phase29DistinctAndOrWhereParityTest, DistinctAndOrWhereChains)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());
        auto conn = connRes.value();
        ASSERT_TRUE(conn->ensure_table_created<DAL::Schema::PersonDTO>().has_value());

        DAL::DbSet<Core::Person, DAL::Schema::PersonDTO, DAL::SQLiteConnection> people(conn);
        ASSERT_TRUE(people.add(Core::Person(Core::PersonId{ 1 }, "Alice", "Smith")).has_value());
        ASSERT_TRUE(people.add(Core::Person(Core::PersonId{ 2 }, "Bob", "Jones")).has_value());
        ASSERT_TRUE(people.add(Core::Person(Core::PersonId{ 3 }, "Charlie", "Smith")).has_value());
        ASSERT_TRUE(people.add(Core::Person(Core::PersonId{ 4 }, "David", "Brown")).has_value());

        EXPECT_EQ(people.count_distinct<"lastName">(), 3u);
        auto distinctLastNames = people.order_by<"lastName">(false).select_distinct<"lastName">();
        ASSERT_EQ(distinctLastNames.size(), 3u);
        EXPECT_EQ(distinctLastNames[0], "Brown");
        EXPECT_EQ(distinctLastNames[1], "Jones");
        EXPECT_EQ(distinctLastNames[2], "Smith");

        auto matched = people.where_equal<"lastName">("Smith")
                           .or_where_like<"firstName">("B%")
                           .or_where_between<"id">(4, 4)
                           .order_by<"id">(false)
                           .select<"id">();
        ASSERT_EQ(matched.size(), 4u);
    }

    TEST(Phase30GroupedAndBatchM2MTest, GroupedChildrenAndBatchManyToMany)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());
        Phase25Context bookDb(connRes.value());
        ASSERT_TRUE(bookDb.ensure_created().has_value());

        auto a1 = bookDb.authors.add_returning(Phase25Author{ .name = "A1" }).value();
        auto a2 = bookDb.authors.add_returning(Phase25Author{ .name = "A2" }).value();
        auto a3 = bookDb.authors.add_returning(Phase25Author{ .name = "A3_Empty" }).value();

        ASSERT_TRUE(
            bookDb.books.add(Phase25Book{ .book_id = 1, .author_id = a1.author_id.value(), .title = "B1", .genre = "G" }).has_value());
        ASSERT_TRUE(
            bookDb.books.add(Phase25Book{ .book_id = 2, .author_id = a1.author_id.value(), .title = "B2", .genre = "G" }).has_value());
        ASSERT_TRUE(
            bookDb.books.add(Phase25Book{ .book_id = 3, .author_id = a2.author_id.value(), .title = "B3", .genre = "G" }).has_value());

        auto groupedRes = bookDb.query_children_grouped<Phase25Book>(std::vector<Phase25Author>{ a1, a2, a3 });
        ASSERT_TRUE(groupedRes.has_value()) << groupedRes.error().to_string();
        ASSERT_EQ(groupedRes->size(), 3u);
        EXPECT_EQ((*groupedRes)[0].first.name, "A1");
        EXPECT_EQ((*groupedRes)[0].second.size(), 2u);
        EXPECT_EQ((*groupedRes)[1].first.name, "A2");
        EXPECT_EQ((*groupedRes)[1].second.size(), 1u);
        EXPECT_EQ((*groupedRes)[2].first.name, "A3_Empty");
        EXPECT_EQ((*groupedRes)[2].second.size(), 0u);

        // Batched M:N navigation across multiple Core::Person sources
        auto connRes2 = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes2.has_value());
        Phase30DbContext m2mDb(connRes2.value());
        ASSERT_TRUE(m2mDb.ensure_created().has_value());

        Core::Person p1(Core::PersonId{ 1 }, "Alice", "Smith");
        Core::Person p2(Core::PersonId{ 2 }, "Bob", "Jones");
        Core::Person p3(Core::PersonId{ 3 }, "Charlie", "Brown");
        ASSERT_TRUE(m2mDb.people.add_range({ p1, p2, p3 }).has_value());

        Core::Something s1(Core::SomethingId{ 1 }, "Laptop", "Tech", "Work");
        Core::Something s2(Core::SomethingId{ 2 }, "Desk", "Office", "Home");
        Core::Something s3(Core::SomethingId{ 3 }, "Chair", "Office", "Home");
        ASSERT_TRUE(m2mDb.somethings.add_range({ s1, s2, s3 }).has_value());

        ASSERT_TRUE(m2mDb.personSomethings.add(Core::Person_Something(Core::PersonId{ 1 }, Core::SomethingId{ 1 }, "Owner")).has_value());
        ASSERT_TRUE(m2mDb.personSomethings.add(Core::Person_Something(Core::PersonId{ 2 }, Core::SomethingId{ 2 }, "Owner")).has_value());

        auto batchItems = m2mDb.query_many_to_many_batch<Core::Something, Core::Person_Something>(std::vector<Core::Person>{ p1, p2, p3 });
        ASSERT_TRUE(batchItems.has_value()) << batchItems.error().to_string();
        EXPECT_EQ(batchItems->size(), 2u);
    }

    TEST(Phase32MultiColumnTupleProjectionTest, SelectColumnsAndSelectDistinctColumns)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());
        auto conn = connRes.value();
        ASSERT_TRUE(conn->ensure_table_created<DAL::Schema::PersonDTO>().has_value());

        DAL::DbSet<Core::Person, DAL::Schema::PersonDTO, DAL::SQLiteConnection> people(conn);
        ASSERT_TRUE(people.add(Core::Person(Core::PersonId{ 1 }, "Bob", "Smith")).has_value());
        ASSERT_TRUE(people.add(Core::Person(Core::PersonId{ 2 }, "Alice", "Smith")).has_value());
        ASSERT_TRUE(people.add(Core::Person(Core::PersonId{ 3 }, "Alice", "Smith")).has_value());
        ASSERT_TRUE(people.add(Core::Person(Core::PersonId{ 4 }, "Charlie", "Adams")).has_value());

        auto rows = people.order_by<"lastName">(false).then_by<"firstName">(false).select_columns<"firstName", "lastName">();
        ASSERT_EQ(rows.size(), 4u);
        {
            const auto& [first, last] = rows[0];
            EXPECT_EQ(first, "Charlie");
            EXPECT_EQ(last, "Adams");
        }
        {
            const auto& [first, last] = rows[1];
            EXPECT_EQ(first, "Alice");
            EXPECT_EQ(last, "Smith");
        }

        auto distinctRows =
            people.order_by<"lastName">(false).then_by<"firstName">(false).select_distinct_columns<"firstName", "lastName">();
        ASSERT_EQ(distinctRows.size(), 3u);
        EXPECT_EQ(std::get<0>(distinctRows[0]), "Charlie");
        EXPECT_EQ(std::get<0>(distinctRows[1]), "Alice");
        EXPECT_EQ(std::get<0>(distinctRows[2]), "Bob");
    }

    // =========================================================================
    // Phase 33: Grouped Many-to-Many Eager Hydration
    // =========================================================================

    TEST(Phase33GroupedManyToManyTest, QueryManyToManyGroupedWithSharedAndEmptyTargets)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());
        Phase30DbContext db(connRes.value());
        ASSERT_TRUE(db.ensure_created().has_value());

        Core::Person p1(Core::PersonId{ 1 }, "Alice", "Smith");
        Core::Person p2(Core::PersonId{ 2 }, "Bob", "Jones");
        Core::Person p3(Core::PersonId{ 3 }, "Charlie", "Empty");
        ASSERT_TRUE(db.people.add_range({ p1, p2, p3 }).has_value());

        Core::Something s1(Core::SomethingId{ 1 }, "Laptop", "Tech", "Work");
        Core::Something s2(Core::SomethingId{ 2 }, "Desk", "Office", "Home");
        ASSERT_TRUE(db.somethings.add_range({ s1, s2 }).has_value());

        // Both p1 and p2 share s1; p1 also owns s2; p3 owns nothing
        ASSERT_TRUE(db.personSomethings.add(Core::Person_Something(Core::PersonId{ 1 }, Core::SomethingId{ 1 }, "Owner")).has_value());
        ASSERT_TRUE(db.personSomethings.add(Core::Person_Something(Core::PersonId{ 1 }, Core::SomethingId{ 2 }, "Owner")).has_value());
        ASSERT_TRUE(db.personSomethings.add(Core::Person_Something(Core::PersonId{ 2 }, Core::SomethingId{ 1 }, "User")).has_value());

        auto groupedRes = db.query_many_to_many_grouped<Core::Something, Core::Person_Something>(std::vector<Core::Person>{ p1, p2, p3 });
        ASSERT_TRUE(groupedRes.has_value()) << groupedRes.error().to_string();
        ASSERT_EQ(groupedRes->size(), 3u);
        EXPECT_EQ((*groupedRes)[0].first.getFirstName(), "Alice");
        EXPECT_EQ((*groupedRes)[0].second.size(), 2u);
        EXPECT_EQ((*groupedRes)[1].first.getFirstName(), "Bob");
        EXPECT_EQ((*groupedRes)[1].second.size(), 1u);
        EXPECT_EQ((*groupedRes)[1].second[0].getName(), "Laptop");
        EXPECT_EQ((*groupedRes)[2].first.getFirstName(), "Charlie");
        EXPECT_EQ((*groupedRes)[2].second.size(), 0u);
    }

    TEST(Phase50DbSetTryCountTest, DbSetTryCountForwarderReturnsCountAndPropagatesErrors)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());
        auto conn = connRes.value();
        Phase30DbContext db(conn);
        ASSERT_TRUE(db.ensure_created().has_value());

        auto emptyCount = db.people.try_count();
        ASSERT_TRUE(emptyCount.has_value()) << emptyCount.error().to_string();
        EXPECT_EQ(*emptyCount, 0u);

        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 0 }, "Ada", "Lovelace")).has_value());
        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 0 }, "Alan", "Turing")).has_value());

        auto twoCount = db.people.try_count();
        ASSERT_TRUE(twoCount.has_value()) << twoCount.error().to_string();
        EXPECT_EQ(*twoCount, 2u);

        conn->close();
        auto closedCount = db.people.try_count();
        ASSERT_FALSE(closedCount.has_value());
        EXPECT_EQ(closedCount.error().code, Core::DbErrorCode::ConnectionError);
    }

    TEST(Phase51DbQueryHardeningTest, OffsetWithoutLimitAndUnwrappedSelectColumns)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());
        Phase30DbContext db(connRes.value());
        ASSERT_TRUE(db.ensure_created().has_value());

        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 1 }, "Ada", "Lovelace")).has_value());
        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 2 }, "Bob", "Smith")).has_value());
        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 3 }, "Charlie", "Adams")).has_value());
        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 4 }, "Diana", "Prince")).has_value());
        ASSERT_TRUE(db.somethings.add(Core::Something(Core::SomethingId{ 10 }, "Engine", "Tech")).has_value());
        ASSERT_TRUE(db.personSomethings.add(Core::Person_Something(Core::PersonId{ 1 }, Core::SomethingId{ 10 }, "Owner")).has_value());

        // offset() without limit() must emit valid SQLite LIMIT <max> OFFSET N
        auto skipped = db.people.order_by<"id">(false).offset(2).try_to_vector();
        ASSERT_TRUE(skipped.has_value()) << skipped.error().to_string();
        ASSERT_EQ(skipped->size(), 2u);
        EXPECT_EQ((*skipped)[0].getId().get(), 3);
        EXPECT_EQ((*skipped)[1].getId().get(), 4);

        auto skippedNames = db.people.order_by<"id">(false).offset(2).try_select<"firstName">();
        ASSERT_TRUE(skippedNames.has_value()) << skippedNames.error().to_string();
        ASSERT_EQ(skippedNames->size(), 2u);
        EXPECT_EQ((*skippedNames)[0], "Charlie");
        EXPECT_EQ((*skippedNames)[1], "Diana");

        // select_columns / select_distinct_columns must unwrap sqlgen::PrimaryKey / ForeignKey to plain scalars
        auto idNameRows = db.people.order_by<"id">(false).offset(2).try_select_columns<"id", "firstName">();
        ASSERT_TRUE(idNameRows.has_value()) << idNameRows.error().to_string();
        static_assert(std::same_as<decltype(idNameRows)::value_type, std::vector<std::tuple<std::int32_t, std::string>>>);
        ASSERT_EQ(idNameRows->size(), 2u);
        const auto& [id0, name0] = (*idNameRows)[0];
        EXPECT_EQ(id0, 3);
        EXPECT_EQ(name0, "Charlie");

        auto fkRows = db.personSomethings.try_select_distinct_columns<"person_id", "something_id", "association_type">();
        ASSERT_TRUE(fkRows.has_value()) << fkRows.error().to_string();
        static_assert(std::same_as<decltype(fkRows)::value_type, std::vector<std::tuple<std::int32_t, std::int32_t, std::string>>>);
        ASSERT_EQ(fkRows->size(), 1u);
        const auto& [pid, sid, assoc] = (*fkRows)[0];
        EXPECT_EQ(pid, 1);
        EXPECT_EQ(sid, 10);
        EXPECT_EQ(assoc, "Owner");
    }

    TEST(Phase51DbQueryHardeningTest, OrderByLimitRemoveWherePreservesUnrelatedTrackedAndIsAtomic)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());
        Phase30DbContext db(connRes.value());
        ASSERT_TRUE(db.ensure_created().has_value());

        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 1 }, "Alice", "Smith")).has_value());
        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 2 }, "Bob", "Smith")).has_value());
        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 3 }, "Charlie", "Smith")).has_value());
        ASSERT_TRUE(db.somethings.add(Core::Something(Core::SomethingId{ 1 }, "Laptop", "Tech")).has_value());
        // FK link references Person 3 so deleting Person 3 fails with ConstraintViolation.
        ASSERT_TRUE(db.personSomethings.add(Core::Person_Something(Core::PersonId{ 3 }, Core::SomethingId{ 1 }, "Owner")).has_value());

        db.enable_change_tracking();
        auto t1 = db.people.find_tracked(1);
        auto t2 = db.people.find_tracked(2);
        ASSERT_NE(t1, nullptr);
        ASSERT_NE(t2, nullptr);
        t2->setFirstName("BobMutated");

        // order_by() must preserve untrack_key_fn_ so only id=1 is evicted and t2 remains tracked.
        auto delOne = db.people.where_equal<"lastName">("Smith").order_by<"id">(false).limit(1).remove_where();
        ASSERT_TRUE(delOne.has_value()) << delOne.error().to_string();
        EXPECT_EQ(db.tracked_entities_count(), 1u);
        ASSERT_TRUE(db.save_changes().has_value());
        EXPECT_EQ(db.people.find(2)->getFirstName(), "BobMutated");

        // Atomic rollback on limited remove_where(): deleting id=2 succeeds, id=3 fails FK -> id=2 must roll back!
        auto delTwoFail = db.people.where_equal<"lastName">("Smith").order_by<"id">(false).limit(2).remove_where();
        ASSERT_FALSE(delTwoFail.has_value());
        EXPECT_EQ(delTwoFail.error().code, Core::DbErrorCode::ConstraintViolation);
        EXPECT_EQ(db.people.count(), 2u);
        EXPECT_TRUE(db.people.contains(2));
        EXPECT_TRUE(db.people.contains(3));
    }

    TEST(Phase51DbQueryHardeningTest, PaginatedCountDoesNotTrackAndAggregatesRejectPagination)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());
        Phase30DbContext db(connRes.value());
        ASSERT_TRUE(db.ensure_created().has_value());

        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 1 }, "Ada", "Lovelace")).has_value());
        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 2 }, "Bob", "Smith")).has_value());
        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 3 }, "Charlie", "Adams")).has_value());

        db.enable_change_tracking();
        db.clear_change_tracker();
        EXPECT_EQ(db.tracked_entities_count(), 0u);

        auto pagedCnt = db.people.order_by<"id">(false).page(2, 1).try_count();
        ASSERT_TRUE(pagedCnt.has_value()) << pagedCnt.error().to_string();
        EXPECT_EQ(*pagedCnt, 2u);
        EXPECT_EQ(db.tracked_entities_count(), 0u);

        auto pagedAgg = db.people.query().limit(2).try_max<"id">();
        ASSERT_FALSE(pagedAgg.has_value());
        EXPECT_EQ(pagedAgg.error().code, Core::DbErrorCode::ConstraintViolation);
    }

} // namespace
