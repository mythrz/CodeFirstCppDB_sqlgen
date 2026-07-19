#pragma once

#include <optional>
#include <vector>
#include <string>
#include <cstddef>
#include "../entities/Person.h"

class IPersonRepository {
public:
    virtual ~IPersonRepository() = default;
    
    // Get all persons
    virtual std::vector<Core::Person> get_all() const = 0;
    
    // Get person by ID
    virtual std::optional<Core::Person> get_by_id(int id) const = 0;
    
    // Find person by ID (custom query)
    virtual std::optional<Core::Person> find_by_id(int id) const = 0;
    
    // Find persons by last name
    virtual std::vector<Core::Person> find_by_last_name(const std::string& lastName) const = 0;
    
    // Insert operations
    virtual bool insert_one(const Core::Person& item) = 0;
    virtual bool insert_many(const std::vector<Core::Person>& items) = 0;
    
    // Update operation
    virtual bool update_one(const Core::Person& item) = 0;
    
    // Delete operation
    virtual void delete_by_id(int id) = 0;
    
    // Check if exists
    virtual bool exists_by_id(int id) const = 0;
    
    // Count
    virtual std::size_t count() const = 0;
};
