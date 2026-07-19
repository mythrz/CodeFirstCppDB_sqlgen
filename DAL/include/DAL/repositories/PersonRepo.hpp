#pragma once

#include "repositories/generic/RepoGen001.hpp"
#include "entities/Person.hpp"

class PersonRepo : public RepoGen001<Person> {
public:
    using RepoGen001<Person>::RepoGen001;

    std::optional<Person> find_by_id(int id) const {
        auto query = select_from<Person>("Id"_c, "FirstName"_c, "LastName"_c)
                   | where("Id"_c == id);

        auto result = conn_.and_then(query | to<Person>);
        if (!result) {
            return std::nullopt;
        }
        return result.value();
    }

    std::vector<Person> find_by_last_name(const std::string& lastName) const {
        auto query = select_from<Person>("Id"_c, "FirstName"_c, "LastName"_c)
                   | where("LastName"_c == lastName);

        auto result = conn_.and_then(query | to<std::vector<Person>>);
        if (!result) {
            return {};
        }
        return result.value();
    }
};