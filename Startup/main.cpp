#include <sqlgen.hpp>
#include <sqlgen/sqlite.hpp>

import std;
import core;
import dal;

// Application-specific DbContext coordinating entities (EF Core style)
class AppDbContext : public DAL::DbContextBase<DAL::SQLiteConnection>
{
public:
    Set<Core::Person> people{ this };
    Set<Core::Something> somethings{ this };
    Set<Core::Person_Something> personSomethings{ this };

    explicit AppDbContext(std::shared_ptr<DAL::SQLiteConnection> conn)
        : DAL::DbContextBase<DAL::SQLiteConnection>(std::move(conn))
    {
    }
};

int main(int argc, char* argv[])
{
    const std::string dbPath = "relationalDB.db";
    std::println("Target SQLite Database path: {}", dbPath);

    // Support optional --no-reset CLI argument to keep database across runs
    bool shouldReset = true;
    for (int i = 1; i < argc; ++i)
    {
        if (std::string_view(argv[i]) == "--no-reset")
        {
            shouldReset = false;
        }
    }

    if (shouldReset && std::filesystem::exists(dbPath))
    {
        std::filesystem::remove(dbPath);
        std::println("Reset: Removed existing database file.");
    }

    std::println("Connecting to SQLite database: {}", dbPath);
    auto rawConnRes = sqlgen::sqlite::connect(dbPath);
    if (!rawConnRes)
    {
        std::println(stderr, "Failed to connect to SQLite: {}", rawConnRes.error().what());
        return 1;
    }

    // 1. DDL: Ensure tables exist
    std::println("Creating tables if they do not exist...");
    auto ddlResult = rawConnRes.and_then(sqlgen::create_table<DAL::Schema::PersonDTO> | sqlgen::if_not_exists)
                               .and_then(sqlgen::create_table<DAL::Schema::SomethingDTO> | sqlgen::if_not_exists)
                               .and_then(sqlgen::create_table<DAL::Schema::Person_SomethingDTO> | sqlgen::if_not_exists);

    if (!ddlResult)
    {
        std::println(stderr, "Failed to create database tables.");
        return 1;
    }

    auto rawConn = rawConnRes.value();

    // Enforce SQLite foreign key constraints
    rawConn->execute("PRAGMA foreign_keys = ON;");

    // 2. Wrap the SQLite connection in DAL's connection adapter
    auto connAdapter = std::make_shared<DAL::SQLiteConnection>(rawConn);

    // 3. Initialize DbContext (unified single & composite key DbSets)
    AppDbContext db(connAdapter);

    // 4. Seed database via clean entity and repository abstractions
    std::println("Inserting initial entities via DbContext / Repositories...");

    Core::Person person1(Core::PersonId{ 1 }, "Rz", "D");
    Core::Person person2(Core::PersonId{ 2 }, "R First", "D Last");
    auto r1 = db.people.add(person1);
    auto r2 = db.people.add(person2);
    if (!r1 || !r2)
    {
        std::println(stderr, "Failed to insert person entities.");
        return 1;
    }

    Core::Something something1(Core::SomethingId{ 1 }, "Laptop", "Electronics", "Developer Workstation");
    Core::Something something2(Core::SomethingId{ 2 }, "Coffee Machine", "Appliances", "Espresso Maker");
    auto s1 = db.somethings.add(something1);
    auto s2 = db.somethings.add(something2);
    if (!s1 || !s2)
    {
        std::println(stderr, "Failed to insert something entities.");
        return 1;
    }

    Core::Person_Something link1(Core::PersonId{ 1 }, Core::SomethingId{ 1 }, "Owner");
    Core::Person_Something link2(Core::PersonId{ 2 }, Core::SomethingId{ 2 }, "User");
    auto l1 = db.personSomethings.add(link1);
    auto l2 = db.personSomethings.add(link2);
    if (!l1 || !l2)
    {
        if (!l1) std::println(stderr, "Failed to insert junction entity 1: {}", l1.error().to_string());
        if (!l2) std::println(stderr, "Failed to insert junction entity 2: {}", l2.error().to_string());
        return 1;
    }

    auto saveRes = db.save_changes();
    if (!saveRes)
    {
        std::println(stderr, "Database save failed: {}", saveRes.error().to_string());
        return 1;
    }
    std::println("Database setup complete. Created tables and persisted domain records.");

    // 5. Query and verify persisted entities through clean domain layer (No DTO leaks!)
    std::println("\nVerifying persisted entities via DbContext / Core domain models:");

    std::println("--- Person Entities (via db.people) ---");
    for (const auto& p : db.people.to_vector())
    {
        std::println("Person [ID: {}]: {} {}", p.getId(), p.getFirstName(), p.getLastName());
    }

    std::println("--- Something Entities (via db.somethings) ---");
    for (const auto& s : db.somethings.to_vector())
    {
        std::println("Something [ID: {}]: {} ({}) - {}", s.getId(), s.getName(), s.getCategory(), s.getDescription().value_or("N/A"));
    }

    std::println("--- Person_Something Junction Entities (via db.personSomethings) ---");
    for (const auto& link : db.personSomethings.to_vector())
    {
        std::println("Person_Something: PersonID {} <-> SomethingID {} ({})",
                     link.getPersonId(), link.getSomethingId(), link.getAssociationType());
    }

    // 6. Demonstrate LINQ-style query
    auto devItems = db.somethings.where([](const Core::Something& item) {
        return item.getCategory() == "Electronics";
    });
    std::println("\nQuery: Found {} Electronics item(s):", devItems.size());
    for (const auto& item : devItems)
    {
        std::println(" -> {}", item.getName());
    }

    std::println("\nDatabase successfully saved to: {}", std::filesystem::absolute(dbPath).string());
    return 0;
}
