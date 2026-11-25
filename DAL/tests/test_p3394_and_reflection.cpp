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
} // namespace
