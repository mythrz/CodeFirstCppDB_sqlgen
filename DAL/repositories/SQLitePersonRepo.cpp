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

    std::vector<Core::Person> SQLitePersonRepository::get_all() const {
        return {};
    }

    std::optional<Core::Person> SQLitePersonRepository::find_by_id(int id) const {
        return get_by_id(id);
    }

    std::vector<Core::Person> SQLitePersonRepository::find_by_last_name(const std::string& lastName) const {
        return {};
    }

    bool SQLitePersonRepository::insert_many(const std::vector<Core::Person>& items) {
        for (const auto& item : items) {
            insert_one(item);
        }
        return true;
    }

    bool SQLitePersonRepository::update_one(const Core::Person& item) {
        return false;
    }

    void SQLitePersonRepository::delete_by_id(int id) {}

    bool SQLitePersonRepository::exists_by_id(int id) const {
        return get_by_id(id).has_value();
    }

    std::size_t SQLitePersonRepository::count() const {
        return 0;
    }
}