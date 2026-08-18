module;
#include <sqlgen/ForeignKey.hpp>

export module dal:schema_person_something;

import std;
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

    static_assert(
        !RelationalEntity<Person_SomethingDTO>,
        "Person_SomethingDTO correctly fails RelationalEntity constraint as it uses a composite key.");
} // namespace DAL::Schema