#pragma once

#include <optional>
#include <vector>
#include <sqlgen.hpp>
#include <sqlgen/sqlite/connect.hpp>

using namespace sqlgen;
using namespace sqlgen::literals;

template <typename T>
class RepoGen001 {
public:
    using Conn = decltype(sqlgen::sqlite::connect(std::declval<std::string>()));

    explicit RepoGen001(const Conn& conn)
        : conn_(conn) {}

    std::vector<T> get_all() const {
        return sqlgen::read<std::vector<T>>(conn_).value();
    }

    std::optional<T> get_by_id(int id) const 
    {
        auto query = select_from<T>("Id"_c, "FirstName"_c, "LastName"_c)
                   | where("Id"_c == id);

        auto result = conn_.and_then(query | to<T>);
        if (!result) 
        {
            return std::nullopt;
        }
        return result.value();
    }

    auto insert_one(const T& item) const 
    {
        return sqlgen::write(conn_, item);
    }

    auto insert_many(const std::vector<T>& items) const 
    {
        return sqlgen::write(conn_, items);
    }

    auto update_one(const T& item) const 
    {
        return sqlgen::write(conn_, item);
    }

    auto delete_by_id(int id) const
    {
         const auto query = delete_from<T> | where("Id"_c == id);
         query(conn_).value();
    }

    bool exists_by_id(int id) const 
    {
        return get_by_id(id).has_value();
    }

    std::size_t count() const 
    {
        return get_all().size();
    }

protected:
    Conn conn_;
};
