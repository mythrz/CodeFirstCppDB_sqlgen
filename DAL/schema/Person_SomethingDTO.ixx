export module dal:schema_person_something;

import std;

export namespace DAL::Schema 
{
    struct Person_SomethingDTO 
    {
        int person_id{0};
        int something_id{0};
        std::string association_type;
    };
} // namespace DAL::Schema