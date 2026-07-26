export module core:iperson_something_repository;

import std;
import :person_something;
import :igeneric_repository;

export namespace Core 
{
    class IPerson_SomethingRepository : public virtual IGenericRepository<Person_Something>
    {
    public:
        virtual ~IPerson_SomethingRepository() = default;
    };
} // namespace Core
