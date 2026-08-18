export module dal:schema_person_something;

import std;

export namespace DAL::Schema
{
    struct Person_SomethingDTO
    {
        std::int32_t person_id{ 0 };
        std::int32_t something_id{ 0 };
        std::string association_type;
    };
} // namespace DAL::Schema