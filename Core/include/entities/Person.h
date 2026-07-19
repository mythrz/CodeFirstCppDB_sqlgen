#pragma once

#include <string>
#include <cstdint>

namespace Core {

class Person {
public:
    Person() = default;
    Person(uint32_t id, const std::string& firstName, const std::string& lastName)
        : id_(id), firstName_(firstName), lastName_(lastName) {}
    
    uint32_t getId() const { return id_; }
    void setId(uint32_t id) { id_ = id; }
    
    const std::string& getFirstName() const { return firstName_; }
    void setFirstName(const std::string& firstName) { firstName_ = firstName; }
    
    const std::string& getLastName() const { return lastName_; }
    void setLastName(const std::string& lastName) { lastName_ = lastName; }

private:
    uint32_t id_ = 0;
    std::string firstName_;
    std::string lastName_;
};

} // namespace Core
