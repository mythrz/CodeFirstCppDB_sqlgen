export module samples:sqlite_person_something_repo;

import std;
import core;
import dal;
import :person_something;
import :iperson_something_repository;
import :schema_person_something;

export namespace DAL::Repositories
{
    template<typename ConnectionHandle>
        requires DatabaseConnection<ConnectionHandle>
    class SQLitePerson_SomethingRepo
        : public GenericRepository<Core::Person_Something, DAL::Schema::Person_SomethingDTO, ConnectionHandle, Core::Person_Something::Key>
        , public Core::IPerson_SomethingRepository
    {
        using Base =
            GenericRepository<Core::Person_Something, DAL::Schema::Person_SomethingDTO, ConnectionHandle, Core::Person_Something::Key>;

    public:
        explicit SQLitePerson_SomethingRepo(std::shared_ptr<ConnectionHandle> conn) noexcept
            : Base(std::move(conn))
        {
        }

        [[nodiscard]] std::optional<Core::Person_Something> get_by_composite_id(
            Core::PersonId personId, Core::SomethingId somethingId) const override
        {
            return this->get_by_id(Core::Person_Something::Key{ personId, somethingId });
        }

        [[nodiscard]] std::vector<Core::Person_Something> find_by_person_id(Core::PersonId personId) const override
        {
            if constexpr (requires {
                              this->conn_->template fetch_where_equal<DAL::Schema::Person_SomethingDTO, "person_id">(personId.get());
                          })
            {
                auto dtos = this->conn_->template fetch_where_equal<DAL::Schema::Person_SomethingDTO, "person_id">(personId.get());
                auto domain_view = dtos |
                    std::views::transform(DAL::Mappers::MapperTraits<Core::Person_Something, DAL::Schema::Person_SomethingDTO>::to_domain);
                return std::vector<Core::Person_Something>(domain_view.begin(), domain_view.end());
            }
            else
            {
                auto dtos = this->conn_->template fetch_all<DAL::Schema::Person_SomethingDTO>();
                std::vector<Core::Person_Something> results;
                for (const auto& dto : dtos)
                {
                    if (dto.person_id.value() == personId.get())
                    {
                        results.push_back(
                            DAL::Mappers::MapperTraits<Core::Person_Something, DAL::Schema::Person_SomethingDTO>::to_domain(dto));
                    }
                }
                return results;
            }
        }

        [[nodiscard]] std::vector<Core::Person_Something> find_by_something_id(Core::SomethingId somethingId) const override
        {
            if constexpr (requires {
                              this->conn_->template fetch_where_equal<DAL::Schema::Person_SomethingDTO, "something_id">(somethingId.get());
                          })
            {
                auto dtos = this->conn_->template fetch_where_equal<DAL::Schema::Person_SomethingDTO, "something_id">(somethingId.get());
                auto domain_view = dtos |
                    std::views::transform(DAL::Mappers::MapperTraits<Core::Person_Something, DAL::Schema::Person_SomethingDTO>::to_domain);
                return std::vector<Core::Person_Something>(domain_view.begin(), domain_view.end());
            }
            else
            {
                auto dtos = this->conn_->template fetch_all<DAL::Schema::Person_SomethingDTO>();
                std::vector<Core::Person_Something> results;
                for (const auto& dto : dtos)
                {
                    if (dto.something_id.value() == somethingId.get())
                    {
                        results.push_back(
                            DAL::Mappers::MapperTraits<Core::Person_Something, DAL::Schema::Person_SomethingDTO>::to_domain(dto));
                    }
                }
                return results;
            }
        }

        [[nodiscard]] std::expected<void, Core::DbError> delete_by_composite_id(
            Core::PersonId personId, Core::SomethingId somethingId) override
        {
            return this->delete_by_id(Core::Person_Something::Key{ personId, somethingId });
        }

        [[nodiscard]] bool exists_by_composite_id(Core::PersonId personId, Core::SomethingId somethingId) const override
        {
            return this->exists_by_id(Core::Person_Something::Key{ personId, somethingId });
        }
    };

} // namespace DAL::Repositories
