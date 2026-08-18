module;
#include <sqlgen/PrimaryKey.hpp>

export module dal:schema_traits;

import std;

export namespace DAL::Schema
{
    // Constraint: any DTO that can be used as a relational row must expose a std::int32_t `id` member (the SQLite primary key column).
    template<typename T>
    concept RelationalEntity = requires(T dto) {
        { dto.id.value() } -> std::same_as<std::int32_t&>;
    };
} // namespace DAL::Schema
