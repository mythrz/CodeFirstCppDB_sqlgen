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
    auto connRes = DAL::SQLiteConnection::connect(dbPath);
    if (!connRes)
    {
        std::println(std::cerr, "Failed to connect to SQLite: {}", connRes.error().to_string());
        return 1;
    }

    // 1. Initialize DbContext and auto-create schema from reflected DbSet members
    AppDbContext db(connRes.value());

    std::println("Ensuring database schema and foreign keys...");
    auto schemaRes = db.ensure_created();
    if (!schemaRes)
    {
        std::println(std::cerr, "Failed to create database schema: {}", schemaRes.error().to_string());
        return 1;
    }

    // 2. Stage seed entities via Unit of Work and commit atomically
    std::println("Staging initial entities via Unit of Work...");
    db.defer_changes(true);

    Core::Person person1(Core::PersonId{ 1 }, "Rz", "D");
    Core::Person person2(Core::PersonId{ 2 }, "R First", "D Last");
    (void)db.people.add(person1);
    (void)db.people.add(person2);

    Core::Something something1(Core::SomethingId{ 1 }, "Laptop", "Electronics", "Developer Workstation");
    Core::Something something2(Core::SomethingId{ 2 }, "Coffee Machine", "Appliances", "Espresso Maker");
    (void)db.somethings.add(something1);
    (void)db.somethings.add(something2);

    Core::Person_Something link1(Core::PersonId{ 1 }, Core::SomethingId{ 1 }, "Owner");
    Core::Person_Something link2(Core::PersonId{ 2 }, Core::SomethingId{ 2 }, "User");
    (void)db.personSomethings.add(link1);
    (void)db.personSomethings.add(link2);

    auto saveRes = db.save_changes();
    if (!saveRes)
    {
        std::println(std::cerr, "Database save failed: {}", saveRes.error().to_string());
        return 1;
    }
    db.defer_changes(false);
    std::println("Database setup complete. Created tables and persisted domain records.");

    // 3. Query and verify persisted entities through clean domain layer (No DTO leaks!)
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

    // 4. Demonstrate LINQ-style query
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
