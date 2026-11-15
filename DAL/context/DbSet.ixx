module;
#include <sqlgen.hpp>

export module dal:db_set;

import std;
import core;
import :schema_traits;
import :mappers;
import :generic_repo;
import :sqlite_connection;

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
            else if constexpr (requires { val.get(); })
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
    } // namespace detail

    // Fluent server-side query builder for DbSet (Phases 10, 12, 13, 14).
    // Translates filtering, sorting, pagination, aggregations, and bulk deletes into sqlgen pipelines.
    template<typename Domain,
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

        DbQuery(std::shared_ptr<RepoType> repo,
                std::shared_ptr<ConnectionHandle> conn,
                WhereCondT where_cond = {},
                OrderByT order_by = {},
                std::optional<std::size_t> limit = std::nullopt,
                std::optional<std::size_t> offset = std::nullopt,
                TrackFn track_fn = {},
                ClearTrackFn clear_track_fn = {})
            : repo_(std::move(repo))
            , conn_(std::move(conn))
            , where_cond_(std::move(where_cond))
            , order_by_(std::move(order_by))
            , limit_(limit)
            , offset_(offset)
            , track_fn_(std::move(track_fn))
            , clear_track_fn_(std::move(clear_track_fn))
        {
        }

        template<typename Condition>
        [[nodiscard]] auto where_sql(Condition cond) const
        {
            if constexpr (std::same_as<WhereCondT, rfl::Nothing>)
            {
                return DbQuery<Domain, DTO, ConnectionHandle, KeyType, Condition, OrderByT>(
                    repo_, conn_, std::move(cond), order_by_, limit_, offset_, track_fn_, clear_track_fn_);
            }
            else
            {
                auto combined = where_cond_ && std::move(cond);
                using CombinedT = decltype(combined);
                return DbQuery<Domain, DTO, ConnectionHandle, KeyType, CombinedT, OrderByT>(
                    repo_, conn_, std::move(combined), order_by_, limit_, offset_, track_fn_, clear_track_fn_);
            }
        }

        template<typename Condition>
        [[nodiscard]] auto or_where_sql(Condition cond) const
        {
            if constexpr (std::same_as<WhereCondT, rfl::Nothing>)
            {
                return where_sql(std::move(cond));
            }
            else
            {
                auto combined = where_cond_ || std::move(cond);
                using CombinedT = decltype(combined);
                return DbQuery<Domain, DTO, ConnectionHandle, KeyType, CombinedT, OrderByT>(
                    repo_, conn_, std::move(combined), order_by_, limit_, offset_, track_fn_, clear_track_fn_);
            }
        }

        template<rfl::internal::StringLiteral ColLiteral, typename Val>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_equal(const Val& val) const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            return where_sql(Schema::col_of<dto_m> == detail::normalize_query_val(val));
        }

        template<rfl::internal::StringLiteral ColLiteral, typename Val>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto or_where_equal(const Val& val) const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            return or_where_sql(Schema::col_of<dto_m> == detail::normalize_query_val(val));
        }

        template<rfl::internal::StringLiteral ColLiteral, typename Val>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_not_equal(const Val& val) const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            return where_sql(Schema::col_of<dto_m> != detail::normalize_query_val(val));
        }

        template<rfl::internal::StringLiteral ColLiteral, typename Val>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_lt(const Val& val) const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            return where_sql(Schema::col_of<dto_m> < detail::normalize_query_val(val));
        }

        template<rfl::internal::StringLiteral ColLiteral, typename Val>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_le(const Val& val) const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            return where_sql(Schema::col_of<dto_m> <= detail::normalize_query_val(val));
        }

        template<rfl::internal::StringLiteral ColLiteral, typename Val>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_gt(const Val& val) const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            return where_sql(Schema::col_of<dto_m> > detail::normalize_query_val(val));
        }

        template<rfl::internal::StringLiteral ColLiteral, typename Val>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_ge(const Val& val) const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            return where_sql(Schema::col_of<dto_m> >= detail::normalize_query_val(val));
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_like(std::string_view pattern) const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            return where_sql(Schema::col_of<dto_m>.like(std::string(pattern)));
        }

        template<rfl::internal::StringLiteral ColLiteral, typename... Ts>
            requires (sizeof...(Ts) > 0) && DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_in(const Ts&... vals) const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            return where_sql(Schema::col_of<dto_m>.in(detail::normalize_query_val(vals)...));
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
            return where_sql(Schema::col_of<dto_m>.in(normalized));
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
            requires (sizeof...(Ts) > 0) && DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_not_in(const Ts&... vals) const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            return where_sql(Schema::col_of<dto_m>.not_in(detail::normalize_query_val(vals)...));
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
                repo_, conn_, where_cond_, std::move(ob), limit_, offset_, track_fn_, clear_track_fn_);
        }

        [[nodiscard]] DbQuery limit(std::size_t limit_count) const
        {
            return DbQuery(repo_, conn_, where_cond_, order_by_, limit_count, offset_, track_fn_, clear_track_fn_);
        }

        [[nodiscard]] DbQuery offset(std::size_t offset_count) const
        {
            return DbQuery(repo_, conn_, where_cond_, order_by_, limit_, offset_count, track_fn_, clear_track_fn_);
        }

        [[nodiscard]] DbQuery page(std::size_t limit_count, std::size_t offset_count = 0) const
        {
            return DbQuery(repo_, conn_, where_cond_, order_by_, limit_count, offset_count, track_fn_, clear_track_fn_);
        }

        [[nodiscard]] std::expected<std::vector<Domain>, Core::DbError> try_to_vector() const
        {
            if (!conn_)
            {
                return std::unexpected<Core::DbError>{
                    Core::DbError::connection_error("Database connection is null")
                };
            }

            if constexpr (requires(const ConnectionHandle& c) {
                c.template try_execute_read<DTO>(sqlgen::read<std::vector<DTO>>);
            })
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
                return repo_->try_get_all();
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

        [[nodiscard]] std::optional<Domain> first_or_default() const
        {
            auto items = limit(1).to_vector();
            if (items.empty())
            {
                return std::nullopt;
            }
            return std::move(items.front());
        }

        [[nodiscard]] std::expected<std::size_t, Core::DbError> try_count() const
        {
            if (!conn_)
            {
                return std::unexpected<Core::DbError>{
                    Core::DbError::connection_error("Database connection is null")
                };
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
                    if constexpr (requires(const ConnectionHandle& c) {
                        c.template try_count_where<DTO>(where_cond_);
                    })
                    {
                        return conn_->template try_count_where<DTO>(where_cond_);
                    }
                }
            }

            auto res = try_to_vector();
            if (!res)
            {
                return std::unexpected<Core::DbError>{ res.error() };
            }
            return res.value().size();
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
            return try_aggregate_impl<ScalarT>(sqlgen::sum(Schema::col_of<dto_m>));
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

        [[nodiscard]] bool any() const
        {
            auto res = limit(1).try_to_vector();
            return res.has_value() && !res.value().empty();
        }

        std::expected<void, Core::DbError> remove_where() const
            requires (!std::same_as<WhereCondT, rfl::Nothing>)
        {
            if (!conn_)
            {
                return std::unexpected<Core::DbError>{
                    Core::DbError::connection_error("Database connection is null")
                };
            }

            if constexpr (requires(ConnectionHandle& c) {
                c.template delete_where<DTO>(where_cond_);
            })
            {
                auto del_res = conn_->template delete_where<DTO>(where_cond_);
                if (del_res && clear_track_fn_)
                {
                    clear_track_fn_();
                }
                return del_res;
            }
            else
            {
                auto res = try_to_vector();
                if (!res)
                {
                    return std::unexpected<Core::DbError>{ res.error() };
                }
                for (const auto& item : res.value())
                {
                    auto key = Schema::EntityMetadata<Domain, DTO>::extract_key(item);
                    auto del_res = repo_->delete_by_id(key);
                    if (!del_res)
                    {
                        return del_res;
                    }
                }
                if (clear_track_fn_)
                {
                    clear_track_fn_();
                }
                return {};
            }
        }

    private:
        template<typename ResultT, typename AggExpr>
        [[nodiscard]] std::expected<std::optional<ResultT>, Core::DbError> try_aggregate_impl(
            const AggExpr& agg_expr) const
        {
            if (!conn_)
            {
                return std::unexpected<Core::DbError>{
                    Core::DbError::connection_error("Database connection is null")
                };
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

        template<typename BaseQuery>
        [[nodiscard]] std::expected<std::vector<DTO>, Core::DbError> execute_with_limit_offset(
            const ConnectionHandle& conn,
            BaseQuery&& base_query) const
        {
            if (limit_.has_value() && offset_.has_value())
            {
                auto q = std::forward<BaseQuery>(base_query)
                       | sqlgen::limit(*limit_)
                       | sqlgen::offset(*offset_);
                return conn.template try_execute_read<DTO>(std::move(q));
            }
            if (limit_.has_value())
            {
                auto q = std::forward<BaseQuery>(base_query) | sqlgen::limit(*limit_);
                return conn.template try_execute_read<DTO>(std::move(q));
            }
            if (offset_.has_value())
            {
                auto q = std::forward<BaseQuery>(base_query) | sqlgen::offset(*offset_);
                return conn.template try_execute_read<DTO>(std::move(q));
            }
            return conn.template try_execute_read<DTO>(std::forward<BaseQuery>(base_query));
        }

        [[nodiscard]] std::expected<std::vector<DTO>, Core::DbError> execute_query(
            const ConnectionHandle& conn) const
        {
            auto with_where = [&]() {
                if constexpr (!std::same_as<WhereCondT, rfl::Nothing>)
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
                    [&](const auto& ob) {
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
    };
    // DbSet<Domain, DTO, ConnectionHandle, KeyType> provides an Entity Framework-style strongly-typed
    // entity set abstraction over a relational table, eliminating manual repository wiring.
    template<typename Domain,
             typename DTO = Schema::SchemaOf_t<Domain>,
             typename ConnectionHandle = SQLiteConnection,
             typename KeyType = Schema::EntityKey_t<Domain, DTO>>
        requires DAL::Mappers::Mappable<Domain, DTO> && Repositories::DatabaseConnection<ConnectionHandle>
    class DbSet
    {
    public:
        using DomainType = Domain;
        using DTOType = DTO;
        using KeyValueType = KeyType;
        using RepoType = Repositories::GenericRepository<Domain, DTO, ConnectionHandle, KeyType>;

        explicit DbSet(std::shared_ptr<ConnectionHandle> conn)
            : repo_(std::make_shared<RepoType>(conn))
            , conn_(std::move(conn))
        {
        }

        template<typename Ctx>
            requires requires(Ctx& ctx, std::function<std::expected<void, Core::DbError>()> op) {
                { ctx.connection() } -> std::convertible_to<std::shared_ptr<ConnectionHandle>>;
                { ctx.is_deferred() } -> std::same_as<bool>;
                ctx.enqueue_op(std::move(op));
            }
        explicit DbSet(Ctx* ctx)
            : repo_(std::make_shared<RepoType>(ctx->connection()))
            , conn_(ctx->connection())
            , is_deferred_fn_([ctx]() { return ctx->is_deferred(); })
            , enqueue_op_fn_([ctx](std::function<std::expected<void, Core::DbError>()> op) {
                ctx->enqueue_op(std::move(op));
            })
        {
            if constexpr (requires(Ctx& c) {
                c.is_change_tracking_enabled();
                c.clear_change_tracker();
            })
            {
                is_tracking_enabled_fn_ = [ctx]() { return ctx->is_change_tracking_enabled(); };
                upsert_tracked_fn_ = [ctx](
                    std::type_index type,
                    std::string key_repr,
                    bool refresh_baseline,
                    std::shared_ptr<void> owned,
                    const void* borrowed,
                    std::function<std::optional<std::function<std::expected<void, Core::DbError>()>>(
                        const std::shared_ptr<void>&, const void*)> detect_fn)
                {
                    ctx->upsert_tracked_entry(
                        type,
                        std::move(key_repr),
                        refresh_baseline,
                        std::move(owned),
                        borrowed,
                        std::move(detect_fn));
                };
                untrack_fn_ = [ctx](std::type_index type, const std::string& key_repr) {
                    ctx->untrack_key(type, key_repr);
                };
                untrack_type_fn_ = [ctx](std::type_index type) {
                    ctx->untrack_type(type);
                };
                find_tracked_fn_ = [ctx](std::type_index type, const std::string& key_repr) {
                    return ctx->find_tracked_instance(type, key_repr);
                };
            }
        }

        template<typename Ctx>
            requires requires(const Ctx& ctx) {
                { ctx.connection() } -> std::convertible_to<std::shared_ptr<ConnectionHandle>>;
            } && (!requires(Ctx& ctx) { ctx.is_deferred(); })
        explicit DbSet(const Ctx* ctx)
            : repo_(std::make_shared<RepoType>(ctx->connection()))
            , conn_(ctx->connection())
        {
        }

        // Retrieves all domain entities from the underlying table
        [[nodiscard]] std::vector<Domain> to_vector() const
        {
            auto items = repo_->get_all();
            if (is_tracking_enabled())
            {
                for (const auto& item : items)
                {
                    record_snapshot(item);
                }
            }
            return items;
        }

        // Monadic read: retrieves all domain entities or returns a structured DbError
        [[nodiscard]] std::expected<std::vector<Domain>, Core::DbError> try_to_vector() const
        {
            auto res = repo_->try_get_all();
            if (res && is_tracking_enabled())
            {
                for (const auto& item : res.value())
                {
                    record_snapshot(item);
                }
            }
            return res;
        }

        // Finds an entity by its primary key (SingleKey, CompositeKey, or scalar)
        [[nodiscard]] std::optional<Domain> find(const KeyType& id) const
        {
            auto item = repo_->get_by_id(id);
            if (item && is_tracking_enabled())
            {
                record_snapshot(*item);
            }
            return item;
        }

        template<typename K>
            requires Core::SingleKeyType<KeyType>
                  && std::constructible_from<typename KeyType::key_type, K>
                  && (!std::same_as<std::remove_cvref_t<K>, KeyType>)
        [[nodiscard]] std::optional<Domain> find(const K& raw_or_id) const
        {
            return find(KeyType{ static_cast<typename KeyType::key_type>(raw_or_id) });
        }

        template<typename... Ks>
            requires Core::CompositeKeyType<KeyType>
                  && (sizeof...(Ks) == KeyType::arity)
                  && std::constructible_from<KeyType, Ks...>
        [[nodiscard]] std::optional<Domain> find(Ks&&... parts) const
        {
            return find(KeyType{ std::forward<Ks>(parts)... });
        }

        // Monadic read: finds an entity by its primary key or returns a structured DbError
        [[nodiscard]] std::expected<std::optional<Domain>, Core::DbError> try_find(const KeyType& id) const
        {
            auto res = repo_->try_get_by_id(id);
            if (res && res.value() && is_tracking_enabled())
            {
                record_snapshot(*res.value());
            }
            return res;
        }

        template<typename K>
            requires Core::SingleKeyType<KeyType>
                  && std::constructible_from<typename KeyType::key_type, K>
                  && (!std::same_as<std::remove_cvref_t<K>, KeyType>)
        [[nodiscard]] std::expected<std::optional<Domain>, Core::DbError> try_find(const K& raw_or_id) const
        {
            return try_find(KeyType{ static_cast<typename KeyType::key_type>(raw_or_id) });
        }

        template<typename... Ks>
            requires Core::CompositeKeyType<KeyType>
                  && (sizeof...(Ks) == KeyType::arity)
                  && std::constructible_from<KeyType, Ks...>
        [[nodiscard]] std::expected<std::optional<Domain>, Core::DbError> try_find(Ks&&... parts) const
        {
            return try_find(KeyType{ std::forward<Ks>(parts)... });
        }

        // Inserts a single entity into the database table (or stages if deferred)
        std::expected<void, Core::DbError> add(const Domain& item)
        {
            if (should_defer())
            {
                return stage_add(item);
            }
            return repo_->insert_one(item);
        }

        // Inserts a single entity immediately and returns a copy populated with any
        // database-generated auto-increment primary key.
        [[nodiscard]] std::expected<Domain, Core::DbError> add_returning(const Domain& item)
        {
            auto ins_res = repo_->insert_one(item);
            if (!ins_res)
            {
                return std::unexpected<Core::DbError>{ ins_res.error() };
            }

            Domain result = item;
            if constexpr (Schema::SinglePrimaryKey<DTO> && Schema::has_auto_incr_primary_key<DTO>())
            {
                if (!conn_)
                {
                    return std::unexpected<Core::DbError>{
                        Core::DbError::connection_error("Database connection is null")
                    };
                }
                if constexpr (requires(const ConnectionHandle& c) { c.try_last_insert_rowid(); })
                {
                    auto rowid_res = conn_->try_last_insert_rowid();
                    if (!rowid_res)
                    {
                        return std::unexpected<Core::DbError>{ rowid_res.error() };
                    }
                    result = DAL::Mappers::MapperTraits<Domain, DTO>::with_primary_key(item, *rowid_res);
                }
            }

            record_snapshot(result);
            return result;
        }

        // Inserts multiple entities into the database table (or stages if deferred)
        std::expected<void, Core::DbError> add_range(const std::vector<Domain>& items)
        {
            if (should_defer())
            {
                return stage_add_range(items);
            }
            return repo_->insert_many(items);
        }

        // Updates an existing entity (or stages if deferred)
        std::expected<void, Core::DbError> update(const Domain& item)
        {
            if (should_defer())
            {
                return stage_update(item);
            }
            return repo_->update_one(item);
        }

        // Removes an entity by primary key (or stages if deferred)
        std::expected<void, Core::DbError> remove(const KeyType& id)
        {
            untrack_key(id);
            if (should_defer())
            {
                return stage_remove(id);
            }
            return repo_->delete_by_id(id);
        }

        // Atomic upsert (insert or replace) for a single entity (Phase 13.1)
        std::expected<void, Core::DbError> upsert(const Domain& item)
        {
            if (should_defer())
            {
                return stage_upsert(item);
            }
            return upsert_immediate(item);
        }

        // Atomic batch upsert (insert or replace) for multiple entities (Phase 13.1)
        std::expected<void, Core::DbError> upsert_range(const std::vector<Domain>& items)
        {
            if (should_defer())
            {
                return stage_upsert_range(items);
            }
            return upsert_many_immediate(items);
        }

        std::expected<void, Core::DbError> stage_upsert(const Domain& item)
        {
            if (!enqueue_op_fn_)
            {
                return std::unexpected<Core::DbError>{
                    Core::DbError::connection_error("Cannot stage operation: DbSet is not bound to a DbContext")
                };
            }
            enqueue_op_fn_([this, item]() { return upsert_immediate(item); });
            return {};
        }

        std::expected<void, Core::DbError> stage_upsert_range(const std::vector<Domain>& items)
        {
            if (!enqueue_op_fn_)
            {
                return std::unexpected<Core::DbError>{
                    Core::DbError::connection_error("Cannot stage operation: DbSet is not bound to a DbContext")
                };
            }
            enqueue_op_fn_([this, items]() { return upsert_many_immediate(items); });
            return {};
        }

        // Explicit Unit of Work staging methods (always enqueue into DbContext)
        std::expected<void, Core::DbError> stage_add(const Domain& item)
        {
            if (!enqueue_op_fn_)
            {
                return std::unexpected<Core::DbError>{
                    Core::DbError::connection_error("Cannot stage operation: DbSet is not bound to a DbContext")
                };
            }
            enqueue_op_fn_([repo = repo_, item]() { return repo->insert_one(item); });
            return {};
        }

        std::expected<void, Core::DbError> stage_add_range(const std::vector<Domain>& items)
        {
            if (!enqueue_op_fn_)
            {
                return std::unexpected<Core::DbError>{
                    Core::DbError::connection_error("Cannot stage operation: DbSet is not bound to a DbContext")
                };
            }
            enqueue_op_fn_([repo = repo_, items]() { return repo->insert_many(items); });
            return {};
        }

        std::expected<void, Core::DbError> stage_update(const Domain& item)
        {
            if (!enqueue_op_fn_)
            {
                return std::unexpected<Core::DbError>{
                    Core::DbError::connection_error("Cannot stage operation: DbSet is not bound to a DbContext")
                };
            }
            enqueue_op_fn_([repo = repo_, item]() { return repo->update_one(item); });
            return {};
        }

        std::expected<void, Core::DbError> stage_remove(const KeyType& id)
        {
            if (!enqueue_op_fn_)
            {
                return std::unexpected<Core::DbError>{
                    Core::DbError::connection_error("Cannot stage operation: DbSet is not bound to a DbContext")
                };
            }
            untrack_key(id);
            enqueue_op_fn_([repo = repo_, id]() { return repo->delete_by_id(id); });
            return {};
        }

        template<typename K>
            requires Core::SingleKeyType<KeyType>
                  && std::constructible_from<typename KeyType::key_type, K>
                  && (!std::same_as<std::remove_cvref_t<K>, KeyType>)
        std::expected<void, Core::DbError> stage_remove(const K& raw_or_id)
        {
            return stage_remove(KeyType{ static_cast<typename KeyType::key_type>(raw_or_id) });
        }

        template<typename... Ks>
            requires Core::CompositeKeyType<KeyType>
                  && (sizeof...(Ks) == KeyType::arity)
                  && std::constructible_from<KeyType, Ks...>
        std::expected<void, Core::DbError> stage_remove(Ks&&... parts)
        {
            return stage_remove(KeyType{ std::forward<Ks>(parts)... });
        }

        template<typename K>
            requires Core::SingleKeyType<KeyType>
                  && std::constructible_from<typename KeyType::key_type, K>
                  && (!std::same_as<std::remove_cvref_t<K>, KeyType>)
        std::expected<void, Core::DbError> remove(const K& raw_or_id)
        {
            return remove(KeyType{ static_cast<typename KeyType::key_type>(raw_or_id) });
        }

        template<typename... Ks>
            requires Core::CompositeKeyType<KeyType>
                  && (sizeof...(Ks) == KeyType::arity)
                  && std::constructible_from<KeyType, Ks...>
        std::expected<void, Core::DbError> remove(Ks&&... parts)
        {
            return remove(KeyType{ std::forward<Ks>(parts)... });
        }

        // Checks whether an entity with the given primary key exists
        [[nodiscard]] bool contains(const KeyType& id) const
        {
            return repo_->exists_by_id(id);
        }

        template<typename K>
            requires Core::SingleKeyType<KeyType>
                  && std::constructible_from<typename KeyType::key_type, K>
                  && (!std::same_as<std::remove_cvref_t<K>, KeyType>)
        [[nodiscard]] bool contains(const K& raw_or_id) const
        {
            return contains(KeyType{ static_cast<typename KeyType::key_type>(raw_or_id) });
        }

        template<typename... Ks>
            requires Core::CompositeKeyType<KeyType>
                  && (sizeof...(Ks) == KeyType::arity)
                  && std::constructible_from<KeyType, Ks...>
        [[nodiscard]] bool contains(Ks&&... parts) const
        {
            return contains(KeyType{ std::forward<Ks>(parts)... });
        }

        // Returns total row count for this entity table
        [[nodiscard]] std::size_t count() const
        {
            return repo_->count();
        }

        // In-memory LINQ-style filter: db.set.where([](const auto& x) { return ...; })
        template<typename Predicate>
        [[nodiscard]] std::vector<Domain> where(Predicate&& pred) const
        {
            auto all = to_vector();
            std::vector<Domain> filtered;
            for (auto& item : all)
            {
                if (std::invoke(pred, item))
                {
                    filtered.push_back(std::move(item));
                }
            }
            return filtered;
        }

        // First matching element, or std::nullopt
        template<typename Predicate>
        [[nodiscard]] std::optional<Domain> first_or_default(Predicate&& pred) const
        {
            auto all = to_vector();
            for (auto& item : all)
            {
                if (std::invoke(pred, item))
                {
                    return std::move(item);
                }
            }
            return std::nullopt;
        }

        // =====================================================================
        // Task 10.1: Fluent Server-Side Query Pushdown
        // =====================================================================

        [[nodiscard]] auto query() const
        {
            std::function<void(const Domain&)> track_cb{};
            std::function<void()> clear_cb{};
            if (is_tracking_enabled())
            {
                track_cb = [this](const Domain& entity) { record_snapshot(entity); };
                if (untrack_type_fn_)
                {
                    clear_cb = [fn = untrack_type_fn_]() { fn(std::type_index(typeid(Domain))); };
                }
            }
            return DbQuery<Domain, DTO, ConnectionHandle, KeyType>(
                repo_, conn_, rfl::Nothing{}, rfl::Nothing{}, std::nullopt, std::nullopt, std::move(track_cb), std::move(clear_cb));
        }

        template<typename Condition>
        [[nodiscard]] auto where_sql(Condition cond) const
        {
            return query().where_sql(std::move(cond));
        }

        template<rfl::internal::StringLiteral ColLiteral, typename Val>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_equal(const Val& val) const
        {
            return query().template where_equal<ColLiteral>(val);
        }

        template<rfl::internal::StringLiteral ColLiteral, typename Val>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_not_equal(const Val& val) const
        {
            return query().template where_not_equal<ColLiteral>(val);
        }

        template<rfl::internal::StringLiteral ColLiteral, typename Val>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_lt(const Val& val) const
        {
            return query().template where_lt<ColLiteral>(val);
        }

        template<rfl::internal::StringLiteral ColLiteral, typename Val>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_le(const Val& val) const
        {
            return query().template where_le<ColLiteral>(val);
        }

        template<rfl::internal::StringLiteral ColLiteral, typename Val>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_gt(const Val& val) const
        {
            return query().template where_gt<ColLiteral>(val);
        }

        template<rfl::internal::StringLiteral ColLiteral, typename Val>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_ge(const Val& val) const
        {
            return query().template where_ge<ColLiteral>(val);
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_like(std::string_view pattern) const
        {
            return query().template where_like<ColLiteral>(pattern);
        }

        template<rfl::internal::StringLiteral ColLiteral, typename... Ts>
            requires (sizeof...(Ts) > 0) && DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_in(const Ts&... vals) const
        {
            return query().template where_in<ColLiteral>(vals...);
        }

        template<rfl::internal::StringLiteral ColLiteral, typename Val>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_in_vec(const std::vector<Val>& vals) const
        {
            return query().template where_in_vec<ColLiteral>(vals);
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_null() const
        {
            return query().template where_null<ColLiteral>();
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_not_null() const
        {
            return query().template where_not_null<ColLiteral>();
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_not_like(std::string_view pattern) const
        {
            return query().template where_not_like<ColLiteral>(pattern);
        }

        template<rfl::internal::StringLiteral ColLiteral, typename... Ts>
            requires (sizeof...(Ts) > 0) && DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_not_in(const Ts&... vals) const
        {
            return query().template where_not_in<ColLiteral>(vals...);
        }

        template<rfl::internal::StringLiteral ColLiteral, typename Val>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        std::expected<void, Core::DbError> remove_where_equal(const Val& val) const
        {
            return where_equal<ColLiteral>(val).remove_where();
        }

        template<typename ParentDomain>
        [[nodiscard]] auto where_foreign_key(const ParentDomain& parent) const
        {
            const auto parent_key = Schema::EntityMetadata<ParentDomain>::extract_key(parent);
            const auto raw_pk = detail::normalize_query_val(parent_key);
            return where_sql(Schema::make_fk_where<DTO, Schema::SchemaOf_t<ParentDomain>>(raw_pk));
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto order_by(bool desc = false) const
        {
            return query().template order_by<ColLiteral>(desc);
        }

        [[nodiscard]] auto page(std::size_t limit_count, std::size_t offset_count = 0) const
        {
            return query().page(limit_count, offset_count);
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto try_min() const
        {
            return query().template try_min<ColLiteral>();
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto min() const
        {
            return query().template min<ColLiteral>();
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto try_max() const
        {
            return query().template try_max<ColLiteral>();
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto max() const
        {
            return query().template max<ColLiteral>();
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto try_sum() const
        {
            return query().template try_sum<ColLiteral>();
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto sum() const
        {
            return query().template sum<ColLiteral>();
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] std::expected<std::optional<double>, Core::DbError> try_avg() const
        {
            return query().template try_avg<ColLiteral>();
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] std::optional<double> avg() const
        {
            return query().template avg<ColLiteral>();
        }

        // =====================================================================
        // Task 10.2: Identity Map & Snapshot ChangeTracker Helpers
        // =====================================================================

        // Binds a caller-owned Domain instance to the context's ChangeTracker.
        // If find()/to_vector() previously recorded an original snapshot for this key,
        // that original snapshot is preserved so mutations before or after track() are detected.
        void track(Domain& entity) const
        {
            if (!upsert_tracked_fn_)
            {
                return;
            }
            auto key = Schema::EntityMetadata<Domain, DTO>::extract_key(entity);
            auto baseline = std::make_shared<DTO>(DAL::Mappers::MapperTraits<Domain, DTO>::to_dto(entity));
            upsert_tracked_fn_(
                std::type_index(typeid(Domain)),
                format_key(key),
                false,
                nullptr,
                &entity,
                make_detect_fn(std::move(baseline)));
        }

        // Loads an entity into the identity map and returns a shared_ptr whose mutations
        // are automatically detected and persisted on ctx.save_changes().
        [[nodiscard]] std::shared_ptr<Domain> find_tracked(const KeyType& id) const
        {
            const std::string key_repr = format_key(id);
            if (find_tracked_fn_)
            {
                if (auto existing = find_tracked_fn_(std::type_index(typeid(Domain)), key_repr))
                {
                    return std::static_pointer_cast<Domain>(existing);
                }
            }
            auto loaded = repo_->get_by_id(id);
            if (!loaded)
            {
                return nullptr;
            }
            auto shared = std::make_shared<Domain>(std::move(*loaded));
            if (upsert_tracked_fn_)
            {
                auto baseline = std::make_shared<DTO>(DAL::Mappers::MapperTraits<Domain, DTO>::to_dto(*shared));
                upsert_tracked_fn_(
                    std::type_index(typeid(Domain)),
                    key_repr,
                    true,
                    shared,
                    nullptr,
                    make_detect_fn(std::move(baseline)));
            }
            return shared;
        }

        template<typename K>
            requires Core::SingleKeyType<KeyType>
                  && std::constructible_from<typename KeyType::key_type, K>
                  && (!std::same_as<std::remove_cvref_t<K>, KeyType>)
        [[nodiscard]] std::shared_ptr<Domain> find_tracked(const K& raw_or_id) const
        {
            return find_tracked(KeyType{ static_cast<typename KeyType::key_type>(raw_or_id) });
        }

        template<typename... Ks>
            requires Core::CompositeKeyType<KeyType>
                  && (sizeof...(Ks) == KeyType::arity)
                  && std::constructible_from<KeyType, Ks...>
        [[nodiscard]] std::shared_ptr<Domain> find_tracked(Ks&&... parts) const
        {
            return find_tracked(KeyType{ std::forward<Ks>(parts)... });
        }

        [[nodiscard]] std::vector<std::shared_ptr<Domain>> to_tracked_vector() const
        {
            auto items = repo_->get_all();
            std::vector<std::shared_ptr<Domain>> result;
            result.reserve(items.size());
            for (auto& item : items)
            {
                auto key = Schema::EntityMetadata<Domain, DTO>::extract_key(item);
                const std::string key_repr = format_key(key);
                if (find_tracked_fn_)
                {
                    if (auto existing = find_tracked_fn_(std::type_index(typeid(Domain)), key_repr))
                    {
                        result.push_back(std::static_pointer_cast<Domain>(existing));
                        continue;
                    }
                }
                auto shared = std::make_shared<Domain>(std::move(item));
                if (upsert_tracked_fn_)
                {
                    auto baseline = std::make_shared<DTO>(DAL::Mappers::MapperTraits<Domain, DTO>::to_dto(*shared));
                    upsert_tracked_fn_(
                        std::type_index(typeid(Domain)),
                        key_repr,
                        true,
                        shared,
                        nullptr,
                        make_detect_fn(std::move(baseline)));
                }
                result.push_back(std::move(shared));
            }
            return result;
        }

        // Exposes underlying generic repository when direct repository polymorphism is needed
        [[nodiscard]] std::shared_ptr<RepoType> repository() const noexcept
        {
            return repo_;
        }

    private:
        [[nodiscard]] bool should_defer() const noexcept
        {
            return is_deferred_fn_ && is_deferred_fn_();
        }

        [[nodiscard]] bool is_tracking_enabled() const noexcept
        {
            return is_tracking_enabled_fn_ && is_tracking_enabled_fn_();
        }

        [[nodiscard]] static std::string format_key(const KeyType& key)
        {
            return std::format("{}", key);
        }

        void untrack_key(const KeyType& key) const
        {
            if (untrack_fn_)
            {
                untrack_fn_(std::type_index(typeid(Domain)), format_key(key));
            }
        }

        std::expected<void, Core::DbError> upsert_immediate(const Domain& item)
        {
            if constexpr (!Schema::has_auto_incr_primary_key<DTO>())
            {
                if (conn_)
                {
                    if constexpr (requires(ConnectionHandle& c, const DTO& dto) { c.upsert(dto); })
                    {
                        const DTO dto = DAL::Mappers::MapperTraits<Domain, DTO>::to_dto(item);
                        auto res = conn_->upsert(dto);
                        if (!res)
                        {
                            return std::unexpected<Core::DbError>{
                                Core::DbError::map_from_message(res.error().what())
                            };
                        }
                        if (is_tracking_enabled())
                        {
                            record_snapshot(item);
                        }
                        return {};
                    }
                }
            }

            const auto key = Schema::EntityMetadata<Domain, DTO>::extract_key(item);
            auto res = repo_->exists_by_id(key) ? repo_->update_one(item) : repo_->insert_one(item);
            if (res && is_tracking_enabled())
            {
                record_snapshot(item);
            }
            return res;
        }

        std::expected<void, Core::DbError> upsert_many_immediate(const std::vector<Domain>& items)
        {
            if (items.empty())
            {
                return {};
            }

            if constexpr (!Schema::has_auto_incr_primary_key<DTO>())
            {
                if (conn_)
                {
                    if constexpr (requires(ConnectionHandle& c, const std::vector<DTO>& dtos) { c.upsert_many(dtos); })
                    {
                        std::vector<DTO> dtos;
                        dtos.reserve(items.size());
                        for (const auto& item : items)
                        {
                            dtos.push_back(DAL::Mappers::MapperTraits<Domain, DTO>::to_dto(item));
                        }
                        auto res = conn_->upsert_many(dtos);
                        if (!res)
                        {
                            return std::unexpected<Core::DbError>{
                                Core::DbError::map_from_message(res.error().what())
                            };
                        }
                        if (is_tracking_enabled())
                        {
                            for (const auto& item : items)
                            {
                                record_snapshot(item);
                            }
                        }
                        return {};
                    }
                }
            }

            for (const auto& item : items)
            {
                auto res = upsert_immediate(item);
                if (!res)
                {
                    return res;
                }
            }
            return {};
        }

        [[nodiscard]] auto make_detect_fn(std::shared_ptr<DTO> baseline) const
        {
            auto repo = repo_;
            return [repo, baseline = std::move(baseline)](
                const std::shared_ptr<void>& owned,
                const void* borrowed)
                -> std::optional<std::function<std::expected<void, Core::DbError>()>>
            {
                const Domain* ptr = nullptr;
                if (owned)
                {
                    ptr = static_cast<const Domain*>(owned.get());
                }
                else if (borrowed != nullptr)
                {
                    ptr = static_cast<const Domain*>(borrowed);
                }
                if (ptr == nullptr)
                {
                    return std::nullopt;
                }

                DTO current_dto = DAL::Mappers::MapperTraits<Domain, DTO>::to_dto(*ptr);
                if (DAL::Schema::dto_equals(current_dto, *baseline))
                {
                    return std::nullopt;
                }

                Domain updated_domain = *ptr;
                return [repo, updated_domain = std::move(updated_domain), current_dto = std::move(current_dto), baseline]() mutable
                    -> std::expected<void, Core::DbError>
                {
                    auto res = repo->update_one(updated_domain);
                    if (res)
                    {
                        *baseline = std::move(current_dto);
                    }
                    return res;
                };
            };
        }

        void record_snapshot(const Domain& entity) const
        {
            if (!upsert_tracked_fn_)
            {
                return;
            }
            auto key = Schema::EntityMetadata<Domain, DTO>::extract_key(entity);
            auto baseline = std::make_shared<DTO>(DAL::Mappers::MapperTraits<Domain, DTO>::to_dto(entity));
            upsert_tracked_fn_(
                std::type_index(typeid(Domain)),
                format_key(key),
                true,
                nullptr,
                nullptr,
                make_detect_fn(std::move(baseline)));
        }

        std::shared_ptr<RepoType> repo_;
        std::shared_ptr<ConnectionHandle> conn_;
        std::function<bool()> is_deferred_fn_{};
        std::function<void(std::function<std::expected<void, Core::DbError>()>)> enqueue_op_fn_{};
        std::function<bool()> is_tracking_enabled_fn_{};
        std::function<void(
            std::type_index,
            std::string,
            bool,
            std::shared_ptr<void>,
            const void*,
            std::function<std::optional<std::function<std::expected<void, Core::DbError>()>>(
                const std::shared_ptr<void>&, const void*)>)> upsert_tracked_fn_{};
        std::function<void(std::type_index, const std::string&)> untrack_fn_{};
        std::function<void(std::type_index)> untrack_type_fn_{};
        std::function<std::shared_ptr<void>(std::type_index, const std::string&)> find_tracked_fn_{};
    };

} // namespace DAL
