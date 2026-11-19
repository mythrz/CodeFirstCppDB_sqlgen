export module samples:sqlite_person_repo;

import std;
import core;
import dal;
import :person;
import :iperson_repository;
import :schema_person;

export namespace DAL::Repositories
{

    template<typename ConnectionHandle>
        requires DatabaseConnection<ConnectionHandle>
    class SQLitePersonRepo
        : public GenericRepository<Core::Person, DAL::Schema::PersonDTO, ConnectionHandle>
        , public Core::IPersonRepository
    {
        using Base = GenericRepository<Core::Person, DAL::Schema::PersonDTO, ConnectionHandle>;

    public:
        explicit SQLitePersonRepo(std::shared_ptr<ConnectionHandle> conn) noexcept
            : Base(std::move(conn))
        {
        }

        [[nodiscard]] std::vector<Core::Person> find_by_last_name(const std::string& lastName) const override
        {
            auto dtos = this->conn_->template fetch_where_equal<DAL::Schema::PersonDTO, "last_name">(lastName);
            return dtos | std::views::transform(DAL::Mappers::MapperTraits<Core::Person, DAL::Schema::PersonDTO>::to_domain) |
                std::ranges::to<std::vector>();
        }
    };

} // namespace DAL::Repositories
