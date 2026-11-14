module;
#include <meta>
#include <sqlgen/PrimaryKey.hpp>
#include <sqlgen/ForeignKey.hpp>
#include <sqlgen/col.hpp>

export module dal:schema_traits;

import std;
import core;

export namespace DAL::Schema
{
    template<typename T>
    concept HasTableName = requires {
        { T::tablename } -> std::convertible_to<std::string_view>;
    };

    // Detects sqlgen::PrimaryKey<V, auto_incr> fields via static reflection
    template<typename F>
    concept SqlgenPrimaryKeyField = requires(const F& f) {
        typename F::ReflectionType;
        { F::auto_incr } -> std::convertible_to<bool>;
        { f.value() };
    };

    // Detects sqlgen::ForeignKey<V, Table, Col> fields via static reflection
    template<typename F>
    concept SqlgenForeignKeyField = requires(const F& f) {
        typename F::ReflectionType;
        typename F::ForeignTableType;
        typename F::ColumnType;
        { f.value() };
    };

    template<typename T>
    consteval std::size_t primary_key_count()
    {
        std::size_t count = 0;
        static constexpr auto members = std::define_static_array(
            std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::current()));
        template for (constexpr auto m : members)
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
        static constexpr auto members = std::define_static_array(
            std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::current()));
        template for (constexpr auto m : members)
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
    concept CompositePrimaryKey = HasTableName<T> && !SinglePrimaryKey<T>
        && ((primary_key_count<T>() >= 2) || (primary_key_count<T>() == 0 && foreign_key_count<T>() >= 2));

    // Generalized relational entity concept accommodating both single and composite key entities
    template<typename T>
    concept RelationalEntity = HasTableName<T> && (SinglePrimaryKey<T> || CompositePrimaryKey<T>);

    // Trait mapping a Domain entity type to its persistence DTO schema type.
    // Defaults to Domain itself (for self-describing DTOs); specialize for Domain/DTO pairs.
    template<typename Domain>
    struct SchemaOf
    {
        using type = Domain;
    };

    template<typename Domain>
    using SchemaOf_t = typename SchemaOf<Domain>::type;

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
        std::array<char, name.size() + 1> buf{};
        for (std::size_t i = 0; i < name.size(); ++i)
        {
            buf[i] = name[i];
        }
        buf[name.size()] = '\0';
        return rfl::internal::StringLiteral<name.size() + 1>{ buf };
    }

    template<std::meta::info Member>
    inline constexpr auto col_of = sqlgen::Col<col_literal_of<Member>()>{};

    template<typename DTO>
    consteval std::size_t total_member_count()
    {
        static constexpr auto members = std::define_static_array(
            std::meta::nonstatic_data_members_of(^^DTO, std::meta::access_context::current()));
        return members.size();
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
        static constexpr auto members = std::define_static_array(
            std::meta::nonstatic_data_members_of(^^DTO, std::meta::access_context::current()));
        template for (constexpr auto m : members)
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
        static constexpr auto members = std::define_static_array(
            std::meta::nonstatic_data_members_of(^^DTO, std::meta::access_context::current()));
        constexpr std::size_t k_count = key_member_count<DTO>();
        constexpr bool use_pk = (primary_key_count<DTO>() > 0);
        std::array<std::meta::info, k_count> result{};
        std::size_t idx = 0;

        template for (constexpr auto m : members)
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
        static constexpr auto members = std::define_static_array(
            std::meta::nonstatic_data_members_of(^^DTO, std::meta::access_context::current()));
        constexpr std::size_t nk_count = total_member_count<DTO>() - key_member_count<DTO>();
        constexpr bool use_pk = (primary_key_count<DTO>() > 0);
        std::array<std::meta::info, nk_count> result{};
        std::size_t idx = 0;

        template for (constexpr auto m : members)
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
        return [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            return ((col_of<km[Is]> ==
                     static_cast<typename detail::member_reflection_value_type<km[Is]>::type>(
                         std::get<Is>(tuple_parts))) && ...);
        }(std::make_index_sequence<sizeof...(Parts)>{});
    }

    template<typename DTO>
        requires RelationalEntity<DTO>
    [[nodiscard]] constexpr auto make_dto_key_where(const DTO& item)
    {
        static constexpr auto km = key_members_of<DTO>();
        return [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            return ((col_of<km[Is]> == unwrap_field(item.[:km[Is]:])) && ...);
        }(std::make_index_sequence<km.size()>{});
    }

    // =========================================================================
    // Phase 8: Auto-Inferred Key Trait for Zero-DTO POCO Mode
    // =========================================================================

    namespace detail
    {
        template<typename DTO, std::size_t... Is>
        consteval auto infer_key_type_helper(std::index_sequence<Is...>)
        {
            static constexpr auto km = key_members_of<DTO>();
            if constexpr (sizeof...(Is) == 1)
            {
                return std::type_identity<
                    Core::SingleKey<typename member_reflection_value_type<km[0]>::type>>{};
            }
            else
            {
                return std::type_identity<
                    Core::CompositeKey<typename member_reflection_value_type<km[Is]>::type...>>{};
            }
        }
    } // namespace detail

    // Synthesizes Core::SingleKey<V> or Core::CompositeKey<V1, V2, ...> from DTO key fields
    template<typename DTO>
        requires RelationalEntity<DTO>
    using InferredKey_t = typename decltype(
        detail::infer_key_type_helper<DTO>(std::make_index_sequence<key_member_count<DTO>()>{}))::type;

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
        static constexpr auto members = std::define_static_array(
            std::meta::nonstatic_data_members_of(^^DTO, std::meta::access_context::current()));
        std::array<std::meta::info, foreign_key_count<DTO>()> targets{};
        std::size_t idx = 0;

        template for (constexpr auto m : members)
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
    consteval std::array<std::size_t, N> kahn_topological_sort(
        const std::array<std::array<bool, N>, N>& depends_on)
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
        (([&]() {
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
        }()), ...);

        return kahn_topological_sort<N>(depends_on);
    }

} // namespace DAL::Schema
