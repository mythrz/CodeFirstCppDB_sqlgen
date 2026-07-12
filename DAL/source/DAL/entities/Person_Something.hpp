#include "sqlgen/ForeignKey.hpp"
#include <cstdint>
#include "Person.hpp"
#include "Something.hpp"

struct Person_Something
{
    sqlgen::ForeignKey<uint32_t, Person, "Id"> PersonId;
    sqlgen::ForeignKey<uint32_t, Something, "Id"> SomethingId;
    uint32_t Quantity;
};