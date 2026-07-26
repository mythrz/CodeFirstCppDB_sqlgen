#include <sqlgen.hpp>
#include <sqlgen/sqlite.hpp>

import std;
import core;
import dal;

int main() 
{
    const std::string dbPath = "relationalDB.db";
    std::println("Target SQLite Database path: {}", dbPath);

    if (std::filesystem::exists(dbPath)) 
    {
        std::filesystem::remove(dbPath);
        std::println("Removed existing database file.");
    }

    std::println("Connecting to SQLite database: {}", dbPath);
    auto conn = sqlgen::sqlite::connect(dbPath);

    std::vector<DAL::Schema::PersonDTO> persons = {
        DAL::Schema::PersonDTO{.id = 1, .first_name = "Alice", .last_name = "Smith"},
        DAL::Schema::PersonDTO{.id = 2, .first_name = "Bob", .last_name = "Johnson"}
    };

    std::vector<DAL::Schema::SomethingDTO> somethings = {
        DAL::Schema::SomethingDTO
        {
            .id = 1,
            .name = "Laptop",
            .category = "Electronics",
            .description = "Developer Workstation"
        },
        DAL::Schema::SomethingDTO
        {
            .id = 2,
            .name = "Coffee Machine",
            .category = "Appliances",
            .description = "Espresso Maker"
        }
    };

    std::vector<DAL::Schema::Person_SomethingDTO> personSomethings = 
    {
        DAL::Schema::Person_SomethingDTO
        {
            .person_id = 1,
            .something_id = 1,
            .association_type = "Owner"
        },
        DAL::Schema::Person_SomethingDTO
        {
            .person_id = 2,
            .something_id = 2,
            .association_type = "User"
        }
    };

    std::println("Creating tables and inserting 2 rows for each entity...");

    auto result = conn
        .and_then(sqlgen::create_table<DAL::Schema::PersonDTO> | sqlgen::if_not_exists)
        .and_then(sqlgen::create_table<DAL::Schema::SomethingDTO> | sqlgen::if_not_exists)
        .and_then(sqlgen::create_table<DAL::Schema::Person_SomethingDTO> | sqlgen::if_not_exists)
        .and_then(sqlgen::insert(std::ref(persons)))
        .and_then(sqlgen::insert(std::ref(somethings)))
        .and_then(sqlgen::insert(std::ref(personSomethings)));

    if (result) 
    {
        std::println("Database setup complete. Created tables and inserted 2 rows for each entity.");
    } 
    else 
    {
        std::println(stderr, "Failed to initialize database tables or insert records.");
        return 1;
    }

    std::println("\nVerifying persisted entities from SQLite database:");

    auto readPersons = sqlgen::read<std::vector<DAL::Schema::PersonDTO>>(conn);
    if (readPersons) 
    {
        std::println("--- Person Table ---");
        for (const auto& dto : readPersons.value()) 
        {
            Core::Person p = DAL::Mappers::MapperTraits<Core::Person, DAL::Schema::PersonDTO>::to_domain(dto);
            std::println("Person [ID: {}]: {} {}", p.getId(), p.getFirstName(), p.getLastName());
        }
    }

    auto readSomethings = sqlgen::read<std::vector<DAL::Schema::SomethingDTO>>(conn);
    if (readSomethings) 
    {
        std::println("--- Something Table ---");
        for (const auto& dto : readSomethings.value()) 
        {
            Core::Something s = DAL::Mappers::MapperTraits<Core::Something, DAL::Schema::SomethingDTO>::to_domain(dto);
            std::println("Something [ID: {}]: {} ({}) - {}", 
                         s.getId(), s.getName(), s.getCategory(), s.getDescription().value_or("N/A"));
        }
    }

    auto readPersonSomethings = sqlgen::read<std::vector<DAL::Schema::Person_SomethingDTO>>(conn);
    if (readPersonSomethings) 
    {
        std::println("--- Person_Something Table ---");
        for (const auto& dto : readPersonSomethings.value()) 
        {
            Core::Person_Something ps = DAL::Mappers::MapperTraits<Core::Person_Something, DAL::Schema::Person_SomethingDTO>::to_domain(dto);
            std::println("Person_Something: PersonID {} <-> SomethingID {} ({})", 
                         ps.getPersonId(), ps.getSomethingId(), ps.getAssociationType());
        }
    }

    std::println("\nDatabase successfully saved to: {}", std::filesystem::absolute(dbPath).string());
    return 0;
}