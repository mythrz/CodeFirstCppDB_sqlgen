export module dal:sqlite_person_something_repo;

import std;
import core;
import :schema_person_something;
import :mappers;
import :generic_repo;

export namespace DAL::Repositories
{
    template<typename ConnectionHandle>
        requires DatabaseConnection<ConnectionHandle>
    class SQLitePerson_SomethingRepo : public Core::IPerson_SomethingRepository
    {
    public:
        explicit SQLitePerson_SomethingRepo(std::shared_ptr<ConnectionHandle> conn) noexcept
            : conn_(std::move(conn))
        {
        }

        [[nodiscard]] std::vector<Core::Person_Something> get_all() const override
        {
            auto dtos = conn_->template fetch_all<DAL::Schema::Person_SomethingDTO>();
            auto domain_view = dtos | std::views::transform(
                DAL::Mappers::MapperTraits<Core::Person_Something, DAL::Schema::Person_SomethingDTO>::to_domain);
            return std::vector<Core::Person_Something>(domain_view.begin(), domain_view.end());
        }

        [[nodiscard]] std::expected<std::vector<Core::Person_Something>, Core::DbError> try_get_all() const override
        {
            if constexpr (requires { conn_->template try_fetch_all<DAL::Schema::Person_SomethingDTO>(); })
            {
                auto res = conn_->template try_fetch_all<DAL::Schema::Person_SomethingDTO>();
                if (!res)
                {
                    return std::unexpected<Core::DbError>{ res.error() };
                }
                auto domain_view = res.value() | std::views::transform(
                    DAL::Mappers::MapperTraits<Core::Person_Something, DAL::Schema::Person_SomethingDTO>::to_domain);
                return std::vector<Core::Person_Something>(domain_view.begin(), domain_view.end());
            }
            else
            {
                if (!conn_->is_open())
                {
                    return std::unexpected<Core::DbError>{
                        Core::DbError::connection_error("Database connection is closed")
                    };
                }
                return get_all();
            }
        }

        std::expected<void, Core::DbError> insert_one(const Core::Person_Something& item) override
        {
            auto dto = DAL::Mappers::MapperTraits<Core::Person_Something, DAL::Schema::Person_SomethingDTO>::to_dto(item);
            auto res = conn_->insert(dto);
            if (!res)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::map_from_message(res.error().what()) };
            }
            return {};
        }

        std::expected<void, Core::DbError> insert_many(const std::vector<Core::Person_Something>& items) override
        {
            auto dto_view = items | std::views::transform(
                DAL::Mappers::MapperTraits<Core::Person_Something, DAL::Schema::Person_SomethingDTO>::to_dto);
            std::vector<DAL::Schema::Person_SomethingDTO> dtos(dto_view.begin(), dto_view.end());
            auto res = conn_->insert_many(dtos);
            if (!res)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::map_from_message(res.error().what()) };
            }
            return {};
        }

        [[nodiscard]] std::size_t count() const override
        {
            return conn_->template count<DAL::Schema::Person_SomethingDTO>();
        }

        [[nodiscard]] std::optional<Core::Person_Something> get_by_composite_id(
            Core::PersonId personId, Core::SomethingId somethingId) const override
        {
            auto dtos = conn_->template fetch_all<DAL::Schema::Person_SomethingDTO>();
            for (const auto& dto : dtos)
            {
                if (dto.person_id.value() == personId.get() && dto.something_id.value() == somethingId.get())
                {
                    return DAL::Mappers::MapperTraits<Core::Person_Something, DAL::Schema::Person_SomethingDTO>::to_domain(dto);
                }
            }
            return std::nullopt;
        }

        [[nodiscard]] std::vector<Core::Person_Something> find_by_person_id(Core::PersonId personId) const override
        {
            auto dtos = conn_->template fetch_all<DAL::Schema::Person_SomethingDTO>();
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

        [[nodiscard]] std::vector<Core::Person_Something> find_by_something_id(Core::SomethingId somethingId) const override
        {
            auto dtos = conn_->template fetch_all<DAL::Schema::Person_SomethingDTO>();
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

        std::expected<void, Core::DbError> delete_by_composite_id(
            Core::PersonId personId, Core::SomethingId somethingId) override
        {
            if constexpr (requires { conn_->delete_by_composite_id(personId.get(), somethingId.get()); })
            {
                auto res = conn_->delete_by_composite_id(personId.get(), somethingId.get());
                if (!res)
                {
                    return std::unexpected<Core::DbError>{ Core::DbError::map_from_message(res.error().what()) };
                }
                return {};
            }
            else
            {
                return {};
            }
        }

        [[nodiscard]] bool exists_by_composite_id(
            Core::PersonId personId, Core::SomethingId somethingId) const override
        {
            return get_by_composite_id(personId, somethingId).has_value();
        }

    protected:
        std::shared_ptr<ConnectionHandle> conn_;
    };

} // namespace DAL::Repositories
