export module dal:db_set;

import std;
import core;
import :mappers;
import :generic_repo;

export namespace DAL
{
    // DbSet<Domain, DTO, ConnectionHandle> provides an Entity Framework-style strongly-typed
    // entity set abstraction over a relational table, eliminating manual repository wiring.
    template<typename Domain, typename DTO, typename ConnectionHandle, typename KeyType = std::int32_t>
        requires DAL::Mappers::Mappable<Domain, DTO> && Repositories::DatabaseConnection<ConnectionHandle>
    class DbSet
    {
    public:
        using RepoType = Repositories::GenericRepository<Domain, DTO, ConnectionHandle, KeyType>;

        explicit DbSet(std::shared_ptr<ConnectionHandle> conn)
            : repo_(std::make_shared<RepoType>(std::move(conn)))
        {
        }

        // Retrieves all domain entities from the underlying table
        [[nodiscard]] std::vector<Domain> to_vector() const
        {
            return repo_->get_all();
        }

        // Finds an entity by its primary key
        [[nodiscard]] std::optional<Domain> find(KeyType id) const
        {
            return repo_->get_by_id(id);
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
        std::expected<void, Core::DbError> remove(KeyType id)
        {
            return repo_->delete_by_id(id);
        }

        // Checks whether an entity with the given primary key exists
        [[nodiscard]] bool contains(KeyType id) const
        {
            return repo_->exists_by_id(id);
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
