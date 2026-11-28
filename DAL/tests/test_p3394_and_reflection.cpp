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
    struct[[= DAL::Schema::table{ std::define_static_string("AnnotatedWidgets") }]] AnnotatedWidget
    {
        [[= DAL::Schema::primary_key{ true }]] std::int32_t id = 0;
        [[= DAL::Schema::column{ std::define_static_string("widget_title") }]] std::string title;
        [[= DAL::Schema::unique{}]] std::string sku;
    };

    class InvariantProduct
    {
    public:
        InvariantProduct() = default;
        InvariantProduct(std::int32_t id, std::string sku, bool active)
            : id_(id)
            , sku_(std::move(sku))
            , active_(active)
            , hydratedViaConstructor_(true)
        {
        }

        [[nodiscard]] std::int32_t getId() const noexcept
        {
            return id_;
        }
        [[nodiscard]] const std::string& getSku() const noexcept
        {
            return sku_;
        }
        [[nodiscard]] bool isActive() const noexcept
        {
            return active_;
        }
        [[nodiscard]] bool wasHydratedViaConstructor() const noexcept
        {
            return hydratedViaConstructor_;
        }

    private:
        std::int32_t id_{ 0 };
        std::string sku_;
        bool active_{ false };
        bool hydratedViaConstructor_{ false };
    };

    struct InvariantProductDTO
    {
        static constexpr auto tablename = "InvariantProduct";
        sqlgen::PrimaryKey<std::int32_t, true> id{ 0 };
        std::string sku;
        bool active = false;
    };

    TEST(Phase42ReflectionModernizationTest, ConstructorHydrationAndAnnotatedDto)
    {
        // 1. P3096 std::meta::parameters_of constructor hydration
        InvariantProductDTO dto{ .id = 5, .sku = "SKU-99", .active = true };
        auto hydrated = DAL::Mappers::MapperTraits<InvariantProduct, InvariantProductDTO>::to_domain(dto);
        EXPECT_EQ(hydrated.getId(), 5);
        EXPECT_EQ(hydrated.getSku(), "SKU-99");
        EXPECT_TRUE(hydrated.isActive());
        EXPECT_TRUE(hydrated.wasHydratedViaConstructor());

        // 2. P3394 [[=table]] + define_aggregate synthesized DTO
        using SynthDTO = DAL::Schema::SchemaOf_t<AnnotatedWidget>;
        static_assert(DAL::Schema::HasTableName<SynthDTO>);
        static_assert(std::string_view(SynthDTO::tablename) == "AnnotatedWidgets");
    }

    struct[[= DAL::Schema::table{ std::define_static_string("AnnotatedAuthors") }]] AnnotatedAuthor
    {
        [[= DAL::Schema::primary_key{ true }]] std::int32_t id = 0;
        [[= DAL::Schema::unique{}]] std::string handle;
        std::string name;
    };

    struct[[= DAL::Schema::table{ std::define_static_string("AnnotatedArticles") }]] AnnotatedArticle
    {
        [[= DAL::Schema::primary_key{ true }]] std::int32_t id = 0;
        [[= DAL::Schema::references<AnnotatedAuthor>{ std::define_static_string("id") }]] std::int32_t author_id = 0;
        [[= DAL::Schema::column{ std::define_static_string("article_title") }]] std::string title;
    };

    struct AnnotatedBlogContext : DAL::DbContextBase<DAL::SQLiteConnection>
    {
        // Deliberately declared child-first to verify topological sort with AnnotatedDto
        Set<AnnotatedArticle> articles{ this };
        Set<AnnotatedAuthor> authors{ this };

        explicit AnnotatedBlogContext(std::shared_ptr<DAL::SQLiteConnection> c)
            : DAL::DbContextBase<DAL::SQLiteConnection>(std::move(c))
        {
        }
    };

    TEST(Phase42ReflectionModernizationTest, EndToEndAnnotatedEntitiesWithForeignKeySortAndTrackedRef)
    {
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());
        AnnotatedBlogContext db(connRes.value());
        ASSERT_TRUE(db.ensure_created().has_value());
        db.enable_change_tracking(true);

        // Verify foreign key referencing missing author fails cleanly
        auto badArticle = db.articles.add(AnnotatedArticle{ .id = 0, .author_id = 999, .title = "Orphan" });
        ASSERT_FALSE(badArticle.has_value());

        auto authorRes = db.authors.add_returning(AnnotatedAuthor{ .id = 0, .handle = "@ada", .name = "Ada" });
        ASSERT_TRUE(authorRes.has_value()) << authorRes.error().to_string();
        EXPECT_EQ(authorRes->id, 1);

        // Duplicate unique handle must fail
        auto dupAuthor = db.authors.add(AnnotatedAuthor{ .id = 0, .handle = "@ada", .name = "Clone" });
        ASSERT_FALSE(dupAuthor.has_value());

        auto articleRes = db.articles.add_returning(AnnotatedArticle{ .id = 0, .author_id = authorRes->id, .title = "Notes on Engine" });
        ASSERT_TRUE(articleRes.has_value()) << articleRes.error().to_string();
        EXPECT_EQ(articleRes->id, 1);

        // P2988 std::optional<Domain&> tracked entity mutation + save_changes()
        auto trackedRef = db.articles.find_tracked_ref(articleRes->id);
        ASSERT_TRUE(trackedRef.has_value());
        trackedRef->title = "Notes on Analytical Engine";
        ASSERT_TRUE(db.save_changes().has_value());

        auto reloaded = db.articles.find(articleRes->id);
        ASSERT_TRUE(reloaded.has_value());
        EXPECT_EQ(reloaded->title, "Notes on Analytical Engine");

        // Explicit update
        reloaded->title = "Updated Title";
        ASSERT_TRUE(db.articles.update(*reloaded).has_value());
        EXPECT_EQ(db.articles.find(articleRes->id)->title, "Updated Title");
    }

    class OverloadGuardedProduct
    {
    public:
        OverloadGuardedProduct() = default;

        // Deliberately mismatched parameter types for matching names (declared first)
        OverloadGuardedProduct(std::string id, std::int32_t sku, bool active)
            : id_(sku)
            , sku_(std::move(id))
            , active_(active)
            , wrongOverloadUsed_(true)
        {
        }

        // Type-compatible hydrating constructor (declared second)
        OverloadGuardedProduct(std::int32_t id, std::string sku, bool active)
            : id_(id)
            , sku_(std::move(sku))
            , active_(active)
            , wrongOverloadUsed_(false)
        {
        }

        [[nodiscard]] std::int32_t getId() const noexcept
        {
            return id_;
        }
        [[nodiscard]] const std::string& getSku() const noexcept
        {
            return sku_;
        }
        [[nodiscard]] bool isActive() const noexcept
        {
            return active_;
        }
        [[nodiscard]] bool usedWrongOverload() const noexcept
        {
            return wrongOverloadUsed_;
        }

    private:
        std::int32_t id_{ 0 };
        std::string sku_;
        bool active_{ false };
        bool wrongOverloadUsed_{ false };
    };

    TEST(Phase42ReflectionModernizationTest, ConstructorHydrationRejectsTypeMismatchedConstructor)
    {
        InvariantProductDTO dto{ .id = 12, .sku = "GOOD-SKU", .active = true };
        auto hydrated = DAL::Mappers::MapperTraits<OverloadGuardedProduct, InvariantProductDTO>::to_domain(dto);
        EXPECT_FALSE(hydrated.usedWrongOverload());
        EXPECT_EQ(hydrated.getId(), 12);
        EXPECT_EQ(hydrated.getSku(), "GOOD-SKU");
        EXPECT_TRUE(hydrated.isActive());
    }

    // =========================================================================
    // Phase 50 (P1): Annotation Composition & Explicit Composite PK Rules
    // =========================================================================

    struct[[= DAL::Schema::table{ std::define_static_string("ComposedProfiles") }]] ComposedUniqueFkProfile
    {
        [[= DAL::Schema::primary_key{ true }]] std::int32_t id = 0;
        [[= DAL::Schema::unique{}]][[= DAL::Schema::references<AnnotatedAuthor>{ std::define_static_string("id") }]] std::int32_t
            author_id = 0;
        std::string bio;
    };

    struct[[= DAL::Schema::table{ std::define_static_string("SharedPkSettings") }]] SharedPkFkSetting
    {
        [[= DAL::Schema::primary_key{
            false }]][[= DAL::Schema::references<AnnotatedAuthor>{ std::define_static_string("id") }]] std::int32_t author_id = 0;
        std::string theme;
    };

    struct[[= DAL::Schema::table{ std::define_static_string("AnnotatedAuthorArticles") }]] ExplicitCompositeAnnotatedLink
    {
        [[= DAL::Schema::primary_key{
            false }]][[= DAL::Schema::references<AnnotatedAuthor>{ std::define_static_string("id") }]] std::int32_t author_id = 0;
        [[= DAL::Schema::primary_key{
            false }]][[= DAL::Schema::references<AnnotatedArticle>{ std::define_static_string("id") }]] std::int32_t article_id = 0;
        std::string role;
    };

    struct[[= DAL::Schema::table{ std::define_static_string("UnmarkedTwoFkEvents") }]] UnmarkedTwoFkEvent
    {
        [[= DAL::Schema::references<AnnotatedAuthor>{ std::define_static_string("id") }]] std::int32_t author_id = 0;
        [[= DAL::Schema::references<AnnotatedArticle>{ std::define_static_string("id") }]] std::int32_t article_id = 0;
        std::string event_kind;
    };

    struct UnmarkedRawTwoFkDTO
    {
        static constexpr auto tablename = "UnmarkedRawTwoFk";
        sqlgen::ForeignKey<std::int32_t, PocoUser, "user_id"> user_id{ 0 };
        sqlgen::ForeignKey<std::int32_t, PocoRole, "role_id"> role_id{ 0 };
        std::string note;
    };

    struct ComposedAnnotationContext : DAL::DbContextBase<DAL::SQLiteConnection>
    {
        Set<ComposedUniqueFkProfile> profiles{ this };
        Set<SharedPkFkSetting> settings{ this };
        Set<ExplicitCompositeAnnotatedLink> links{ this };
        Set<AnnotatedArticle> articles{ this };
        Set<AnnotatedAuthor> authors{ this };

        explicit ComposedAnnotationContext(std::shared_ptr<DAL::SQLiteConnection> c)
            : DAL::DbContextBase<DAL::SQLiteConnection>(std::move(c))
        {
        }
    };

    TEST(Phase50SchemaSafetyTest, ComposedUniqueFkSharedPkFkAndExplicitCompositePk)
    {
        // 2+ FKs without explicit PK or composite_primary_key MUST NOT silently become a composite PK.
        static_assert(!DAL::Schema::CompositePrimaryKey<DAL::Schema::SchemaOf_t<UnmarkedTwoFkEvent>>);
        static_assert(!DAL::Schema::RelationalEntity<DAL::Schema::SchemaOf_t<UnmarkedTwoFkEvent>>);
        static_assert(!DAL::Schema::CompositePrimaryKey<UnmarkedRawTwoFkDTO>);
        static_assert(!DAL::Schema::RelationalEntity<UnmarkedRawTwoFkDTO>);

        // Explicit composite PK + FK on annotated entity satisfies CompositePrimaryKey.
        static_assert(DAL::Schema::CompositePrimaryKey<DAL::Schema::SchemaOf_t<ExplicitCompositeAnnotatedLink>>);
        static_assert(DAL::Schema::SinglePrimaryKey<DAL::Schema::SchemaOf_t<SharedPkFkSetting>>);

        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());
        ComposedAnnotationContext db(connRes.value());
        ASSERT_TRUE(db.ensure_created().has_value());

        auto author = db.authors.add_returning(AnnotatedAuthor{ .id = 0, .handle = "@grace", .name = "Grace" });
        ASSERT_TRUE(author.has_value()) << author.error().to_string();
        auto article = db.articles.add_returning(AnnotatedArticle{ .id = 0, .author_id = author->id, .title = "Compilers" });
        ASSERT_TRUE(article.has_value()) << article.error().to_string();

        // 1. [[=unique]] + [[=references]] enforces BOTH FK and UNIQUE.
        EXPECT_FALSE(db.profiles.add(ComposedUniqueFkProfile{ .id = 0, .author_id = 999, .bio = "BadFK" }).has_value());
        ASSERT_TRUE(db.profiles.add(ComposedUniqueFkProfile{ .id = 0, .author_id = author->id, .bio = "First" }).has_value());
        auto dupProfile = db.profiles.add(ComposedUniqueFkProfile{ .id = 0, .author_id = author->id, .bio = "Second" });
        ASSERT_FALSE(dupProfile.has_value());
        EXPECT_EQ(dupProfile.error().code, Core::DbErrorCode::DuplicateKey);

        // 2. [[=primary_key{false}]] + [[=references]] enforces BOTH PK and FK (1:1 shared PK).
        EXPECT_FALSE(db.settings.add(SharedPkFkSetting{ .author_id = 999, .theme = "Dark" }).has_value());
        ASSERT_TRUE(db.settings.add(SharedPkFkSetting{ .author_id = author->id, .theme = "Dark" }).has_value());
        auto dupSetting = db.settings.add(SharedPkFkSetting{ .author_id = author->id, .theme = "Light" });
        ASSERT_FALSE(dupSetting.has_value());
        EXPECT_EQ(dupSetting.error().code, Core::DbErrorCode::DuplicateKey);
        auto settingInfo = db.table_info("SharedPkSettings");
        ASSERT_TRUE(settingInfo.has_value());
        EXPECT_EQ((*settingInfo)[0].pk_index, 1);

        // 3. Explicit composite [[=primary_key{false}]] + [[=references]] junction table.
        EXPECT_FALSE(db.links.add(ExplicitCompositeAnnotatedLink{ .author_id = 999, .article_id = article->id, .role = "X" }).has_value());
        ASSERT_TRUE(
            db.links.add(ExplicitCompositeAnnotatedLink{ .author_id = author->id, .article_id = article->id, .role = "Lead" }).has_value());
        auto dupLink = db.links.add(ExplicitCompositeAnnotatedLink{ .author_id = author->id, .article_id = article->id, .role = "Dup" });
        ASSERT_FALSE(dupLink.has_value());
        EXPECT_EQ(dupLink.error().code, Core::DbErrorCode::DuplicateKey);
        auto linkInfo = db.table_info("AnnotatedAuthorArticles");
        ASSERT_TRUE(linkInfo.has_value());
        EXPECT_EQ((*linkInfo)[0].pk_index, 1);
        EXPECT_EQ((*linkInfo)[1].pk_index, 2);
        EXPECT_EQ((*linkInfo)[2].pk_index, 0);
    }

    // =========================================================================
    // Phase 52 (P1): Composite Key Forwarding, NaN Equality & Bijective Ctors
    // =========================================================================

    struct[[= DAL::Schema::table{ std::define_static_string("AnnotatedCompositeLinks") }]][[= DAL::Schema::composite_key{}]]
        AnnotatedCompositeKeyLink
    {
        [[= DAL::Schema::references<AnnotatedAuthor>{ std::define_static_string("id") }]] std::int32_t author_id = 0;
        [[= DAL::Schema::references<AnnotatedArticle>{ std::define_static_string("id") }]] std::int32_t article_id = 0;
        std::string note;
    };

    struct AnnotatedCompositeKeyContext : DAL::DbContextBase<DAL::SQLiteConnection>
    {
        Set<AnnotatedAuthor> authors{ this };
        Set<AnnotatedArticle> articles{ this };
        Set<AnnotatedCompositeKeyLink> links{ this };

        explicit AnnotatedCompositeKeyContext(std::shared_ptr<DAL::SQLiteConnection> c)
            : DAL::DbContextBase<DAL::SQLiteConnection>(std::move(c))
        {
        }
    };

    TEST(Phase52ReflectionHardeningTest, CompositeKeyAnnotationForwardedThroughAnnotatedDto)
    {
        using LinkDto = DAL::Schema::SchemaOf_t<AnnotatedCompositeKeyLink>;
        static_assert(LinkDto::composite_primary_key);
        static_assert(DAL::Schema::CompositePrimaryKey<LinkDto>);
        static_assert(DAL::Schema::RelationalEntity<LinkDto>);

        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());
        AnnotatedCompositeKeyContext db(connRes.value());
        ASSERT_TRUE(db.ensure_created().has_value());

        auto author = db.authors.add_returning(AnnotatedAuthor{ .id = 0, .handle = "@ada", .name = "Ada" });
        ASSERT_TRUE(author.has_value());
        auto article = db.articles.add_returning(AnnotatedArticle{ .id = 0, .author_id = author->id, .title = "Engines" });
        ASSERT_TRUE(article.has_value());

        ASSERT_TRUE(
            db.links.add(AnnotatedCompositeKeyLink{ .author_id = author->id, .article_id = article->id, .note = "v1" }).has_value());
        auto dup = db.links.add(AnnotatedCompositeKeyLink{ .author_id = author->id, .article_id = article->id, .note = "dup" });
        ASSERT_FALSE(dup.has_value());
        EXPECT_EQ(dup.error().code, Core::DbErrorCode::DuplicateKey);

        ASSERT_TRUE(
            db.links.upsert(AnnotatedCompositeKeyLink{ .author_id = author->id, .article_id = article->id, .note = "v2" }).has_value());
        auto found = db.links.find(author->id, article->id);
        ASSERT_TRUE(found.has_value());
        EXPECT_EQ(found->note, "v2");

        auto info = db.table_info("AnnotatedCompositeLinks");
        ASSERT_TRUE(info.has_value());
        EXPECT_EQ((*info)[0].pk_index, 1);
        EXPECT_EQ((*info)[1].pk_index, 2);
        EXPECT_EQ((*info)[2].pk_index, 0);
    }

    struct FloatMetricDTO
    {
        static constexpr auto tablename = "FloatMetrics";
        sqlgen::PrimaryKey<std::int32_t, false> id{ 0 };
        double score = 0.0;
        std::optional<double> opt_score;
    };

    struct FloatMetricContext : DAL::DbContextBase<DAL::SQLiteConnection>
    {
        Set<FloatMetricDTO> metrics{ this };

        explicit FloatMetricContext(std::shared_ptr<DAL::SQLiteConnection> c)
            : DAL::DbContextBase<DAL::SQLiteConnection>(std::move(c))
        {
        }
    };

    TEST(Phase52ReflectionHardeningTest, DtoEqualsAndChangeTrackerHandleNaNFloatsWithoutDirtyLoop)
    {
        const double qnan = std::numeric_limits<double>::quiet_NaN();
        FloatMetricDTO a{ .id = 1, .score = qnan, .opt_score = qnan };
        FloatMetricDTO b{ .id = 1, .score = qnan, .opt_score = qnan };
        FloatMetricDTO c{ .id = 1, .score = 1.0, .opt_score = qnan };
        FloatMetricDTO d{ .id = 1, .score = qnan, .opt_score = std::nullopt };

        EXPECT_TRUE(DAL::Schema::dto_equals(a, b));
        EXPECT_FALSE(DAL::Schema::dto_equals(a, c));
        EXPECT_FALSE(DAL::Schema::dto_equals(a, d));

        // End-to-end ChangeTracker verification: once a tracked entity holds NaN and
        // is flushed, a subsequent save_changes() MUST NOT fire a redundant UPDATE loop.
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());
        auto conn = connRes.value();
        FloatMetricContext db(conn);
        ASSERT_TRUE(db.ensure_created().has_value());
        ASSERT_TRUE(conn->execute_raw("CREATE TABLE metric_updates (n INTEGER NOT NULL);").has_value());
        ASSERT_TRUE(conn->execute_raw(
                            "CREATE TRIGGER trg_metric_updates AFTER UPDATE ON FloatMetrics "
                            "BEGIN INSERT INTO metric_updates(n) VALUES (1); END;")
                        .has_value());

        ASSERT_TRUE(db.metrics.add(FloatMetricDTO{ .id = 1, .score = 1.0, .opt_score = std::nullopt }).has_value());
        db.enable_change_tracking();
        auto tracked = db.metrics.find_tracked(1);
        ASSERT_NE(tracked, nullptr);

        tracked->opt_score = qnan;
        ASSERT_TRUE(db.save_changes().has_value());

        auto updateCount = [&]() -> std::int64_t
        {
            auto rows = conn->query_rows("SELECT COUNT(*) FROM metric_updates;");
            EXPECT_TRUE(rows.has_value());
            return std::get<std::int64_t>(rows->rows[0].cells[0]);
        };
        EXPECT_EQ(updateCount(), 1);

        // Second save_changes() with unchanged NaN MUST emit 0 updates.
        ASSERT_TRUE(db.save_changes().has_value());
        EXPECT_EQ(updateCount(), 1);
    }

    struct TwoColumnScoreDTO
    {
        static constexpr auto tablename = "TwoColumnScores";
        sqlgen::PrimaryKey<std::int32_t, false> user_id{ 0 };
        std::int32_t score = 0;
    };

    class NonBijectiveCtorEntity
    {
    public:
        NonBijectiveCtorEntity() = default;
        // Intentionally non-bijective: both params match `user_id`, ignoring `score`.
        NonBijectiveCtorEntity(std::int32_t user_id, std::int32_t userId)
            : user_id_(user_id)
            , score_(userId)
        {
        }

        [[nodiscard]] std::int32_t getUserId() const noexcept
        {
            return user_id_;
        }
        [[nodiscard]] std::int32_t getScore() const noexcept
        {
            return score_;
        }

    private:
        std::int32_t user_id_{ 0 };
        std::int32_t score_{ 0 };
    };

    class[[= DAL::Schema::table{ std::define_static_string("AliasedProfiles") }]] AliasedCtorEntity
    {
    public:
        // No default constructor: hydration MUST match `displayName` through [[=column{"db_full_name"}]].
        AliasedCtorEntity(std::int32_t id, std::string displayName)
            : id_(id)
            , displayName_(std::move(displayName))
        {
        }

        [[nodiscard]] std::int32_t getId() const noexcept
        {
            return id_;
        }
        [[nodiscard]] const std::string& getDisplayName() const noexcept
        {
            return displayName_;
        }

    private:
        [[= DAL::Schema::primary_key{ true }]] std::int32_t id_{ 0 };
        [[= DAL::Schema::column{ std::define_static_string("db_full_name") }]] std::string displayName_;
    };

    struct AliasedCtorContext : DAL::DbContextBase<DAL::SQLiteConnection>
    {
        Set<AliasedCtorEntity> profiles{ this };

        explicit AliasedCtorContext(std::shared_ptr<DAL::SQLiteConnection> c)
            : DAL::DbContextBase<DAL::SQLiteConnection>(std::move(c))
        {
        }
    };

    TEST(Phase52ReflectionHardeningTest, HydratingConstructorEnforcesBijectivityAndColumnAnnotation)
    {
        // 1. Non-bijective constructor MUST be rejected; member-wise fallback hydrates both columns properly.
        static_assert(
            DAL::Mappers::detail::find_hydrating_constructor<NonBijectiveCtorEntity>(DAL::Schema::members_v<TwoColumnScoreDTO>) ==
            std::meta::info{});
        TwoColumnScoreDTO dto{ .user_id = 7, .score = 99 };
        auto hydrated = DAL::Mappers::MapperTraits<NonBijectiveCtorEntity, TwoColumnScoreDTO>::to_domain(dto);
        EXPECT_EQ(hydrated.getUserId(), 7);
        EXPECT_EQ(hydrated.getScore(), 99);

        // 2. Non-default-constructible entity with [[=column{"db_full_name"}]] hydrates via constructor end-to-end.
        auto connRes = DAL::SQLiteConnection::connect(":memory:");
        ASSERT_TRUE(connRes.has_value());
        AliasedCtorContext db(connRes.value());
        ASSERT_TRUE(db.ensure_created().has_value());

        auto added = db.profiles.add_returning(AliasedCtorEntity(0, "Grace Hopper"));
        ASSERT_TRUE(added.has_value()) << added.error().to_string();
        EXPECT_EQ(added->getId(), 1);
        EXPECT_EQ(added->getDisplayName(), "Grace Hopper");

        auto found = db.profiles.find(1);
        ASSERT_TRUE(found.has_value());
        EXPECT_EQ(found->getDisplayName(), "Grace Hopper");
    }

    struct AmbiguousQueryColsDTO
    {
        static constexpr auto tablename = "AmbiguousQueryCols";
        sqlgen::PrimaryKey<std::int32_t, false> user_id{ 0 };
        std::int32_t userid = 0;
    };

    TEST(Phase53ReflectionAmbiguityTest, ValidQueryColumnRejectsAmbiguousMatches)
    {
        static_assert(DAL::Mappers::ValidQueryColumn<AmbiguousQueryColsDTO, AmbiguousQueryColsDTO, "user_id">);
        static_assert(DAL::Mappers::ValidQueryColumn<AmbiguousQueryColsDTO, AmbiguousQueryColsDTO, "userid">);
        // "userId" matches both `user_id` and `userid` via snake_to_camel -> MUST be rejected as ambiguous.
        static_assert(!DAL::Mappers::ValidQueryColumn<AmbiguousQueryColsDTO, AmbiguousQueryColsDTO, "userId">);
        SUCCEED();
    }

    // =========================================================================
    // Phase U1: Ergonomic P3394 Annotations, Nullable/Self FKs, Indexes, Version
    // =========================================================================

    struct U1AuthorTag
    {
    };
    using U1AuthorId = Core::Id<U1AuthorTag, std::int32_t>;

    struct[[= DAL::Schema::table()]] U1DefaultNamedEntity
    {
        [[= DAL::Schema::primary_key{ true }]] std::int32_t id = 0;
        std::string label;
        [[= DAL::Schema::ignore{}]] std::string transient_cache = "ephemeral";
    };

    struct[[= DAL::Schema::table("U1Authors")]][[= DAL::Schema::composite_index("idx_u1_country_name", "country, display_name")]] U1Author
    {
        [[= DAL::Schema::primary_key{ true }]] U1AuthorId id{ 0 };
        [[= DAL::Schema::column("display_name")]][[= DAL::Schema::index{}]] std::string name;
        std::string country;
        [[= DAL::Schema::check("age >= 0")]] std::int32_t age = 0;
        [[= DAL::Schema::default_sql("'active'")]] std::string status = "active";
        [[= DAL::Schema::ignore{}]] int runtime_score = 42;
    };

    struct[[= DAL::Schema::table("U1Books")]] U1Book
    {
        [[= DAL::Schema::primary_key{ true }]] std::int32_t id = 0;
        std::string title;
        [[= DAL::Schema::references<U1Author>("id", DAL::Schema::FkAction::Cascade)]] U1AuthorId author_id{ 0 };
    };

    struct[[= DAL::Schema::table("U1Departments")]] U1Department
    {
        [[= DAL::Schema::primary_key{ true }]] std::int32_t id = 0;
        std::string name;
    };

    struct[[= DAL::Schema::table("U1Employees")]] U1Employee
    {
        [[= DAL::Schema::primary_key{ true }]] std::int32_t id = 0;
        std::string name;
        [[= DAL::Schema::references<U1Department>("id", DAL::Schema::FkAction::SetNull)]] std::optional<std::int32_t> dept_id;
    };

    struct[[= DAL::Schema::table("U1Categories")]] U1Category
    {
        [[= DAL::Schema::primary_key{ true }]] std::int32_t id = 0;
        std::string name;
        [[= DAL::Schema::references_self("id", DAL::Schema::FkAction::Cascade)]] std::optional<std::int32_t> parent_id;
    };

    struct[[= DAL::Schema::table("U1VersionedDocs")]] U1VersionedDoc
    {
        [[= DAL::Schema::primary_key{ true }]] std::int32_t id = 0;
        std::string body;
        [[= DAL::Schema::version{}]] std::int64_t row_version = 1;
    };

    struct U1DbContext : DAL::DbContextBase<DAL::SQLiteConnection>
    {
        Set<U1DefaultNamedEntity> defaults{ this };
        Set<U1Author> authors{ this };
        Set<U1Book> books{ this };
        Set<U1Department> departments{ this };
        Set<U1Employee> employees{ this };
        Set<U1Category> categories{ this };
        Set<U1VersionedDoc> docs{ this };

        explicit U1DbContext(std::shared_ptr<DAL::SQLiteConnection> c)
            : DAL::DbContextBase<DAL::SQLiteConnection>(std::move(c))
        {
        }
    };

    TEST(PhaseU1AnnotationsTest, ConciseAnnotationsDefaultTableNameIgnoreIndexesCheckAndStrongId)
    {
        auto connRes = DAL::SQLiteConnection::open_in_memory();
        ASSERT_TRUE(connRes.has_value()) << connRes.error().to_string();
        U1DbContext db(connRes.value());
        ASSERT_TRUE(db.ensure_created().has_value());

        // 1. Default [[=table()]] derives table name from class identifier, and [[=ignore{}]] is excluded from schema.
        auto defCols = db.table_info("U1DefaultNamedEntity");
        ASSERT_TRUE(defCols.has_value()) << defCols.error().to_string();
        EXPECT_EQ(defCols->size(), 2u);
        for (const auto& c : *defCols)
        {
            EXPECT_NE(c.name, "transient_cache");
        }
        auto addedDef = db.defaults.add_returning(U1DefaultNamedEntity{ .id = 0, .label = "hello", .transient_cache = "dirty" });
        ASSERT_TRUE(addedDef.has_value()) << addedDef.error().to_string();
        EXPECT_EQ(addedDef->id, 1);
        EXPECT_EQ(addedDef->label, "hello");
        EXPECT_EQ(addedDef->transient_cache, "ephemeral");

        // 2. Concise [[=table("...")]] / [[=column("...")]] / Core::Id PK+FK / [[=index]] / [[=composite_index]] / [[=check]].
        auto indexes = connRes.value()->list_indexes("U1Authors");
        ASSERT_TRUE(indexes.has_value()) << indexes.error().to_string();
        bool foundSingleIdx = false;
        bool foundCompositeIdx = false;
        for (const auto& ix : *indexes)
        {
            if (ix.name == "idx_U1Authors_display_name")
            {
                foundSingleIdx = true;
            }
            if (ix.name == "idx_u1_country_name")
            {
                foundCompositeIdx = true;
            }
        }
        EXPECT_TRUE(foundSingleIdx);
        EXPECT_TRUE(foundCompositeIdx);

        // CHECK constraint rejects negative age
        auto badAuthor = db.authors.add(U1Author{ .id = U1AuthorId{ 0 }, .name = "Bad", .country = "US", .age = -5 });
        EXPECT_FALSE(badAuthor.has_value());
        EXPECT_EQ(badAuthor.error().code, Core::DbErrorCode::ConstraintViolation);

        auto author = db.authors.add_returning(U1Author{ .id = U1AuthorId{ 0 }, .name = "Ada", .country = "UK", .age = 36 });
        ASSERT_TRUE(author.has_value()) << author.error().to_string();
        EXPECT_EQ(author->id.get(), 1);
        EXPECT_EQ(author->name, "Ada");

        // Core::Id FK round-trip + ON DELETE CASCADE
        auto book = db.books.add_returning(U1Book{ .id = 0, .title = "Notes", .author_id = author->id });
        ASSERT_TRUE(book.has_value()) << book.error().to_string();
        EXPECT_EQ(book->author_id, author->id);

        auto parentAuthor = db.query_parent<U1Author>(*book);
        ASSERT_TRUE(parentAuthor.has_value()) << parentAuthor.error().to_string();
        ASSERT_TRUE(parentAuthor->has_value());
        EXPECT_EQ((*parentAuthor)->name, "Ada");

        // Deleting author must cascade-delete U1Book
        ASSERT_TRUE(db.authors.remove(author->id).has_value());
        EXPECT_EQ(db.books.count(), 0u);
    }

    TEST(PhaseU1AnnotationsTest, NullableFkSetNullAndSelfReferencingTree)
    {
        auto connRes = DAL::SQLiteConnection::open_in_memory();
        ASSERT_TRUE(connRes.has_value());
        U1DbContext db(connRes.value());
        ASSERT_TRUE(db.ensure_created().has_value());

        auto dept = db.departments.add_returning(U1Department{ .id = 0, .name = "R&D" });
        ASSERT_TRUE(dept.has_value()) << dept.error().to_string();

        // Nullable FK with value AND with nullopt
        auto e1 = db.employees.add_returning(U1Employee{ .id = 0, .name = "Assigned", .dept_id = dept->id });
        auto e2 = db.employees.add_returning(U1Employee{ .id = 0, .name = "Contractor", .dept_id = std::nullopt });
        ASSERT_TRUE(e1.has_value()) << e1.error().to_string();
        ASSERT_TRUE(e2.has_value()) << e2.error().to_string();

        auto p1 = db.query_parent<U1Department>(*e1);
        ASSERT_TRUE(p1.has_value() && p1->has_value());
        EXPECT_EQ((*p1)->name, "R&D");

        auto p2 = db.query_parent<U1Department>(*e2);
        ASSERT_TRUE(p2.has_value());
        EXPECT_FALSE(p2->has_value());

        // Deleting department triggers ON DELETE SET NULL
        ASSERT_TRUE(db.departments.remove(dept->id).has_value());
        auto e1After = db.employees.find(e1->id);
        ASSERT_TRUE(e1After.has_value());
        EXPECT_FALSE(e1After->dept_id.has_value());

        // Self-referencing tree: root -> child, deleting root cascades to child, invalid parent_id fails FK
        auto root = db.categories.add_returning(U1Category{ .id = 0, .name = "Root", .parent_id = std::nullopt });
        ASSERT_TRUE(root.has_value()) << root.error().to_string();
        auto child = db.categories.add_returning(U1Category{ .id = 0, .name = "Child", .parent_id = root->id });
        ASSERT_TRUE(child.has_value()) << child.error().to_string();
        auto orphan = db.categories.add(U1Category{ .id = 0, .name = "Bad", .parent_id = 9999 });
        EXPECT_FALSE(orphan.has_value());
        EXPECT_EQ(orphan.error().code, Core::DbErrorCode::ConstraintViolation);

        ASSERT_TRUE(db.categories.remove(root->id).has_value());
        EXPECT_EQ(db.categories.count(), 0u);
    }

    TEST(PhaseU1AnnotationsTest, OptimisticConcurrencyVersionColumnDetectsStaleOverwrite)
    {
        auto connRes = DAL::SQLiteConnection::open_in_memory();
        ASSERT_TRUE(connRes.has_value());
        U1DbContext db(connRes.value());
        ASSERT_TRUE(db.ensure_created().has_value());

        auto created = db.docs.add_returning(U1VersionedDoc{ .id = 0, .body = "v1", .row_version = 1 });
        ASSERT_TRUE(created.has_value()) << created.error().to_string();

        U1VersionedDoc copyA = *created;
        U1VersionedDoc copyB = *created;

        copyA.body = "edited-by-A";
        ASSERT_TRUE(db.docs.update(copyA).has_value());

        auto fresh = db.docs.find(created->id);
        ASSERT_TRUE(fresh.has_value());
        EXPECT_EQ(fresh->body, "edited-by-A");
        EXPECT_EQ(fresh->row_version, 2);

        // Stale copyB still has row_version == 1 -> must fail with ConcurrencyConflict
        copyB.body = "stale-overwrite-by-B";
        auto conflictRes = db.docs.update(copyB);
        ASSERT_FALSE(conflictRes.has_value());
        EXPECT_EQ(conflictRes.error().code, Core::DbErrorCode::ConcurrencyConflict);

        // Updating with refreshed version == 2 succeeds -> increments to 3
        fresh->body = "edited-by-B-after-refresh";
        ASSERT_TRUE(db.docs.update(*fresh).has_value());
        auto fresh2 = db.docs.find(created->id);
        ASSERT_TRUE(fresh2.has_value());
        EXPECT_EQ(fresh2->row_version, 3);

        // Updating a non-existent row still returns NotFound (not ConcurrencyConflict)
        auto notFoundRes = db.docs.update(U1VersionedDoc{ .id = 999, .body = "ghost", .row_version = 1 });
        ASSERT_FALSE(notFoundRes.has_value());
        EXPECT_EQ(notFoundRes.error().code, Core::DbErrorCode::NotFound);
    }
} // namespace
