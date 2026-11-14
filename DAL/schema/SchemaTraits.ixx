module;
#include <sqlgen/PrimaryKey.hpp>
#include <sqlgen/ForeignKey.hpp>

export module dal:schema_traits;

import std;

export namespace DAL::Schema
{
    template<typename T>
    concept HasTableName = requires {
        { T::tablename } -> std::convertible_to<std::string_view>;
    };

    // Entities with a single primary key (e.g. PersonDTO, SomethingDTO)
    template<typename T>
    concept SinglePrimaryKey = requires(T dto) {
        { dto.id.value() } -> std::same_as<std::int32_t&>;
    };

    // Entities with composite keys (e.g. junction tables like Person_SomethingDTO)
    template<typename T>
    concept CompositePrimaryKey = !SinglePrimaryKey<T> && requires(T dto) {
        { dto.person_id.value() } -> std::convertible_to<std::int32_t>;
        { dto.something_id.value() } -> std::convertible_to<std::int32_t>;
    };

    // Generalized relational entity concept accommodating both single and composite key entities
    template<typename T>
    concept RelationalEntity = HasTableName<T> && (SinglePrimaryKey<T> || CompositePrimaryKey<T>);

} // namespace DAL::Schema
