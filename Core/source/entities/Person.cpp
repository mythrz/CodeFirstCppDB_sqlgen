#include "../../include/entities/Person.h"

namespace Core {

Person::Person(uint32_t id, const std::string& firstName, const std::string& lastName)
    : id_(id), firstName_(firstName), lastName_(lastName) {}

uint32_t Person::getId() const { return id_; }
void Person::setId(uint32_t id) { id_ = id; }

const std::string& Person::getFirstName() const { return firstName_; }
void Person::setFirstName(const std::string& firstName) { firstName_ = firstName; }

const std::string& Person::getLastName() const { return lastName_; }
void Person::setLastName(const std::string& lastName) { lastName_ = lastName; }

} // namespace Core