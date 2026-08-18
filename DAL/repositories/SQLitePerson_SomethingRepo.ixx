export module dal:sqlite_person_something_repo;

import std;
import core;
import :generic_repo;

export namespace DAL::Repositories
{
    template<typename ConnectionHandle>
        requires DatabaseConnection<ConnectionHandle>
    class SQLitePerson_SomethingRepo
        : public GenericRepository<Core::Person_Something, DAL::Schema::Person_SomethingDTO, ConnectionHandle>
        , public Core::IPerson_SomethingRepository
    {
        using Base = GenericRepository<Core::Person_Something, DAL::Schema::Person_SomethingDTO, ConnectionHandle>;

    public:
        explicit SQLitePerson_SomethingRepo(std::shared_ptr<ConnectionHandle> conn) noexcept
            : Base(std::move(conn))
        {
        }
    };

} // namespace DAL::Repositories