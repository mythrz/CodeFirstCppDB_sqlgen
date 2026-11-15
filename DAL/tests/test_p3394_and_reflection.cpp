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
} // namespace
