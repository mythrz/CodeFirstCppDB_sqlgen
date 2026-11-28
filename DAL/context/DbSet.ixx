module;
#include <sqlgen.hpp>

export module codefirst.dal:db_set;

import std;
import codefirst.core;
import :schema_traits;
import :entity_metadata;
import :mappers;
import :generic_repo;
import :sqlite_connection;
export import :change_tracker;
export import :db_query;

export namespace DAL
{
    // DbSet<Domain, DTO, ConnectionHandle, KeyType> provides an Entity Framework-style strongly-typed
    // entity set abstraction over a relational table, eliminating manual repository wiring.
    template<
        typename Domain,
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

        explicit DbSet(std::shared_ptr<ConnectionHandle> conn) pre(conn != nullptr)
            : repo_(std::make_shared<RepoType>(conn))
            , conn_(std::move(conn))
            , local_state_(std::make_shared<detail::ContextState>())
        {
            bind_tracking_callbacks(local_state_);
        }

        template<typename Ctx>
            requires requires(Ctx& ctx, std::function<std::expected<void, Core::DbError>()> op) {
                { ctx.connection() } -> std::convertible_to<std::shared_ptr<ConnectionHandle>>;
                { ctx.is_deferred() } -> std::same_as<bool>;
                ctx.enqueue_op(std::move(op));
            }
        explicit DbSet(Ctx* ctx) pre(ctx != nullptr)
            : repo_(std::make_shared<RepoType>(ctx->connection()))
            , conn_(ctx->connection())
            , bound_to_context_(true)
        {
            if constexpr (requires(Ctx& c) { c.weak_context_state(); })
            {
                const auto weak_state = ctx->weak_context_state();
                is_deferred_fn_ = [weak_state]() noexcept
                {
                    auto st = weak_state.lock();
                    return st && st->defer_changes_;
                };
                enqueue_op_fn_ = [weak_state](std::move_only_function<std::expected<void, Core::DbError>()> op) -> bool
                {
                    if (auto st = weak_state.lock())
                    {
                        st->pending_ops_.push_back(std::move(op));
                        return true;
                    }
                    return false;
                };
                bind_tracking_callbacks(weak_state);
            }
            else
            {
                is_deferred_fn_ = [ctx]()
                {
                    return ctx->is_deferred();
                };
                enqueue_op_fn_ = [ctx](std::move_only_function<std::expected<void, Core::DbError>()> op) -> bool
                {
                    ctx->enqueue_op(std::function<std::expected<void, Core::DbError>()>(std::move(op)));
                    return true;
                };
                if constexpr (requires(Ctx& c) {
                                  c.is_change_tracking_enabled();
                                  c.clear_change_tracker();
                              })
                {
                    is_tracker_alive_fn_ = []() noexcept
                    {
                        return true;
                    };
                    is_tracking_enabled_fn_ = [ctx]()
                    {
                        return ctx->is_change_tracking_enabled();
                    };
                    upsert_tracked_fn_ =
                        [ctx](
                            std::type_index type,
                            std::string key_repr,
                            bool refresh_baseline,
                            std::shared_ptr<void> owned,
                            std::function<std::optional<std::function<std::expected<void, Core::DbError>()>>(const std::shared_ptr<void>&)>
                                detect_fn,
                            std::function<std::shared_ptr<void>()> snap_fn,
                            std::function<void(const std::shared_ptr<void>&)> restore_fn)
                    {
                        ctx->upsert_tracked_entry(
                            type,
                            std::move(key_repr),
                            refresh_baseline,
                            std::move(owned),
                            std::move(detect_fn),
                            std::move(snap_fn),
                            std::move(restore_fn));
                    };
                    untrack_fn_ = [ctx](std::type_index type, const std::string& key_repr)
                    {
                        ctx->untrack_key(type, key_repr);
                    };
                    untrack_type_fn_ = [ctx](std::type_index type)
                    {
                        ctx->untrack_type(type);
                    };
                    find_tracked_fn_ = [ctx](std::type_index type, const std::string& key_repr)
                    {
                        return ctx->find_tracked_instance(type, key_repr);
                    };
                }
            }
        }

        template<typename Ctx>
            requires requires(const Ctx& ctx) {
                { ctx.connection() } -> std::convertible_to<std::shared_ptr<ConnectionHandle>>;
            } && (!requires(Ctx& ctx) { ctx.is_deferred(); })
        explicit DbSet(const Ctx* ctx) pre(ctx != nullptr)
            : repo_(std::make_shared<RepoType>(ctx->connection()))
            , conn_(ctx->connection())
            , local_state_(std::make_shared<detail::ContextState>())
        {
            bind_tracking_callbacks(local_state_);
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
            requires Core::SingleKeyType<KeyType> && std::constructible_from<typename KeyType::key_type, K> &&
            (!std::same_as<std::remove_cvref_t<K>, KeyType>)
        [[nodiscard]] std::optional<Domain> find(const K& raw_or_id) const
        {
            auto key = detail::try_make_single_key<KeyType>(raw_or_id);
            if (!key)
            {
                return std::nullopt;
            }
            return find(*key);
        }

        template<typename... Ks>
            requires Core::CompositeKeyType<KeyType> && (sizeof...(Ks) == KeyType::arity) && std::constructible_from<KeyType, Ks...>
        [[nodiscard]] std::optional<Domain> find(Ks&&... parts) const
        {
            auto key = detail::try_make_composite_key<KeyType>(parts...);
            if (!key)
            {
                return std::nullopt;
            }
            return find(*key);
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
            requires Core::SingleKeyType<KeyType> && std::constructible_from<typename KeyType::key_type, K> &&
            (!std::same_as<std::remove_cvref_t<K>, KeyType>)
        [[nodiscard]] std::expected<std::optional<Domain>, Core::DbError> try_find(const K& raw_or_id) const
        {
            auto key = detail::try_make_single_key<KeyType>(raw_or_id);
            if (!key)
            {
                return std::unexpected<Core::DbError>{ key.error() };
            }
            return try_find(*key);
        }

        template<typename... Ks>
            requires Core::CompositeKeyType<KeyType> && (sizeof...(Ks) == KeyType::arity) && std::constructible_from<KeyType, Ks...>
        [[nodiscard]] std::expected<std::optional<Domain>, Core::DbError> try_find(Ks&&... parts) const
        {
            auto key = detail::try_make_composite_key<KeyType>(parts...);
            if (!key)
            {
                return std::unexpected<Core::DbError>{ key.error() };
            }
            return try_find(*key);
        }

        // Inserts a single entity into the database table (or stages if deferred)
        [[nodiscard]] std::expected<void, Core::DbError> add(const Domain& item)
        {
            if (auto alive = check_context_alive(); !alive)
            {
                return alive;
            }
            if (should_defer())
            {
                return stage_add(item);
            }
            auto res = repo_->insert_one(item);
            if (res && is_tracking_enabled() && !has_unset_auto_incr_key(item))
            {
                record_snapshot(item);
            }
            return res;
        }

        // Inserts a single entity atomically (wrapping insert + key hydration in a
        // transaction or savepoint with RAII rollback) and returns a copy populated with any
        // database-generated auto-increment primary key.
        [[nodiscard]] std::expected<Domain, Core::DbError> add_returning(const Domain& item)
        {
            if (auto alive = check_context_alive(); !alive)
            {
                return std::unexpected<Core::DbError>{ alive.error() };
            }
            return add_returning_impl(item, true);
        }

        // Inserts multiple entities atomically in a transaction (or savepoint if already in one)
        // with RAII rollback on both returned errors and thrown exceptions, updating tracker state
        // only after commit.
        [[nodiscard]] std::expected<std::vector<Domain>, Core::DbError> add_range_returning(const std::vector<Domain>& items)
        {
            if (auto alive = check_context_alive(); !alive)
            {
                return std::unexpected<Core::DbError>{ alive.error() };
            }
            std::vector<Domain> results;
            results.reserve(items.size());
            if (items.empty())
            {
                return results;
            }

            detail::WriteBatchTxGuard<ConnectionHandle> tx_guard(conn_);
            if (auto begin_res = tx_guard.begin(); !begin_res)
            {
                return std::unexpected<Core::DbError>{ begin_res.error() };
            }

            for (const auto& item : items)
            {
                auto res = add_returning_impl(item, false);
                if (!res)
                {
                    return std::unexpected<Core::DbError>{ res.error() };
                }
                results.push_back(std::move(*res));
            }

            if (auto commit_res = tx_guard.commit(); !commit_res)
            {
                return std::unexpected<Core::DbError>{ commit_res.error() };
            }

            if (is_tracking_enabled())
            {
                for (const auto& r : results)
                {
                    record_snapshot(r);
                }
            }
            return results;
        }

        template<std::ranges::input_range R>
            requires(!std::same_as<std::remove_cvref_t<R>, std::vector<Domain>>) &&
            std::convertible_to<std::ranges::range_reference_t<R>, const Domain&>
        [[nodiscard]] std::expected<std::vector<Domain>, Core::DbError> add_range_returning(R&& items)
        {
            std::vector<Domain> vec;
            if constexpr (std::ranges::sized_range<R>)
            {
                vec.reserve(std::ranges::size(items));
            }
            for (auto&& item : items)
            {
                vec.push_back(std::forward<decltype(item)>(item));
            }
            return add_range_returning(vec);
        }

        // Inserts multiple entities atomically into the database table (or stages if deferred)
        [[nodiscard]] std::expected<void, Core::DbError> add_range(const std::vector<Domain>& items)
        {
            if (auto alive = check_context_alive(); !alive)
            {
                return alive;
            }
            if (should_defer())
            {
                return stage_add_range(items);
            }
            if (items.empty())
            {
                return {};
            }
            detail::WriteBatchTxGuard<ConnectionHandle> tx_guard(conn_);
            if (auto begin_res = tx_guard.begin(); !begin_res)
            {
                return std::unexpected<Core::DbError>{ begin_res.error() };
            }
            auto res = repo_->insert_many(items);
            if (!res)
            {
                return res;
            }
            if (auto commit_res = tx_guard.commit(); !commit_res)
            {
                return commit_res;
            }
            if (is_tracking_enabled())
            {
                for (const auto& item : items)
                {
                    if (!has_unset_auto_incr_key(item))
                    {
                        record_snapshot(item);
                    }
                }
            }
            return {};
        }

        template<std::ranges::input_range R>
            requires(!std::same_as<std::remove_cvref_t<R>, std::vector<Domain>>) &&
            std::convertible_to<std::ranges::range_reference_t<R>, const Domain&>
        [[nodiscard]] std::expected<void, Core::DbError> add_range(R&& items)
        {
            std::vector<Domain> vec;
            if constexpr (std::ranges::sized_range<R>)
            {
                vec.reserve(std::ranges::size(items));
            }
            for (auto&& item : items)
            {
                vec.push_back(std::forward<decltype(item)>(item));
            }
            return add_range(vec);
        }

        // Updates an existing entity (or stages if deferred)
        [[nodiscard]] std::expected<void, Core::DbError> update(const Domain& item)
        {
            if (auto alive = check_context_alive(); !alive)
            {
                return alive;
            }
            if (should_defer())
            {
                return stage_update(item);
            }
            auto res = repo_->update_one(item);
            if (res && is_tracking_enabled())
            {
                record_write_snapshot(item);
            }
            return res;
        }

        // Removes an entity by primary key (or stages if deferred).
        // Tracker eviction is deferred until SQLite confirms the delete.
        [[nodiscard]] std::expected<void, Core::DbError> remove(const KeyType& id)
        {
            if (auto alive = check_context_alive(); !alive)
            {
                return alive;
            }
            if (should_defer())
            {
                return stage_remove(id);
            }
            auto res = repo_->delete_by_id(id);
            if (res)
            {
                untrack_key(id);
            }
            return res;
        }

        // Upserts a single entity: uses SQLite ON CONFLICT DO UPDATE for assigned/composite keys,
        // or a transaction/savepoint-guarded exists+update/insert for auto-increment keys.
        [[nodiscard]] std::expected<void, Core::DbError> upsert(const Domain& item)
        {
            if (auto alive = check_context_alive(); !alive)
            {
                return alive;
            }
            if (should_defer())
            {
                return stage_upsert(item);
            }
            return upsert_immediate(item);
        }

        // Atomic batch upsert: wraps the entire range (both assigned-key and auto-increment paths)
        // in a transaction or savepoint so any mid-batch failure rolls back all earlier rows,
        // updating tracker state only after commit.
        [[nodiscard]] std::expected<void, Core::DbError> upsert_range(const std::vector<Domain>& items)
        {
            if (auto alive = check_context_alive(); !alive)
            {
                return alive;
            }
            if (should_defer())
            {
                return stage_upsert_range(items);
            }
            return upsert_many_immediate(items);
        }

        template<std::ranges::input_range R>
            requires(!std::same_as<std::remove_cvref_t<R>, std::vector<Domain>>) &&
            std::convertible_to<std::ranges::range_reference_t<R>, const Domain&>
        [[nodiscard]] std::expected<void, Core::DbError> upsert_range(R&& items)
        {
            std::vector<Domain> vec;
            if constexpr (std::ranges::sized_range<R>)
            {
                vec.reserve(std::ranges::size(items));
            }
            for (auto&& item : items)
            {
                vec.push_back(std::forward<decltype(item)>(item));
            }
            return upsert_range(vec);
        }

        [[nodiscard]] std::expected<void, Core::DbError> stage_upsert(const Domain& item)
        {
            if (!enqueue_op_fn_)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error(
                    "Cannot stage operation: DbSet is not bound to a DbContext") };
            }
            if (!enqueue_op_fn_(
                    [repo = repo_,
                     conn = conn_,
                     upsert_fn = upsert_tracked_fn_,
                     find_fn = find_tracked_fn_,
                     tracking_fn = is_tracking_enabled_fn_,
                     item]()
                    {
                        return upsert_immediate_with(repo, conn, upsert_fn, find_fn, tracking_fn, item);
                    }))
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error(
                    "Cannot stage operation: owning DbContext has been destroyed") };
            }
            return {};
        }

        [[nodiscard]] std::expected<void, Core::DbError> stage_upsert_range(const std::vector<Domain>& items)
        {
            if (!enqueue_op_fn_)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error(
                    "Cannot stage operation: DbSet is not bound to a DbContext") };
            }
            if (!enqueue_op_fn_(
                    [repo = repo_,
                     conn = conn_,
                     upsert_fn = upsert_tracked_fn_,
                     find_fn = find_tracked_fn_,
                     tracking_fn = is_tracking_enabled_fn_,
                     items]()
                    {
                        return upsert_many_immediate_with(repo, conn, upsert_fn, find_fn, tracking_fn, items);
                    }))
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error(
                    "Cannot stage operation: owning DbContext has been destroyed") };
            }
            return {};
        }

        // Explicit Unit of Work staging methods (always enqueue into DbContext)
        [[nodiscard]] std::expected<void, Core::DbError> stage_add(const Domain& item)
        {
            if (!enqueue_op_fn_)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error(
                    "Cannot stage operation: DbSet is not bound to a DbContext") };
            }
            if (!enqueue_op_fn_(
                    [repo = repo_, upsert_fn = upsert_tracked_fn_, tracking_fn = is_tracking_enabled_fn_, item]()
                    {
                        auto res = repo->insert_one(item);
                        if (res && tracking_fn && tracking_fn() && !has_unset_auto_incr_key(item))
                        {
                            record_snapshot_with(repo, upsert_fn, item);
                        }
                        return res;
                    }))
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error(
                    "Cannot stage operation: owning DbContext has been destroyed") };
            }
            return {};
        }

        [[nodiscard]] std::expected<void, Core::DbError> stage_add_range(const std::vector<Domain>& items)
        {
            if (!enqueue_op_fn_)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error(
                    "Cannot stage operation: DbSet is not bound to a DbContext") };
            }
            if (!enqueue_op_fn_(
                    [repo = repo_, upsert_fn = upsert_tracked_fn_, tracking_fn = is_tracking_enabled_fn_, items]()
                    {
                        auto res = repo->insert_many(items);
                        if (res && tracking_fn && tracking_fn())
                        {
                            for (const auto& item : items)
                            {
                                if (!has_unset_auto_incr_key(item))
                                {
                                    record_snapshot_with(repo, upsert_fn, item);
                                }
                            }
                        }
                        return res;
                    }))
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error(
                    "Cannot stage operation: owning DbContext has been destroyed") };
            }
            return {};
        }

        [[nodiscard]] std::expected<void, Core::DbError> stage_update(const Domain& item)
        {
            if (!enqueue_op_fn_)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error(
                    "Cannot stage operation: DbSet is not bound to a DbContext") };
            }
            if (!enqueue_op_fn_(
                    [repo = repo_,
                     upsert_fn = upsert_tracked_fn_,
                     find_fn = find_tracked_fn_,
                     tracking_fn = is_tracking_enabled_fn_,
                     item]()
                    {
                        auto res = repo->update_one(item);
                        if (res && tracking_fn && tracking_fn())
                        {
                            record_write_snapshot_with(repo, upsert_fn, find_fn, item);
                        }
                        return res;
                    }))
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error(
                    "Cannot stage operation: owning DbContext has been destroyed") };
            }
            return {};
        }

        [[nodiscard]] std::expected<void, Core::DbError> stage_remove(const KeyType& id)
        {
            if (!enqueue_op_fn_)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error(
                    "Cannot stage operation: DbSet is not bound to a DbContext") };
            }
            const std::string key_repr = format_key(id);
            if (!enqueue_op_fn_(
                    [repo = repo_, untrack_fn = untrack_fn_, id, key_repr]()
                    {
                        auto res = repo->delete_by_id(id);
                        if (res && untrack_fn)
                        {
                            untrack_fn(std::type_index(typeid(Domain)), key_repr);
                        }
                        return res;
                    }))
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error(
                    "Cannot stage operation: owning DbContext has been destroyed") };
            }
            return {};
        }

        template<typename K>
            requires Core::SingleKeyType<KeyType> && std::constructible_from<typename KeyType::key_type, K> &&
            (!std::same_as<std::remove_cvref_t<K>, KeyType>)
        [[nodiscard]] std::expected<void, Core::DbError> stage_remove(const K& raw_or_id)
        {
            auto key = detail::try_make_single_key<KeyType>(raw_or_id);
            if (!key)
            {
                return std::unexpected<Core::DbError>{ key.error() };
            }
            return stage_remove(*key);
        }

        template<typename... Ks>
            requires Core::CompositeKeyType<KeyType> && (sizeof...(Ks) == KeyType::arity) && std::constructible_from<KeyType, Ks...>
        [[nodiscard]] std::expected<void, Core::DbError> stage_remove(Ks&&... parts)
        {
            auto key = detail::try_make_composite_key<KeyType>(parts...);
            if (!key)
            {
                return std::unexpected<Core::DbError>{ key.error() };
            }
            return stage_remove(*key);
        }

        template<typename K>
            requires Core::SingleKeyType<KeyType> && std::constructible_from<typename KeyType::key_type, K> &&
            (!std::same_as<std::remove_cvref_t<K>, KeyType>)
        [[nodiscard]] std::expected<void, Core::DbError> remove(const K& raw_or_id)
        {
            auto key = detail::try_make_single_key<KeyType>(raw_or_id);
            if (!key)
            {
                return std::unexpected<Core::DbError>{ key.error() };
            }
            return remove(*key);
        }

        template<typename... Ks>
            requires Core::CompositeKeyType<KeyType> && (sizeof...(Ks) == KeyType::arity) && std::constructible_from<KeyType, Ks...>
        [[nodiscard]] std::expected<void, Core::DbError> remove(Ks&&... parts)
        {
            auto key = detail::try_make_composite_key<KeyType>(parts...);
            if (!key)
            {
                return std::unexpected<Core::DbError>{ key.error() };
            }
            return remove(*key);
        }

        // Checks whether an entity with the given primary key exists
        [[nodiscard]] bool contains(const KeyType& id) const
        {
            return repo_->exists_by_id(id);
        }

        template<typename K>
            requires Core::SingleKeyType<KeyType> && std::constructible_from<typename KeyType::key_type, K> &&
            (!std::same_as<std::remove_cvref_t<K>, KeyType>)
        [[nodiscard]] bool contains(const K& raw_or_id) const
        {
            auto key = detail::try_make_single_key<KeyType>(raw_or_id);
            if (!key)
            {
                return false;
            }
            return contains(*key);
        }

        template<typename... Ks>
            requires Core::CompositeKeyType<KeyType> && (sizeof...(Ks) == KeyType::arity) && std::constructible_from<KeyType, Ks...>
        [[nodiscard]] bool contains(Ks&&... parts) const
        {
            auto key = detail::try_make_composite_key<KeyType>(parts...);
            if (!key)
            {
                return false;
            }
            return contains(*key);
        }

        // Returns total row count for this entity table with full error propagation
        [[nodiscard]] std::expected<std::size_t, Core::DbError> try_count() const
        {
            return query().try_count();
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

        [[nodiscard]] std::expected<std::optional<Domain>, Core::DbError> try_first_or_default() const
        {
            return query().try_first_or_default();
        }

        [[nodiscard]] std::optional<Domain> first_or_default() const
        {
            return query().first_or_default();
        }

        [[nodiscard]] std::expected<bool, Core::DbError> try_any() const
        {
            return query().try_any();
        }

        [[nodiscard]] bool any() const
        {
            return query().any();
        }

        // =====================================================================
        // Task 10.1: Fluent Server-Side Query Pushdown
        // =====================================================================

        [[nodiscard]] auto query() const
        {
            std::function<void(const Domain&)> track_cb{};
            std::function<void()> clear_cb{};
            std::function<void(const KeyType&)> untrack_key_cb{};
            std::function<bool()> is_context_alive_cb{};
            if (bound_to_context_ && is_tracker_alive_fn_)
            {
                is_context_alive_cb = is_tracker_alive_fn_;
            }
            if (is_tracking_enabled())
            {
                track_cb = [repo = repo_, upsert_fn = upsert_tracked_fn_](const Domain& entity)
                {
                    record_snapshot_with(repo, upsert_fn, entity);
                };
                if (untrack_type_fn_)
                {
                    clear_cb = [fn = untrack_type_fn_]()
                    {
                        fn(std::type_index(typeid(Domain)));
                    };
                }
                if (untrack_fn_)
                {
                    untrack_key_cb = [fn = untrack_fn_](const KeyType& key)
                    {
                        fn(std::type_index(typeid(Domain)), Core::encode_key(key));
                    };
                }
            }
            return DbQuery<Domain, DTO, ConnectionHandle, KeyType>(
                repo_,
                conn_,
                rfl::Nothing{},
                rfl::Nothing{},
                std::nullopt,
                std::nullopt,
                std::move(track_cb),
                std::move(clear_cb),
                std::move(untrack_key_cb),
                std::move(is_context_alive_cb));
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

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_contains(std::string_view needle) const
        {
            return query().template where_contains<ColLiteral>(needle);
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_starts_with(std::string_view prefix) const
        {
            return query().template where_starts_with<ColLiteral>(prefix);
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_ends_with(std::string_view suffix) const
        {
            return query().template where_ends_with<ColLiteral>(suffix);
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_icontains(std::string_view needle) const
        {
            return query().template where_icontains<ColLiteral>(needle);
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_not_contains(std::string_view needle) const
        {
            return query().template where_not_contains<ColLiteral>(needle);
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_not_starts_with(std::string_view prefix) const
        {
            return query().template where_not_starts_with<ColLiteral>(prefix);
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_not_ends_with(std::string_view suffix) const
        {
            return query().template where_not_ends_with<ColLiteral>(suffix);
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_iequal(std::string_view val) const
        {
            return query().template where_iequal<ColLiteral>(val);
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_not_iequal(std::string_view val) const
        {
            return query().template where_not_iequal<ColLiteral>(val);
        }

        template<typename Fn>
        [[nodiscard]] auto where_group(Fn&& fn) const
        {
            return query().where_group(std::forward<Fn>(fn));
        }

        template<rfl::internal::StringLiteral ColLiteral, std::ranges::input_range R>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_in_range(R&& range) const
        {
            return query().template where_in_range<ColLiteral>(std::forward<R>(range));
        }

        template<rfl::internal::StringLiteral ColLiteral, std::ranges::input_range R>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_not_in_range(R&& range) const
        {
            return query().template where_not_in_range<ColLiteral>(std::forward<R>(range));
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto group_by() const
        {
            return query().template group_by<ColLiteral>();
        }

        [[nodiscard]] std::expected<std::optional<Domain>, Core::DbError> try_single_or_default() const
        {
            return query().try_single_or_default();
        }

        [[nodiscard]] std::expected<Domain, Core::DbError> try_single() const
        {
            return query().try_single();
        }

        [[nodiscard]] std::generator<std::expected<Domain, Core::DbError>> stream() const
        {
            auto q = query();
            for (auto&& row : q.stream())
            {
                co_yield std::move(row);
            }
        }

        // C++26 P2996 Reflected Member NTTP forwarding overloads (e.g. db.books.where_equal<^^Book::title>(...))
        template<std::meta::info Member, typename Val>
            requires DAL::Mappers::ValidQueryMember<Domain, DTO, Member>
        [[nodiscard]] auto where_equal(const Val& val) const
        {
            return query().template where_equal<Member>(val);
        }

        template<std::meta::info Member, typename Val>
            requires DAL::Mappers::ValidQueryMember<Domain, DTO, Member>
        [[nodiscard]] auto where_not_equal(const Val& val) const
        {
            return query().template where_not_equal<Member>(val);
        }

        template<std::meta::info Member, typename Val>
            requires DAL::Mappers::ValidQueryMember<Domain, DTO, Member>
        [[nodiscard]] auto where_lt(const Val& val) const
        {
            return query().template where_lt<Member>(val);
        }

        template<std::meta::info Member, typename Val>
            requires DAL::Mappers::ValidQueryMember<Domain, DTO, Member>
        [[nodiscard]] auto where_le(const Val& val) const
        {
            return query().template where_le<Member>(val);
        }

        template<std::meta::info Member, typename Val>
            requires DAL::Mappers::ValidQueryMember<Domain, DTO, Member>
        [[nodiscard]] auto where_gt(const Val& val) const
        {
            return query().template where_gt<Member>(val);
        }

        template<std::meta::info Member, typename Val>
            requires DAL::Mappers::ValidQueryMember<Domain, DTO, Member>
        [[nodiscard]] auto where_ge(const Val& val) const
        {
            return query().template where_ge<Member>(val);
        }

        template<std::meta::info Member, typename Val>
            requires DAL::Mappers::ValidQueryMember<Domain, DTO, Member>
        [[nodiscard]] auto where_between(const Val& low, const Val& high) const
        {
            return query().template where_between<Member>(low, high);
        }

        template<std::meta::info Member>
            requires DAL::Mappers::ValidQueryMember<Domain, DTO, Member>
        [[nodiscard]] auto where_like(std::string_view pattern) const
        {
            return query().template where_like<Member>(pattern);
        }

        template<std::meta::info Member>
            requires DAL::Mappers::ValidQueryMember<Domain, DTO, Member>
        [[nodiscard]] auto where_contains(std::string_view needle) const
        {
            return query().template where_contains<Member>(needle);
        }

        template<std::meta::info Member>
            requires DAL::Mappers::ValidQueryMember<Domain, DTO, Member>
        [[nodiscard]] auto where_icontains(std::string_view needle) const
        {
            return query().template where_icontains<Member>(needle);
        }

        template<std::meta::info Member>
            requires DAL::Mappers::ValidQueryMember<Domain, DTO, Member>
        [[nodiscard]] auto where_not_contains(std::string_view needle) const
        {
            return query().template where_not_contains<Member>(needle);
        }

        template<std::meta::info Member>
            requires DAL::Mappers::ValidQueryMember<Domain, DTO, Member>
        [[nodiscard]] auto where_starts_with(std::string_view prefix) const
        {
            return query().template where_starts_with<Member>(prefix);
        }

        template<std::meta::info Member>
            requires DAL::Mappers::ValidQueryMember<Domain, DTO, Member>
        [[nodiscard]] auto where_not_starts_with(std::string_view prefix) const
        {
            return query().template where_not_starts_with<Member>(prefix);
        }

        template<std::meta::info Member>
            requires DAL::Mappers::ValidQueryMember<Domain, DTO, Member>
        [[nodiscard]] auto where_ends_with(std::string_view suffix) const
        {
            return query().template where_ends_with<Member>(suffix);
        }

        template<std::meta::info Member>
            requires DAL::Mappers::ValidQueryMember<Domain, DTO, Member>
        [[nodiscard]] auto where_not_ends_with(std::string_view suffix) const
        {
            return query().template where_not_ends_with<Member>(suffix);
        }

        template<std::meta::info Member>
            requires DAL::Mappers::ValidQueryMember<Domain, DTO, Member>
        [[nodiscard]] auto where_iequal(std::string_view val) const
        {
            return query().template where_iequal<Member>(val);
        }

        template<std::meta::info Member>
            requires DAL::Mappers::ValidQueryMember<Domain, DTO, Member>
        [[nodiscard]] auto where_not_iequal(std::string_view val) const
        {
            return query().template where_not_iequal<Member>(val);
        }

        template<std::meta::info Member, typename... Ts>
            requires(sizeof...(Ts) > 0) && DAL::Mappers::ValidQueryMember<Domain, DTO, Member>
        [[nodiscard]] auto where_in(const Ts&... vals) const
        {
            return query().template where_in<Member>(vals...);
        }

        template<std::meta::info Member, std::ranges::input_range R>
            requires DAL::Mappers::ValidQueryMember<Domain, DTO, Member>
        [[nodiscard]] auto where_in_range(R&& range) const
        {
            return query().template where_in_range<Member>(std::forward<R>(range));
        }

        template<std::meta::info Member, std::ranges::input_range R>
            requires DAL::Mappers::ValidQueryMember<Domain, DTO, Member>
        [[nodiscard]] auto where_not_in_range(R&& range) const
        {
            return query().template where_not_in_range<Member>(std::forward<R>(range));
        }

        template<std::meta::info Member>
            requires DAL::Mappers::ValidQueryMember<Domain, DTO, Member>
        [[nodiscard]] auto where_null() const
        {
            return query().template where_null<Member>();
        }

        template<std::meta::info Member>
            requires DAL::Mappers::ValidQueryMember<Domain, DTO, Member>
        [[nodiscard]] auto where_not_null() const
        {
            return query().template where_not_null<Member>();
        }

        template<std::meta::info Member>
            requires DAL::Mappers::ValidQueryMember<Domain, DTO, Member>
        [[nodiscard]] auto order_by(bool desc = false) const
        {
            return query().template order_by<Member>(desc);
        }

        template<std::meta::info Member>
            requires DAL::Mappers::ValidQueryMember<Domain, DTO, Member>
        [[nodiscard]] auto group_by() const
        {
            return query().template group_by<Member>();
        }

        template<std::meta::info Member>
            requires DAL::Mappers::ValidQueryMember<Domain, DTO, Member>
        [[nodiscard]] auto try_select() const
        {
            return query().template try_select<Member>();
        }

        template<std::meta::info Member>
            requires DAL::Mappers::ValidQueryMember<Domain, DTO, Member>
        [[nodiscard]] auto try_select_distinct() const
        {
            return query().template try_select_distinct<Member>();
        }

        template<std::meta::info... Members>
            requires(sizeof...(Members) >= 2) && (DAL::Mappers::ValidQueryMember<Domain, DTO, Members> && ...)
        [[nodiscard]] auto try_select_columns() const
        {
            return query().template try_select_columns<Members...>();
        }

        template<std::meta::info Member>
            requires DAL::Mappers::ValidQueryMember<Domain, DTO, Member>
        [[nodiscard]] auto try_min() const
        {
            return query().template try_min<Member>();
        }

        template<std::meta::info Member>
            requires DAL::Mappers::ValidQueryMember<Domain, DTO, Member>
        [[nodiscard]] auto try_max() const
        {
            return query().template try_max<Member>();
        }

        template<std::meta::info Member>
            requires DAL::Mappers::ValidQueryMember<Domain, DTO, Member>
        [[nodiscard]] auto try_sum() const
        {
            return query().template try_sum<Member>();
        }

        template<std::meta::info Member>
            requires DAL::Mappers::ValidQueryMember<Domain, DTO, Member>
        [[nodiscard]] std::expected<std::optional<double>, Core::DbError> try_avg() const
        {
            return query().template try_avg<Member>();
        }

        template<rfl::internal::StringLiteral ColLiteral, typename... Ts>
            requires(sizeof...(Ts) > 0) && DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
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
            requires(sizeof...(Ts) > 0) && DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_not_in(const Ts&... vals) const
        {
            return query().template where_not_in<ColLiteral>(vals...);
        }

        template<rfl::internal::StringLiteral ColLiteral, typename Val>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_between(const Val& low, const Val& high) const
        {
            return query().template where_between<ColLiteral>(low, high);
        }

        template<rfl::internal::StringLiteral ColLiteral, typename Val>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_not_in_vec(const std::vector<Val>& vals) const
        {
            return query().template where_not_in_vec<ColLiteral>(vals);
        }

        template<rfl::internal::StringLiteral ColLiteral, typename Val>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] std::expected<std::size_t, Core::DbError> remove_where_equal(const Val& val) const
        {
            return where_equal<ColLiteral>(val).remove_where();
        }

        template<std::meta::info Member, typename Val>
            requires DAL::Mappers::ValidQueryMember<Domain, DTO, Member>
        [[nodiscard]] std::expected<std::size_t, Core::DbError> remove_where_equal(const Val& val) const
        {
            return where_equal<Member>(val).remove_where();
        }

        template<rfl::internal::StringLiteral MatchCol, rfl::internal::StringLiteral... SetCols, typename MatchVal, typename... SetVals>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, MatchCol> && (sizeof...(SetCols) >= 1) &&
            (sizeof...(SetCols) == sizeof...(SetVals)) && (DAL::Mappers::ValidQueryColumn<Domain, DTO, SetCols> && ...)
        [[nodiscard]] std::expected<std::size_t, Core::DbError> update_where_equal(
            const MatchVal& match_val, const SetVals&... new_vals) const
        {
            return where_equal<MatchCol>(match_val).template update_where<SetCols...>(new_vals...);
        }

        template<std::meta::info MatchMember, std::meta::info... SetMembers, typename MatchVal, typename... SetVals>
            requires DAL::Mappers::ValidQueryMember<Domain, DTO, MatchMember> && (sizeof...(SetMembers) >= 1) &&
            (sizeof...(SetMembers) == sizeof...(SetVals)) && (DAL::Mappers::ValidQueryMember<Domain, DTO, SetMembers> && ...)
        [[nodiscard]] std::expected<std::size_t, Core::DbError> update_where_equal(
            const MatchVal& match_val, const SetVals&... new_vals) const
        {
            return where_equal<MatchMember>(match_val).template update_where<SetMembers...>(new_vals...);
        }

        template<rfl::internal::StringLiteral IndexName, rfl::internal::StringLiteral... ColLiterals>
            requires(sizeof...(ColLiterals) >= 1) && (DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiterals> && ...)
        [[nodiscard]] std::expected<void, Core::DbError> ensure_index_created(bool unique = false) const
        {
            if (!conn_)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is null") };
            }
            return conn_->template ensure_index_created<IndexName, DTO>(
                unique, Schema::col_of<DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiterals>()>...);
        }

        template<rfl::internal::StringLiteral IndexName, rfl::internal::StringLiteral... ColLiterals>
            requires(sizeof...(ColLiterals) >= 1) && (DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiterals> && ...)
        [[nodiscard]] std::expected<void, Core::DbError> ensure_unique_index_created() const
        {
            return ensure_index_created<IndexName, ColLiterals...>(true);
        }

        template<typename ParentDomain>
        [[nodiscard]] auto where_foreign_key(const ParentDomain& parent) const
        {
            using ParentDTO = Schema::SchemaOf_t<ParentDomain>;
            const ParentDTO parent_dto = DAL::Mappers::MapperTraits<ParentDomain, ParentDTO>::to_dto(parent);
            const auto ref_val = Schema::extract_referenced_parent_value<DTO, ParentDTO>(parent_dto);
            return where_sql(Schema::make_fk_where<DTO, ParentDTO>(ref_val));
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto order_by(bool desc = false) const
        {
            return query().template order_by<ColLiteral>(desc);
        }

        [[nodiscard]] auto page(std::size_t limit_count, std::size_t offset_count = 0) const pre(limit_count > 0)
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

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto try_select() const
        {
            return query().template try_select<ColLiteral>();
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto select() const
        {
            return query().template select<ColLiteral>();
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] std::expected<std::size_t, Core::DbError> try_count_distinct() const
        {
            return query().template try_count_distinct<ColLiteral>();
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] std::size_t count_distinct() const
        {
            return query().template count_distinct<ColLiteral>();
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto try_select_distinct() const
        {
            return query().template try_select_distinct<ColLiteral>();
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto select_distinct() const
        {
            return query().template select_distinct<ColLiteral>();
        }

        template<rfl::internal::StringLiteral... ColLiterals>
            requires(sizeof...(ColLiterals) >= 2) && (DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiterals> && ...)
        [[nodiscard]] auto try_select_columns() const
        {
            return query().template try_select_columns<ColLiterals...>();
        }

        template<rfl::internal::StringLiteral... ColLiterals>
            requires(sizeof...(ColLiterals) >= 2) && (DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiterals> && ...)
        [[nodiscard]] auto select_columns() const
        {
            return query().template select_columns<ColLiterals...>();
        }

        template<rfl::internal::StringLiteral... ColLiterals>
            requires(sizeof...(ColLiterals) >= 2) && (DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiterals> && ...)
        [[nodiscard]] auto try_select_distinct_columns() const
        {
            return query().template try_select_distinct_columns<ColLiterals...>();
        }

        template<rfl::internal::StringLiteral... ColLiterals>
            requires(sizeof...(ColLiterals) >= 2) && (DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiterals> && ...)
        [[nodiscard]] auto select_distinct_columns() const
        {
            return query().template select_distinct_columns<ColLiterals...>();
        }

        // =====================================================================
        // Task 10.2: Identity Map & Snapshot ChangeTracker Helpers
        // =====================================================================

        // Value-owned tracking: stores a shared copy inside the ChangeTracker so caller stack
        // lifetimes can never dangle.
        [[nodiscard]] std::shared_ptr<Domain> track_copy(Domain entity) const
        {
            auto key = Schema::EntityMetadata<Domain, DTO>::extract_key(entity);
            auto owned = std::make_shared<Domain>(std::move(entity));
            auto baseline = std::make_shared<DTO>(DAL::Mappers::MapperTraits<Domain, DTO>::to_dto(*owned));
            upsert_snapshot_entry(upsert_tracked_fn_, format_key(key), false, owned, repo_, std::move(baseline));
            return owned;
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
            auto baseline = std::make_shared<DTO>(DAL::Mappers::MapperTraits<Domain, DTO>::to_dto(*shared));
            upsert_snapshot_entry(upsert_tracked_fn_, key_repr, true, shared, repo_, std::move(baseline));
            return shared;
        }

        template<typename K>
            requires Core::SingleKeyType<KeyType> && std::constructible_from<typename KeyType::key_type, K> &&
            (!std::same_as<std::remove_cvref_t<K>, KeyType>)
        [[nodiscard]] std::shared_ptr<Domain> find_tracked(const K& raw_or_id) const
        {
            auto key = detail::try_make_single_key<KeyType>(raw_or_id);
            if (!key)
            {
                return nullptr;
            }
            return find_tracked(*key);
        }

        template<typename... Ks>
            requires Core::CompositeKeyType<KeyType> && (sizeof...(Ks) == KeyType::arity) && std::constructible_from<KeyType, Ks...>
        [[nodiscard]] std::shared_ptr<Domain> find_tracked(Ks&&... parts) const
        {
            auto key = detail::try_make_composite_key<KeyType>(parts...);
            if (!key)
            {
                return nullptr;
            }
            return find_tracked(*key);
        }

        // Returns a non-owning reference to the tracked entity in the change tracker if present (P2988).
        // Returns std::nullopt if the owning DbContext/tracker has expired so references can never dangle.
        [[nodiscard]] std::optional<Domain&> find_tracked_ref(const KeyType& id) const
        {
            if (!is_tracker_alive_fn_ || !is_tracker_alive_fn_())
            {
                return std::nullopt;
            }
            auto ptr = find_tracked(id);
            if (!ptr)
            {
                return std::nullopt;
            }
            if (!find_tracked_fn_ || !find_tracked_fn_(std::type_index(typeid(Domain)), format_key(id)))
            {
                return std::nullopt;
            }
            return *ptr;
        }

        template<typename K>
            requires Core::SingleKeyType<KeyType> && (!std::same_as<std::remove_cvref_t<K>, KeyType>) &&
            std::constructible_from<typename KeyType::key_type, K>
        [[nodiscard]] std::optional<Domain&> find_tracked_ref(const K& raw_or_id) const
        {
            auto key = detail::try_make_single_key<KeyType>(raw_or_id);
            if (!key)
            {
                return std::nullopt;
            }
            return find_tracked_ref(*key);
        }

        template<typename... Ks>
            requires Core::CompositeKeyType<KeyType> && (sizeof...(Ks) == KeyType::arity) && std::constructible_from<KeyType, Ks...>
        [[nodiscard]] std::optional<Domain&> find_tracked_ref(Ks&&... parts) const
        {
            auto key = detail::try_make_composite_key<KeyType>(parts...);
            if (!key)
            {
                return std::nullopt;
            }
            return find_tracked_ref(*key);
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
                auto baseline = std::make_shared<DTO>(DAL::Mappers::MapperTraits<Domain, DTO>::to_dto(*shared));
                upsert_snapshot_entry(upsert_tracked_fn_, key_repr, true, shared, repo_, std::move(baseline));
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
        [[nodiscard]] std::expected<void, Core::DbError> check_context_alive() const noexcept
        {
            if (bound_to_context_ && is_tracker_alive_fn_ && !is_tracker_alive_fn_())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error(
                    "Cannot mutate through DbSet: owning DbContext has expired") };
            }
            return {};
        }

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
            return Core::encode_key(key);
        }

        void untrack_key(const KeyType& key) const
        {
            if (untrack_fn_)
            {
                untrack_fn_(std::type_index(typeid(Domain)), format_key(key));
            }
        }

        using UpsertTrackedFn = std::function<void(
            std::type_index,
            std::string,
            bool,
            std::shared_ptr<void>,
            std::function<std::optional<std::function<std::expected<void, Core::DbError>()>>(const std::shared_ptr<void>&)>,
            std::function<std::shared_ptr<void>()>,
            std::function<void(const std::shared_ptr<void>&)>)>;

        void bind_tracking_callbacks(const std::weak_ptr<detail::ContextState>& weak_state)
        {
            is_tracker_alive_fn_ = [weak_state]() noexcept
            {
                return !weak_state.expired();
            };
            is_tracking_enabled_fn_ = [weak_state]() noexcept
            {
                auto st = weak_state.lock();
                return st && st->change_tracking_enabled_;
            };
            upsert_tracked_fn_ =
                [weak_state](
                    std::type_index type,
                    std::string key_repr,
                    bool refresh_baseline,
                    std::shared_ptr<void> owned,
                    std::function<std::optional<std::function<std::expected<void, Core::DbError>()>>(const std::shared_ptr<void>&)>
                        detect_fn,
                    std::function<std::shared_ptr<void>()> snap_fn,
                    std::function<void(const std::shared_ptr<void>&)> restore_fn)
            {
                if (auto st = weak_state.lock())
                {
                    st->upsert_tracked_entry(
                        type,
                        std::move(key_repr),
                        refresh_baseline,
                        std::move(owned),
                        std::move(detect_fn),
                        std::move(snap_fn),
                        std::move(restore_fn));
                }
            };
            untrack_fn_ = [weak_state](std::type_index type, const std::string& key_repr)
            {
                if (auto st = weak_state.lock())
                {
                    st->untrack_key(type, key_repr);
                }
            };
            untrack_type_fn_ = [weak_state](std::type_index type)
            {
                if (auto st = weak_state.lock())
                {
                    st->untrack_type(type);
                }
            };
            find_tracked_fn_ = [weak_state](std::type_index type, const std::string& key_repr) -> std::shared_ptr<void>
            {
                if (auto st = weak_state.lock())
                {
                    return st->find_tracked_instance(type, key_repr);
                }
                return nullptr;
            };
        }

        std::expected<void, Core::DbError> upsert_immediate(const Domain& item)
        {
            return upsert_immediate_with(repo_, conn_, upsert_tracked_fn_, find_tracked_fn_, is_tracking_enabled_fn_, item);
        }

        std::expected<void, Core::DbError> upsert_many_immediate(const std::vector<Domain>& items)
        {
            return upsert_many_immediate_with(repo_, conn_, upsert_tracked_fn_, find_tracked_fn_, is_tracking_enabled_fn_, items);
        }

        [[nodiscard]] static Core::DbError map_conn_error(const std::shared_ptr<ConnectionHandle>& conn, std::string_view msg)
        {
            if (conn)
            {
                if constexpr (requires(const ConnectionHandle& c, std::string_view m) { c.map_error(m); })
                {
                    return conn->map_error(msg);
                }
            }
            return Core::DbError::map_from_message(msg);
        }

        [[nodiscard]] std::expected<Domain, Core::DbError> add_returning_impl(const Domain& item, bool record_tracking)
        {
            detail::WriteBatchTxGuard<ConnectionHandle> tx_guard(conn_);
            if (auto begin_res = tx_guard.begin(); !begin_res)
            {
                return std::unexpected<Core::DbError>{ begin_res.error() };
            }

            Domain result = item;
            if constexpr (Schema::SinglePrimaryKey<DTO> && Schema::has_auto_incr_primary_key<DTO>())
            {
                if (!conn_)
                {
                    return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is null") };
                }
                if constexpr (requires(ConnectionHandle& c, const DTO& d) { c.template try_insert_returning_pk<DTO>(d); })
                {
                    const DTO dto = DAL::Mappers::MapperTraits<Domain, DTO>::to_dto(item);
                    auto pk_res = conn_->template try_insert_returning_pk<DTO>(dto);
                    if (!pk_res)
                    {
                        return std::unexpected<Core::DbError>{ pk_res.error() };
                    }
                    auto hydrated = DAL::Mappers::MapperTraits<Domain, DTO>::try_with_primary_key(item, *pk_res);
                    if (!hydrated)
                    {
                        return std::unexpected<Core::DbError>{ hydrated.error() };
                    }
                    result = std::move(*hydrated);
                }
                else
                {
                    auto ins_res = repo_->insert_one(item);
                    if (!ins_res)
                    {
                        return std::unexpected<Core::DbError>{ ins_res.error() };
                    }
                    if constexpr (requires(const ConnectionHandle& c) { c.try_last_insert_rowid(); })
                    {
                        auto rowid_res = conn_->try_last_insert_rowid();
                        if (!rowid_res)
                        {
                            return std::unexpected<Core::DbError>{ rowid_res.error() };
                        }
                        auto hydrated = DAL::Mappers::MapperTraits<Domain, DTO>::try_with_primary_key(item, *rowid_res);
                        if (!hydrated)
                        {
                            return std::unexpected<Core::DbError>{ hydrated.error() };
                        }
                        result = std::move(*hydrated);
                    }
                }
            }
            else
            {
                if constexpr (requires(ConnectionHandle& c, const DTO& d) { c.template try_insert_exact_one<DTO>(d); })
                {
                    if (!conn_)
                    {
                        return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is null") };
                    }
                    const DTO dto = DAL::Mappers::MapperTraits<Domain, DTO>::to_dto(item);
                    auto ins_res = conn_->template try_insert_exact_one<DTO>(dto);
                    if (!ins_res)
                    {
                        return std::unexpected<Core::DbError>{ ins_res.error() };
                    }
                }
                else
                {
                    auto ins_res = repo_->insert_one(item);
                    if (!ins_res)
                    {
                        return std::unexpected<Core::DbError>{ ins_res.error() };
                    }
                }
            }

            if (auto commit_res = tx_guard.commit(); !commit_res)
            {
                return std::unexpected<Core::DbError>{ commit_res.error() };
            }

            if (record_tracking && is_tracking_enabled())
            {
                record_snapshot(result);
            }
            return result;
        }

        [[nodiscard]] static bool has_unset_auto_incr_key(const Domain& item)
        {
            if constexpr (Schema::SinglePrimaryKey<DTO> && Schema::has_auto_incr_primary_key<DTO>())
            {
                const DTO dto = DAL::Mappers::MapperTraits<Domain, DTO>::to_dto(item);
                return dto_has_unset_auto_incr_key(dto);
            }
            else
            {
                return false;
            }
        }

        [[nodiscard]] static bool dto_has_unset_auto_incr_key(const DTO& dto)
        {
            if constexpr (Schema::SinglePrimaryKey<DTO> && Schema::has_auto_incr_primary_key<DTO>())
            {
                constexpr auto pk_m = Schema::primary_key_member<DTO>();
                const auto val = Schema::unwrap_field(dto.[:pk_m:]);
                using ValT = std::remove_cvref_t<decltype(val)>;
                if constexpr (std::is_integral_v<ValT> && !std::is_same_v<ValT, bool>)
                {
                    return val == 0;
                }
            }
            return false;
        }

        static std::expected<Domain, Core::DbError> upsert_single_returning_hydrated(
            const std::shared_ptr<RepoType>& repo, const std::shared_ptr<ConnectionHandle>& conn, const Domain& item)
        {
            const DTO dto = DAL::Mappers::MapperTraits<Domain, DTO>::to_dto(item);
            const bool needs_rowid = dto_has_unset_auto_incr_key(dto);

            if (conn)
            {
                if constexpr (
                    Schema::SinglePrimaryKey<DTO> && Schema::has_auto_incr_primary_key<DTO>() &&
                    requires(ConnectionHandle& c, const DTO& d) { c.template try_upsert_returning_pk<DTO>(d); })
                {
                    if (needs_rowid)
                    {
                        auto pk_res = conn->template try_upsert_returning_pk<DTO>(dto);
                        if (!pk_res)
                        {
                            return std::unexpected<Core::DbError>{ pk_res.error() };
                        }
                        return DAL::Mappers::MapperTraits<Domain, DTO>::try_with_primary_key(item, *pk_res);
                    }
                }
                if constexpr (requires(ConnectionHandle& c, const DTO& d) { c.try_upsert(d); })
                {
                    auto res = conn->try_upsert(dto);
                    if (!res)
                    {
                        return std::unexpected<Core::DbError>{ res.error() };
                    }
                    if constexpr (
                        Schema::SinglePrimaryKey<DTO> && Schema::has_auto_incr_primary_key<DTO>() &&
                        requires(const ConnectionHandle& c) { c.try_last_insert_rowid(); })
                    {
                        if (needs_rowid)
                        {
                            auto rowid_res = conn->try_last_insert_rowid();
                            if (!rowid_res)
                            {
                                return std::unexpected<Core::DbError>{ rowid_res.error() };
                            }
                            return DAL::Mappers::MapperTraits<Domain, DTO>::try_with_primary_key(item, *rowid_res);
                        }
                    }
                    return item;
                }
                else if constexpr (requires(ConnectionHandle& c, const DTO& d) { c.upsert(d); })
                {
                    auto res = conn->upsert(dto);
                    if (!res)
                    {
                        return std::unexpected<Core::DbError>{ map_conn_error(conn, res.error().what()) };
                    }
                    if constexpr (
                        Schema::SinglePrimaryKey<DTO> && Schema::has_auto_incr_primary_key<DTO>() &&
                        requires(const ConnectionHandle& c) { c.try_last_insert_rowid(); })
                    {
                        if (needs_rowid)
                        {
                            auto rowid_res = conn->try_last_insert_rowid();
                            if (!rowid_res)
                            {
                                return std::unexpected<Core::DbError>{ rowid_res.error() };
                            }
                            return DAL::Mappers::MapperTraits<Domain, DTO>::try_with_primary_key(item, *rowid_res);
                        }
                    }
                    return item;
                }
            }

            const auto key = Schema::EntityMetadata<Domain, DTO>::extract_key(item);
            auto res = (!needs_rowid && repo->exists_by_id(key)) ? repo->update_one(item) : repo->insert_one(item);
            if (!res)
            {
                return std::unexpected<Core::DbError>{ res.error() };
            }
            if constexpr (Schema::SinglePrimaryKey<DTO> && Schema::has_auto_incr_primary_key<DTO>() && requires(const ConnectionHandle& c) {
                              c.try_last_insert_rowid();
                          })
            {
                if (needs_rowid && conn)
                {
                    auto rowid_res = conn->try_last_insert_rowid();
                    if (!rowid_res)
                    {
                        return std::unexpected<Core::DbError>{ rowid_res.error() };
                    }
                    return DAL::Mappers::MapperTraits<Domain, DTO>::try_with_primary_key(item, *rowid_res);
                }
            }
            return item;
        }

        static std::expected<void, Core::DbError> upsert_immediate_with(
            const std::shared_ptr<RepoType>& repo,
            const std::shared_ptr<ConnectionHandle>& conn,
            const UpsertTrackedFn& upsert_fn,
            const std::function<std::shared_ptr<void>(std::type_index, const std::string&)>& find_fn,
            const std::function<bool()>& tracking_fn,
            const Domain& item,
            bool record_tracking = true)
        {
            const bool tracking = record_tracking && tracking_fn && tracking_fn();
            detail::WriteBatchTxGuard<ConnectionHandle> tx_guard(conn);
            if (auto begin_res = tx_guard.begin(); !begin_res)
            {
                return begin_res;
            }

            auto hydrated = upsert_single_returning_hydrated(repo, conn, item);
            if (!hydrated)
            {
                return std::unexpected<Core::DbError>{ hydrated.error() };
            }

            if (auto commit_res = tx_guard.commit(); !commit_res)
            {
                return commit_res;
            }
            if (tracking)
            {
                record_write_snapshot_with(repo, upsert_fn, find_fn, *hydrated);
            }
            return {};
        }

        static std::expected<void, Core::DbError> upsert_many_immediate_with(
            const std::shared_ptr<RepoType>& repo,
            const std::shared_ptr<ConnectionHandle>& conn,
            const UpsertTrackedFn& upsert_fn,
            const std::function<std::shared_ptr<void>(std::type_index, const std::string&)>& find_fn,
            const std::function<bool()>& tracking_fn,
            const std::vector<Domain>& items)
        {
            if (items.empty())
            {
                return {};
            }

            const bool tracking = tracking_fn && tracking_fn();
            detail::WriteBatchTxGuard<ConnectionHandle> tx_guard(conn);
            if (auto begin_res = tx_guard.begin(); !begin_res)
            {
                return begin_res;
            }

            bool needs_per_item_hydration = false;
            if (tracking && Schema::has_auto_incr_primary_key<DTO>())
            {
                for (const auto& item : items)
                {
                    if (has_unset_auto_incr_key(item))
                    {
                        needs_per_item_hydration = true;
                        break;
                    }
                }
            }

            std::vector<Domain> hydrated_to_track;
            bool executed_bulk = false;
            if (!needs_per_item_hydration && conn)
            {
                if constexpr (requires(ConnectionHandle& c, const std::vector<DTO>& dtos) { c.try_upsert_many(dtos); })
                {
                    std::vector<DTO> dtos;
                    dtos.reserve(items.size());
                    for (const auto& item : items)
                    {
                        dtos.push_back(DAL::Mappers::MapperTraits<Domain, DTO>::to_dto(item));
                    }
                    auto res = conn->try_upsert_many(dtos);
                    if (!res)
                    {
                        return std::unexpected<Core::DbError>{ res.error() };
                    }
                    executed_bulk = true;
                }
                else if constexpr (requires(ConnectionHandle& c, const std::vector<DTO>& dtos) { c.upsert_many(dtos); })
                {
                    std::vector<DTO> dtos;
                    dtos.reserve(items.size());
                    for (const auto& item : items)
                    {
                        dtos.push_back(DAL::Mappers::MapperTraits<Domain, DTO>::to_dto(item));
                    }
                    auto res = conn->upsert_many(dtos);
                    if (!res)
                    {
                        return std::unexpected<Core::DbError>{ map_conn_error(conn, res.error().what()) };
                    }
                    executed_bulk = true;
                }
            }

            if (!executed_bulk)
            {
                if (tracking)
                {
                    hydrated_to_track.reserve(items.size());
                }
                for (const auto& item : items)
                {
                    auto hydrated = upsert_single_returning_hydrated(repo, conn, item);
                    if (!hydrated)
                    {
                        return std::unexpected<Core::DbError>{ hydrated.error() };
                    }
                    if (tracking)
                    {
                        hydrated_to_track.push_back(std::move(*hydrated));
                    }
                }
            }

            if (auto commit_res = tx_guard.commit(); !commit_res)
            {
                return commit_res;
            }

            if (tracking)
            {
                const auto& to_track = executed_bulk ? items : hydrated_to_track;
                for (const auto& item : to_track)
                {
                    record_write_snapshot_with(repo, upsert_fn, find_fn, item);
                }
            }
            return {};
        }

        [[nodiscard]] auto make_detect_fn(std::shared_ptr<DTO> baseline) const
        {
            return make_detect_fn_with(repo_, std::move(baseline));
        }

        [[nodiscard]] static auto make_detect_fn_with(std::shared_ptr<RepoType> repo, std::shared_ptr<DTO> baseline)
        {
            return [repo, baseline = std::move(baseline)](
                       const std::shared_ptr<void>& owned) -> std::optional<std::function<std::expected<void, Core::DbError>()>>
            {
                if (!owned)
                {
                    return std::nullopt;
                }
                const Domain* ptr = static_cast<const Domain*>(owned.get());

                DTO current_dto = DAL::Mappers::MapperTraits<Domain, DTO>::to_dto(*ptr);
                if (DAL::Schema::dto_equals(current_dto, *baseline))
                {
                    return std::nullopt;
                }

                const auto baseline_key = Schema::EntityMetadata<Domain, DTO>::extract_dto_key(*baseline);
                const auto current_key = Schema::EntityMetadata<Domain, DTO>::extract_dto_key(current_dto);
                if (current_key != baseline_key)
                {
                    return []() -> std::expected<void, Core::DbError>
                    {
                        return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation(
                            "Cannot modify primary key of a tracked entity") };
                    };
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

        static void upsert_snapshot_entry(
            const UpsertTrackedFn& upsert_fn,
            std::string key_repr,
            bool refresh_baseline,
            std::shared_ptr<Domain> owned,
            const std::shared_ptr<RepoType>& repo,
            std::shared_ptr<DTO> baseline)
        {
            if (!upsert_fn)
            {
                return;
            }
            auto snap_fn = [baseline]() -> std::shared_ptr<void>
            {
                return std::make_shared<DTO>(*baseline);
            };
            auto restore_fn = [baseline](const std::shared_ptr<void>& snap)
            {
                if (snap)
                {
                    *baseline = *static_cast<const DTO*>(snap.get());
                }
            };
            upsert_fn(
                std::type_index(typeid(Domain)),
                std::move(key_repr),
                refresh_baseline,
                std::move(owned),
                make_detect_fn_with(repo, std::move(baseline)),
                std::move(snap_fn),
                std::move(restore_fn));
        }

        void record_snapshot(const Domain& entity) const
        {
            record_snapshot_with(repo_, upsert_tracked_fn_, entity);
        }

        void record_write_snapshot(const Domain& entity) const
        {
            record_write_snapshot_with(repo_, upsert_tracked_fn_, find_tracked_fn_, entity);
        }

        static void record_snapshot_with(
            const std::shared_ptr<RepoType>& repo, const UpsertTrackedFn& upsert_tracked_fn, const Domain& entity)
        {
            if (!upsert_tracked_fn)
            {
                return;
            }
            auto key = Schema::EntityMetadata<Domain, DTO>::extract_key(entity);
            auto baseline = std::make_shared<DTO>(DAL::Mappers::MapperTraits<Domain, DTO>::to_dto(entity));
            upsert_snapshot_entry(upsert_tracked_fn, format_key(key), true, nullptr, repo, std::move(baseline));
        }

        static void record_write_snapshot_with(
            const std::shared_ptr<RepoType>& repo,
            const UpsertTrackedFn& upsert_tracked_fn,
            const std::function<std::shared_ptr<void>(std::type_index, const std::string&)>& find_tracked_fn,
            const Domain& entity)
        {
            if (!upsert_tracked_fn)
            {
                return;
            }
            auto key = Schema::EntityMetadata<Domain, DTO>::extract_key(entity);
            const std::string key_repr = format_key(key);
            auto baseline = std::make_shared<DTO>(DAL::Mappers::MapperTraits<Domain, DTO>::to_dto(entity));
            std::shared_ptr<Domain> replacement_owned{};
            if (find_tracked_fn)
            {
                if (auto existing_void = find_tracked_fn(std::type_index(typeid(Domain)), key_repr))
                {
                    const Domain* existing_ptr = static_cast<const Domain*>(existing_void.get());
                    const DTO existing_dto = DAL::Mappers::MapperTraits<Domain, DTO>::to_dto(*existing_ptr);
                    if (!DAL::Schema::dto_equals(existing_dto, *baseline))
                    {
                        replacement_owned = std::make_shared<Domain>(entity);
                    }
                }
            }
            upsert_snapshot_entry(upsert_tracked_fn, key_repr, true, std::move(replacement_owned), repo, std::move(baseline));
        }

        std::shared_ptr<RepoType> repo_;
        std::shared_ptr<ConnectionHandle> conn_;
        bool bound_to_context_{ false };
        std::shared_ptr<detail::ContextState> local_state_{};
        std::function<bool()> is_deferred_fn_{};
        std::function<bool(std::move_only_function<std::expected<void, Core::DbError>()>)> enqueue_op_fn_{};
        std::function<bool()> is_tracker_alive_fn_{};
        std::function<bool()> is_tracking_enabled_fn_{};
        UpsertTrackedFn upsert_tracked_fn_{};
        std::function<void(std::type_index, const std::string&)> untrack_fn_{};
        std::function<void(std::type_index)> untrack_type_fn_{};
        std::function<std::shared_ptr<void>(std::type_index, const std::string&)> find_tracked_fn_{};
    };

} // namespace DAL
