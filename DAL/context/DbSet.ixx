export module dal:db_set;

import std;
import core;
import :schema_traits;
import :mappers;
import :generic_repo;
import :sqlite_connection;

export namespace DAL
{
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
            : repo_(std::make_shared<RepoType>(std::move(conn)))
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
            , is_deferred_fn_([ctx]() { return ctx->is_deferred(); })
            , enqueue_op_fn_([ctx](std::function<std::expected<void, Core::DbError>()> op) {
                ctx->enqueue_op(std::move(op));
            })
        {
        }

        template<typename Ctx>
            requires requires(const Ctx& ctx) {
                { ctx.connection() } -> std::convertible_to<std::shared_ptr<ConnectionHandle>>;
            } && (!requires(Ctx& ctx) { ctx.is_deferred(); })
        explicit DbSet(const Ctx* ctx)
            : repo_(std::make_shared<RepoType>(ctx->connection()))
        {
        }

        // Retrieves all domain entities from the underlying table
        [[nodiscard]] std::vector<Domain> to_vector() const
        {
            return repo_->get_all();
        }

        // Monadic read: retrieves all domain entities or returns a structured DbError
        [[nodiscard]] std::expected<std::vector<Domain>, Core::DbError> try_to_vector() const
        {
            return repo_->try_get_all();
        }

        // Finds an entity by its primary key (SingleKey, CompositeKey, or scalar)
        [[nodiscard]] std::optional<Domain> find(const KeyType& id) const
        {
            return repo_->get_by_id(id);
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
            return repo_->try_get_by_id(id);
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
            if (should_defer())
            {
                return stage_remove(id);
            }
            return repo_->delete_by_id(id);
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

        std::shared_ptr<RepoType> repo_;
        std::function<bool()> is_deferred_fn_{};
        std::function<void(std::function<std::expected<void, Core::DbError>()>)> enqueue_op_fn_{};
    };

} // namespace DAL
