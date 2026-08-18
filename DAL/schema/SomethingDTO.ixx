module;
#include <sqlgen/PrimaryKey.hpp>

export module dal:schema_something;

import std;
import :schema_traits;

export namespace DAL::Schema
{
    struct SomethingDTO
    {
        static constexpr auto tablename = "Something";
        sqlgen::PrimaryKey<std::int32_t, true> id{ 0 };
        std::string name;
        std::string category;
        std::optional<std::string> description;
    };

    static_assert(RelationalEntity<SomethingDTO>, "SomethingDTO fails RelationalEntity constraint.");
} // namespace DAL::Schema