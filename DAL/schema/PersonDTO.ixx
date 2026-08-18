module;
#include <sqlgen/PrimaryKey.hpp>

export module dal:schema_person;

import std;
import :schema_traits;

export namespace DAL::Schema
{
    struct PersonDTO
    {
        static constexpr auto tablename = "Person";
        sqlgen::PrimaryKey<std::int32_t, true> id{0};
        std::string first_name;
        std::string last_name;
    };

    static_assert(RelationalEntity<PersonDTO>, "PersonDTO fails RelationalEntity constraint.");
} // namespace DAL::Schema