module;
#include <sqlgen/ForeignKey.hpp>

export module dal:schema_person_something;

import std;
import core;
import :schema_traits;
import :schema_person;
import :schema_something;

export namespace DAL::Schema
{
    struct Person_SomethingDTO
    {
        static constexpr auto tablename = "Person_Something";
        sqlgen::ForeignKey<std::int32_t, PersonDTO, "id"> person_id{ 0 };
        sqlgen::ForeignKey<std::int32_t, SomethingDTO, "id"> something_id{ 0 };
        std::string association_type;
    };

    template<>
    struct SchemaOf<Core::Person_Something>
    {
        using type = Person_SomethingDTO;
    };

    static_assert(
        RelationalEntity<Person_SomethingDTO>,
        "Person_SomethingDTO must satisfy RelationalEntity constraint as a composite-key entity.");
    static_assert(
        CompositePrimaryKey<Person_SomethingDTO>,
        "Person_SomethingDTO must satisfy CompositePrimaryKey constraint.");
    static_assert(HasSchema<Core::Person_Something>, "Core::Person_Something must satisfy HasSchema constraint.");
} // namespace DAL::Schema
