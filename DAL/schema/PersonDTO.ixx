export module dal:schema_person;

import std;

export namespace DAL::Schema 
{

    struct PersonDTO 
    {
        int id;
        std::string first_name;
        std::string last_name;
    };

    // c++20 Concept: enforces structural conformity for database entities
    template<typename T>
    concept RelationalEntity = requires(T dto) 
    {
        { dto.id } -> std::same_as<int&>;
    };

    static_assert(RelationalEntity<PersonDTO>, "PersonDTO fails RelationalEntity constraint.");
}