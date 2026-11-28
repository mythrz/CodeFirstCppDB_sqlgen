module;
#include <meta>
#include <rfl/internal/cpp20/num_fields.hpp>
#include <sqlgen/ForeignKey.hpp>
#include <sqlgen/PrimaryKey.hpp>
#include <sqlgen/Unique.hpp>
#include <sqlgen/col.hpp>

export module codefirst.dal:schema_traits;

import std;
import codefirst.core;

export namespace DAL::Schema
{
    enum class FkAction
    {
        NoAction,
        Restrict,
        SetNull,
        SetDefault,
        Cascade
    };

    inline constexpr char kEmptyStaticStr[] = "";
    inline constexpr char kDefaultIdColStr[] = "id";

    // P3394 C++26 Annotation tags for single-class entities (consteval constructors eliminate std::define_static_string boilerplate)
    struct table
    {
        const char* name = kEmptyStaticStr;
        bool has_custom_name = false;

        consteval table() noexcept = default;
        consteval table(std::string_view sv)
            : name(std::define_static_string(sv))
            , has_custom_name(true)
        {
        }
        consteval table(const char* s)
            : name(std::define_static_string(std::string_view{ s }))
            , has_custom_name(true)
        {
        }
    };

    struct column
    {
        const char* name = kEmptyStaticStr;

        consteval column(std::string_view sv)
            : name(std::define_static_string(sv))
        {
        }
        consteval column(const char* s)
            : name(std::define_static_string(std::string_view{ s }))
        {
        }
    };

    struct primary_key
    {
        bool auto_increment = false;
    };

    struct unique
    {
    };

    struct composite_key
    {
    };

    struct ignore
    {
    };

    struct version
    {
    };

    struct index
    {
        const char* name = kEmptyStaticStr;
        bool has_custom_name = false;
        bool unique = false;

        consteval index() noexcept = default;
        consteval explicit index(bool is_unique) noexcept
            : unique(is_unique)
        {
        }
        consteval index(std::string_view n, bool is_unique = false)
            : name(std::define_static_string(n))
            , has_custom_name(true)
            , unique(is_unique)
        {
        }
        consteval index(const char* n, bool is_unique = false)
            : name(std::define_static_string(std::string_view{ n }))
            , has_custom_name(true)
            , unique(is_unique)
        {
        }
    };

    struct composite_index
    {
        const char* name = kEmptyStaticStr;
        const char* columns_csv = kEmptyStaticStr;
        bool unique = false;

        consteval composite_index(std::string_view n, std::string_view cols, bool is_unique = false)
            : name(std::define_static_string(n))
            , columns_csv(std::define_static_string(cols))
            , unique(is_unique)
        {
        }
        consteval composite_index(const char* n, const char* cols, bool is_unique = false)
            : name(std::define_static_string(std::string_view{ n }))
            , columns_csv(std::define_static_string(std::string_view{ cols }))
            , unique(is_unique)
        {
        }
    };

    struct default_sql
    {
        const char* expr = kEmptyStaticStr;

        consteval default_sql(std::string_view sv)
            : expr(std::define_static_string(sv))
        {
        }
        consteval default_sql(const char* s)
            : expr(std::define_static_string(std::string_view{ s }))
        {
        }
    };

    struct check
    {
        const char* expr = kEmptyStaticStr;

        consteval check(std::string_view sv)
            : expr(std::define_static_string(sv))
        {
        }
        consteval check(const char* s)
            : expr(std::define_static_string(std::string_view{ s }))
        {
        }
    };

    template<typename Parent>
    struct references
    {
        const char* col = kDefaultIdColStr;
        FkAction on_delete = FkAction::NoAction;
        FkAction on_update = FkAction::NoAction;

        consteval references() noexcept = default;
        consteval references(std::string_view c, FkAction del = FkAction::NoAction, FkAction upd = FkAction::NoAction)
            : col(std::define_static_string(c))
            , on_delete(del)
            , on_update(upd)
        {
        }
        consteval references(const char* c, FkAction del = FkAction::NoAction, FkAction upd = FkAction::NoAction)
            : col(std::define_static_string(std::string_view{ c }))
            , on_delete(del)
            , on_update(upd)
        {
        }
        consteval references(FkAction del, FkAction upd = FkAction::NoAction) noexcept
            : col(kDefaultIdColStr)
            , on_delete(del)
            , on_update(upd)
        {
        }
    };

    struct references_self
    {
        const char* col = kDefaultIdColStr;
        FkAction on_delete = FkAction::NoAction;
        FkAction on_update = FkAction::NoAction;

        consteval references_self() noexcept = default;
        consteval references_self(std::string_view c, FkAction del = FkAction::NoAction, FkAction upd = FkAction::NoAction)
            : col(std::define_static_string(c))
            , on_delete(del)
            , on_update(upd)
        {
        }
        consteval references_self(const char* c, FkAction del = FkAction::NoAction, FkAction upd = FkAction::NoAction)
            : col(std::define_static_string(std::string_view{ c }))
            , on_delete(del)
            , on_update(upd)
        {
        }
        consteval references_self(FkAction del, FkAction upd = FkAction::NoAction) noexcept
            : col(kDefaultIdColStr)
            , on_delete(del)
            , on_update(upd)
        {
        }
    };

    struct FieldDdlMeta
    {
        bool is_primary_key = false;
        bool is_unique = false;
        bool is_version = false;
        bool has_index = false;
        bool index_unique = false;
        bool has_index_name = false;
        const char* index_name = kEmptyStaticStr;
        bool has_default_sql = false;
        const char* default_expr = kEmptyStaticStr;
        bool has_check_sql = false;
        const char* check_expr = kEmptyStaticStr;
        bool has_self_fk = false;
        const char* self_fk_col = kEmptyStaticStr;
        FkAction on_delete = FkAction::NoAction;
        FkAction on_update = FkAction::NoAction;
    };

    struct CompositeIndexMeta
    {
        const char* name = kEmptyStaticStr;
        const char* columns_csv = kEmptyStaticStr;
        bool unique = false;
    };

    namespace detail
    {
        template<typename>
        inline constexpr bool is_optional_v = false;
        template<typename T>
        inline constexpr bool is_optional_v<std::optional<T>> = true;

        template<typename T>
        inline constexpr bool is_sys_time_v = false;

        template<typename Duration>
        inline constexpr bool is_sys_time_v<std::chrono::time_point<std::chrono::system_clock, Duration>> = true;

        template<typename T>
        inline constexpr bool is_ymd_v = std::is_same_v<std::remove_cvref_t<T>, std::chrono::year_month_day>;

        template<typename T>
        inline constexpr bool is_fs_path_v = std::is_same_v<std::remove_cvref_t<T>, std::filesystem::path>;

        template<typename T>
        inline constexpr bool is_byte_array_v = false;

        template<std::size_t N>
        inline constexpr bool is_byte_array_v<std::array<std::byte, N>> = true;

        template<typename T>
        struct byte_array_extent : std::integral_constant<std::size_t, 0>
        {
        };

        template<std::size_t N>
        struct byte_array_extent<std::array<std::byte, N>> : std::integral_constant<std::size_t, N>
        {
        };

        template<typename T>
        inline constexpr std::size_t byte_array_extent_v = byte_array_extent<std::remove_cvref_t<T>>::value;

        template<typename Duration>
        [[nodiscard]] inline std::string format_sys_time_iso8601(std::chrono::sys_time<Duration> tp)
        {
            const auto dp = std::chrono::floor<std::chrono::days>(tp);
            const std::chrono::year_month_day ymd{ dp };
            const std::chrono::hh_mm_ss<Duration> hms{ tp - dp };
            const int yr = static_cast<int>(ymd.year());
            const unsigned mo = static_cast<unsigned>(ymd.month());
            const unsigned da = static_cast<unsigned>(ymd.day());
            const auto hr = static_cast<unsigned>(hms.hours().count());
            const auto mi = static_cast<unsigned>(hms.minutes().count());
            const auto sc = static_cast<unsigned>(hms.seconds().count());

            if constexpr (std::ratio_greater_equal_v<typename Duration::period, std::ratio<1>>)
            {
                return std::format("{:04d}-{:02d}-{:02d}T{:02d}:{:02d}:{:02d}Z", yr, mo, da, hr, mi, sc);
            }
            else if constexpr (std::ratio_greater_equal_v<typename Duration::period, std::milli>)
            {
                const auto sub = std::chrono::duration_cast<std::chrono::milliseconds>(hms.subseconds()).count();
                return std::format("{:04d}-{:02d}-{:02d}T{:02d}:{:02d}:{:02d}.{:03d}Z", yr, mo, da, hr, mi, sc, static_cast<int>(sub));
            }
            else if constexpr (std::ratio_greater_equal_v<typename Duration::period, std::micro>)
            {
                const auto sub = std::chrono::duration_cast<std::chrono::microseconds>(hms.subseconds()).count();
                return std::format("{:04d}-{:02d}-{:02d}T{:02d}:{:02d}:{:02d}.{:06d}Z", yr, mo, da, hr, mi, sc, static_cast<int>(sub));
            }
            else
            {
                const auto sub = std::chrono::duration_cast<std::chrono::nanoseconds>(hms.subseconds()).count();
                return std::format(
                    "{:04d}-{:02d}-{:02d}T{:02d}:{:02d}:{:02d}.{:09d}Z", yr, mo, da, hr, mi, sc, static_cast<long long>(sub));
            }
        }

        [[nodiscard]] inline std::expected<std::chrono::year_month_day, Core::DbError> parse_ymd_iso8601(std::string_view sv)
        {
            if (sv.size() != 10 || sv[4] != '-' || sv[7] != '-')
            {
                return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation(
                    std::format("Invalid ISO-8601 date ({} bytes)", sv.size())) };
            }
            auto parse_uint = [](std::string_view part) -> std::optional<unsigned>
            {
                for (const char ch : part)
                {
                    if (ch < '0' || ch > '9')
                    {
                        return std::nullopt;
                    }
                }
                unsigned v = 0;
                const auto [ptr, ec] = std::from_chars(part.data(), part.data() + part.size(), v);
                if (ec != std::errc{} || ptr != part.data() + part.size())
                {
                    return std::nullopt;
                }
                return v;
            };
            const auto y = parse_uint(sv.substr(0, 4));
            const auto m = parse_uint(sv.substr(5, 2));
            const auto d = parse_uint(sv.substr(8, 2));
            if (!y || !m || !d)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation(
                    std::format("Invalid ISO-8601 date ({} bytes)", sv.size())) };
            }
            const std::chrono::year_month_day ymd{ std::chrono::year{ static_cast<int>(*y) },
                                                   std::chrono::month{ *m },
                                                   std::chrono::day{ *d } };
            if (!ymd.ok())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation("Calendar date is invalid") };
            }
            return ymd;
        }

        [[nodiscard]] inline std::string format_ymd_iso8601(std::chrono::year_month_day ymd)
        {
            return std::format(
                "{:04d}-{:02d}-{:02d}", static_cast<int>(ymd.year()), static_cast<unsigned>(ymd.month()), static_cast<unsigned>(ymd.day()));
        }

        template<typename SysTimeT>
        [[nodiscard]] inline std::expected<SysTimeT, Core::DbError> parse_sys_time_iso8601(std::string_view sv)
        {
            using Duration = typename SysTimeT::duration;
            if (sv.size() < 19 || (sv[10] != 'T' && sv[10] != 't' && sv[10] != ' ') || sv[13] != ':' || sv[16] != ':')
            {
                return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation(
                    std::format("Invalid ISO-8601 timestamp ({} bytes)", sv.size())) };
            }
            auto ymd_res = parse_ymd_iso8601(sv.substr(0, 10));
            if (!ymd_res)
            {
                return std::unexpected<Core::DbError>{ ymd_res.error() };
            }
            auto parse_2 = [](std::string_view part) -> std::optional<unsigned>
            {
                if (part.size() != 2 || part[0] < '0' || part[0] > '9' || part[1] < '0' || part[1] > '9')
                {
                    return std::nullopt;
                }
                return static_cast<unsigned>((part[0] - '0') * 10 + (part[1] - '0'));
            };
            const auto hr = parse_2(sv.substr(11, 2));
            const auto mi = parse_2(sv.substr(14, 2));
            const auto sc = parse_2(sv.substr(17, 2));
            if (!hr || !mi || !sc || *hr > 23u || *mi > 59u || *sc > 59u)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation(
                    std::format("Invalid ISO-8601 time-of-day ({} bytes)", sv.size())) };
            }

            std::size_t pos = 19;
            std::int64_t frac_ns = 0;
            if (pos < sv.size() && sv[pos] == '.')
            {
                ++pos;
                const std::size_t frac_start = pos;
                std::int64_t scale = 100'000'000LL;
                while (pos < sv.size() && sv[pos] >= '0' && sv[pos] <= '9')
                {
                    if (scale > 0)
                    {
                        frac_ns += static_cast<std::int64_t>(sv[pos] - '0') * scale;
                        scale /= 10;
                    }
                    ++pos;
                }
                if (pos == frac_start)
                {
                    return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation("Invalid ISO-8601 fractional seconds") };
                }
            }
            if (pos < sv.size() && (sv[pos] == 'Z' || sv[pos] == 'z'))
            {
                ++pos;
            }
            if (pos != sv.size())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation(
                    std::format("Unexpected trailing characters in ISO-8601 timestamp ({} bytes)", sv.size())) };
            }

            const std::chrono::sys_days days_tp{ *ymd_res };
            const auto secs_tp = days_tp + std::chrono::hours{ *hr } + std::chrono::minutes{ *mi } + std::chrono::seconds{ *sc };
            const auto total_ns = std::chrono::time_point_cast<std::chrono::nanoseconds>(secs_tp) + std::chrono::nanoseconds{ frac_ns };
            return std::chrono::time_point_cast<Duration>(total_ns);
        }

        template<typename T>
        struct DtoStorageType
        {
            using type = T;
        };

        template<typename Tag, typename V>
        struct DtoStorageType<Core::Id<Tag, V>>
        {
            using type = typename DtoStorageType<V>::type;
        };

        template<typename Duration>
        struct DtoStorageType<std::chrono::time_point<std::chrono::system_clock, Duration>>
        {
            using type = std::string;
        };

        template<>
        struct DtoStorageType<std::chrono::year_month_day>
        {
            using type = std::string;
        };

        template<>
        struct DtoStorageType<std::filesystem::path>
        {
            using type = std::string;
        };

        template<std::size_t N>
        struct DtoStorageType<std::array<std::byte, N>>
        {
            using type = std::vector<std::byte>;
        };

        template<typename E>
            requires std::is_enum_v<E>
        struct DtoStorageType<E>
        {
            using type = std::underlying_type_t<E>;
        };

        template<typename Inner>
        struct DtoStorageType<std::optional<Inner>>
        {
            using type = std::optional<typename DtoStorageType<Inner>::type>;
        };

        template<typename T>
        using dto_storage_type_t = typename DtoStorageType<std::remove_cvref_t<T>>::type;
    } // namespace detail

    // Collects all persisted nonstatic data members (including public unambiguous base classes,
    // skipping any member annotated with [[=DAL::Schema::ignore{}]]).
    consteval std::vector<std::meta::info> collect_data_members(
        std::meta::info type, std::meta::access_context ctx = std::meta::access_context::current())
    {
        std::vector<std::meta::info> result;
        for (auto base : std::meta::bases_of(type, ctx))
        {
            auto base_members = collect_data_members(std::meta::type_of(base), ctx);
            result.insert(result.end(), base_members.begin(), base_members.end());
        }
        for (auto m : std::meta::nonstatic_data_members_of(type, ctx))
        {
            if (std::meta::annotations_of_with_type(m, ^^ignore).empty())
            {
                result.push_back(m);
            }
        }
        return result;
    }

    template<typename T>
    inline constexpr auto members_v = std::define_static_array(collect_data_members(^^T));

    template<typename T>
    inline constexpr auto unchecked_members_v = std::define_static_array(collect_data_members(^^T, std::meta::access_context::unchecked()));

    template<typename T>
    concept HasTableName = requires {
        { T::tablename } -> std::convertible_to<std::string_view>;
    };

    // Detects sqlgen::PrimaryKey<V, auto_incr> fields (exact type match)
    template<typename F>
    inline constexpr bool is_sqlgen_primary_key_v = false;
    template<typename T, bool AutoIncr>
    inline constexpr bool is_sqlgen_primary_key_v<sqlgen::PrimaryKey<T, AutoIncr>> = true;

    template<typename F>
    concept SqlgenPrimaryKeyField = is_sqlgen_primary_key_v<std::remove_cvref_t<F>>;

    // Detects sqlgen::ForeignKey<V, Table, Col> fields (exact type match)
    template<typename F>
    inline constexpr bool is_sqlgen_foreign_key_v = false;
    template<typename T, typename ForeignTableType, rfl::internal::StringLiteral ColName>
    inline constexpr bool is_sqlgen_foreign_key_v<sqlgen::ForeignKey<T, ForeignTableType, ColName>> = true;

    template<typename F>
    concept SqlgenForeignKeyField = is_sqlgen_foreign_key_v<std::remove_cvref_t<F>>;

    // Detects nullable std::optional<sqlgen::ForeignKey<V, Table, Col>> fields
    template<typename F>
    inline constexpr bool is_optional_sqlgen_foreign_key_v = false;
    template<typename T, typename ForeignTableType, rfl::internal::StringLiteral ColName>
    inline constexpr bool is_optional_sqlgen_foreign_key_v<std::optional<sqlgen::ForeignKey<T, ForeignTableType, ColName>>> = true;

    template<typename F>
    concept AnyForeignKeyField = SqlgenForeignKeyField<F> || is_optional_sqlgen_foreign_key_v<std::remove_cvref_t<F>>;

    template<typename F>
    struct UnwrapFkType
    {
        using type = std::remove_cvref_t<F>;
    };

    template<typename T, typename ForeignTableType, rfl::internal::StringLiteral ColName>
    struct UnwrapFkType<std::optional<sqlgen::ForeignKey<T, ForeignTableType, ColName>>>
    {
        using type = sqlgen::ForeignKey<T, ForeignTableType, ColName>;
    };

    template<typename F>
    using unwrap_fk_type_t = typename UnwrapFkType<std::remove_cvref_t<F>>::type;

    template<typename T, std::size_t Idx>
    consteval bool is_dto_primary_key_at()
    {
        if constexpr (requires { T::pk_field_flags; })
        {
            return T::pk_field_flags[Idx];
        }
        else
        {
            using RawFieldType = [:std::meta::type_of(members_v<T>[Idx]):];
            using FieldType = std::remove_cvref_t<RawFieldType>;
            return SqlgenPrimaryKeyField<FieldType>;
        }
    }

    template<typename T>
    consteval std::size_t primary_key_count()
    {
        constexpr std::size_t N = members_v<T>.size();
        return []<std::size_t... Is>(std::index_sequence<Is...>) consteval
        {
            return (static_cast<std::size_t>(is_dto_primary_key_at<T, Is>() ? 1u : 0u) + ... + 0u);
        }(std::make_index_sequence<N>{});
    }

    template<typename T>
    consteval std::size_t foreign_key_count()
    {
        std::size_t count = 0;
        template for (constexpr auto m : members_v<T>)
        {
            using RawFieldType = [:std::meta::type_of(m):];
            using FieldType = std::remove_cvref_t<RawFieldType>;
            if constexpr (AnyForeignKeyField<FieldType>)
            {
                ++count;
            }
        }
        return count;
    }

    template<typename T>
    consteval bool has_explicit_composite_key()
    {
        if constexpr (requires {
                          { T::composite_primary_key } -> std::convertible_to<bool>;
                      })
        {
            if (static_cast<bool>(T::composite_primary_key))
            {
                return true;
            }
        }
        return !std::meta::annotations_of_with_type(^^T, ^^composite_key).empty();
    }

    // Entities with a single primary key (e.g. PersonDTO, SomethingDTO)
    template<typename T>
    concept SinglePrimaryKey = HasTableName<T> && (primary_key_count<T>() == 1);

    // Entities with composite primary keys: either 2+ explicit primary key fields
    // (sqlgen::PrimaryKey or [[=DAL::Schema::primary_key{false}]]), or an explicitly marked
    // junction DTO (`static constexpr bool composite_primary_key = true` / `[[=composite_key{}]]`)
    // with 2+ foreign key columns. Never silently inferred from 2+ arbitrary foreign keys.
    template<typename T>
    concept CompositePrimaryKey = HasTableName<T> && !SinglePrimaryKey<T> &&
        ((primary_key_count<T>() >= 2) || (primary_key_count<T>() == 0 && has_explicit_composite_key<T>() && foreign_key_count<T>() >= 2));

    // Generalized relational entity concept accommodating both single and composite key entities
    template<typename T>
    concept RelationalEntity = HasTableName<T> && (SinglePrimaryKey<T> || CompositePrimaryKey<T>);

    // Trait mapping a Domain entity type to its persistence DTO schema type.
    // Supports optional P3394 [[=table{"..."}]] single-class entities via define_aggregate,
    // explicit SchemaOf<Domain> specializations, or zero-DTO aggregates (Domain == DTO).
    template<typename T>
    consteval bool has_table_annotation()
    {
        return !std::meta::annotations_of_with_type(^^T, ^^table).empty();
    }

    template<typename T>
        requires(has_table_annotation<T>())
    consteval std::string_view annotated_table_name()
    {
        auto anns = std::meta::annotations_of_with_type(^^T, ^^table);
        if (anns.size() > 1)
        {
            throw std::invalid_argument("Entity cannot have multiple [[=DAL::Schema::table]] annotations");
        }
        const auto ext = std::meta::extract<table>(anns[0]);
        std::string_view sv = ext.has_custom_name ? std::string_view{ ext.name } : std::meta::identifier_of(^^T);
        if (sv.empty())
        {
            throw std::invalid_argument("[[=DAL::Schema::table]] name cannot be empty");
        }
        if (sv.find('"') != std::string_view::npos)
        {
            throw std::invalid_argument("[[=DAL::Schema::table]] name cannot contain double quotes");
        }
        return sv;
    }

    template<typename Domain>
        requires(has_table_annotation<Domain>())
    struct AnnotatedDto;

    template<typename Domain>
    struct SchemaOf
    {
        using type = Domain;
    };

    template<typename Domain>
        requires(has_table_annotation<Domain>())
    struct SchemaOf<Domain>
    {
        using type = AnnotatedDto<Domain>;
    };

    template<typename Domain>
    using SchemaOf_t = typename SchemaOf<Domain>::type;

    template<typename T>
    using AnnotatedPkAuto = sqlgen::PrimaryKey<T, true>;
    template<typename T>
    using AnnotatedPkManual = sqlgen::PrimaryKey<T, false>;

    template<auto NameFn>
    consteval auto to_string_literal()
    {
        constexpr std::string_view name = NameFn();
        return [&]<std::size_t... Is>(std::index_sequence<Is...>) consteval
        {
            return rfl::internal::StringLiteral<name.size() + 1>{ std::array<char, name.size() + 1>{ name[Is]..., '\0' } };
        }(std::make_index_sequence<name.size()>{});
    }

    template<typename Domain>
        requires(has_table_annotation<Domain>())
    consteval auto annotated_pk_flags()
    {
        static constexpr auto& dom_members = unchecked_members_v<Domain>;
        std::array<bool, dom_members.size()> flags{};
        std::size_t idx = 0;
        template for (constexpr auto m : dom_members)
        {
            flags[idx++] = !std::meta::annotations_of_with_type(m, ^^primary_key).empty();
        }
        return flags;
    }

    template<typename Domain>
        requires(has_table_annotation<Domain>())
    consteval auto annotated_unique_flags()
    {
        static constexpr auto& dom_members = unchecked_members_v<Domain>;
        std::array<bool, dom_members.size()> flags{};
        std::size_t idx = 0;
        template for (constexpr auto m : dom_members)
        {
            flags[idx++] = !std::meta::annotations_of_with_type(m, ^^unique).empty();
        }
        return flags;
    }

    template<typename Domain>
        requires(has_table_annotation<Domain>())
    consteval auto annotated_field_ddl_meta()
    {
        static constexpr auto& dom_members = unchecked_members_v<Domain>;
        std::array<FieldDdlMeta, dom_members.size()> meta{};
        std::size_t idx = 0;
        std::size_t version_count = 0;
        template for (constexpr auto m : dom_members)
        {
            FieldDdlMeta fm{};
            fm.is_primary_key = !std::meta::annotations_of_with_type(m, ^^primary_key).empty();
            fm.is_unique = !std::meta::annotations_of_with_type(m, ^^unique).empty();

            constexpr auto ver_anns = std::define_static_array(std::meta::annotations_of_with_type(m, ^^version));
            if constexpr (!ver_anns.empty())
            {
                using RawFieldT = [:std::meta::remove_cvref(std::meta::type_of(m)):];
                if constexpr (!std::is_integral_v<RawFieldT> || std::is_same_v<RawFieldT, bool>)
                {
                    throw std::invalid_argument("[[=DAL::Schema::version]] column must have an integral type");
                }
                fm.is_version = true;
                ++version_count;
            }

            constexpr auto idx_anns = std::define_static_array(std::meta::annotations_of_with_type(m, ^^index));
            if constexpr (idx_anns.size() > 1)
            {
                throw std::invalid_argument("Field cannot have multiple [[=DAL::Schema::index]] annotations");
            }
            if constexpr (!idx_anns.empty())
            {
                const auto ix = std::meta::extract<index>(idx_anns[0]);
                fm.has_index = true;
                fm.index_unique = ix.unique;
                fm.has_index_name = ix.has_custom_name;
                fm.index_name = ix.name;
            }

            constexpr auto def_anns = std::define_static_array(std::meta::annotations_of_with_type(m, ^^default_sql));
            if constexpr (def_anns.size() > 1)
            {
                throw std::invalid_argument("Field cannot have multiple [[=DAL::Schema::default_sql]] annotations");
            }
            if constexpr (!def_anns.empty())
            {
                const auto d = std::meta::extract<default_sql>(def_anns[0]);
                const std::string_view sv{ d.expr };
                if (sv.empty() || sv.find(';') != std::string_view::npos)
                {
                    throw std::invalid_argument("[[=DAL::Schema::default_sql]] expression must be non-empty and contain no semicolons");
                }
                fm.has_default_sql = true;
                fm.default_expr = d.expr;
            }

            constexpr auto chk_anns = std::define_static_array(std::meta::annotations_of_with_type(m, ^^check));
            if constexpr (chk_anns.size() > 1)
            {
                throw std::invalid_argument("Field cannot have multiple [[=DAL::Schema::check]] annotations");
            }
            if constexpr (!chk_anns.empty())
            {
                const auto c = std::meta::extract<check>(chk_anns[0]);
                const std::string_view sv{ c.expr };
                if (sv.empty() || sv.find(';') != std::string_view::npos)
                {
                    throw std::invalid_argument("[[=DAL::Schema::check]] expression must be non-empty and contain no semicolons");
                }
                fm.has_check_sql = true;
                fm.check_expr = c.expr;
            }

            constexpr auto self_fk_anns = std::define_static_array(std::meta::annotations_of_with_type(m, ^^references_self));
            if constexpr (self_fk_anns.size() > 1)
            {
                throw std::invalid_argument("Field cannot have multiple [[=DAL::Schema::references_self]] annotations");
            }
            if constexpr (!self_fk_anns.empty())
            {
                const auto sfk = std::meta::extract<references_self>(self_fk_anns[0]);
                const std::string_view col_sv{ sfk.col };
                if (col_sv.empty() || col_sv.find('"') != std::string_view::npos)
                {
                    throw std::invalid_argument("[[=DAL::Schema::references_self]] column must be non-empty and contain no double quotes");
                }
                fm.has_self_fk = true;
                fm.self_fk_col = sfk.col;
                fm.on_delete = sfk.on_delete;
                fm.on_update = sfk.on_update;
            }

            static constexpr auto all_anns = std::define_static_array(std::meta::annotations_of(m));
            template for (constexpr auto ann : all_anns)
            {
                constexpr auto ann_type = std::meta::type_of(ann);
                if constexpr (std::meta::has_template_arguments(ann_type) && std::meta::template_of(ann_type) == ^^references)
                {
                    using AnnT = [:ann_type:];
                    const auto ref = std::meta::extract<AnnT>(ann);
                    fm.on_delete = ref.on_delete;
                    fm.on_update = ref.on_update;
                }
            }

            meta[idx++] = fm;
        }
        if (version_count > 1)
        {
            throw std::invalid_argument("Entity cannot have multiple [[=DAL::Schema::version]] columns");
        }
        return meta;
    }

    template<typename Domain>
        requires(has_table_annotation<Domain>())
    consteval auto annotated_composite_indexes()
    {
        static constexpr auto ci_anns = std::define_static_array(std::meta::annotations_of_with_type(^^Domain, ^^composite_index));
        std::array<CompositeIndexMeta, ci_anns.size()> result{};
        for (std::size_t i = 0; i < ci_anns.size(); ++i)
        {
            const auto ci = std::meta::extract<composite_index>(ci_anns[i]);
            const std::string_view n{ ci.name };
            const std::string_view cols{ ci.columns_csv };
            if (n.empty() || n.find('"') != std::string_view::npos || cols.empty() || cols.find('"') != std::string_view::npos)
            {
                throw std::invalid_argument(
                    "[[=DAL::Schema::composite_index]] name and columns must be non-empty and contain no double quotes");
            }
            result[i] = CompositeIndexMeta{ .name = ci.name, .columns_csv = ci.columns_csv, .unique = ci.unique };
        }
        return result;
    }

    template<typename Domain>
        requires(has_table_annotation<Domain>())
    consteval void synthesize_annotated_dto_fields(std::meta::info target_fields_struct)
    {
        static constexpr auto& dom_members = unchecked_members_v<Domain>;
        constexpr auto pk_flags = annotated_pk_flags<Domain>();
        std::size_t total_pk_count = 0;
        for (bool is_pk : pk_flags)
        {
            if (is_pk)
            {
                ++total_pk_count;
            }
        }

        std::vector<std::meta::info> specs;
        specs.reserve(dom_members.size());
        std::vector<std::string> seen_col_names;
        seen_col_names.reserve(dom_members.size());

        template for (constexpr auto m : dom_members)
        {
            using RawDomFieldT = [:std::meta::remove_cvref(std::meta::type_of(m)):];
            using StorageFieldT = detail::dto_storage_type_t<RawDomFieldT>;
            constexpr auto field_type = ^^StorageFieldT;
            std::meta::info target_type = field_type;

            constexpr auto pk_anns = std::define_static_array(std::meta::annotations_of_with_type(m, ^^primary_key));
            constexpr auto uniq_anns = std::define_static_array(std::meta::annotations_of_with_type(m, ^^unique));
            static constexpr auto all_anns = std::define_static_array(std::meta::annotations_of(m));

            if constexpr (pk_anns.size() > 1)
            {
                throw std::invalid_argument("Field cannot have multiple [[=DAL::Schema::primary_key]] annotations");
            }
            if constexpr (uniq_anns.size() > 1)
            {
                throw std::invalid_argument("Field cannot have multiple [[=DAL::Schema::unique]] annotations");
            }

            std::meta::info fk_target_type{};
            std::size_t fk_count = 0;
            template for (constexpr auto ann : all_anns)
            {
                constexpr auto ann_type = std::meta::type_of(ann);
                if constexpr (std::meta::has_template_arguments(ann_type) && std::meta::template_of(ann_type) == ^^references)
                {
                    ++fk_count;
                    using AnnT = [:ann_type:];
                    using ParentDomain = [:std::meta::template_arguments_of(ann_type)[0]:];
                    constexpr auto ref_lit = to_string_literal<[]() consteval
                                                               {
                                                                   std::string_view sv{ std::meta::extract<AnnT>(ann).col };
                                                                   if (sv.empty() || sv.find('"') != std::string_view::npos)
                                                                   {
                                                                       throw std::invalid_argument(
                                                                           "[[=DAL::Schema::references]] column must be non-empty and "
                                                                           "contain no double quotes");
                                                                   }
                                                                   return sv;
                                                               }>();
                    if constexpr (detail::is_optional_v<StorageFieldT>)
                    {
                        using InnerStorageT = typename StorageFieldT::value_type;
                        fk_target_type = ^^std::optional<sqlgen::ForeignKey<InnerStorageT, SchemaOf_t<ParentDomain>, ref_lit>>;
                    }
                    else
                    {
                        fk_target_type = ^^sqlgen::ForeignKey<StorageFieldT, SchemaOf_t<ParentDomain>, ref_lit>;
                    }
                }
            }
            if (fk_count > 1)
            {
                throw std::invalid_argument("Field cannot have multiple [[=DAL::Schema::references<...>]] annotations");
            }
            const bool has_fk = (fk_count == 1);

            if constexpr (!pk_anns.empty())
            {
                constexpr auto pk = std::meta::extract<primary_key>(pk_anns[0]);
                if constexpr (pk.auto_increment)
                {
                    if (has_fk)
                    {
                        throw std::invalid_argument(
                            "Conflicting annotations: auto-increment [[=DAL::Schema::primary_key{true}]] cannot be combined with "
                            "[[=DAL::Schema::references<...>]]");
                    }
                    if (total_pk_count >= 2)
                    {
                        throw std::invalid_argument(
                            "Conflicting annotations: auto-increment [[=DAL::Schema::primary_key{true}]] cannot be used in a composite "
                            "primary key");
                    }
                    target_type = std::meta::substitute(
                        ^^AnnotatedPkAuto,
                        {
                            field_type });
                }
                else if (has_fk)
                {
                    // Compose manual PK + FK: keep sqlgen::ForeignKey for FK metadata/DDL while
                    // AnnotatedDto::pk_field_flags records primary key membership for table/column DDL.
                    target_type = fk_target_type;
                }
                else
                {
                    target_type = std::meta::substitute(
                        ^^AnnotatedPkManual,
                        {
                            field_type });
                }
            }
            else if (has_fk)
            {
                // Compose FK + optional [[=unique{}]]: keep sqlgen::ForeignKey while
                // AnnotatedDto::unique_field_flags propagates UNIQUE into DDL.
                target_type = fk_target_type;
            }
            else if constexpr (!uniq_anns.empty())
            {
                target_type = std::meta::substitute(
                    ^^sqlgen::Unique,
                    {
                        field_type });
            }

            std::string_view col_name = std::meta::identifier_of(m);
            constexpr auto col_anns = std::define_static_array(std::meta::annotations_of_with_type(m, ^^column));
            if constexpr (col_anns.size() > 1)
            {
                throw std::invalid_argument("Field cannot have multiple [[=DAL::Schema::column]] annotations");
            }
            if constexpr (!col_anns.empty())
            {
                col_name = std::string_view{ std::meta::extract<column>(col_anns[0]).name };
                if (col_name.empty() || col_name.find('"') != std::string_view::npos)
                {
                    throw std::invalid_argument("[[=DAL::Schema::column]] name must be non-empty and contain no double quotes");
                }
            }
            else
            {
                if (col_name.size() > 2 && (col_name.starts_with("m_") || col_name.starts_with("M_")))
                {
                    col_name.remove_prefix(2);
                }
                if (col_name.ends_with("_") && col_name.size() > 1)
                {
                    col_name.remove_suffix(1);
                }
            }

            std::string safe_col_name;
            safe_col_name.reserve(col_name.size());
            std::ranges::copy(col_name, std::back_inserter(safe_col_name));

            for (const auto& prev : seen_col_names)
            {
                if (prev == safe_col_name)
                {
                    throw std::invalid_argument("Annotated entity has duplicate synthesized column name");
                }
            }
            seen_col_names.push_back(safe_col_name);

            specs.push_back(std::meta::data_member_spec(target_type, { .name = std::move(safe_col_name) }));
        }

        std::meta::define_aggregate(target_fields_struct, specs);
    }

    template<typename Domain>
        requires(has_table_annotation<Domain>())
    struct AnnotatedDtoHolder
    {
        struct Fields;
        consteval
        {
            synthesize_annotated_dto_fields<Domain>(^^Fields);
        }
    };

    template<typename Domain>
        requires(has_table_annotation<Domain>())
    struct AnnotatedDto : public AnnotatedDtoHolder<Domain>::Fields
    {
        using source_domain_type = Domain;
        static constexpr auto tablename = std::define_static_string(annotated_table_name<Domain>());
        static constexpr bool composite_primary_key = has_explicit_composite_key<Domain>();
        static constexpr auto pk_field_flags = annotated_pk_flags<Domain>();
        static constexpr auto unique_field_flags = annotated_unique_flags<Domain>();
        static constexpr auto field_ddl_meta = annotated_field_ddl_meta<Domain>();
        static constexpr auto composite_indexes = annotated_composite_indexes<Domain>();
    };

    template<typename Domain>
    concept HasSchema = RelationalEntity<SchemaOf_t<Domain>>;

    // =========================================================================
    // Phase 5: Compile-Time Reflection Bridge to sqlgen::Col & WHERE ASTs
    // =========================================================================

    namespace detail
    {
        template<typename RawT>
        struct ReflectionScalarOf
        {
            using CleanT = std::remove_cvref_t<RawT>;
            using type = CleanT;
        };

        template<typename RawT>
            requires requires { typename std::remove_cvref_t<RawT>::ReflectionType; }
        struct ReflectionScalarOf<RawT>
        {
            using type = typename std::remove_cvref_t<RawT>::ReflectionType;
        };

        template<typename InnerT>
        struct ReflectionScalarOf<std::optional<InnerT>>
        {
            using type = typename ReflectionScalarOf<InnerT>::type;
        };

        template<std::meta::info Member>
        struct member_reflection_value_type
        {
            using raw_type = [:std::meta::type_of(Member):];
            using type = typename ReflectionScalarOf<raw_type>::type;
        };
    } // namespace detail

    // Unwraps sqlgen::PrimaryKey / sqlgen::ForeignKey (including std::optional<ForeignKey>) to their underlying value,
    // preserving std::optional<T> and plain fields unchanged.
    template<typename T>
    [[nodiscard]] constexpr auto unwrap_field(const T& val)
    {
        using CleanT = std::remove_cvref_t<T>;
        if constexpr (detail::is_optional_v<CleanT>)
        {
            using InnerT = typename CleanT::value_type;
            if constexpr (requires(const InnerT& x) { x.value(); } && !detail::is_optional_v<InnerT>)
            {
                using ValT = std::remove_cvref_t<decltype(val->value())>;
                if (!val.has_value())
                {
                    return std::optional<ValT>{ std::nullopt };
                }
                return std::optional<ValT>{ val->value() };
            }
            else
            {
                return val;
            }
        }
        else if constexpr (requires { val.value(); })
        {
            return val.value();
        }
        else
        {
            return val;
        }
    }

    template<std::meta::info Member>
    consteval auto col_literal_of()
    {
        return to_string_literal<[]() consteval
                                 {
                                     return std::meta::identifier_of(Member);
                                 }>();
    }

    template<std::meta::info Member>
    inline constexpr auto col_of = sqlgen::Col<col_literal_of<Member>(), "", false>{};

    template<typename DTO>
    consteval std::size_t total_member_count()
    {
        return members_v<DTO>.size();
    }

    template<typename DTO>
    consteval std::size_t key_member_count()
    {
        constexpr std::size_t pk_count = primary_key_count<DTO>();
        return pk_count > 0 ? pk_count : foreign_key_count<DTO>();
    }

    template<typename DTO>
        requires SinglePrimaryKey<DTO>
    consteval std::meta::info primary_key_member()
    {
        constexpr std::size_t N = members_v<DTO>.size();
        std::meta::info found{};
        bool has_found = false;
        [&]<std::size_t... Is>(std::index_sequence<Is...>) consteval
        {
            (
                [&]() consteval
                {
                    if constexpr (is_dto_primary_key_at<DTO, Is>())
                    {
                        found = members_v<DTO>[Is];
                        has_found = true;
                    }
                }(),
                ...);
        }(std::make_index_sequence<N>{});
        if (!has_found)
        {
            throw std::invalid_argument("primary_key_member: no PrimaryKey field found");
        }
        return found;
    }

    template<typename DTO>
        requires RelationalEntity<DTO>
    consteval auto key_members_of()
    {
        constexpr std::size_t k_count = key_member_count<DTO>();
        constexpr bool use_pk = (primary_key_count<DTO>() > 0);
        constexpr std::size_t N = members_v<DTO>.size();
        std::array<std::meta::info, k_count> result{};
        std::size_t idx = 0;

        [&]<std::size_t... Is>(std::index_sequence<Is...>) consteval
        {
            (
                [&]() consteval
                {
                    constexpr auto m = members_v<DTO>[Is];
                    using RawFieldType = [:std::meta::type_of(m):];
                    using FieldType = std::remove_cvref_t<RawFieldType>;
                    if constexpr (use_pk ? is_dto_primary_key_at<DTO, Is>() : SqlgenForeignKeyField<FieldType>)
                    {
                        result[idx++] = m;
                    }
                }(),
                ...);
        }(std::make_index_sequence<N>{});
        return result;
    }

    template<typename DTO>
        requires RelationalEntity<DTO>
    consteval auto non_key_members_of()
    {
        constexpr std::size_t nk_count = total_member_count<DTO>() - key_member_count<DTO>();
        constexpr bool use_pk = (primary_key_count<DTO>() > 0);
        constexpr std::size_t N = members_v<DTO>.size();
        std::array<std::meta::info, nk_count> result{};
        std::size_t idx = 0;

        [&]<std::size_t... Is>(std::index_sequence<Is...>) consteval
        {
            (
                [&]() consteval
                {
                    constexpr auto m = members_v<DTO>[Is];
                    using RawFieldType = [:std::meta::type_of(m):];
                    using FieldType = std::remove_cvref_t<RawFieldType>;
                    if constexpr (!(use_pk ? is_dto_primary_key_at<DTO, Is>() : SqlgenForeignKeyField<FieldType>))
                    {
                        result[idx++] = m;
                    }
                }(),
                ...);
        }(std::make_index_sequence<N>{});
        return result;
    }

    template<typename DTO, typename IdType>
        requires SinglePrimaryKey<DTO>
    [[nodiscard]] constexpr auto make_pk_where(const IdType& id)
    {
        constexpr auto pk_m = primary_key_member<DTO>();
        using ValueType = typename detail::member_reflection_value_type<pk_m>::type;
        return col_of<pk_m> == static_cast<ValueType>(id);
    }

    template<typename DTO, typename... Parts>
        requires RelationalEntity<DTO> && (sizeof...(Parts) == key_member_count<DTO>()) && (sizeof...(Parts) >= 2)
    [[nodiscard]] constexpr auto make_composite_where(const Parts&... parts)
    {
        static constexpr auto km = key_members_of<DTO>();
        auto tuple_parts = std::forward_as_tuple(parts...);
        return [&]<std::size_t... Is>(std::index_sequence<Is...>)
        {
            return (
                (col_of<km[Is]> == static_cast<typename detail::member_reflection_value_type<km[Is]>::type>(std::get<Is>(tuple_parts))) &&
                ...);
        }(std::make_index_sequence<sizeof...(Parts)>{});
    }

    template<typename DTO>
        requires RelationalEntity<DTO>
    [[nodiscard]] constexpr auto make_dto_key_where(const DTO& item)
    {
        static constexpr auto km = key_members_of<DTO>();
        return [&]<std::size_t... Is>(std::index_sequence<Is...>)
        {
            return ((col_of<km[Is]> == unwrap_field(item.[:km[Is]:])) && ...);
        }(std::make_index_sequence<km.size()>{});
    }

    // =========================================================================
    // Phase 8: Auto-Inferred Key Trait for Zero-DTO POCO Mode
    // =========================================================================

    namespace detail
    {
        template<typename... KeyTypes>
        consteval auto wrap_key_types()
        {
            if constexpr (sizeof...(KeyTypes) == 1)
            {
                return std::type_identity<Core::SingleKey<KeyTypes...[0]>>{};
            }
            else
            {
                return std::type_identity<Core::CompositeKey<KeyTypes...>>{};
            }
        }

        template<typename DTO, std::size_t... Is>
        consteval auto infer_key_type_helper(std::index_sequence<Is...>)
        {
            static constexpr auto km = key_members_of<DTO>();
            return wrap_key_types<typename member_reflection_value_type<km[Is]>::type...>();
        }
    } // namespace detail

    // Synthesizes Core::SingleKey<V> or Core::CompositeKey<V1, V2, ...> from DTO key fields
    template<typename DTO>
        requires RelationalEntity<DTO>
    using InferredKey_t = typename decltype(detail::infer_key_type_helper<DTO>(std::make_index_sequence<key_member_count<DTO>()>{}))::type;

    // Resolves Domain::Key when explicitly defined, or falls back to InferredKey_t<DTO>
    template<typename Domain, typename DTO = SchemaOf_t<Domain>>
        requires RelationalEntity<DTO>
    struct EntityKey
    {
        using type = InferredKey_t<DTO>;
    };

    template<typename Domain, typename DTO>
        requires RelationalEntity<DTO> && requires { typename Domain::Key; } && Core::Key<typename Domain::Key>
    struct EntityKey<Domain, DTO>
    {
        using type = typename Domain::Key;
    };

    template<typename Domain, typename DTO = SchemaOf_t<Domain>>
        requires RelationalEntity<DTO>
    using EntityKey_t = typename EntityKey<Domain, DTO>::type;

    // =========================================================================
    // Phase 9: Compile-Time Topological Foreign-Key Sorting
    // =========================================================================

    template<typename DTO>
    consteval auto foreign_table_types_of()
    {
        std::array<std::meta::info, foreign_key_count<DTO>()> targets{};
        std::size_t idx = 0;

        template for (constexpr auto m : members_v<DTO>)
        {
            using RawFieldType = [:std::meta::type_of(m):];
            using FieldType = std::remove_cvref_t<RawFieldType>;
            if constexpr (AnyForeignKeyField<FieldType>)
            {
                using FkT = unwrap_fk_type_t<FieldType>;
                targets[idx++] = std::meta::dealias(^^typename FkT::ForeignTableType);
            }
        }
        return targets;
    }

    // Computes a topological ordering (parents before children) from a dependency matrix
    // where depends_on[child][parent] == true. Preserves declaration order for ties.
    template<std::size_t N>
    consteval std::array<std::size_t, N> kahn_topological_sort(const std::array<std::array<bool, N>, N>& depends_on)
    {
        std::array<std::size_t, N> in_degree{};
        for (std::size_t c = 0; c < N; ++c)
        {
            for (std::size_t p = 0; p < N; ++p)
            {
                if (depends_on[c][p])
                {
                    ++in_degree[c];
                }
            }
        }

        std::array<std::size_t, N> order{};
        std::array<bool, N> visited{};
        std::size_t out_idx = 0;

        for (std::size_t step = 0; step < N; ++step)
        {
            bool found = false;
            for (std::size_t i = 0; i < N; ++i)
            {
                if (!visited[i] && in_degree[i] == 0)
                {
                    visited[i] = true;
                    order[out_idx++] = i;
                    for (std::size_t c = 0; c < N; ++c)
                    {
                        if (depends_on[c][i])
                        {
                            --in_degree[c];
                        }
                    }
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                throw std::invalid_argument("Cyclic foreign-key dependency detected among DTOs");
            }
        }

        return order;
    }

    template<std::size_t N>
    consteval std::array<std::size_t, N> reverse_index_array(const std::array<std::size_t, N>& arr)
    {
        std::array<std::size_t, N> rev{};
        for (std::size_t i = 0; i < N; ++i)
        {
            rev[i] = arr[N - 1 - i];
        }
        return rev;
    }

    template<typename... DTOs>
    consteval std::array<std::size_t, sizeof...(DTOs)> topological_dto_order()
    {
        constexpr std::size_t N = sizeof...(DTOs);
        constexpr std::array<std::meta::info, N> types{ std::meta::dealias(^^DTOs)... };
        std::array<std::array<bool, N>, N> depends_on{};

        std::size_t child_idx = 0;
        ((
             [&]()
             {
                 constexpr auto targets = foreign_table_types_of<DTOs>();
                 for (std::meta::info target : targets)
                 {
                     for (std::size_t parent_idx = 0; parent_idx < N; ++parent_idx)
                     {
                         if (parent_idx != child_idx && types[parent_idx] == target)
                         {
                             depends_on[child_idx][parent_idx] = true;
                         }
                     }
                 }
                 ++child_idx;
             }()),
         ...);

        return kahn_topological_sort<N>(depends_on);
    }

    // =========================================================================
    // Phase 11: Native C++26 Reflection DTO Equality (Zero-JSON)
    // =========================================================================

    namespace detail
    {
        template<typename T>
        [[nodiscard]] constexpr bool field_values_equal(const T& lhs, const T& rhs)
        {
            using CleanT = std::remove_cvref_t<T>;
            if constexpr (is_optional_v<CleanT>)
            {
                if (lhs.has_value() != rhs.has_value())
                {
                    return false;
                }
                if (!lhs.has_value())
                {
                    return true;
                }
                return field_values_equal(*lhs, *rhs);
            }
            else if constexpr (std::floating_point<CleanT>)
            {
                if (std::isnan(lhs) && std::isnan(rhs))
                {
                    return true;
                }
                return lhs == rhs;
            }
            else
            {
                return lhs == rhs;
            }
        }
    } // namespace detail

    template<typename DTO>
    [[nodiscard]] constexpr bool dto_equals(const DTO& a, const DTO& b)
    {
        template for (constexpr auto m : members_v<DTO>)
        {
            if (!detail::field_values_equal(unwrap_field(a.[:m:]), unwrap_field(b.[:m:])))
            {
                return false;
            }
        }
        return true;
    }

    // =========================================================================
    // Phase 14: Compile-Time Foreign-Key Introspection Helpers
    // =========================================================================

    template<typename ChildDTO, typename ParentDTO>
    consteval std::size_t count_fk_members_referencing()
    {
        constexpr auto parent_target = std::meta::dealias(^^ParentDTO);
        std::size_t count = 0;
        template for (constexpr auto m : members_v<ChildDTO>)
        {
            using RawFieldT = [:std::meta::type_of(m):];
            using FieldT = std::remove_cvref_t<RawFieldT>;
            if constexpr (AnyForeignKeyField<FieldT>)
            {
                using TargetT = typename unwrap_fk_type_t<FieldT>::ForeignTableType;
                if (std::meta::dealias(^^TargetT) == parent_target)
                {
                    ++count;
                }
            }
        }
        return count;
    }

    template<typename ChildDTO, typename ParentDTO>
        requires(count_fk_members_referencing<ChildDTO, ParentDTO>() == 1)
    consteval std::meta::info find_fk_member_referencing()
    {
        constexpr auto parent_target = std::meta::dealias(^^ParentDTO);
        std::meta::info result{};
        template for (constexpr auto m : members_v<ChildDTO>)
        {
            using RawFieldT = [:std::meta::type_of(m):];
            using FieldT = std::remove_cvref_t<RawFieldT>;
            if constexpr (AnyForeignKeyField<FieldT>)
            {
                using TargetT = typename unwrap_fk_type_t<FieldT>::ForeignTableType;
                if (std::meta::dealias(^^TargetT) == parent_target)
                {
                    result = m;
                }
            }
        }
        return result;
    }

    namespace detail
    {
        template<typename>
        struct ForeignKeyColumnName;

        template<rfl::internal::StringLiteral ColName>
        struct ForeignKeyColumnName<rfl::Literal<ColName>>
        {
            static constexpr std::string_view value = ColName.string_view();
        };
    } // namespace detail

    template<typename ChildDTO, typename ParentDTO>
        requires(count_fk_members_referencing<ChildDTO, ParentDTO>() == 1)
    consteval std::meta::info referenced_parent_member_of()
    {
        constexpr auto fk_m = find_fk_member_referencing<ChildDTO, ParentDTO>();
        using RawFieldT = [:std::meta::type_of(fk_m):];
        using FkT = unwrap_fk_type_t<RawFieldT>;
        constexpr std::string_view target_col = detail::ForeignKeyColumnName<typename FkT::ColumnType>::value;
        std::meta::info found{};
        bool has_found = false;
        template for (constexpr auto m : members_v<ParentDTO>)
        {
            if (std::meta::identifier_of(m) == target_col)
            {
                found = m;
                has_found = true;
            }
        }
        if (!has_found)
        {
            throw std::invalid_argument("referenced_parent_member_of: referenced column not found on ParentDTO");
        }
        return found;
    }

    template<typename ChildDTO, typename ParentDTO>
        requires(count_fk_members_referencing<ChildDTO, ParentDTO>() == 1)
    [[nodiscard]] constexpr auto extract_referenced_parent_value(const ParentDTO& parent_dto)
    {
        constexpr auto target_m = referenced_parent_member_of<ChildDTO, ParentDTO>();
        return unwrap_field(parent_dto.[:target_m:]);
    }

    template<typename ChildDTO, typename ParentDTO, typename KeyValue>
        requires(count_fk_members_referencing<ChildDTO, ParentDTO>() == 1)
    [[nodiscard]] auto make_fk_where(const KeyValue& parent_pk_val)
    {
        constexpr auto fk_m = find_fk_member_referencing<ChildDTO, ParentDTO>();
        using ValueType = typename detail::member_reflection_value_type<fk_m>::type;
        return col_of<fk_m> == static_cast<ValueType>(parent_pk_val);
    }

    template<typename ChildDTO, typename ParentDTO>
        requires(count_fk_members_referencing<ChildDTO, ParentDTO>() == 1)
    [[nodiscard]] constexpr auto extract_fk_value(const ChildDTO& child_dto)
    {
        constexpr auto fk_m = find_fk_member_referencing<ChildDTO, ParentDTO>();
        return unwrap_field(child_dto.[:fk_m:]);
    }

    template<typename DTO>
    consteval bool has_auto_incr_primary_key()
    {
        bool found = false;
        template for (constexpr auto m : members_v<DTO>)
        {
            using RawFieldT = [:std::meta::type_of(m):];
            using FieldT = std::remove_cvref_t<RawFieldT>;
            if constexpr (requires { FieldT::auto_incr; })
            {
                if constexpr (FieldT::auto_incr)
                {
                    found = true;
                }
            }
        }
        return found;
    }

    template<typename DTO>
    consteval bool has_version_column()
    {
        if constexpr (requires { DTO::field_ddl_meta; })
        {
            for (const auto& fm : DTO::field_ddl_meta)
            {
                if (fm.is_version)
                {
                    return true;
                }
            }
        }
        return false;
    }

    template<typename DTO>
        requires(has_version_column<DTO>())
    consteval std::size_t version_member_index()
    {
        for (std::size_t i = 0; i < DTO::field_ddl_meta.size(); ++i)
        {
            if (DTO::field_ddl_meta[i].is_version)
            {
                return i;
            }
        }
        throw std::invalid_argument("version_member_index: no version column found");
    }

    template<typename DTO>
        requires(has_version_column<DTO>())
    consteval std::meta::info version_member()
    {
        return members_v<DTO>[version_member_index<DTO>()];
    }

} // namespace DAL::Schema

#ifndef REFLECTCPP_USE_CPP26_REFLECTION
namespace rfl::internal::cpp20
{
    template<typename Domain>
        requires(DAL::Schema::has_table_annotation<Domain>())
    struct CountFieldsHelper<DAL::Schema::AnnotatedDto<Domain>>
    {
        static consteval std::size_t count_fields()
        {
            return DAL::Schema::members_v<typename DAL::Schema::AnnotatedDtoHolder<Domain>::Fields>.size();
        }
    };

    template<typename Domain>
        requires(DAL::Schema::has_table_annotation<Domain>())
    struct CountFieldsHelper<const DAL::Schema::AnnotatedDto<Domain>> : CountFieldsHelper<DAL::Schema::AnnotatedDto<Domain>>
    {
    };
} // namespace rfl::internal::cpp20
#endif
