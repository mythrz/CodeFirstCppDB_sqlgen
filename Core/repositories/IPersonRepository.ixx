export module core:iperson_repository;

import std;
import :person;
import :igeneric_repository;

export namespace Core 
{
    class IPersonRepository : public virtual IGenericRepository<Person>
    {
    public:
        virtual ~IPersonRepository() = default;
        
        [[nodiscard]] virtual std::vector<Core::Person> find_by_last_name(const std::string& lastName) const = 0;
    };
} // namespace Core
