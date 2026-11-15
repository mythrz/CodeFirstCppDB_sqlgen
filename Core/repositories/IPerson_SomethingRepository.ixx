export module samples:iperson_something_repository;

import std;
import core;
import :person_something;

export namespace Core 
{
    class IPerson_SomethingRepository : public virtual IRepository<Person_Something>
    {
    public:
        virtual ~IPerson_SomethingRepository() = default;

        [[nodiscard]] virtual std::optional<Person_Something> get_by_composite_id(PersonId personId, SomethingId somethingId) const = 0;
        [[nodiscard]] virtual std::vector<Person_Something> find_by_person_id(PersonId personId) const = 0;
        [[nodiscard]] virtual std::vector<Person_Something> find_by_something_id(SomethingId somethingId) const = 0;
        virtual std::expected<void, DbError> delete_by_composite_id(PersonId personId, SomethingId somethingId) = 0;
        [[nodiscard]] virtual bool exists_by_composite_id(PersonId personId, SomethingId somethingId) const = 0;
    };
} // namespace Core
