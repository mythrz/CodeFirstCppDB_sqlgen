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
             typename KeyType = typename Domain::Key>
        requires DAL::Mappers::Mappable<Domain, DTO> && Repositories::DatabaseConnection<ConnectionHandle>
    class DbSet
    {
    public:
        using RepoType = Repositories::GenericRepository<Domain, DTO, ConnectionHandle, KeyType>;

        explicit DbSet(std::shared_ptr<ConnectionHandle> conn)
            : repo_(std::make_shared<RepoType>(std::move(conn)))
        {
        }

        template<typename Ctx>
            requires requires(const Ctx& ctx) {
                { ctx.connection() } -> std::convertible_to<std::shared_ptr<ConnectionHandle>>;
            }
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

        // Inserts a single entity into the database table
        std::expected<void, Core::DbError> add(const Domain& item)
        {
            return repo_->insert_one(item);
        }

        // Inserts multiple entities into the database table
        std::expected<void, Core::DbError> add_range(const std::vector<Domain>& items)
        {
            return repo_->insert_many(items);
        }

        // Updates an existing entity
        std::expected<void, Core::DbError> update(const Domain& item)
        {
            return repo_->update_one(item);
        }

        // Removes an entity by primary key
        std::expected<void, Core::DbError> remove(const KeyType& id)
        {
            return repo_->delete_by_id(id);
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
        std::shared_ptr<RepoType> repo_;
    };

} // namespace DAL
