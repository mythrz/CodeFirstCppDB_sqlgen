export module core:isomething_repository;

import std;
import :something;
import :igeneric_repository;

export namespace Core 
{
    class ISomethingRepository : public virtual IGenericRepository<Something>
    {
    public:
        virtual ~ISomethingRepository() = default;
    };
} // namespace Core
