export module dal:generic_repo;

import std;
import core;
import :mappers;

export namespace DAL::Repositories
{
    // Minimal connection concept verifying basic liveness
    template<typename T>
    concept DatabaseConnection = requires(T& conn) {
        { conn.is_open() } -> std::same_as<bool>;
    };

    // Full contract concept for reading DTOs from connection
    template<typename Conn, typename DTO>
    concept ReadableConnection = DatabaseConnection<Conn> && requires(Conn& conn, std::int32_t id) {
        { conn.template fetch_all<DTO>() } -> std::same_as<std::vector<DTO>>;
        { conn.template count<DTO>() } -> std::convertible_to<std::size_t>;
    };

    // Full contract concept for writing DTOs to connection
    template<typename Conn, typename DTO>
    concept WritableConnection = DatabaseConnection<Conn> && requires(Conn& conn, const DTO& dto, const std::vector<DTO>& dtos) {
        conn.insert(dto);
        conn.insert_many(dtos);
        conn.update(dto);
    };

    template<typename Domain, typename DTO, typename ConnectionHandle, typename KeyType = std::int32_t>
        requires DAL::Mappers::Mappable<Domain, DTO> && DatabaseConnection<ConnectionHandle>
    class GenericRepository : public virtual Core::IGenericRepository<Domain, KeyType>
    {
    public:
        explicit GenericRepository(std::shared_ptr<ConnectionHandle> conn)
            : conn_(std::move(conn))
        {
        }

        [[nodiscard]] std::vector<Domain> get_all() const override
        {
            auto dtos = conn_->template fetch_all<DTO>();
            auto domain_view = dtos | std::views::transform(DAL::Mappers::MapperTraits<Domain, DTO>::to_domain);
            return std::vector<Domain>(domain_view.begin(), domain_view.end());
        }

        [[nodiscard]] std::optional<Domain> get_by_id(KeyType id) const override
        {
            return conn_->template get_by_id<DTO>(static_cast<std::int32_t>(id)).transform(
                DAL::Mappers::MapperTraits<Domain, DTO>::to_domain);
        }

        std::expected<void, Core::DbError> insert_one(const Domain& item) override
        {
            auto dto = DAL::Mappers::MapperTraits<Domain, DTO>::to_dto(item);
            auto res = conn_->insert(dto);
            if (!res)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::map_from_message(res.error().what()) };
            }
            return {};
        }

        std::expected<void, Core::DbError> insert_many(const std::vector<Domain>& items) override
        {
            auto dto_view = items | std::views::transform(DAL::Mappers::MapperTraits<Domain, DTO>::to_dto);
            std::vector<DTO> dtos(dto_view.begin(), dto_view.end());
            auto res = conn_->insert_many(dtos);
            if (!res)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::map_from_message(res.error().what()) };
            }
            return {};
        }

        std::expected<void, Core::DbError> update_one(const Domain& item) override
        {
            auto dto = DAL::Mappers::MapperTraits<Domain, DTO>::to_dto(item);
            auto res = conn_->update(dto);
            if (!res)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::map_from_message(res.error().what()) };
            }
            return {};
        }

        std::expected<void, Core::DbError> delete_by_id(KeyType id) override
        {
            auto res = conn_->template delete_by_id<DTO>(static_cast<std::int32_t>(id));
            if (!res)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::map_from_message(res.error().what()) };
            }
            return {};
        }

        [[nodiscard]] bool exists_by_id(KeyType id) const override
        {
            return conn_->template exists<DTO>(static_cast<std::int32_t>(id));
        }

        [[nodiscard]] std::size_t count() const override
        {
            return conn_->template count<DTO>();
        }

    protected:
        std::shared_ptr<ConnectionHandle> conn_;
    };

} // namespace DAL::Repositories
