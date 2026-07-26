export module dal:mappers;

import std;
import core;
import :schema_person;
import :schema_something;
import :schema_person_something;

export namespace DAL::Mappers 
{

template <typename Domain, typename DTO>
struct MapperTraits;

template <>
struct MapperTraits<Core::Person, DAL::Schema::PersonDTO> 
{
    [[nodiscard]] static Core::Person to_domain(const DAL::Schema::PersonDTO& dto) {
        return Core::Person(
            static_cast<std::uint32_t>(dto.id),
            dto.first_name,
            dto.last_name
        );
    }

    [[nodiscard]] static DAL::Schema::PersonDTO to_dto(const Core::Person& domain) 
    {
        return DAL::Schema::PersonDTO
        {
            .id = static_cast<std::int32_t>(domain.getId()),
            .first_name = domain.getFirstName(),
            .last_name = domain.getLastName()
        };
    }
};

template <>
struct MapperTraits<Core::Something, DAL::Schema::SomethingDTO> 
{
    [[nodiscard]] static Core::Something to_domain(const DAL::Schema::SomethingDTO& dto) 
    {
        return Core::Something
        (
            static_cast<std::uint32_t>(dto.id),
            dto.name,
            dto.category,
            dto.description
        );
    }

    [[nodiscard]] static DAL::Schema::SomethingDTO to_dto(const Core::Something& domain) 
    {
        return DAL::Schema::SomethingDTO
        {
            .id = static_cast<std::int32_t>(domain.getId()),
            .name = domain.getName(),
            .category = domain.getCategory(),
            .description = domain.getDescription()
        };
    }
};

template <>
struct MapperTraits<Core::Person_Something, DAL::Schema::Person_SomethingDTO> 
{
    [[nodiscard]] static Core::Person_Something to_domain(const DAL::Schema::Person_SomethingDTO& dto) 
    {
        return Core::Person_Something
        (
            static_cast<std::uint32_t>(dto.person_id),
            static_cast<std::uint32_t>(dto.something_id),
            dto.association_type
        );
    }

    [[nodiscard]] static DAL::Schema::Person_SomethingDTO to_dto(const Core::Person_Something& domain) 
    {
        return DAL::Schema::Person_SomethingDTO
        {
            .person_id = static_cast<std::int32_t>(domain.getPersonId()),
            .something_id = static_cast<std::int32_t>(domain.getSomethingId()),
            .association_type = domain.getAssociationType()
        };
    }
};

template <typename Domain, typename DTO>
concept Mappable = requires(const Domain& domain, const DTO& dto) 
{
    { MapperTraits<Domain, DTO>::to_domain(dto) } -> std::same_as<Domain>;
    { MapperTraits<Domain, DTO>::to_dto(domain) } -> std::same_as<DTO>;
};

} // namespace DAL::Mappers
