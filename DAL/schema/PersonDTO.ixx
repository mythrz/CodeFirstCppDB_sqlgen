export module dal:schema_person;

import std;

export namespace DAL::Schema
{
    struct PersonDTO
    {
        std::int32_t id{ 0 };
        std::string first_name;
        std::string last_name;
    };

    template<typename T>
    concept RelationalEntity = requires(T dto) {
        { dto.id } -> std::same_as<std::int32_t&>;
    };

    static_assert(RelationalEntity<PersonDTO>, "PersonDTO fails RelationalEntity constraint.");
} // namespace DAL::Schema