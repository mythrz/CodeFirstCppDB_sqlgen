export module samples:iperson_repository;

import std;
import core;
import :person;

export namespace Core 
{
    class IPersonRepository : public virtual IGenericRepository<Person>
    {
    public:
        virtual ~IPersonRepository() = default;
        
        [[nodiscard]] virtual std::vector<Core::Person> find_by_last_name(const std::string& lastName) const = 0;
    };
} // namespace Core
