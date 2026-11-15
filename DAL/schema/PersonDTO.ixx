module;
#include <sqlgen/PrimaryKey.hpp>

export module samples:schema_person;

import std;
import core;
import dal;
import :person;

export namespace DAL::Schema
{
    struct PersonDTO
    {
        static constexpr auto tablename = "Person";
        sqlgen::PrimaryKey<std::int32_t, true> id{0};
        std::string first_name;
        std::string last_name;
    };

    template<>
    struct SchemaOf<Core::Person>
    {
        using type = PersonDTO;
    };

    static_assert(RelationalEntity<PersonDTO>, "PersonDTO fails RelationalEntity constraint.");
    static_assert(HasSchema<Core::Person>, "Core::Person must satisfy HasSchema constraint.");
} // namespace DAL::Schema