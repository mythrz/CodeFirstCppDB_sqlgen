module;
#include <meta>
#include <rfl/internal/num_fields.hpp>
#include <sqlgen/ForeignKey.hpp>
#include <sqlgen/PrimaryKey.hpp>
#include <sqlgen/Unique.hpp>
#include <sqlgen/col.hpp>

export module dal:schema_traits;

import std;
import core;

export namespace DAL::Schema
{
    // P3394 C++26 Annotation tags for optional single-class entities (Phase 42/45)
    struct table
    {
        const char* name;
    };
    struct column
    {
        const char* name;
    };
    struct primary_key
    {
        bool auto_increment = false;
    };
    struct unique
    {
    };
    template<typename Parent>
    struct references
    {
        const char* col = "id";
    };

    // Collects all accessible nonstatic data members (including public unambiguous base classes,
    // enabling P3394 define_aggregate base structs to mirror flat aggregates).
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
            result.push_back(m);
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

    template<typename T>
    consteval std::size_t primary_key_count()
    {
        std::size_t count = 0;
        template for (constexpr auto m : members_v<T>)
        {
            using RawFieldType = [:std::meta::type_of(m):];
            using FieldType = std::remove_cvref_t<RawFieldType>;
            if constexpr (SqlgenPrimaryKeyField<FieldType>)
            {
                ++count;
            }
        }
        return count;
    }

    template<typename T>
    consteval std::size_t foreign_key_count()
    {
        std::size_t count = 0;
        template for (constexpr auto m : members_v<T>)
        {
            using RawFieldType = [:std::meta::type_of(m):];
            using FieldType = std::remove_cvref_t<RawFieldType>;
            if constexpr (SqlgenForeignKeyField<FieldType>)
            {
                ++count;
            }
        }
        return count;
    }

    // Entities with a single primary key (e.g. PersonDTO, SomethingDTO)
    template<typename T>
    concept SinglePrimaryKey = HasTableName<T> && (primary_key_count<T>() == 1);

    // Entities with composite keys (e.g. junction tables like Person_SomethingDTO,
    // or multi-column PrimaryKey DTOs)
    template<typename T>
    concept CompositePrimaryKey = HasTableName<T> && !SinglePrimaryKey<T> &&
        ((primary_key_count<T>() >= 2) || (primary_key_count<T>() == 0 && foreign_key_count<T>() >= 2));

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
        return std::meta::extract<table>(anns[0]).name;
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

    template<typename Domain>
        requires(has_table_annotation<Domain>())
    consteval void synthesize_annotated_dto_fields(std::meta::info target_fields_struct)
    {
        static constexpr auto& dom_members = unchecked_members_v<Domain>;
        std::vector<std::meta::info> specs;
        specs.reserve(dom_members.size());

        template for (constexpr auto m : dom_members)
        {
            constexpr auto field_type = std::meta::remove_cvref(std::meta::type_of(m));
            std::meta::info target_type = field_type;

            constexpr auto pk_anns = std::define_static_array(std::meta::annotations_of_with_type(m, ^^primary_key));
            constexpr auto uniq_anns = std::define_static_array(std::meta::annotations_of_with_type(m, ^^unique));
            static constexpr auto all_anns = std::define_static_array(std::meta::annotations_of(m));

            std::meta::info fk_target_type{};
            bool has_fk = false;
            template for (constexpr auto ann : all_anns)
            {
                constexpr auto ann_type = std::meta::type_of(ann);
                if constexpr (std::meta::has_template_arguments(ann_type) && std::meta::template_of(ann_type) == ^^references)
                {
                    using AnnT = [:ann_type:];
                    constexpr auto ref_val = std::meta::extract<AnnT>(ann);
                    constexpr std::string_view ref_col = ref_val.col;
                    using ParentDomain = [:std::meta::template_arguments_of(ann_type)[0]:];
                    constexpr auto ref_lit = [&]<std::size_t... Is>(std::index_sequence<Is...>) consteval
                    {
                        return rfl::internal::StringLiteral<ref_col.size() + 1>{ std::array<char, ref_col.size() + 1>{ ref_col[Is]...,
                                                                                                                       '\0' } };
                    }(std::make_index_sequence<ref_col.size()>{});
                    using FieldT = [:field_type:];
                    fk_target_type = ^^sqlgen::ForeignKey<FieldT, SchemaOf_t<ParentDomain>, ref_lit>;
                    has_fk = true;
                }
            }

            if constexpr (!pk_anns.empty())
            {
                constexpr auto pk = std::meta::extract<primary_key>(pk_anns[0]);
                if constexpr (pk.auto_increment)
                {
                    target_type = std::meta::substitute(
                        ^^AnnotatedPkAuto,
                        {
                            field_type });
                }
                else if (has_fk)
                {
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
            if constexpr (!col_anns.empty())
            {
                col_name = std::meta::extract<column>(col_anns[0]).name;
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
        static constexpr auto tablename = std::define_static_string(annotated_table_name<Domain>());
    };

    template<typename Domain>
    concept HasSchema = RelationalEntity<SchemaOf_t<Domain>>;

    // =========================================================================
    // Phase 5: Compile-Time Reflection Bridge to sqlgen::Col & WHERE ASTs
    // =========================================================================

    namespace detail
    {
        template<typename>
        inline constexpr bool is_optional_v = false;
        template<typename T>
        inline constexpr bool is_optional_v<std::optional<T>> = true;

        template<std::meta::info Member>
        struct member_reflection_value_type
        {
            using raw_type = [:std::meta::type_of(Member):];
            using type = typename std::remove_cvref_t<raw_type>::ReflectionType;
        };
    } // namespace detail

    // Unwraps sqlgen::PrimaryKey / sqlgen::ForeignKey to their underlying value,
    // preserving std::optional<T> and plain fields unchanged.
    template<typename T>
    [[nodiscard]] constexpr auto unwrap_field(const T& val)
    {
        if constexpr (detail::is_optional_v<std::remove_cvref_t<T>>)
        {
            return val;
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
        constexpr std::string_view name = std::meta::identifier_of(Member);
        return [&]<std::size_t... Is>(std::index_sequence<Is...>) consteval
        {
            return rfl::internal::StringLiteral<name.size() + 1>{ std::array<char, name.size() + 1>{ name[Is]..., '\0' } };
        }(std::make_index_sequence<name.size()>{});
    }

    template<std::meta::info Member>
    inline constexpr auto col_of = sqlgen::Col<col_literal_of<Member>()>{};

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
        template for (constexpr auto m : members_v<DTO>)
        {
            using RawFieldType = [:std::meta::type_of(m):];
            using FieldType = std::remove_cvref_t<RawFieldType>;
            if constexpr (SqlgenPrimaryKeyField<FieldType>)
            {
                return m;
            }
        }
        throw std::invalid_argument("primary_key_member: no PrimaryKey field found");
    }

    template<typename DTO>
        requires RelationalEntity<DTO>
    consteval auto key_members_of()
    {
        constexpr std::size_t k_count = key_member_count<DTO>();
        constexpr bool use_pk = (primary_key_count<DTO>() > 0);
        std::array<std::meta::info, k_count> result{};
        std::size_t idx = 0;

        template for (constexpr auto m : members_v<DTO>)
        {
            using RawFieldType = [:std::meta::type_of(m):];
            using FieldType = std::remove_cvref_t<RawFieldType>;
            if constexpr (use_pk ? SqlgenPrimaryKeyField<FieldType> : SqlgenForeignKeyField<FieldType>)
            {
                result[idx++] = m;
            }
        }
        return result;
    }

    template<typename DTO>
        requires RelationalEntity<DTO>
    consteval auto non_key_members_of()
    {
        constexpr std::size_t nk_count = total_member_count<DTO>() - key_member_count<DTO>();
        constexpr bool use_pk = (primary_key_count<DTO>() > 0);
        std::array<std::meta::info, nk_count> result{};
        std::size_t idx = 0;

        template for (constexpr auto m : members_v<DTO>)
        {
            using RawFieldType = [:std::meta::type_of(m):];
            using FieldType = std::remove_cvref_t<RawFieldType>;
            if constexpr (!(use_pk ? SqlgenPrimaryKeyField<FieldType> : SqlgenForeignKeyField<FieldType>))
            {
                result[idx++] = m;
            }
        }
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
            if constexpr (SqlgenForeignKeyField<FieldType>)
            {
                targets[idx++] = std::meta::dealias(^^typename FieldType::ForeignTableType);
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

    template<typename DTO>
    [[nodiscard]] constexpr bool dto_equals(const DTO& a, const DTO& b)
    {
        template for (constexpr auto m : members_v<DTO>)
        {
            if (unwrap_field(a.[:m:]) != unwrap_field(b.[:m:]))
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
            if constexpr (SqlgenForeignKeyField<FieldT>)
            {
                using TargetT = typename FieldT::ForeignTableType;
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
            if constexpr (SqlgenForeignKeyField<FieldT>)
            {
                using TargetT = typename FieldT::ForeignTableType;
                if (std::meta::dealias(^^TargetT) == parent_target)
                {
                    result = m;
                }
            }
        }
        return result;
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

} // namespace DAL::Schema

namespace rfl::internal
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
} // namespace rfl::internal
