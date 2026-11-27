module;
#include <sqlgen.hpp>

export module codefirst.dal:db_query;

import std;
import codefirst.core;
import :schema_traits;
import :entity_metadata;
import :mappers;
import :generic_repo;
import :sqlite_connection;
import :change_tracker;

export namespace DAL
{
    namespace detail
    {
        template<typename Val>
        [[nodiscard]] constexpr auto normalize_query_val(const Val& val)
        {
            using T = std::remove_cvref_t<Val>;
            if constexpr (Core::SingleKeyType<T>)
            {
                return normalize_query_val(val.value);
            }
            else if constexpr (Core::StrongIdType<T>)
            {
                return val.get();
            }
            else if constexpr (std::is_convertible_v<T, std::string_view> && !std::same_as<T, std::string>)
            {
                return std::string(val);
            }
            else
            {
                return val;
            }
        }

        template<typename Val>
        [[nodiscard]] inline std::optional<Core::DbError> validate_query_val(const Val& val)
        {
            using T = std::remove_cvref_t<Val>;
            if constexpr (Core::SingleKeyType<T>)
            {
                return validate_query_val(val.value);
            }
            else if constexpr (Core::StrongIdType<T>)
            {
                return validate_query_val(val.get());
            }
            else if constexpr (Schema::detail::is_optional_v<T>)
            {
                if (val.has_value())
                {
                    return validate_query_val(*val);
                }
                return std::nullopt;
            }
            else if constexpr (
                std::is_integral_v<T> && std::is_unsigned_v<T> && !std::is_same_v<T, bool> && (sizeof(T) >= sizeof(std::int64_t)))
            {
                if (val > static_cast<T>(std::numeric_limits<std::int64_t>::max()))
                {
                    return Core::DbError::constraint_violation(
                        "Unsigned 64-bit query parameter exceeds SQLite signed 64-bit integer range");
                }
                return std::nullopt;
            }
            else
            {
                return std::nullopt;
            }
        }

        template<typename... Ts>
        [[nodiscard]] inline std::optional<Core::DbError> first_query_error(const std::optional<Core::DbError>& existing, const Ts&... vals)
        {
            if (existing)
            {
                return existing;
            }
            std::optional<Core::DbError> err;
            (
                [&]()
                {
                    if (!err)
                    {
                        err = validate_query_val(vals);
                    }
                }(),
                ...);
            return err;
        }

        template<typename Val>
        [[nodiscard]] inline std::optional<Core::DbError> first_query_vec_error(
            const std::optional<Core::DbError>& existing, const std::vector<Val>& vals)
        {
            if (existing)
            {
                return existing;
            }
            for (const auto& v : vals)
            {
                if (auto err = validate_query_val(v))
                {
                    return err;
                }
            }
            return std::nullopt;
        }

        template<typename T>
        struct UnwrapColumnScalar
        {
            using type = T;
        };

        template<typename T>
        struct UnwrapColumnScalar<std::optional<T>>
        {
            using type = typename UnwrapColumnScalar<T>::type;
        };

        template<typename T>
            requires requires { typename T::ReflectionType; }
        struct UnwrapColumnScalar<T>
        {
            using type = typename UnwrapColumnScalar<typename T::ReflectionType>::type;
        };

        template<typename T>
        struct IsOptionalField : std::false_type
        {
        };

        template<typename T>
        struct IsOptionalField<std::optional<T>> : std::true_type
        {
        };

        template<typename T>
            requires requires { typename T::ReflectionType; }
        struct IsOptionalField<T> : IsOptionalField<typename T::ReflectionType>
        {
        };

        template<typename ScalarT>
        struct SumResultType
        {
            using type = ScalarT;
        };

        template<typename ScalarT>
            requires(std::is_integral_v<ScalarT> && std::is_signed_v<ScalarT> && !std::is_same_v<ScalarT, bool>)
        struct SumResultType<ScalarT>
        {
            using type = std::int64_t;
        };

        template<typename ScalarT>
            requires(std::is_integral_v<ScalarT> && !std::is_signed_v<ScalarT> && !std::is_same_v<ScalarT, bool>)
        struct SumResultType<ScalarT>
        {
            using type = std::uint64_t;
        };

        template<typename ScalarT>
            requires std::is_floating_point_v<ScalarT>
        struct SumResultType<ScalarT>
        {
            using type = double;
        };

        template<typename RawFieldT>
        using ProjectedColumn_t = std::conditional_t<
            IsOptionalField<std::remove_cvref_t<RawFieldT>>::value,
            std::optional<typename UnwrapColumnScalar<std::remove_cvref_t<RawFieldT>>::type>,
            typename UnwrapColumnScalar<std::remove_cvref_t<RawFieldT>>::type>;

        template<typename T>
        [[nodiscard]] auto unwrap_projected_value(T&& val)
        {
            using CleanT = std::remove_cvref_t<T>;
            if constexpr (Schema::detail::is_optional_v<CleanT>)
            {
                using InnerT = typename CleanT::value_type;
                if constexpr (requires { typename InnerT::ReflectionType; } && requires(const InnerT& x) { x.value(); })
                {
                    using UnwrappedInnerT = std::remove_cvref_t<decltype(unwrap_projected_value(std::declval<InnerT>()))>;
                    if (!val.has_value())
                    {
                        return std::optional<UnwrappedInnerT>{ std::nullopt };
                    }
                    return std::optional<UnwrappedInnerT>{ unwrap_projected_value(std::move(*val)) };
                }
                else
                {
                    return std::forward<T>(val);
                }
            }
            else if constexpr (requires { typename CleanT::ReflectionType; } && requires { val.value(); })
            {
                return unwrap_projected_value(val.value());
            }
            else
            {
                return std::forward<T>(val);
            }
        }

        inline constexpr std::size_t kSqliteUnboundedLimit = static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max());

        template<typename T>
        inline constexpr bool is_range_checked_integer_v = std::is_integral_v<std::remove_cvref_t<T>> &&
            !std::is_same_v<std::remove_cvref_t<T>, bool> && !std::is_same_v<std::remove_cvref_t<T>, char> &&
            !std::is_same_v<std::remove_cvref_t<T>, wchar_t> && !std::is_same_v<std::remove_cvref_t<T>, char8_t> &&
            !std::is_same_v<std::remove_cvref_t<T>, char16_t> && !std::is_same_v<std::remove_cvref_t<T>, char32_t>;

        template<typename TargetT, typename SrcT>
        [[nodiscard]] std::expected<TargetT, Core::DbError> try_narrow_key_part(const SrcT& src)
        {
            using CleanTarget = std::remove_cvref_t<TargetT>;
            using CleanSrc = std::remove_cvref_t<SrcT>;

            if constexpr (Core::StrongIdType<CleanTarget>)
            {
                if constexpr (std::same_as<CleanSrc, CleanTarget>)
                {
                    return src;
                }
                else if constexpr (Core::StrongIdType<CleanSrc>)
                {
                    auto inner = try_narrow_key_part<typename CleanTarget::value_type>(src.get());
                    if (!inner)
                    {
                        return std::unexpected<Core::DbError>{ inner.error() };
                    }
                    return CleanTarget{ std::move(*inner) };
                }
                else
                {
                    auto inner = try_narrow_key_part<typename CleanTarget::value_type>(src);
                    if (!inner)
                    {
                        return std::unexpected<Core::DbError>{ inner.error() };
                    }
                    return CleanTarget{ std::move(*inner) };
                }
            }
            else if constexpr (is_range_checked_integer_v<CleanTarget> && is_range_checked_integer_v<CleanSrc>)
            {
                if (!std::in_range<CleanTarget>(src))
                {
                    return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation(
                        "Key value is out of range for entity key type") };
                }
                return static_cast<CleanTarget>(src);
            }
            else if constexpr (is_range_checked_integer_v<CleanTarget> && std::is_floating_point_v<CleanSrc>)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation(
                    "Floating-point value cannot be narrowed to an integral entity key") };
            }
            else
            {
                return static_cast<CleanTarget>(src);
            }
        }

        template<typename KeyType, typename K>
            requires Core::SingleKeyType<KeyType>
        [[nodiscard]] std::expected<KeyType, Core::DbError> try_make_single_key(const K& raw_or_id)
        {
            auto part = try_narrow_key_part<typename KeyType::key_type>(raw_or_id);
            if (!part)
            {
                return std::unexpected<Core::DbError>{ part.error() };
            }
            return KeyType{ std::move(*part) };
        }

        template<typename KeyType, typename... Ks>
            requires Core::CompositeKeyType<KeyType> && (sizeof...(Ks) == KeyType::arity)
        [[nodiscard]] std::expected<KeyType, Core::DbError> try_make_composite_key(const Ks&... parts)
        {
            return [&]<std::size_t... Is>(std::index_sequence<Is...>) -> std::expected<KeyType, Core::DbError>
            {
                auto narrowed = std::make_tuple(try_narrow_key_part<std::tuple_element_t<Is, typename KeyType::tuple_type>>(parts)...);
                std::optional<Core::DbError> first_err;
                (
                    [&]()
                    {
                        if (!first_err && !std::get<Is>(narrowed).has_value())
                        {
                            first_err = std::get<Is>(narrowed).error();
                        }
                    }(),
                    ...);
                if (first_err)
                {
                    return std::unexpected<Core::DbError>{ std::move(*first_err) };
                }
                return KeyType{ std::move(*std::get<Is>(narrowed))... };
            }(std::make_index_sequence<sizeof...(Ks)>{});
        }

        template<typename T>
        struct UnwrapKeyPartScalar
        {
            using type = T;
        };

        template<typename T>
            requires Core::StrongIdType<T>
        struct UnwrapKeyPartScalar<T>
        {
            using type = typename T::value_type;
        };

        template<typename KeyType>
        struct KeyTupleFor;

        template<typename KeyType>
            requires Core::SingleKeyType<KeyType>
        struct KeyTupleFor<KeyType>
        {
            using type = std::tuple<typename UnwrapKeyPartScalar<typename KeyType::key_type>::type>;

            [[nodiscard]] static KeyType from_tuple(const type& t)
            {
                return KeyType{ static_cast<typename KeyType::key_type>(std::get<0>(t)) };
            }
        };

        template<typename KeyType>
            requires Core::CompositeKeyType<KeyType>
        struct KeyTupleFor<KeyType>
        {
            template<typename Seq>
            struct Impl;

            template<std::size_t... Is>
            struct Impl<std::index_sequence<Is...>>
            {
                using type = std::tuple<typename UnwrapKeyPartScalar<std::tuple_element_t<Is, typename KeyType::tuple_type>>::type...>;

                [[nodiscard]] static KeyType from_tuple(const type& t)
                {
                    return KeyType{ static_cast<std::tuple_element_t<Is, typename KeyType::tuple_type>>(std::get<Is>(t))... };
                }
            };

            using Helper = Impl<std::make_index_sequence<KeyType::arity>>;
            using type = typename Helper::type;

            [[nodiscard]] static KeyType from_tuple(const type& t)
            {
                return Helper::from_tuple(t);
            }
        };

        template<typename KeyType>
        using KeyTupleFor_t = typename KeyTupleFor<KeyType>::type;

        template<typename DTO, std::meta::info TargetMember>
        [[nodiscard]] consteval bool is_dto_key_member() noexcept
        {
            constexpr auto km = Schema::key_members_of<DTO>();
            for (const auto m : km)
            {
                if (m == TargetMember)
                {
                    return true;
                }
            }
            return false;
        }

        template<typename OrderByVariant, typename NewAscCol, typename NewDescCol>
        struct AppendOrderByVariant;

        template<typename... PrevOrderBys, typename NewAscCol, typename NewDescCol>
        struct AppendOrderByVariant<std::variant<PrevOrderBys...>, NewAscCol, NewDescCol>
        {
            template<typename PrevOB, typename NextCol>
            struct AppendOne;

            template<typename... Cols, typename NextCol>
            struct AppendOne<sqlgen::OrderBy<Cols...>, NextCol>
            {
                using type = sqlgen::OrderBy<Cols..., NextCol>;
            };

            using type =
                std::variant<typename AppendOne<PrevOrderBys, NewAscCol>::type..., typename AppendOne<PrevOrderBys, NewDescCol>::type...>;
        };

    } // namespace detail

    // Fluent server-side query builder for DbSet (Phases 10, 12, 13, 14).
    // Translates filtering, sorting, pagination, aggregations, and bulk deletes into sqlgen pipelines.
    template<
        typename Domain,
        typename DTO,
        typename ConnectionHandle,
        typename KeyType = Schema::EntityKey_t<Domain, DTO>,
        typename WhereCondT = rfl::Nothing,
        typename OrderByT = rfl::Nothing>
        requires DAL::Mappers::Mappable<Domain, DTO> && Repositories::DatabaseConnection<ConnectionHandle>
    class DbQuery
    {
    public:
        using RepoType = Repositories::GenericRepository<Domain, DTO, ConnectionHandle, KeyType>;
        using TrackFn = std::function<void(const Domain&)>;
        using ClearTrackFn = std::function<void()>;
        using UntrackKeyFn = std::function<void(const KeyType&)>;
        using IsContextAliveFn = std::function<bool()>;

        DbQuery(
            std::shared_ptr<RepoType> repo,
            std::shared_ptr<ConnectionHandle> conn,
            WhereCondT where_cond = {},
            OrderByT order_by = {},
            std::optional<std::size_t> limit = std::nullopt,
            std::optional<std::size_t> offset = std::nullopt,
            TrackFn track_fn = {},
            ClearTrackFn clear_track_fn = {},
            UntrackKeyFn untrack_key_fn = {},
            IsContextAliveFn is_context_alive_fn = {},
            std::optional<Core::DbError> validation_error = std::nullopt)
            : repo_(std::move(repo))
            , conn_(std::move(conn))
            , where_cond_(std::move(where_cond))
            , order_by_(std::move(order_by))
            , limit_(limit)
            , offset_(offset)
            , track_fn_(std::move(track_fn))
            , clear_track_fn_(std::move(clear_track_fn))
            , untrack_key_fn_(std::move(untrack_key_fn))
            , is_context_alive_fn_(std::move(is_context_alive_fn))
            , validation_error_(std::move(validation_error))
        {
        }

        template<typename Condition>
        [[nodiscard]] auto where_sql(Condition cond) const
        {
            return where_sql_with_error(std::move(cond), validation_error_);
        }

        template<typename Condition>
        [[nodiscard]] auto or_where_sql(Condition cond) const
        {
            return or_where_sql_with_error(std::move(cond), validation_error_);
        }

        template<rfl::internal::StringLiteral ColLiteral, typename Val>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_equal(const Val& val) const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            return where_sql_with_error(
                Schema::col_of<dto_m> == detail::normalize_query_val(val), detail::first_query_error(validation_error_, val));
        }

        template<rfl::internal::StringLiteral ColLiteral, typename Val>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto or_where_equal(const Val& val) const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            return or_where_sql_with_error(
                Schema::col_of<dto_m> == detail::normalize_query_val(val), detail::first_query_error(validation_error_, val));
        }

        template<rfl::internal::StringLiteral ColLiteral, typename Val>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto or_where_not_equal(const Val& val) const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            return or_where_sql_with_error(
                Schema::col_of<dto_m> != detail::normalize_query_val(val), detail::first_query_error(validation_error_, val));
        }

        template<rfl::internal::StringLiteral ColLiteral, typename Val>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto or_where_lt(const Val& val) const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            return or_where_sql_with_error(
                Schema::col_of<dto_m> < detail::normalize_query_val(val), detail::first_query_error(validation_error_, val));
        }

        template<rfl::internal::StringLiteral ColLiteral, typename Val>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto or_where_le(const Val& val) const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            return or_where_sql_with_error(
                Schema::col_of<dto_m> <= detail::normalize_query_val(val), detail::first_query_error(validation_error_, val));
        }

        template<rfl::internal::StringLiteral ColLiteral, typename Val>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto or_where_gt(const Val& val) const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            return or_where_sql_with_error(
                Schema::col_of<dto_m> > detail::normalize_query_val(val), detail::first_query_error(validation_error_, val));
        }

        template<rfl::internal::StringLiteral ColLiteral, typename Val>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto or_where_ge(const Val& val) const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            return or_where_sql_with_error(
                Schema::col_of<dto_m> >= detail::normalize_query_val(val), detail::first_query_error(validation_error_, val));
        }

        template<rfl::internal::StringLiteral ColLiteral, typename Val>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto or_where_between(const Val& low, const Val& high) const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            return or_where_sql_with_error(
                (Schema::col_of<dto_m> >= detail::normalize_query_val(low)) && (Schema::col_of<dto_m> <= detail::normalize_query_val(high)),
                detail::first_query_error(validation_error_, low, high));
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto or_where_like(std::string_view pattern) const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            return or_where_sql(Schema::col_of<dto_m>.like(std::string(pattern)));
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto or_where_not_like(std::string_view pattern) const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            return or_where_sql(Schema::col_of<dto_m>.not_like(std::string(pattern)));
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto or_where_contains(std::string_view needle) const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            std::string pattern;
            pattern.reserve(needle.size() + 2);
            pattern.push_back('%');
            pattern += detail::escape_like_literal(needle);
            pattern.push_back('%');
            return or_where_sql(detail::EscapedLikeCond{ std::string{ std::meta::identifier_of(dto_m) }, std::move(pattern), false });
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto or_where_starts_with(std::string_view prefix) const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            std::string pattern = detail::escape_like_literal(prefix);
            pattern.push_back('%');
            return or_where_sql(detail::EscapedLikeCond{ std::string{ std::meta::identifier_of(dto_m) }, std::move(pattern), false });
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto or_where_ends_with(std::string_view suffix) const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            std::string pattern;
            pattern.reserve(suffix.size() + 1);
            pattern.push_back('%');
            pattern += detail::escape_like_literal(suffix);
            return or_where_sql(detail::EscapedLikeCond{ std::string{ std::meta::identifier_of(dto_m) }, std::move(pattern), false });
        }

        template<rfl::internal::StringLiteral ColLiteral, typename... Ts>
            requires(sizeof...(Ts) > 0) && DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto or_where_in(const Ts&... vals) const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            return or_where_sql_with_error(
                Schema::col_of<dto_m>.in(detail::normalize_query_val(vals)...), detail::first_query_error(validation_error_, vals...));
        }

        template<rfl::internal::StringLiteral ColLiteral, typename Val>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto or_where_in_vec(const std::vector<Val>& vals) const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            using NormalizedT = std::remove_cvref_t<decltype(detail::normalize_query_val(std::declval<const Val&>()))>;
            std::vector<NormalizedT> normalized;
            normalized.reserve(vals.size());
            for (const auto& v : vals)
            {
                normalized.push_back(detail::normalize_query_val(v));
            }
            return or_where_sql_with_error(Schema::col_of<dto_m>.in(normalized), detail::first_query_vec_error(validation_error_, vals));
        }

        template<rfl::internal::StringLiteral ColLiteral, typename... Ts>
            requires(sizeof...(Ts) > 0) && DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto or_where_not_in(const Ts&... vals) const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            return or_where_sql_with_error(
                Schema::col_of<dto_m>.not_in(detail::normalize_query_val(vals)...), detail::first_query_error(validation_error_, vals...));
        }

        template<rfl::internal::StringLiteral ColLiteral, typename Val>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto or_where_not_in_vec(const std::vector<Val>& vals) const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            using NormalizedT = std::remove_cvref_t<decltype(detail::normalize_query_val(std::declval<const Val&>()))>;
            std::vector<NormalizedT> normalized;
            normalized.reserve(vals.size());
            for (const auto& v : vals)
            {
                normalized.push_back(detail::normalize_query_val(v));
            }
            return or_where_sql_with_error(
                Schema::col_of<dto_m>.not_in(normalized), detail::first_query_vec_error(validation_error_, vals));
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto or_where_null() const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            return or_where_sql(Schema::col_of<dto_m>.is_null());
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto or_where_not_null() const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            return or_where_sql(Schema::col_of<dto_m>.is_not_null());
        }

        template<rfl::internal::StringLiteral ColLiteral, typename Val>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_not_equal(const Val& val) const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            return where_sql_with_error(
                Schema::col_of<dto_m> != detail::normalize_query_val(val), detail::first_query_error(validation_error_, val));
        }

        template<rfl::internal::StringLiteral ColLiteral, typename Val>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_lt(const Val& val) const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            return where_sql_with_error(
                Schema::col_of<dto_m> < detail::normalize_query_val(val), detail::first_query_error(validation_error_, val));
        }

        template<rfl::internal::StringLiteral ColLiteral, typename Val>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_le(const Val& val) const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            return where_sql_with_error(
                Schema::col_of<dto_m> <= detail::normalize_query_val(val), detail::first_query_error(validation_error_, val));
        }

        template<rfl::internal::StringLiteral ColLiteral, typename Val>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_gt(const Val& val) const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            return where_sql_with_error(
                Schema::col_of<dto_m> > detail::normalize_query_val(val), detail::first_query_error(validation_error_, val));
        }

        template<rfl::internal::StringLiteral ColLiteral, typename Val>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_ge(const Val& val) const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            return where_sql_with_error(
                Schema::col_of<dto_m> >= detail::normalize_query_val(val), detail::first_query_error(validation_error_, val));
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_like(std::string_view pattern) const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            return where_sql(Schema::col_of<dto_m>.like(std::string(pattern)));
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_contains(std::string_view needle) const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            std::string pattern;
            pattern.reserve(needle.size() + 2);
            pattern.push_back('%');
            pattern += detail::escape_like_literal(needle);
            pattern.push_back('%');
            return where_sql(detail::EscapedLikeCond{ std::string{ std::meta::identifier_of(dto_m) }, std::move(pattern), false });
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_starts_with(std::string_view prefix) const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            std::string pattern = detail::escape_like_literal(prefix);
            pattern.push_back('%');
            return where_sql(detail::EscapedLikeCond{ std::string{ std::meta::identifier_of(dto_m) }, std::move(pattern), false });
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_ends_with(std::string_view suffix) const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            std::string pattern;
            pattern.reserve(suffix.size() + 1);
            pattern.push_back('%');
            pattern += detail::escape_like_literal(suffix);
            return where_sql(detail::EscapedLikeCond{ std::string{ std::meta::identifier_of(dto_m) }, std::move(pattern), false });
        }

        template<rfl::internal::StringLiteral ColLiteral, typename... Ts>
            requires(sizeof...(Ts) > 0) && DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_in(const Ts&... vals) const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            return where_sql_with_error(
                Schema::col_of<dto_m>.in(detail::normalize_query_val(vals)...), detail::first_query_error(validation_error_, vals...));
        }

        template<rfl::internal::StringLiteral ColLiteral, typename Val>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_in_vec(const std::vector<Val>& vals) const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            using NormalizedT = std::remove_cvref_t<decltype(detail::normalize_query_val(std::declval<const Val&>()))>;
            std::vector<NormalizedT> normalized;
            normalized.reserve(vals.size());
            for (const auto& v : vals)
            {
                normalized.push_back(detail::normalize_query_val(v));
            }
            return where_sql_with_error(Schema::col_of<dto_m>.in(normalized), detail::first_query_vec_error(validation_error_, vals));
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_null() const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            return where_sql(Schema::col_of<dto_m>.is_null());
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_not_null() const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            return where_sql(Schema::col_of<dto_m>.is_not_null());
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_not_like(std::string_view pattern) const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            return where_sql(Schema::col_of<dto_m>.not_like(std::string(pattern)));
        }

        template<rfl::internal::StringLiteral ColLiteral, typename... Ts>
            requires(sizeof...(Ts) > 0) && DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_not_in(const Ts&... vals) const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            return where_sql_with_error(
                Schema::col_of<dto_m>.not_in(detail::normalize_query_val(vals)...), detail::first_query_error(validation_error_, vals...));
        }

        template<rfl::internal::StringLiteral ColLiteral, typename Val>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_between(const Val& low, const Val& high) const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            return where_sql_with_error(
                (Schema::col_of<dto_m> >= detail::normalize_query_val(low)) && (Schema::col_of<dto_m> <= detail::normalize_query_val(high)),
                detail::first_query_error(validation_error_, low, high));
        }

        template<rfl::internal::StringLiteral ColLiteral, typename Val>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_not_in_vec(const std::vector<Val>& vals) const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            using NormalizedT = std::remove_cvref_t<decltype(detail::normalize_query_val(std::declval<const Val&>()))>;
            std::vector<NormalizedT> normalized;
            normalized.reserve(vals.size());
            for (const auto& v : vals)
            {
                normalized.push_back(detail::normalize_query_val(v));
            }
            return where_sql_with_error(Schema::col_of<dto_m>.not_in(normalized), detail::first_query_vec_error(validation_error_, vals));
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto order_by(bool desc = false) const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            using AscT = decltype(sqlgen::order_by(Schema::col_of<dto_m>));
            using DescT = decltype(sqlgen::order_by(Schema::col_of<dto_m>.desc()));
            using VarT = std::variant<AscT, DescT>;
            VarT ob = desc ? VarT{ DescT{} } : VarT{ AscT{} };
            return DbQuery<Domain, DTO, ConnectionHandle, KeyType, WhereCondT, VarT>(
                repo_,
                conn_,
                where_cond_,
                std::move(ob),
                limit_,
                offset_,
                track_fn_,
                clear_track_fn_,
                untrack_key_fn_,
                is_context_alive_fn_,
                validation_error_);
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires(!std::same_as<OrderByT, rfl::Nothing>) && DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto then_by(bool desc = false) const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            using NewAscCol = std::remove_cvref_t<decltype(Schema::col_of<dto_m>)>;
            using NewDescCol = std::remove_cvref_t<decltype(Schema::col_of<dto_m>.desc())>;
            using Helper = detail::AppendOrderByVariant<OrderByT, NewAscCol, NewDescCol>;
            using NewVarT = typename Helper::type;

            NewVarT combined = std::visit(
                [desc](const auto& prev_ob) -> NewVarT
                {
                    using PrevOB = std::remove_cvref_t<decltype(prev_ob)>;
                    if (desc)
                    {
                        using NextOB = typename Helper::template AppendOne<PrevOB, NewDescCol>::type;
                        return NewVarT{ NextOB{} };
                    }
                    else
                    {
                        using NextOB = typename Helper::template AppendOne<PrevOB, NewAscCol>::type;
                        return NewVarT{ NextOB{} };
                    }
                },
                order_by_);

            return DbQuery<Domain, DTO, ConnectionHandle, KeyType, WhereCondT, NewVarT>(
                repo_,
                conn_,
                where_cond_,
                std::move(combined),
                limit_,
                offset_,
                track_fn_,
                clear_track_fn_,
                untrack_key_fn_,
                is_context_alive_fn_,
                validation_error_);
        }

        [[nodiscard]] DbQuery limit(std::size_t limit_count) const pre(limit_count > 0)
        {
            auto err = validation_error_;
            if (!err && limit_count > detail::kSqliteUnboundedLimit)
            {
                err = Core::DbError::constraint_violation("Query limit exceeds SQLite signed 64-bit integer range");
            }
            return DbQuery(
                repo_,
                conn_,
                where_cond_,
                order_by_,
                limit_count,
                offset_,
                track_fn_,
                clear_track_fn_,
                untrack_key_fn_,
                is_context_alive_fn_,
                std::move(err));
        }

        [[nodiscard]] DbQuery offset(std::size_t offset_count) const
        {
            auto err = validation_error_;
            if (!err && offset_count > detail::kSqliteUnboundedLimit)
            {
                err = Core::DbError::constraint_violation("Query offset exceeds SQLite signed 64-bit integer range");
            }
            return DbQuery(
                repo_,
                conn_,
                where_cond_,
                order_by_,
                limit_,
                offset_count,
                track_fn_,
                clear_track_fn_,
                untrack_key_fn_,
                is_context_alive_fn_,
                std::move(err));
        }

        [[nodiscard]] DbQuery page(std::size_t limit_count, std::size_t offset_count = 0) const pre(limit_count > 0)
        {
            auto err = validation_error_;
            if (!err && limit_count > detail::kSqliteUnboundedLimit)
            {
                err = Core::DbError::constraint_violation("Query limit exceeds SQLite signed 64-bit integer range");
            }
            if (!err && offset_count > detail::kSqliteUnboundedLimit)
            {
                err = Core::DbError::constraint_violation("Query offset exceeds SQLite signed 64-bit integer range");
            }
            return DbQuery(
                repo_,
                conn_,
                where_cond_,
                order_by_,
                limit_count,
                offset_count,
                track_fn_,
                clear_track_fn_,
                untrack_key_fn_,
                is_context_alive_fn_,
                std::move(err));
        }

        [[nodiscard]] std::expected<std::vector<Domain>, Core::DbError> try_to_vector() const
        {
            if (!conn_)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is null") };
            }
            if (!conn_->is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is closed") };
            }
            if (validation_error_)
            {
                return std::unexpected<Core::DbError>{ *validation_error_ };
            }
            if (limit_.has_value() && *limit_ == 0)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation("Query limit must be greater than 0") };
            }

            if constexpr (requires(const ConnectionHandle& c) { c.template try_execute_read<DTO>(sqlgen::read<std::vector<DTO>>); })
            {
                auto dto_res = execute_query(*conn_);
                if (!dto_res)
                {
                    return std::unexpected<Core::DbError>{ dto_res.error() };
                }

                std::vector<Domain> out;
                out.reserve(dto_res.value().size());
                for (const auto& dto : dto_res.value())
                {
                    Domain entity = DAL::Mappers::MapperTraits<Domain, DTO>::to_domain(dto);
                    if (track_fn_)
                    {
                        track_fn_(entity);
                    }
                    out.push_back(std::move(entity));
                }
                return out;
            }
            else
            {
                if constexpr (!std::same_as<WhereCondT, rfl::Nothing> || !std::same_as<OrderByT, rfl::Nothing>)
                {
                    return std::unexpected<Core::DbError>{ Core::DbError::unknown(
                        "Connection adapter does not support server-side WHERE or ORDER BY queries") };
                }
                else
                {
                    auto all_res = repo_->try_get_all();
                    if (!all_res)
                    {
                        return std::unexpected<Core::DbError>{ all_res.error() };
                    }
                    auto out = std::move(*all_res);
                    if (offset_.has_value())
                    {
                        const std::size_t off = std::min(*offset_, out.size());
                        out.erase(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(off));
                    }
                    if (limit_.has_value() && out.size() > *limit_)
                    {
                        out.resize(*limit_);
                    }
                    if (track_fn_)
                    {
                        for (const auto& entity : out)
                        {
                            track_fn_(entity);
                        }
                    }
                    return out;
                }
            }
        }

        [[nodiscard]] std::vector<Domain> to_vector() const
        {
            auto res = try_to_vector();
            if (res)
            {
                return std::move(res.value());
            }
            return {};
        }

        [[nodiscard]] std::expected<std::optional<Domain>, Core::DbError> try_first_or_default() const
        {
            if (!conn_)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is null") };
            }
            if (!conn_->is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is closed") };
            }
            if (validation_error_)
            {
                return std::unexpected<Core::DbError>{ *validation_error_ };
            }
            if (limit_.has_value() && *limit_ == 0)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation("Query limit must be greater than 0") };
            }
            auto res = limit(1).try_to_vector();
            if (!res)
            {
                return std::unexpected<Core::DbError>{ res.error() };
            }
            if (res->empty())
            {
                return std::optional<Domain>{ std::nullopt };
            }
            return std::optional<Domain>{ std::move(res->front()) };
        }

        [[nodiscard]] std::optional<Domain> first_or_default() const
        {
            return try_first_or_default().value_or(std::nullopt);
        }

        [[nodiscard]] std::expected<std::size_t, Core::DbError> try_count() const
        {
            if (!conn_)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is null") };
            }
            if (!conn_->is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is closed") };
            }
            if (validation_error_)
            {
                return std::unexpected<Core::DbError>{ *validation_error_ };
            }
            if (limit_.has_value() && *limit_ == 0)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation("Query limit must be greater than 0") };
            }

            if (!limit_.has_value() && !offset_.has_value())
            {
                if constexpr (std::same_as<WhereCondT, rfl::Nothing>)
                {
                    if constexpr (requires(const ConnectionHandle& c) { c.template try_count<DTO>(); })
                    {
                        return conn_->template try_count<DTO>();
                    }
                }
                else
                {
                    if constexpr (requires(const ConnectionHandle& c) { c.template try_count_where<DTO>(where_cond_); })
                    {
                        return conn_->template try_count_where<DTO>(where_cond_);
                    }
                }
            }

            if constexpr (requires(const ConnectionHandle& c) { c.template try_execute_read<DTO>(sqlgen::read<std::vector<DTO>>); })
            {
                auto dto_res = execute_query(*conn_);
                if (!dto_res)
                {
                    return std::unexpected<Core::DbError>{ dto_res.error() };
                }
                return dto_res->size();
            }
            else
            {
                if constexpr (!std::same_as<WhereCondT, rfl::Nothing> || !std::same_as<OrderByT, rfl::Nothing>)
                {
                    return std::unexpected<Core::DbError>{ Core::DbError::unknown(
                        "Connection adapter does not support server-side WHERE or ORDER BY queries") };
                }
                else
                {
                    auto all_res = repo_->try_get_all();
                    if (!all_res)
                    {
                        return std::unexpected<Core::DbError>{ all_res.error() };
                    }
                    std::size_t count_val = all_res->size();
                    if (offset_.has_value())
                    {
                        count_val = (*offset_ >= count_val) ? 0u : (count_val - *offset_);
                    }
                    if (limit_.has_value() && count_val > *limit_)
                    {
                        count_val = *limit_;
                    }
                    return count_val;
                }
            }
        }

        [[nodiscard]] std::size_t count() const
        {
            return try_count().value_or(0);
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto try_min() const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            using RawT = [:std::meta::type_of(dto_m):];
            using ScalarT = typename detail::UnwrapColumnScalar<std::remove_cvref_t<RawT>>::type;
            return try_aggregate_impl<ScalarT>(sqlgen::min(Schema::col_of<dto_m>));
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto min() const
        {
            return try_min<ColLiteral>().value_or(std::nullopt);
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto try_max() const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            using RawT = [:std::meta::type_of(dto_m):];
            using ScalarT = typename detail::UnwrapColumnScalar<std::remove_cvref_t<RawT>>::type;
            return try_aggregate_impl<ScalarT>(sqlgen::max(Schema::col_of<dto_m>));
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto max() const
        {
            return try_max<ColLiteral>().value_or(std::nullopt);
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto try_sum() const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            using RawT = [:std::meta::type_of(dto_m):];
            using ScalarT = typename detail::UnwrapColumnScalar<std::remove_cvref_t<RawT>>::type;
            using SumT = typename detail::SumResultType<ScalarT>::type;
            return try_aggregate_impl<SumT>(sqlgen::sum(Schema::col_of<dto_m>));
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto sum() const
        {
            return try_sum<ColLiteral>().value_or(std::nullopt);
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] std::expected<std::optional<double>, Core::DbError> try_avg() const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            return try_aggregate_impl<double>(sqlgen::avg(Schema::col_of<dto_m>));
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] std::optional<double> avg() const
        {
            return try_avg<ColLiteral>().value_or(std::nullopt);
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto try_select() const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            using SplicedT = [:std::meta::type_of(dto_m):];
            using RawT = std::remove_cvref_t<SplicedT>;
            using ScalarT = typename detail::UnwrapColumnScalar<RawT>::type;
            using ColValueT = std::conditional_t<detail::IsOptionalField<RawT>::value, std::optional<ScalarT>, ScalarT>;

            if (!conn_)
            {
                return std::expected<std::vector<ColValueT>, Core::DbError>{ std::unexpected<Core::DbError>{
                    Core::DbError::connection_error("Database connection is null") } };
            }
            if (!conn_->is_open())
            {
                return std::expected<std::vector<ColValueT>, Core::DbError>{ std::unexpected<Core::DbError>{
                    Core::DbError::connection_error("Database connection is closed") } };
            }
            if (validation_error_)
            {
                return std::expected<std::vector<ColValueT>, Core::DbError>{ std::unexpected<Core::DbError>{ *validation_error_ } };
            }
            if (limit_.has_value() && *limit_ == 0)
            {
                return std::expected<std::vector<ColValueT>, Core::DbError>{ std::unexpected<Core::DbError>{
                    Core::DbError::constraint_violation("Query limit must be greater than 0") } };
            }

            using ProjRow = detail::SingleValueRow<RawT>;

            auto with_where = [&]()
            {
                if constexpr (!std::same_as<WhereCondT, rfl::Nothing> && !detail::is_custom_where_cond_v<WhereCondT>)
                {
                    return sqlgen::select_from<DTO>(Schema::col_of<dto_m> | sqlgen::as<"val">) | sqlgen::where(where_cond_);
                }
                else
                {
                    return sqlgen::select_from<DTO>(Schema::col_of<dto_m> | sqlgen::as<"val">);
                }
            }();

            auto rows_res = [&]() -> std::expected<std::vector<ProjRow>, Core::DbError>
            {
                if constexpr (!std::same_as<OrderByT, rfl::Nothing>)
                {
                    return std::visit(
                        [&](const auto& ob)
                        {
                            return execute_projection<ProjRow>(*conn_, with_where | ob);
                        },
                        order_by_);
                }
                else
                {
                    return execute_projection<ProjRow>(*conn_, std::move(with_where));
                }
            }();

            if (!rows_res)
            {
                return std::expected<std::vector<ColValueT>, Core::DbError>{ std::unexpected<Core::DbError>{ rows_res.error() } };
            }

            std::vector<ColValueT> result;
            result.reserve(rows_res->size());
            for (auto& r : *rows_res)
            {
                result.push_back(detail::unwrap_projected_value(std::move(r.val)));
            }
            return std::expected<std::vector<ColValueT>, Core::DbError>{ std::move(result) };
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto select() const
        {
            auto res = try_select<ColLiteral>();
            using VecT = std::remove_cvref_t<decltype(res.value())>;
            if (res)
            {
                return VecT{ std::move(res.value()) };
            }
            return VecT{};
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] std::expected<std::size_t, Core::DbError> try_count_distinct() const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            auto res = try_aggregate_impl<std::size_t>(sqlgen::count_distinct(Schema::col_of<dto_m>));
            if (!res)
            {
                return std::unexpected<Core::DbError>{ res.error() };
            }
            return res->value_or(0);
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] std::size_t count_distinct() const
        {
            return try_count_distinct<ColLiteral>().value_or(0);
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto try_select_distinct() const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            using SplicedT = [:std::meta::type_of(dto_m):];
            using RawT = std::remove_cvref_t<SplicedT>;
            using ScalarT = typename detail::UnwrapColumnScalar<RawT>::type;
            using ColValueT = std::conditional_t<detail::IsOptionalField<RawT>::value, std::optional<ScalarT>, ScalarT>;

            if (!conn_)
            {
                return std::expected<std::vector<ColValueT>, Core::DbError>{ std::unexpected<Core::DbError>{
                    Core::DbError::connection_error("Database connection is null") } };
            }
            if (!conn_->is_open())
            {
                return std::expected<std::vector<ColValueT>, Core::DbError>{ std::unexpected<Core::DbError>{
                    Core::DbError::connection_error("Database connection is closed") } };
            }
            if (validation_error_)
            {
                return std::expected<std::vector<ColValueT>, Core::DbError>{ std::unexpected<Core::DbError>{ *validation_error_ } };
            }
            if (limit_.has_value() && *limit_ == 0)
            {
                return std::expected<std::vector<ColValueT>, Core::DbError>{ std::unexpected<Core::DbError>{
                    Core::DbError::constraint_violation("Query limit must be greater than 0") } };
            }

            using ProjRow = detail::SingleValueRow<RawT>;

            auto with_group = [&]()
            {
                if constexpr (!std::same_as<WhereCondT, rfl::Nothing> && !detail::is_custom_where_cond_v<WhereCondT>)
                {
                    return sqlgen::select_from<DTO>(Schema::col_of<dto_m> | sqlgen::as<"val">) | sqlgen::where(where_cond_) |
                        sqlgen::group_by(Schema::col_of<dto_m>);
                }
                else
                {
                    return sqlgen::select_from<DTO>(Schema::col_of<dto_m> | sqlgen::as<"val">) | sqlgen::group_by(Schema::col_of<dto_m>);
                }
            }();

            auto rows_res = [&]() -> std::expected<std::vector<ProjRow>, Core::DbError>
            {
                if constexpr (!std::same_as<OrderByT, rfl::Nothing>)
                {
                    return std::visit(
                        [&](const auto& ob)
                        {
                            return execute_projection<ProjRow>(*conn_, with_group | ob);
                        },
                        order_by_);
                }
                else
                {
                    return execute_projection<ProjRow>(*conn_, std::move(with_group));
                }
            }();

            if (!rows_res)
            {
                return std::expected<std::vector<ColValueT>, Core::DbError>{ std::unexpected<Core::DbError>{ rows_res.error() } };
            }

            std::vector<ColValueT> result;
            result.reserve(rows_res->size());
            for (auto& r : *rows_res)
            {
                result.push_back(detail::unwrap_projected_value(std::move(r.val)));
            }
            return std::expected<std::vector<ColValueT>, Core::DbError>{ std::move(result) };
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto select_distinct() const
        {
            auto res = try_select_distinct<ColLiteral>();
            using VecT = std::remove_cvref_t<decltype(res.value())>;
            if (res)
            {
                return VecT{ std::move(res.value()) };
            }
            return VecT{};
        }

        template<rfl::internal::StringLiteral... ColLiterals>
            requires(sizeof...(ColLiterals) >= 2) && (DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiterals> && ...)
        [[nodiscard]] auto try_select_columns() const
        {
            using RowTuple = std::tuple<detail::ProjectedColumn_t<
                decltype(std::declval<const DTO&>().[:DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiterals>():])>...>;

            if (!conn_)
            {
                return std::expected<std::vector<RowTuple>, Core::DbError>{ std::unexpected<Core::DbError>{
                    Core::DbError::connection_error("Database connection is null") } };
            }
            if (!conn_->is_open())
            {
                return std::expected<std::vector<RowTuple>, Core::DbError>{ std::unexpected<Core::DbError>{
                    Core::DbError::connection_error("Database connection is closed") } };
            }
            if (validation_error_)
            {
                return std::expected<std::vector<RowTuple>, Core::DbError>{ std::unexpected<Core::DbError>{ *validation_error_ } };
            }
            if (limit_.has_value() && *limit_ == 0)
            {
                return std::expected<std::vector<RowTuple>, Core::DbError>{ std::unexpected<Core::DbError>{
                    Core::DbError::constraint_violation("Query limit must be greater than 0") } };
            }

            auto with_where = [&]()
            {
                if constexpr (!std::same_as<WhereCondT, rfl::Nothing> && !detail::is_custom_where_cond_v<WhereCondT>)
                {
                    return sqlgen::select_from<DTO>(Schema::col_of<DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiterals>()>...) |
                        sqlgen::where(where_cond_);
                }
                else
                {
                    return sqlgen::select_from<DTO>(Schema::col_of<DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiterals>()>...);
                }
            }();

            if constexpr (!std::same_as<OrderByT, rfl::Nothing>)
            {
                return std::visit(
                    [&](const auto& ob)
                    {
                        return execute_tuple_projection<RowTuple>(*conn_, with_where | ob);
                    },
                    order_by_);
            }
            else
            {
                return execute_tuple_projection<RowTuple>(*conn_, std::move(with_where));
            }
        }

        template<rfl::internal::StringLiteral... ColLiterals>
            requires(sizeof...(ColLiterals) >= 2) && (DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiterals> && ...)
        [[nodiscard]] auto select_columns() const
        {
            auto res = try_select_columns<ColLiterals...>();
            using VecT = std::remove_cvref_t<decltype(res.value())>;
            if (res)
            {
                return VecT{ std::move(res.value()) };
            }
            return VecT{};
        }

        template<rfl::internal::StringLiteral... ColLiterals>
            requires(sizeof...(ColLiterals) >= 2) && (DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiterals> && ...)
        [[nodiscard]] auto try_select_distinct_columns() const
        {
            using RowTuple = std::tuple<detail::ProjectedColumn_t<
                decltype(std::declval<const DTO&>().[:DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiterals>():])>...>;

            if (!conn_)
            {
                return std::expected<std::vector<RowTuple>, Core::DbError>{ std::unexpected<Core::DbError>{
                    Core::DbError::connection_error("Database connection is null") } };
            }
            if (!conn_->is_open())
            {
                return std::expected<std::vector<RowTuple>, Core::DbError>{ std::unexpected<Core::DbError>{
                    Core::DbError::connection_error("Database connection is closed") } };
            }
            if (validation_error_)
            {
                return std::expected<std::vector<RowTuple>, Core::DbError>{ std::unexpected<Core::DbError>{ *validation_error_ } };
            }
            if (limit_.has_value() && *limit_ == 0)
            {
                return std::expected<std::vector<RowTuple>, Core::DbError>{ std::unexpected<Core::DbError>{
                    Core::DbError::constraint_violation("Query limit must be greater than 0") } };
            }

            auto with_group = [&]()
            {
                if constexpr (!std::same_as<WhereCondT, rfl::Nothing> && !detail::is_custom_where_cond_v<WhereCondT>)
                {
                    return sqlgen::select_from<DTO>(Schema::col_of<DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiterals>()>...) |
                        sqlgen::where(where_cond_) |
                        sqlgen::group_by(Schema::col_of<DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiterals>()>...);
                }
                else
                {
                    return sqlgen::select_from<DTO>(Schema::col_of<DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiterals>()>...) |
                        sqlgen::group_by(Schema::col_of<DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiterals>()>...);
                }
            }();

            if constexpr (!std::same_as<OrderByT, rfl::Nothing>)
            {
                return std::visit(
                    [&](const auto& ob)
                    {
                        return execute_tuple_projection<RowTuple>(*conn_, with_group | ob);
                    },
                    order_by_);
            }
            else
            {
                return execute_tuple_projection<RowTuple>(*conn_, std::move(with_group));
            }
        }

        template<rfl::internal::StringLiteral... ColLiterals>
            requires(sizeof...(ColLiterals) >= 2) && (DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiterals> && ...)
        [[nodiscard]] auto select_distinct_columns() const
        {
            auto res = try_select_distinct_columns<ColLiterals...>();
            using VecT = std::remove_cvref_t<decltype(res.value())>;
            if (res)
            {
                return VecT{ std::move(res.value()) };
            }
            return VecT{};
        }

        template<rfl::internal::StringLiteral IndexName, rfl::internal::StringLiteral... ColLiterals>
            requires(sizeof...(ColLiterals) >= 1) && (DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiterals> && ...)
        [[nodiscard]] std::expected<void, Core::DbError> ensure_index_created(bool unique = false) const
        {
            if (!conn_)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is null") };
            }
            if (validation_error_)
            {
                return std::unexpected<Core::DbError>{ *validation_error_ };
            }
            if constexpr (!std::same_as<WhereCondT, rfl::Nothing>)
            {
                return conn_->template ensure_filtered_index_created<IndexName, DTO>(
                    unique, where_cond_, Schema::col_of<DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiterals>()>...);
            }
            else
            {
                return conn_->template ensure_index_created<IndexName, DTO>(
                    unique, Schema::col_of<DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiterals>()>...);
            }
        }

        template<rfl::internal::StringLiteral IndexName, rfl::internal::StringLiteral... ColLiterals>
            requires(sizeof...(ColLiterals) >= 1) && (DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiterals> && ...)
        [[nodiscard]] std::expected<void, Core::DbError> ensure_unique_index_created() const
        {
            return ensure_index_created<IndexName, ColLiterals...>(true);
        }

        [[nodiscard]] std::expected<bool, Core::DbError> try_any() const
        {
            if (!conn_)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is null") };
            }
            if (!conn_->is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is closed") };
            }
            if (validation_error_)
            {
                return std::unexpected<Core::DbError>{ *validation_error_ };
            }
            if (limit_.has_value() && *limit_ == 0)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation("Query limit must be greater than 0") };
            }
            if (!limit_.has_value() && !offset_.has_value())
            {
                auto cnt_res = try_count();
                if (!cnt_res)
                {
                    return std::unexpected<Core::DbError>{ cnt_res.error() };
                }
                return *cnt_res > 0;
            }
            if constexpr (requires(const ConnectionHandle& c) { c.template try_execute_read<DTO>(sqlgen::read<std::vector<DTO>>); })
            {
                auto dto_res = limit(1).execute_query(*conn_);
                if (!dto_res)
                {
                    return std::unexpected<Core::DbError>{ dto_res.error() };
                }
                return !dto_res->empty();
            }
            else
            {
                auto cnt_res = try_count();
                if (!cnt_res)
                {
                    return std::unexpected<Core::DbError>{ cnt_res.error() };
                }
                return *cnt_res > 0;
            }
        }

        [[nodiscard]] bool any() const
        {
            return try_any().value_or(false);
        }

        [[nodiscard]] std::expected<void, Core::DbError> remove_where() const
            requires(!std::same_as<WhereCondT, rfl::Nothing>)
        {
            if (is_context_alive_fn_ && !is_context_alive_fn_())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error(
                    "Cannot mutate through DbQuery: owning DbContext has expired") };
            }
            if (!conn_)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is null") };
            }
            if (!conn_->is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is closed") };
            }
            if (validation_error_)
            {
                return std::unexpected<Core::DbError>{ *validation_error_ };
            }
            if (limit_.has_value() && *limit_ == 0)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation("Query limit must be greater than 0") };
            }

            using KeyTuple = detail::KeyTupleFor_t<KeyType>;
            if (!limit_.has_value() && !offset_.has_value())
            {
                if constexpr (requires(ConnectionHandle& c) { c.template delete_where_returning_keys<DTO, KeyTuple>(where_cond_); })
                {
                    auto del_res = conn_->template delete_where_returning_keys<DTO, KeyTuple>(where_cond_);
                    if (!del_res)
                    {
                        return std::unexpected<Core::DbError>{ del_res.error() };
                    }
                    if (untrack_key_fn_)
                    {
                        for (const auto& tuple_key : *del_res)
                        {
                            untrack_key_fn_(detail::KeyTupleFor<KeyType>::from_tuple(tuple_key));
                        }
                    }
                    else if (!del_res->empty() && clear_track_fn_)
                    {
                        clear_track_fn_();
                    }
                    return {};
                }
                else if constexpr (requires(ConnectionHandle& c) { c.template delete_where<DTO>(where_cond_); })
                {
                    auto del_res = conn_->template delete_where<DTO>(where_cond_);
                    if (del_res && clear_track_fn_)
                    {
                        clear_track_fn_();
                    }
                    return del_res;
                }
            }

            // Paginated delete: execute as a single atomic SQLite DELETE ... WHERE rowid IN (SELECT rowid ...) RETURNING ...
            // statement when supported so concurrent writers cannot mutate predicate columns between select and delete.
            if constexpr (requires(ConnectionHandle& c) {
                              c.template delete_where_paginated_returning_keys<DTO, KeyTuple>(sqlgen::read<std::vector<DTO>>);
                          })
            {
                auto del_res = execute_paginated_delete<KeyTuple>(*conn_);
                if (!del_res)
                {
                    return std::unexpected<Core::DbError>{ del_res.error() };
                }
                if (untrack_key_fn_)
                {
                    for (const auto& tuple_key : *del_res)
                    {
                        untrack_key_fn_(detail::KeyTupleFor<KeyType>::from_tuple(tuple_key));
                    }
                }
                else if (!del_res->empty() && clear_track_fn_)
                {
                    clear_track_fn_();
                }
                return {};
            }

            // Fallback for custom non-SQLite adapters: acquire an immediate write transaction before reading keys.
            detail::WriteBatchTxGuard<ConnectionHandle> tx_guard(conn_);
            if (auto begin_res = tx_guard.begin(true); !begin_res)
            {
                return begin_res;
            }

            std::vector<KeyType> keys_to_delete;
            if constexpr (requires(const ConnectionHandle& c) { c.template try_execute_read<DTO>(sqlgen::read<std::vector<DTO>>); })
            {
                auto dto_res = execute_query(*conn_);
                if (!dto_res)
                {
                    return std::unexpected<Core::DbError>{ dto_res.error() };
                }
                keys_to_delete.reserve(dto_res->size());
                for (const auto& dto : *dto_res)
                {
                    keys_to_delete.push_back(Schema::EntityMetadata<Domain, DTO>::extract_dto_key(dto));
                }
            }
            else
            {
                return std::unexpected<Core::DbError>{ Core::DbError::unknown(
                    "Connection adapter does not support server-side WHERE queries") };
            }

            for (const auto& key : keys_to_delete)
            {
                auto del_res = repo_->delete_by_id(key);
                if (!del_res)
                {
                    return del_res;
                }
            }

            if (auto commit_res = tx_guard.commit(); !commit_res)
            {
                return commit_res;
            }

            if (untrack_key_fn_)
            {
                for (const auto& key : keys_to_delete)
                {
                    untrack_key_fn_(key);
                }
            }
            else if (!keys_to_delete.empty() && clear_track_fn_)
            {
                clear_track_fn_();
            }
            return {};
        }

        template<rfl::internal::StringLiteral... ColLiterals, typename... Vals>
            requires(sizeof...(ColLiterals) >= 1) && (sizeof...(ColLiterals) == sizeof...(Vals)) &&
            (!std::same_as<WhereCondT, rfl::Nothing>) && (DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiterals> && ...)
        [[nodiscard]] std::expected<void, Core::DbError> update_where(const Vals&... new_vals) const
        {
            if (is_context_alive_fn_ && !is_context_alive_fn_())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error(
                    "Cannot mutate through DbQuery: owning DbContext has expired") };
            }
            if (!conn_)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is null") };
            }
            if (!conn_->is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is closed") };
            }
            if (auto err = detail::first_query_error(validation_error_, new_vals...))
            {
                return std::unexpected<Core::DbError>{ std::move(*err) };
            }
            if (limit_.has_value() || offset_.has_value())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation(
                    "Bulk update_where does not support limit or offset pagination") };
            }
            auto make_set_expr = []<rfl::internal::StringLiteral ColLit, typename ValT>(const ValT& v)
            {
                constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLit>();
                return Schema::col_of<dto_m>.set(detail::normalize_query_val(v));
            };

            constexpr bool updates_key_column =
                (detail::is_dto_key_member<DTO, DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiterals>()>() || ...);
            using KeyTuple = detail::KeyTupleFor_t<KeyType>;

            if constexpr (sizeof...(ColLiterals) == 1)
            {
                auto set_expr = make_set_expr.template operator()<ColLiterals...>(new_vals...);
                if constexpr (requires(ConnectionHandle& c) {
                                  c.template update_where_returning_keys<DTO, KeyTuple>(set_expr, where_cond_);
                              })
                {
                    auto upd_res = conn_->template update_where_returning_keys<DTO, KeyTuple>(set_expr, where_cond_);
                    if (!upd_res)
                    {
                        return std::unexpected<Core::DbError>{ upd_res.error() };
                    }
                    if (!upd_res->empty())
                    {
                        if (!updates_key_column && untrack_key_fn_)
                        {
                            for (const auto& tuple_key : *upd_res)
                            {
                                untrack_key_fn_(detail::KeyTupleFor<KeyType>::from_tuple(tuple_key));
                            }
                        }
                        else if (clear_track_fn_)
                        {
                            clear_track_fn_();
                        }
                    }
                    return {};
                }
                else
                {
                    auto res = conn_->template update_where<DTO>(set_expr, where_cond_);
                    if (res && clear_track_fn_)
                    {
                        clear_track_fn_();
                    }
                    return res;
                }
            }
            else
            {
                if constexpr (requires(ConnectionHandle& c) {
                                  c.template update_where_many_returning_keys<DTO, KeyTuple>(
                                      where_cond_, make_set_expr.template operator()<ColLiterals>(new_vals)...);
                              })
                {
                    auto upd_res = conn_->template update_where_many_returning_keys<DTO, KeyTuple>(
                        where_cond_, make_set_expr.template operator()<ColLiterals>(new_vals)...);
                    if (!upd_res)
                    {
                        return std::unexpected<Core::DbError>{ upd_res.error() };
                    }
                    if (!upd_res->empty())
                    {
                        if (!updates_key_column && untrack_key_fn_)
                        {
                            for (const auto& tuple_key : *upd_res)
                            {
                                untrack_key_fn_(detail::KeyTupleFor<KeyType>::from_tuple(tuple_key));
                            }
                        }
                        else if (clear_track_fn_)
                        {
                            clear_track_fn_();
                        }
                    }
                    return {};
                }
                else
                {
                    auto res =
                        conn_->template update_where_many<DTO>(where_cond_, make_set_expr.template operator()<ColLiterals>(new_vals)...);
                    if (res && clear_track_fn_)
                    {
                        clear_track_fn_();
                    }
                    return res;
                }
            }
        }

    private:
        template<typename Condition>
        [[nodiscard]] auto where_sql_with_error(Condition cond, std::optional<Core::DbError> err) const
        {
            if constexpr (std::same_as<WhereCondT, rfl::Nothing>)
            {
                return DbQuery<Domain, DTO, ConnectionHandle, KeyType, Condition, OrderByT>(
                    repo_,
                    conn_,
                    std::move(cond),
                    order_by_,
                    limit_,
                    offset_,
                    track_fn_,
                    clear_track_fn_,
                    untrack_key_fn_,
                    is_context_alive_fn_,
                    std::move(err));
            }
            else if constexpr (detail::is_custom_where_cond_v<WhereCondT> || detail::is_custom_where_cond_v<Condition>)
            {
                detail::AndCond<WhereCondT, Condition> combined{ where_cond_, std::move(cond) };
                using CombinedT = decltype(combined);
                return DbQuery<Domain, DTO, ConnectionHandle, KeyType, CombinedT, OrderByT>(
                    repo_,
                    conn_,
                    std::move(combined),
                    order_by_,
                    limit_,
                    offset_,
                    track_fn_,
                    clear_track_fn_,
                    untrack_key_fn_,
                    is_context_alive_fn_,
                    std::move(err));
            }
            else
            {
                auto combined = where_cond_ && std::move(cond);
                using CombinedT = decltype(combined);
                return DbQuery<Domain, DTO, ConnectionHandle, KeyType, CombinedT, OrderByT>(
                    repo_,
                    conn_,
                    std::move(combined),
                    order_by_,
                    limit_,
                    offset_,
                    track_fn_,
                    clear_track_fn_,
                    untrack_key_fn_,
                    is_context_alive_fn_,
                    std::move(err));
            }
        }

        template<typename Condition>
        [[nodiscard]] auto or_where_sql_with_error(Condition cond, std::optional<Core::DbError> err) const
        {
            if constexpr (std::same_as<WhereCondT, rfl::Nothing>)
            {
                return where_sql_with_error(std::move(cond), std::move(err));
            }
            else if constexpr (detail::is_custom_where_cond_v<WhereCondT> || detail::is_custom_where_cond_v<Condition>)
            {
                detail::OrCond<WhereCondT, Condition> combined{ where_cond_, std::move(cond) };
                using CombinedT = decltype(combined);
                return DbQuery<Domain, DTO, ConnectionHandle, KeyType, CombinedT, OrderByT>(
                    repo_,
                    conn_,
                    std::move(combined),
                    order_by_,
                    limit_,
                    offset_,
                    track_fn_,
                    clear_track_fn_,
                    untrack_key_fn_,
                    is_context_alive_fn_,
                    std::move(err));
            }
            else
            {
                auto combined = where_cond_ || std::move(cond);
                using CombinedT = decltype(combined);
                return DbQuery<Domain, DTO, ConnectionHandle, KeyType, CombinedT, OrderByT>(
                    repo_,
                    conn_,
                    std::move(combined),
                    order_by_,
                    limit_,
                    offset_,
                    track_fn_,
                    clear_track_fn_,
                    untrack_key_fn_,
                    is_context_alive_fn_,
                    std::move(err));
            }
        }

        template<typename RowTuple, typename BaseQuery>
        [[nodiscard]] std::expected<std::vector<RowTuple>, Core::DbError> execute_tuple_projection(
            const ConnectionHandle& conn, BaseQuery&& base_query) const
        {
            auto run = [&](auto&& q) -> std::expected<std::vector<RowTuple>, Core::DbError>
            {
                if constexpr (detail::is_custom_where_cond_v<WhereCondT>)
                {
                    if constexpr (requires { conn.template try_execute_tuple_read_where<RowTuple, DTO>(std::move(q), where_cond_); })
                    {
                        return conn.template try_execute_tuple_read_where<RowTuple, DTO>(std::move(q), where_cond_);
                    }
                    else
                    {
                        return std::unexpected<Core::DbError>{ Core::DbError::unknown(
                            "Connection adapter does not support escaped LIKE predicates") };
                    }
                }
                else
                {
                    return conn.template try_execute_tuple_read<RowTuple>(std::move(q));
                }
            };

            if (limit_.has_value() && offset_.has_value())
            {
                return run(std::forward<BaseQuery>(base_query) | sqlgen::limit(*limit_) | sqlgen::offset(*offset_));
            }
            if (limit_.has_value())
            {
                return run(std::forward<BaseQuery>(base_query) | sqlgen::limit(*limit_));
            }
            if (offset_.has_value())
            {
                return run(std::forward<BaseQuery>(base_query) | sqlgen::limit(detail::kSqliteUnboundedLimit) | sqlgen::offset(*offset_));
            }
            return run(std::forward<BaseQuery>(base_query));
        }

        template<typename OutRowT, typename BaseQuery>
        [[nodiscard]] std::expected<std::vector<OutRowT>, Core::DbError> execute_projection(
            const ConnectionHandle& conn, BaseQuery&& base_query) const
        {
            auto run = [&](auto&& q) -> std::expected<std::vector<OutRowT>, Core::DbError>
            {
                if constexpr (detail::is_custom_where_cond_v<WhereCondT>)
                {
                    if constexpr (requires { conn.template try_execute_read_where<OutRowT, DTO>(std::move(q), where_cond_); })
                    {
                        return conn.template try_execute_read_where<OutRowT, DTO>(std::move(q), where_cond_);
                    }
                    else
                    {
                        return std::unexpected<Core::DbError>{ Core::DbError::unknown(
                            "Connection adapter does not support escaped LIKE predicates") };
                    }
                }
                else
                {
                    return conn.template try_execute_read<OutRowT>(std::move(q));
                }
            };

            if (limit_.has_value() && offset_.has_value())
            {
                return run(
                    std::forward<BaseQuery>(base_query) | sqlgen::limit(*limit_) | sqlgen::offset(*offset_) |
                    sqlgen::to<std::vector<OutRowT>>);
            }
            if (limit_.has_value())
            {
                return run(std::forward<BaseQuery>(base_query) | sqlgen::limit(*limit_) | sqlgen::to<std::vector<OutRowT>>);
            }
            if (offset_.has_value())
            {
                return run(
                    std::forward<BaseQuery>(base_query) | sqlgen::limit(detail::kSqliteUnboundedLimit) | sqlgen::offset(*offset_) |
                    sqlgen::to<std::vector<OutRowT>>);
            }
            return run(std::forward<BaseQuery>(base_query) | sqlgen::to<std::vector<OutRowT>>);
        }

        template<typename ResultT, typename AggExpr>
        [[nodiscard]] std::expected<std::optional<ResultT>, Core::DbError> try_aggregate_impl(const AggExpr& agg_expr) const
        {
            if (!conn_)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is null") };
            }
            if (validation_error_)
            {
                return std::unexpected<Core::DbError>{ *validation_error_ };
            }
            if (limit_.has_value() || offset_.has_value())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation(
                    "Aggregate queries do not support limit or offset pagination") };
            }
            if constexpr (std::same_as<WhereCondT, rfl::Nothing>)
            {
                return conn_->template try_aggregate<DTO, ResultT>(agg_expr);
            }
            else
            {
                return conn_->template try_aggregate_where<DTO, ResultT>(agg_expr, where_cond_);
            }
        }

        template<typename KeyTuple, typename BaseQuery>
        [[nodiscard]] std::expected<std::vector<KeyTuple>, Core::DbError> execute_paginated_delete_with_limit_offset(
            ConnectionHandle& conn, BaseQuery&& base_query) const
        {
            auto run = [&](auto&& q) -> std::expected<std::vector<KeyTuple>, Core::DbError>
            {
                if constexpr (detail::is_custom_where_cond_v<WhereCondT>)
                {
                    if constexpr (requires {
                                      conn.template delete_where_paginated_returning_keys_where<DTO, KeyTuple>(std::move(q), where_cond_);
                                  })
                    {
                        return conn.template delete_where_paginated_returning_keys_where<DTO, KeyTuple>(std::move(q), where_cond_);
                    }
                    else
                    {
                        return std::unexpected<Core::DbError>{ Core::DbError::unknown(
                            "Connection adapter does not support escaped LIKE predicates") };
                    }
                }
                else
                {
                    return conn.template delete_where_paginated_returning_keys<DTO, KeyTuple>(std::move(q));
                }
            };

            if (limit_.has_value() && offset_.has_value())
            {
                return run(std::forward<BaseQuery>(base_query) | sqlgen::limit(*limit_) | sqlgen::offset(*offset_));
            }
            if (limit_.has_value())
            {
                return run(std::forward<BaseQuery>(base_query) | sqlgen::limit(*limit_));
            }
            if (offset_.has_value())
            {
                return run(std::forward<BaseQuery>(base_query) | sqlgen::limit(detail::kSqliteUnboundedLimit) | sqlgen::offset(*offset_));
            }
            return run(std::forward<BaseQuery>(base_query));
        }

        template<typename KeyTuple>
        [[nodiscard]] std::expected<std::vector<KeyTuple>, Core::DbError> execute_paginated_delete(ConnectionHandle& conn) const
        {
            auto with_where = [&]()
            {
                if constexpr (!std::same_as<WhereCondT, rfl::Nothing> && !detail::is_custom_where_cond_v<WhereCondT>)
                {
                    return sqlgen::read<std::vector<DTO>> | sqlgen::where(where_cond_);
                }
                else
                {
                    return sqlgen::read<std::vector<DTO>>;
                }
            }();

            if constexpr (!std::same_as<OrderByT, rfl::Nothing>)
            {
                return std::visit(
                    [&](const auto& ob)
                    {
                        return execute_paginated_delete_with_limit_offset<KeyTuple>(conn, with_where | ob);
                    },
                    order_by_);
            }
            else
            {
                return execute_paginated_delete_with_limit_offset<KeyTuple>(conn, std::move(with_where));
            }
        }

        template<typename BaseQuery>
        [[nodiscard]] std::expected<std::vector<DTO>, Core::DbError> execute_with_limit_offset(
            const ConnectionHandle& conn, BaseQuery&& base_query) const
        {
            auto run = [&](auto&& q) -> std::expected<std::vector<DTO>, Core::DbError>
            {
                if constexpr (detail::is_custom_where_cond_v<WhereCondT>)
                {
                    if constexpr (requires { conn.template try_execute_read_where<DTO, DTO>(std::move(q), where_cond_); })
                    {
                        return conn.template try_execute_read_where<DTO, DTO>(std::move(q), where_cond_);
                    }
                    else
                    {
                        return std::unexpected<Core::DbError>{ Core::DbError::unknown(
                            "Connection adapter does not support escaped LIKE predicates") };
                    }
                }
                else
                {
                    return conn.template try_execute_read<DTO>(std::move(q));
                }
            };

            if (limit_.has_value() && offset_.has_value())
            {
                return run(std::forward<BaseQuery>(base_query) | sqlgen::limit(*limit_) | sqlgen::offset(*offset_));
            }
            if (limit_.has_value())
            {
                return run(std::forward<BaseQuery>(base_query) | sqlgen::limit(*limit_));
            }
            if (offset_.has_value())
            {
                return run(std::forward<BaseQuery>(base_query) | sqlgen::limit(detail::kSqliteUnboundedLimit) | sqlgen::offset(*offset_));
            }
            return run(std::forward<BaseQuery>(base_query));
        }

        [[nodiscard]] std::expected<std::vector<DTO>, Core::DbError> execute_query(const ConnectionHandle& conn) const
        {
            auto with_where = [&]()
            {
                if constexpr (!std::same_as<WhereCondT, rfl::Nothing> && !detail::is_custom_where_cond_v<WhereCondT>)
                {
                    return sqlgen::read<std::vector<DTO>> | sqlgen::where(where_cond_);
                }
                else
                {
                    return sqlgen::read<std::vector<DTO>>;
                }
            }();

            if constexpr (!std::same_as<OrderByT, rfl::Nothing>)
            {
                return std::visit(
                    [&](const auto& ob)
                    {
                        return execute_with_limit_offset(conn, with_where | ob);
                    },
                    order_by_);
            }
            else
            {
                return execute_with_limit_offset(conn, std::move(with_where));
            }
        }

        std::shared_ptr<RepoType> repo_;
        std::shared_ptr<ConnectionHandle> conn_;
        WhereCondT where_cond_{};
        OrderByT order_by_{};
        std::optional<std::size_t> limit_{};
        std::optional<std::size_t> offset_{};
        TrackFn track_fn_{};
        ClearTrackFn clear_track_fn_{};
        UntrackKeyFn untrack_key_fn_{};
        IsContextAliveFn is_context_alive_fn_{};
        std::optional<Core::DbError> validation_error_{};
    };
} // namespace DAL
