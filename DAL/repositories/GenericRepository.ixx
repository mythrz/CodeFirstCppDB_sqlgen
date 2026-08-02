export module dal:generic_repo;

import std;
import :mappers;

export namespace DAL::Repositories 
{
    template <typename T>
    concept DatabaseConnection = requires(T conn) 
    {
        { conn.is_open() } -> std::same_as<bool>;
    };

    template <typename Domain, typename DTO, typename ConnectionHandle>
        requires DAL::Mappers::Mappable<Domain, DTO> 
    class GenericRepository : public virtual Core::IGenericRepository<Domain>
    {
    public:
        explicit GenericRepository(ConnectionHandle& conn) : conn_(conn) {}

        [[nodiscard]] std::vector<Domain> get_all() const 
        {
            auto dtos = conn_.template fetch_all<DTO>();
            auto domain_view = dtos | std::views::transform(DAL::Mappers::MapperTraits<Domain, DTO>::to_domain);
            return std::vector<Domain>(domain_view.begin(), domain_view.end());
        }

        [[nodiscard]] std::optional<Domain> get_by_id(std::int32_t id) const 
        {
            return conn_.template get_by_id<DTO>(id)
                .transform(DAL::Mappers::MapperTraits<Domain, DTO>::to_domain);
        }

        std::expected<void, std::string> insert_one(const Domain& item)
        {
            auto dto = DAL::Mappers::MapperTraits<Domain, DTO>::to_dto(item);
            bool ok = conn_.insert(dto);
            return ok ? std::expected<void, std::string>{} : std::unexpected<std::string>{"insert_one failed"};
        }

        std::expected<void, std::string> insert_many(const std::vector<Domain>& items)
        {
            auto dto_view = items | std::views::transform(DAL::Mappers::MapperTraits<Domain, DTO>::to_dto);
            std::vector<DTO> dtos(dto_view.begin(), dto_view.end());
            bool ok = conn_.insert_many(dtos);
            return ok ? std::expected<void, std::string>{} : std::unexpected<std::string>{"insert_many failed"};
        }

        std::expected<void, std::string> update_one(const Domain& item)
        {
            auto dto = DAL::Mappers::MapperTraits<Domain, DTO>::to_dto(item);
            bool ok = conn_.update(dto);
            return ok ? std::expected<void, std::string>{} : std::unexpected<std::string>{"update_one failed"};
        }

        std::expected<void, std::string> delete_by_id(std::int32_t id)
        {
            // The current connection API does not report deletion errors.
            // Preserve the expected-based repository API; deletion is successful
            // from the repository's perspective unless the connection throws.
            conn_.template delete_by_id<DTO>(id);
            return {};
        }

        [[nodiscard]] bool exists_by_id(std::int32_t id) const 
        {
            return conn_.template exists<DTO>(id);
        }

        [[nodiscard]] std::size_t count() const 
        {
            return conn_.template count<DTO>();
        }

    protected:
        ConnectionHandle& conn_;
    };

} // namespace DAL