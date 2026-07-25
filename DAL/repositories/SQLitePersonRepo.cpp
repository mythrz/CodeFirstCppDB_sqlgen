module;

#include <sqlgen/sqlite.hpp>
#include <rfl.hpp>

module dal;

import std;

namespace DAL 
{
    static auto connect(const std::string& path) {
        return sqlgen::sqlite::connect(path);
    }

    std::optional<Core::Person> SQLitePersonRepository::get_by_id(int id) const {
        auto conn = connect(m_dbPath);
        
        auto result = sqlgen::read<std::vector<Schema::PersonDTO>>(conn);
        if (!result.has_value()) return std::nullopt;

        auto it = std::ranges::find_if(result.value(), [id](const auto& dto) {
            return dto.id == id;
        });

        if (it != result.value().end()) {
            return Core::Person(it->id, it->first_name, it->last_name);
        }
        
        return std::nullopt;
    }

    bool SQLitePersonRepository::insert_one(const Core::Person& item) {
        auto conn = connect(m_dbPath);

        Schema::PersonDTO dto{
            .id = static_cast<int>(item.getId()),
            .first_name = item.getFirstName(),
            .last_name = item.getLastName()
        };

        sqlgen::write(conn, dto);
        return true; 
    }
}