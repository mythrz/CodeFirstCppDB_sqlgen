#include <gtest/gtest.h>
#include <sqlgen.hpp>
#include <sqlgen/sqlite.hpp>

import std;
import codefirst.core;
import codefirst.dal;
import codefirst.samples;

#include "TestSupport.hpp"

namespace
{
    class Phase5SqlPushdownTest : public ::testing::Test
    {
    protected:
        std::shared_ptr<DAL::SQLiteConnection> conn;

        void SetUp() override
        {
            auto connRes = DAL::SQLiteConnection::connect(":memory:");
            ASSERT_TRUE(connRes.has_value());
            conn = connRes.value();

            ASSERT_TRUE(conn->ensure_table_created<DAL::Schema::PersonDTO>().has_value());
            ASSERT_TRUE(conn->ensure_table_created<DAL::Schema::SomethingDTO>().has_value());
            ASSERT_TRUE(conn->ensure_table_created<DAL::Schema::Person_SomethingDTO>().has_value());
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
        EXPECT_EQ(products.sum<"score">(), std::optional<std::int64_t>(60));
        ASSERT_TRUE(products.avg<"price">().has_value());
        EXPECT_NEAR(*products.avg<"price">(), 30.0, 1e-6);

        // Filtered aggregation
        EXPECT_EQ(products.where_not_null<"notes">().sum<"score">(), std::optional<std::int64_t>(40));
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

    TEST(Phase53QueryProjectionTest, SelectAndSelectDistinctHandleNullableColumnWithNullRows)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());
        Phase30DbContext db(connRes.value());
        ASSERT_TRUE(db.ensure_created().has_value());

        ASSERT_TRUE(db.somethings.add(Core::Something(Core::SomethingId{ 0 }, "Item1", "CatA", std::nullopt)).has_value());
        ASSERT_TRUE(db.somethings.add(Core::Something(Core::SomethingId{ 0 }, "Item2", "CatA", "HasDesc")).has_value());
        ASSERT_TRUE(db.somethings.add(Core::Something(Core::SomethingId{ 0 }, "Item3", "CatA", std::nullopt)).has_value());
        ASSERT_TRUE(db.somethings.add(Core::Something(Core::SomethingId{ 0 }, "Item4", "CatA", "HasDesc")).has_value());

        // Single-column select on std::optional<std::string> MUST return std::nullopt instead of throwing bad_optional_access.
        auto allDescs = db.somethings.order_by<"id">(false).try_select<"description">();
        ASSERT_TRUE(allDescs.has_value()) << allDescs.error().to_string();
        ASSERT_EQ(allDescs->size(), 4u);
        EXPECT_FALSE((*allDescs)[0].has_value());
        EXPECT_EQ((*allDescs)[1], std::optional<std::string>{ "HasDesc" });
        EXPECT_FALSE((*allDescs)[2].has_value());
        EXPECT_EQ((*allDescs)[3], std::optional<std::string>{ "HasDesc" });

        auto distinctDescs = db.somethings.order_by<"description">(false).try_select_distinct<"description">();
        ASSERT_TRUE(distinctDescs.has_value()) << distinctDescs.error().to_string();
        ASSERT_EQ(distinctDescs->size(), 2u);
        EXPECT_FALSE((*distinctDescs)[0].has_value());
        EXPECT_EQ((*distinctDescs)[1], std::optional<std::string>{ "HasDesc" });
    }

    // =========================================================================
    // Phase 58: Atomic Paginated Delete & Unified limit(0)/page(0) Contract
    // =========================================================================

    TEST(Phase58PaginatedDeleteAndLimitTest, AtomicPaginatedRemoveWhereRespectsConcurrentPredicateChange)
    {
        const auto dbPath = make_unique_temp_db_path("codefirst_phase58_paginated_delete");
        const ScopedTempDbCleanup cleanup{ dbPath };

        DAL::SQLiteConnectionOptions opts;
        opts.foreign_keys = true;
        opts.busy_timeout = std::chrono::milliseconds(5000);
        opts.wal_mode = true;

        auto connARes = DAL::SQLiteConnection::connect(dbPath, opts);
        ASSERT_TRUE(connARes.has_value());
        auto connBRes = DAL::SQLiteConnection::connect(dbPath, opts);
        ASSERT_TRUE(connBRes.has_value());
        auto connA = connARes.value();
        auto connB = connBRes.value();

        Phase30DbContext dbA(connA);
        ASSERT_TRUE(dbA.ensure_created().has_value());
        ASSERT_TRUE(dbA.people.add(Core::Person(Core::PersonId{ 1 }, "Row1", "Stale")).has_value());
        ASSERT_TRUE(dbA.people.add(Core::Person(Core::PersonId{ 2 }, "Row2", "Stale")).has_value());
        ASSERT_TRUE(dbA.people.add(Core::Person(Core::PersonId{ 3 }, "Row3", "Keep")).has_value());

        // Conn A opens BEGIN IMMEDIATE and changes Row1 from "Stale" -> "Active" before committing.
        ASSERT_TRUE(connA->begin_immediate_transaction().has_value());
        ASSERT_TRUE(dbA.people.where_equal<"id">(1).template update_where<"lastName">("Active").has_value());

        std::expected<void, Core::DbError> delResB{};
        std::thread threadB(
            [&]()
            {
                Phase30DbContext dbB(connB);
                // Because paginated remove_where is a single atomic DELETE ... WHERE rowid IN (SELECT ...),
                // Conn B blocks until Conn A commits, then deletes ONLY Row2 (the first remaining "Stale" row),
                // NEVER deleting Row1 which became "Active".
                delResB = dbB.people.where_equal<"lastName">("Stale").order_by<"id">(false).limit(1).remove_where();
            });

        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        ASSERT_TRUE(connA->commit().has_value());
        threadB.join();

        ASSERT_TRUE(delResB.has_value()) << delResB.error().to_string();
        EXPECT_TRUE(dbA.people.contains(1));
        EXPECT_EQ(dbA.people.find(1)->getLastName(), "Active");
        EXPECT_FALSE(dbA.people.contains(2));
        EXPECT_TRUE(dbA.people.contains(3));

        connA->close();
        connB->close();
    }

#if !defined(CODEFIRST_CONTRACTS_ENFORCED)
    TEST(Phase58PaginatedDeleteAndLimitTest, ZeroLimitAndZeroPageReturnConstraintViolationWhenContractsDisabled)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());
        Phase30DbContext db(connRes.value());
        ASSERT_TRUE(db.ensure_created().has_value());
        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 1 }, "Ada", "Lovelace")).has_value());

        auto q0 = db.people.query().limit(0);
        auto p0 = db.people.page(0, 0);

        auto rVec = q0.try_to_vector();
        ASSERT_FALSE(rVec.has_value());
        EXPECT_EQ(rVec.error().code, Core::DbErrorCode::ConstraintViolation);

        auto rFirst = p0.try_first_or_default();
        ASSERT_FALSE(rFirst.has_value());
        EXPECT_EQ(rFirst.error().code, Core::DbErrorCode::ConstraintViolation);

        auto rCount = q0.try_count();
        ASSERT_FALSE(rCount.has_value());
        EXPECT_EQ(rCount.error().code, Core::DbErrorCode::ConstraintViolation);

        auto rAny = q0.try_any();
        ASSERT_FALSE(rAny.has_value());
        EXPECT_EQ(rAny.error().code, Core::DbErrorCode::ConstraintViolation);

        auto rSel = q0.try_select<"firstName">();
        ASSERT_FALSE(rSel.has_value());
        EXPECT_EQ(rSel.error().code, Core::DbErrorCode::ConstraintViolation);

        auto rSelDist = q0.try_select_distinct<"firstName">();
        ASSERT_FALSE(rSelDist.has_value());
        EXPECT_EQ(rSelDist.error().code, Core::DbErrorCode::ConstraintViolation);

        auto rCols = q0.try_select_columns<"id", "firstName">();
        ASSERT_FALSE(rCols.has_value());
        EXPECT_EQ(rCols.error().code, Core::DbErrorCode::ConstraintViolation);

        auto rColsDist = q0.try_select_distinct_columns<"id", "firstName">();
        ASSERT_FALSE(rColsDist.has_value());
        EXPECT_EQ(rColsDist.error().code, Core::DbErrorCode::ConstraintViolation);

        auto rDel = db.people.where_equal<"id">(1).limit(0).remove_where();
        ASSERT_FALSE(rDel.has_value());
        EXPECT_EQ(rDel.error().code, Core::DbErrorCode::ConstraintViolation);
        EXPECT_EQ(db.people.count(), 1u);
    }
#endif

    // =========================================================================
    // Phase 60 (Security S1): Parameterized Query AST, Predicate Validation & Escaped LIKE
    // =========================================================================

    struct SecMetricRow
    {
        static constexpr const char* tablename = "SecMetricRows";
        sqlgen::PrimaryKey<std::int32_t> id{ 0 };
        double score = 0.0;
        double nan = 0.0;
        double inf = 0.0;
        std::uint64_t big_val = 0;
        std::string label;
    };

    TEST(Phase60SecurityQueryTest, NaNInfinityAndSubnormalDoublesAreBoundNotInterpolated)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());
        auto conn = connRes.value();
        ASSERT_TRUE(conn->ensure_table_created<SecMetricRow>().has_value());

        DAL::DbSet<SecMetricRow> metrics(conn);
        const double pos_inf = std::numeric_limits<double>::infinity();
        const double qnan = std::numeric_limits<double>::quiet_NaN();

        ASSERT_TRUE(
            metrics.add(SecMetricRow{ .id = 1, .score = 1.5, .nan = 1.5, .inf = 1.5, .big_val = 10, .label = "hijack_bait" }).has_value());
        ASSERT_TRUE(
            metrics.add(SecMetricRow{ .id = 2, .score = pos_inf, .nan = 0.0, .inf = 0.0, .big_val = 20, .label = "is_inf" }).has_value());
        ASSERT_TRUE(
            metrics.add(SecMetricRow{ .id = 3, .score = 1e-9, .nan = 0.0, .inf = 0.0, .big_val = 30, .label = "tiny" }).has_value());
        ASSERT_TRUE(metrics.add(SecMetricRow{ .id = 4, .score = 0.0, .nan = 0.0, .inf = 0.0, .big_val = 40, .label = "zero" }).has_value());

        // Previously, sqlgen::sqlite::to_sql serialized NaN as unquoted `nan`, turning `WHERE score = nan`
        // into a column-to-column comparison against column "nan" (matching row 1!). With `?` binding,
        // NaN binds as SQL NULL so `score = NULL` matches zero rows cleanly with no SQL error.
        auto nanRows = metrics.where_equal<"score">(qnan).try_to_vector();
        ASSERT_TRUE(nanRows.has_value()) << nanRows.error().to_string();
        EXPECT_TRUE(nanRows->empty());

        // Infinity binds via sqlite3_bind_double as IEEE +Inf, matching ONLY row 2 (not row 1 where score == inf).
        auto infRows = metrics.where_equal<"score">(pos_inf).try_to_vector();
        ASSERT_TRUE(infRows.has_value()) << infRows.error().to_string();
        ASSERT_EQ(infRows->size(), 1u);
        EXPECT_EQ((*infRows)[0].id.value(), 2);

        // Subnormal/small double 1e-9 must NOT truncate to 0.000000 and match row 4!
        auto tinyRows = metrics.where_equal<"score">(1e-9).try_to_vector();
        ASSERT_TRUE(tinyRows.has_value()) << tinyRows.error().to_string();
        ASSERT_EQ(tinyRows->size(), 1u);
        EXPECT_EQ((*tinyRows)[0].id.value(), 3);

        // Bulk update_where with IEEE +Inf and 1e-9 must also bind cleanly without SQL syntax/hijack issues.
        ASSERT_TRUE(metrics.where_equal<"id">(4).update_where<"score">(1e-9).has_value());
        auto tinyAfterUpdate = metrics.where_equal<"score">(1e-9).try_count();
        ASSERT_TRUE(tinyAfterUpdate.has_value()) << tinyAfterUpdate.error().to_string();
        EXPECT_EQ(*tinyAfterUpdate, 2u);

        ASSERT_TRUE(metrics.where_equal<"id">(4).update_where<"score">(pos_inf).has_value());
        auto infAfterUpdate = metrics.where_equal<"score">(pos_inf).try_count();
        ASSERT_TRUE(infAfterUpdate.has_value()) << infAfterUpdate.error().to_string();
        EXPECT_EQ(*infAfterUpdate, 2u);
    }

    TEST(Phase60SecurityQueryTest, OversizedUint64AndLimitOffsetAreRejectedWithConstraintViolation)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());
        auto conn = connRes.value();
        ASSERT_TRUE(conn->ensure_table_created<SecMetricRow>().has_value());

        DAL::DbSet<SecMetricRow> metrics(conn);
        ASSERT_TRUE(metrics.add(SecMetricRow{ .id = 1, .score = 1.0, .nan = 0.0, .inf = 0.0, .big_val = 42, .label = "ok" }).has_value());
        // Insert a negative sentinel row via raw SQL so any wrap-around of UINT64_MAX -> -1 would match it.
        ASSERT_TRUE(conn->execute_raw(
                            "INSERT INTO \"SecMetricRows\" (\"id\", \"score\", \"nan\", \"inf\", \"big_val\", \"label\") "
                            "VALUES (2, 0.0, 0.0, 0.0, -1, 'neg_sentinel');")
                        .has_value());

        constexpr std::uint64_t kMaxU64 = std::numeric_limits<std::uint64_t>::max();
        constexpr std::uint64_t kJustOverI64 = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1ULL;

        auto rEq = metrics.where_equal<"big_val">(kMaxU64).try_to_vector();
        ASSERT_FALSE(rEq.has_value());
        EXPECT_EQ(rEq.error().code, Core::DbErrorCode::ConstraintViolation);

        auto rOr = metrics.where_equal<"id">(1).or_where_equal<"big_val">(kJustOverI64).try_count();
        ASSERT_FALSE(rOr.has_value());
        EXPECT_EQ(rOr.error().code, Core::DbErrorCode::ConstraintViolation);

        auto rIn = metrics.where_in<"big_val">(std::uint64_t{ 42 }, kMaxU64).try_to_vector();
        ASSERT_FALSE(rIn.has_value());
        EXPECT_EQ(rIn.error().code, Core::DbErrorCode::ConstraintViolation);

        const std::vector<std::uint64_t> badVec{ std::uint64_t{ 42 }, kJustOverI64 };
        auto rInVec = metrics.where_in_vec<"big_val">(badVec).try_to_vector();
        ASSERT_FALSE(rInVec.has_value());
        EXPECT_EQ(rInVec.error().code, Core::DbErrorCode::ConstraintViolation);

        auto rBetween = metrics.where_between<"big_val">(std::uint64_t{ 0 }, kMaxU64).try_any();
        ASSERT_FALSE(rBetween.has_value());
        EXPECT_EQ(rBetween.error().code, Core::DbErrorCode::ConstraintViolation);

        auto rDel = metrics.where_equal<"big_val">(kMaxU64).remove_where();
        ASSERT_FALSE(rDel.has_value());
        EXPECT_EQ(rDel.error().code, Core::DbErrorCode::ConstraintViolation);

        auto rUpd = metrics.where_equal<"id">(1).update_where<"big_val">(kMaxU64);
        ASSERT_FALSE(rUpd.has_value());
        EXPECT_EQ(rUpd.error().code, Core::DbErrorCode::ConstraintViolation);

        // Neither row should have been modified or deleted.
        EXPECT_EQ(metrics.count(), 2u);
        auto row1 = metrics.find(1);
        ASSERT_TRUE(row1.has_value());
        EXPECT_EQ(row1->big_val, 42ULL);

        // Oversized limit / offset must also fail cleanly with ConstraintViolation.
        if constexpr (sizeof(std::size_t) >= sizeof(std::int64_t))
        {
            const std::size_t kOverI64 = static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()) + 1u;
            auto rLim = metrics.query().limit(kOverI64).try_to_vector();
            ASSERT_FALSE(rLim.has_value());
            EXPECT_EQ(rLim.error().code, Core::DbErrorCode::ConstraintViolation);

            auto rOff = metrics.query().offset(kOverI64).try_to_vector();
            ASSERT_FALSE(rOff.has_value());
            EXPECT_EQ(rOff.error().code, Core::DbErrorCode::ConstraintViolation);
        }
    }

    TEST(Phase60SecurityQueryTest, LongEmbeddedNulStringsWorkWithoutExprDepthOverflowAndPartialIndexHardening)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());
        auto conn = connRes.value();
        ASSERT_TRUE(conn->ensure_table_created<SecMetricRow>().has_value());

        DAL::DbSet<SecMetricRow> metrics(conn);

        // 2,000 NUL bytes would create >4,000 chained `|| char(0) ||` expressions and crash SQLITE_LIMIT_EXPR_DEPTH (1000)
        // under string interpolation, whereas bound `?` handles it in O(1) depth.
        std::string manyNuls;
        manyNuls.push_back('A');
        manyNuls.append(2000, '\0');
        manyNuls.push_back('Z');

        ASSERT_TRUE(
            metrics.add(SecMetricRow{ .id = 1, .score = 1.0, .nan = 0.0, .inf = 0.0, .big_val = 1, .label = manyNuls }).has_value());

        auto found = metrics.where_equal<"label">(manyNuls).try_to_vector();
        ASSERT_TRUE(found.has_value()) << found.error().to_string();
        ASSERT_EQ(found->size(), 1u);
        EXPECT_EQ((*found)[0].label, manyNuls);

        std::string manyNuls2;
        manyNuls2.push_back('B');
        manyNuls2.append(2000, '\0');
        manyNuls2.push_back('Y');
        ASSERT_TRUE(metrics.where_equal<"label">(manyNuls).update_where<"label">(manyNuls2).has_value());

        auto found2 = metrics.where_equal<"label">(manyNuls2).try_count();
        ASSERT_TRUE(found2.has_value()) << found2.error().to_string();
        EXPECT_EQ(*found2, 1u);

        // Partial index DDL with 2,000 NUL bytes uses O(1)-depth CAST(X'...' AS TEXT).
        auto idxNulRes = metrics.where_equal<"label">(manyNuls2).ensure_index_created<"idx_sec_many_nuls", "label">();
        ASSERT_TRUE(idxNulRes.has_value()) << idxNulRes.error().to_string();

        // Partial index DDL with non-finite floats must be rejected cleanly.
        auto idxNanRes =
            metrics.where_equal<"score">(std::numeric_limits<double>::quiet_NaN()).ensure_index_created<"idx_sec_nan", "score">();
        ASSERT_FALSE(idxNanRes.has_value());
        EXPECT_EQ(idxNanRes.error().code, Core::DbErrorCode::ConstraintViolation);
    }

    TEST(Phase60SecurityQueryTest, EscapedLikeHelpersTreatWildcardsLiterallyAcrossAllTerminals)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());
        Phase30DbContext db(connRes.value());
        ASSERT_TRUE(db.ensure_created().has_value());

        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 1 }, "100% legit", "Tag")).has_value());
        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 2 }, "1000 legit", "Tag")).has_value());
        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 3 }, "a_b", "Tag")).has_value());
        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 4 }, "axb", "Tag")).has_value());
        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 5 }, "c\\d", "Tag")).has_value());
        ASSERT_TRUE(db.people.add(Core::Person(Core::PersonId{ 6 }, "prefix%mid_end\\tail", "Other")).has_value());

        // Contains with '%' must NOT match "1000 legit"
        auto pctRows = db.people.where_contains<"firstName">("100%").try_to_vector();
        ASSERT_TRUE(pctRows.has_value()) << pctRows.error().to_string();
        ASSERT_EQ(pctRows->size(), 1u);
        EXPECT_EQ((*pctRows)[0].getId().get(), 1);

        // Contains with '_' must NOT match "axb"
        auto usRows = db.people.where_contains<"firstName">("a_b").try_to_vector();
        ASSERT_TRUE(usRows.has_value()) << usRows.error().to_string();
        ASSERT_EQ(usRows->size(), 1u);
        EXPECT_EQ((*usRows)[0].getId().get(), 3);

        // Contains with '\' must match literal backslash
        auto bsRows = db.people.where_contains<"firstName">("c\\d").try_to_vector();
        ASSERT_TRUE(bsRows.has_value()) << bsRows.error().to_string();
        ASSERT_EQ(bsRows->size(), 1u);
        EXPECT_EQ((*bsRows)[0].getId().get(), 5);

        // Starts with & ends with escaping
        auto swRows = db.people.where_starts_with<"firstName">("prefix%").try_select<"firstName">();
        ASSERT_TRUE(swRows.has_value()) << swRows.error().to_string();
        ASSERT_EQ(swRows->size(), 1u);
        EXPECT_EQ((*swRows)[0], "prefix%mid_end\\tail");

        auto ewRows = db.people.where_ends_with<"firstName">("_end\\tail").try_select_columns<"id", "lastName">();
        ASSERT_TRUE(ewRows.has_value()) << ewRows.error().to_string();
        ASSERT_EQ(ewRows->size(), 1u);
        EXPECT_EQ(std::get<0>((*ewRows)[0]), 6);
        EXPECT_EQ(std::get<1>((*ewRows)[0]), "Other");

        // Mixed composition: standard sqlgen predicate + escaped LIKE OR + count + update_where + paginated remove_where
        auto mixedCount = db.people.where_contains<"firstName">("100%")
                              .or_where_starts_with<"firstName">("a_b")
                              .or_where_ends_with<"firstName">("\\d")
                              .try_count();
        ASSERT_TRUE(mixedCount.has_value()) << mixedCount.error().to_string();
        EXPECT_EQ(*mixedCount, 3u);

        auto updRes = db.people.where_contains<"firstName">("100%").where_equal<"lastName">("Tag").update_where<"lastName">("Discount");
        ASSERT_TRUE(updRes.has_value()) << updRes.error().to_string();
        EXPECT_EQ(db.people.find(1)->getLastName(), "Discount");
        EXPECT_EQ(db.people.find(2)->getLastName(), "Tag");

        // Paginated remove_where with escaped LIKE predicate
        auto delRes = db.people.where_contains<"firstName">("%").order_by<"id">(false).limit(1).remove_where();
        ASSERT_TRUE(delRes.has_value()) << delRes.error().to_string();
        EXPECT_FALSE(db.people.contains(1)); // "100% legit" deleted
        EXPECT_TRUE(db.people.contains(6)); // "prefix%mid_end\tail" kept due to limit(1)
        EXPECT_TRUE(db.people.contains(2)); // "1000 legit" untouched
    }

} // namespace
