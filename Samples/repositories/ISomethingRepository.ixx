export module codefirst.samples:isomething_repository;

import std;
import codefirst.core;
import :something;

export namespace Core
{
    class ISomethingRepository : public virtual IGenericRepository<Something>
    {
    public:
        virtual ~ISomethingRepository() = default;
    };
} // namespace Core
