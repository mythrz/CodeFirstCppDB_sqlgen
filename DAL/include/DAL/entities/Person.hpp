#pragma once

#include "sqlgen/PrimaryKey.hpp"
#include <string>
#include <cstdint>

struct Person 
{
    sqlgen::PrimaryKey<uint32_t> Id;
    std::string FirstName;
    std::string LastName;
};
