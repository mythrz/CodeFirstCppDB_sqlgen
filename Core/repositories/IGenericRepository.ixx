export module core:igeneric_repository;

import std;
import :db_error;

export namespace Core
{

    template<typename DomainEntity>
    class IGenericRepository
    {
    public:
        virtual ~IGenericRepository() = default;

        [[nodiscard]] virtual std::vector<DomainEntity> get_all() const = 0;
        [[nodiscard]] virtual std::optional<DomainEntity> get_by_id(int id) const = 0;

        virtual std::expected<void, DbError> insert_one(const DomainEntity& item) = 0;
        virtual std::expected<void, DbError> insert_many(const std::vector<DomainEntity>& items) = 0;
        virtual std::expected<void, DbError> update_one(const DomainEntity& item) = 0;
        virtual std::expected<void, DbError> delete_by_id(int id) = 0;

        [[nodiscard]] virtual bool exists_by_id(int id) const = 0;
        [[nodiscard]] virtual std::size_t count() const = 0;
    };

} // namespace Core