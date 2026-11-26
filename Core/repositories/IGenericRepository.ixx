export module codefirst.core:igeneric_repository;

import std;
import :db_error;

export namespace Core
{
    // Base repository contract applicable to any entity (including junction / composite-key tables)
    template<typename DomainEntity>
    class IRepository
    {
    public:
        virtual ~IRepository() = default;

        [[nodiscard]] virtual std::vector<DomainEntity> get_all() const = 0;
        [[nodiscard]] virtual std::expected<std::vector<DomainEntity>, DbError> try_get_all() const = 0;
        virtual std::expected<void, DbError> insert_one(const DomainEntity& item) = 0;
        virtual std::expected<void, DbError> insert_many(const std::vector<DomainEntity>& items) = 0;
        [[nodiscard]] virtual std::size_t count() const = 0;
    };

    // Generic repository contract for entities identifiable by a primary key
    template<typename DomainEntity, typename KeyType = std::int32_t>
    class IGenericRepository : public virtual IRepository<DomainEntity>
    {
    public:
        virtual ~IGenericRepository() = default;

        [[nodiscard]] virtual std::optional<DomainEntity> get_by_id(KeyType id) const = 0;
        [[nodiscard]] virtual std::expected<std::optional<DomainEntity>, DbError> try_get_by_id(KeyType id) const = 0;
        virtual std::expected<void, DbError> update_one(const DomainEntity& item) = 0;
        virtual std::expected<void, DbError> delete_by_id(KeyType id) = 0;
        [[nodiscard]] virtual bool exists_by_id(KeyType id) const = 0;
    };

} // namespace Core
