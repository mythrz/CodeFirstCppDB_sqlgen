export module dal:schema_something;

import std;

export namespace DAL::Schema
{
    struct SomethingDTO
    {
        std::int32_t id{ 0 };
        std::string name;
        std::string category;
        std::optional<std::string> description;
    };

} // namespace DAL::Schema