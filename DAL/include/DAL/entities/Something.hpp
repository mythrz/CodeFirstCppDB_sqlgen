#pragma once

#include "sqlgen/PrimaryKey.hpp"
#include <string>
#include <cstdint>

struct Something 
{
    sqlgen::PrimaryKey<uint32_t> Id;
    std::string Name;
};
