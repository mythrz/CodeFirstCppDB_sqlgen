module;
#include <sqlgen/PrimaryKey.hpp>

export module dal:schema_something;

import std;
import core;
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

    template<>
    struct SchemaOf<Core::Something>
    {
        using type = SomethingDTO;
    };

    static_assert(RelationalEntity<SomethingDTO>, "SomethingDTO fails RelationalEntity constraint.");
    static_assert(HasSchema<Core::Something>, "Core::Something must satisfy HasSchema constraint.");
} // namespace DAL::Schema