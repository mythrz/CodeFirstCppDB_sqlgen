export module samples:sqlite_something_repo;

import std;
import core;
import dal;
import :something;
import :isomething_repository;
import :schema_something;

export namespace DAL::Repositories
{
    template<typename ConnectionHandle>
        requires DatabaseConnection<ConnectionHandle>
    class SQLiteSomethingRepo
        : public GenericRepository<Core::Something, DAL::Schema::SomethingDTO, ConnectionHandle>
        , public Core::ISomethingRepository
    {
        using Base = GenericRepository<Core::Something, DAL::Schema::SomethingDTO, ConnectionHandle>;

    public:
        explicit SQLiteSomethingRepo(std::shared_ptr<ConnectionHandle> conn) noexcept
            : Base(std::move(conn))
        {
        }
    };
} // namespace DAL::Repositories