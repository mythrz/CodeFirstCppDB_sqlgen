export module core:iperson_repository;

import std;
import :person;
import :igeneric_repository;

export namespace Core 
{
    class IPersonRepository : public IGenericRepository<Person>
    {
    public:
        virtual ~IPersonRepository() = default;
        
        // // Read Operations
        // [[nodiscard]] virtual std::vector<Core::Person> get_all() const = 0;
        // [[nodiscard]] virtual std::optional<Core::Person> get_by_id(int id) const = 0;
        // [[nodiscard]] virtual std::optional<Core::Person> find_by_id(int id) const = 0;
        [[nodiscard]] virtual std::vector<Core::Person> find_by_last_name(const std::string& lastName) const = 0;
        
        // // Write Operations
        // virtual bool insert_one(const Core::Person& item) = 0;
        // virtual bool insert_many(const std::vector<Core::Person>& items) = 0;
        // virtual bool update_one(const Core::Person& item) = 0;
        // virtual void delete_by_id(int id) = 0;
        
        // // Metadata
        // [[nodiscard]] virtual bool exists_by_id(int id) const = 0;
        // [[nodiscard]] virtual std::size_t count() const = 0;
    };
} // namespace Core
