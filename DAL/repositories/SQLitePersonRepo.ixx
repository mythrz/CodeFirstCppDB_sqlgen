export module dal:sqlite_repo;

import std;
import core;
import :schema;

export namespace DAL {

    class SQLitePersonRepository final : public Core::IPersonRepository {
    private:
        std::string m_dbPath;

    public:
        explicit SQLitePersonRepository(std::string dbPath) : m_dbPath(std::move(dbPath)) {}

        ~SQLitePersonRepository() override = default;

        [[nodiscard]] std::vector<Core::Person> get_all() const override;
        [[nodiscard]] std::optional<Core::Person> get_by_id(int id) const override;
        [[nodiscard]] std::optional<Core::Person> find_by_id(int id) const override;
        [[nodiscard]] std::vector<Core::Person> find_by_last_name(const std::string& lastName) const override;
        
        bool insert_one(const Core::Person& item) override;
        bool insert_many(const std::vector<Core::Person>& items) override;
        bool update_one(const Core::Person& item) override;
        void delete_by_id(int id) override;
        
        [[nodiscard]] bool exists_by_id(int id) const override;
        [[nodiscard]] std::size_t count() const override;
    };
}