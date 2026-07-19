#pragma once

#include "repositories/generic/RepoGen001.hpp"
#include "entities/Person.hpp"
#include "../../../Core/include/repositories/IPersonRepository.hpp"

// Mapping functions between DAL's Person struct and Core's Person class
inline Core::Person to_core(const Person& dto) {
    return Core::Person(dto.Id.value(), dto.FirstName, dto.LastName);
}

inline Person to_dto(const Core::Person& core) {
    return Person{
        core.getId(),
        core.getFirstName(),
        core.getLastName()
    };
}

class PersonRepo : public RepoGen001<Person>, public IPersonRepository {
public:
    using RepoGen001<Person>::RepoGen001;

    // IPersonRepository implementation - returns Core::Person
    std::vector<Core::Person> get_all() const override {
        auto dtoPersons = RepoGen001<Person>::get_all();
        std::vector<Core::Person> result;
        result.reserve(dtoPersons.size());
        for (const auto& dto : dtoPersons) {
            result.push_back(to_core(dto));
        }
        return result;
    }

    std::optional<Core::Person> get_by_id(int id) const override {
        auto result = RepoGen001<Person>::get_by_id(id);
        if (!result) return std::nullopt;
        return to_core(*result);
    }

    std::optional<Core::Person> find_by_id(int id) const override {
        auto query = select_from<Person>("Id"_c, "FirstName"_c, "LastName"_c)
                   | where("Id"_c == id);

        auto result = conn_.and_then(query | to<Person>);
        if (!result) {
            return std::nullopt;
        }
        return to_core(result.value());
    }

    std::vector<Core::Person> find_by_last_name(const std::string& lastName) const override {
        auto query = select_from<Person>("Id"_c, "FirstName"_c, "LastName"_c)
                   | where("LastName"_c == lastName);

        auto result = conn_.and_then(query | to<std::vector<Person>>);
        if (!result) {
            return {};
        }
        std::vector<Core::Person> coreResult;
        for (const auto& dto : result.value()) {
            coreResult.push_back(to_core(dto));
        }
        return coreResult;
    }

    bool insert_one(const Core::Person& item) override {
        return RepoGen001<Person>::insert_one(to_dto(item)).has_value();
    }

    bool insert_many(const std::vector<Core::Person>& items) override {
        std::vector<Person> dtoItems;
        dtoItems.reserve(items.size());
        for (const auto& core : items) {
            dtoItems.push_back(to_dto(core));
        }
        return RepoGen001<Person>::insert_many(dtoItems).has_value();
    }

    bool update_one(const Core::Person& item) override {
        return RepoGen001<Person>::update_one(to_dto(item)).has_value();
    }

    void delete_by_id(int id) override {
        RepoGen001<Person>::delete_by_id(id);
    }

    bool exists_by_id(int id) const override {
        return RepoGen001<Person>::exists_by_id(id);
    }

    std::size_t count() const override {
        return RepoGen001<Person>::count();
    }
};
