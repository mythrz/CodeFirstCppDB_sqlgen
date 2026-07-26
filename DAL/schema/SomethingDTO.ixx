export module dal:schema_something;

import std;

export namespace DAL::Schema 
{
    struct SomethingDTO 
    {
        int id{0};
        std::string name;
        std::string category;
        std::optional<std::string> description;
    };

} // namespace DAL::Schema