export module samples:isomething_repository;

import std;
import core;
import :something;

export namespace Core
{
    class ISomethingRepository : public virtual IGenericRepository<Something>
    {
    public:
        virtual ~ISomethingRepository() = default;
    };
} // namespace Core
