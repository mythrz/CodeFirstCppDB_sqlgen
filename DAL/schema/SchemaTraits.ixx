module;
#include <meta>
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

    // Detects sqlgen::PrimaryKey<V, auto_incr> fields via static reflection
    template<typename F>
    concept SqlgenPrimaryKeyField = requires(const F& f) {
        typename F::ReflectionType;
        { F::auto_incr } -> std::convertible_to<bool>;
        { f.value() };
    };

    // Detects sqlgen::ForeignKey<V, Table, Col> fields via static reflection
    template<typename F>
    concept SqlgenForeignKeyField = requires(const F& f) {
        typename F::ReflectionType;
        typename F::ForeignTableType;
        typename F::ColumnType;
        { f.value() };
    };

    template<typename T>
    consteval std::size_t primary_key_count()
    {
        std::size_t count = 0;
        static constexpr auto members = std::define_static_array(
            std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::current()));
        template for (constexpr auto m : members)
        {
            using RawFieldType = [:std::meta::type_of(m):];
            using FieldType = std::remove_cvref_t<RawFieldType>;
            if constexpr (SqlgenPrimaryKeyField<FieldType>)
            {
                ++count;
            }
        }
        return count;
    }

    template<typename T>
    consteval std::size_t foreign_key_count()
    {
        std::size_t count = 0;
        static constexpr auto members = std::define_static_array(
            std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::current()));
        template for (constexpr auto m : members)
        {
            using RawFieldType = [:std::meta::type_of(m):];
            using FieldType = std::remove_cvref_t<RawFieldType>;
            if constexpr (SqlgenForeignKeyField<FieldType>)
            {
                ++count;
            }
        }
        return count;
    }

    // Entities with a single primary key (e.g. PersonDTO, SomethingDTO)
    template<typename T>
    concept SinglePrimaryKey = HasTableName<T> && (primary_key_count<T>() == 1);

    // Entities with composite keys (e.g. junction tables like Person_SomethingDTO,
    // or multi-column PrimaryKey DTOs)
    template<typename T>
    concept CompositePrimaryKey = HasTableName<T> && !SinglePrimaryKey<T>
        && ((primary_key_count<T>() >= 2) || (primary_key_count<T>() == 0 && foreign_key_count<T>() >= 2));

    // Generalized relational entity concept accommodating both single and composite key entities
    template<typename T>
    concept RelationalEntity = HasTableName<T> && (SinglePrimaryKey<T> || CompositePrimaryKey<T>);

    // Trait mapping a Domain entity type to its persistence DTO schema type.
    // Defaults to Domain itself (for self-describing DTOs); specialize for Domain/DTO pairs.
    template<typename Domain>
    struct SchemaOf
    {
        using type = Domain;
    };

    template<typename Domain>
    using SchemaOf_t = typename SchemaOf<Domain>::type;

    template<typename Domain>
    concept HasSchema = RelationalEntity<SchemaOf_t<Domain>>;

} // namespace DAL::Schema
