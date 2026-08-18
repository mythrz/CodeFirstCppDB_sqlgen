export module dal:sqlite_person_repo;

import std;
import core;
import :generic_repo;

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
            auto dtos = this->conn_->template query<DAL::Schema::PersonDTO>().where("last_name", lastName).execute();

            auto domain_view = dtos | std::views::transform(DAL::Mappers::MapperTraits<Core::Person, DAL::Schema::PersonDTO>::to_domain);
            return std::vector<Core::Person>(domain_view.begin(), domain_view.end());
        }
    };

} // namespace DAL::Repositories