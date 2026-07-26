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

        // [[nodiscard]] std::optional<Domain> get_by_id(std::int32_t id) const 
        // {
        //     // C++23 monadic optional transformation
        //     return conn_.template find_by_id<DTO>(id)
        //         .transform(DAL::Mappers::MapperTraits<Domain, DTO>::to_domain);
        // }
        [[nodiscard]] std::optional<Domain> get_by_id(std::int32_t id) const 
        {
            // C++23 monadic optional transformation
            return conn_.template get_by_id<DTO>(id)
                .transform(DAL::Mappers::MapperTraits<Domain, DTO>::to_domain);
        }
        // /// TODO: remove. this is duplicated
        // [[nodiscard]] std::optional<Domain> find_by_id(std::int32_t id) const override
        // {
        //     return get_by_id(id);
        // }

        bool insert_one(const Domain& item) 
        {
            auto dto = DAL::Mappers::MapperTraits<Domain, DTO>::to_dto(item);
            return conn_.insert(dto);
        }

        // bool insert_many(const std::vector<Domain>& items) 
        // {
        //     auto dto_view = items | std::views::transform(DAL::Mappers::MapperTraits<Domain, DTO>::to_dto);
        //     std::vector<DTO> dtos(dto_view.begin(), dto_view.end());
        //     return conn_.insert_batch(dtos);
        // }
        bool insert_many(const std::vector<Domain>& items) 
        {
            auto dto_view = items | std::views::transform(DAL::Mappers::MapperTraits<Domain, DTO>::to_dto);
            std::vector<DTO> dtos(dto_view.begin(), dto_view.end());
            return conn_.insert_many(dtos);
        }

        bool update_one(const Domain& item) 
        {
            auto dto = DAL::Mappers::MapperTraits<Domain, DTO>::to_dto(item);
            return conn_.update(dto);
        }

        void delete_by_id(std::int32_t id) 
        {
            conn_.template delete_by_id<DTO>(id);
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

} // namespace DAL::Repositories