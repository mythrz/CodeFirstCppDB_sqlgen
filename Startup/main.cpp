import std;
import core;
import dal;
import samples;

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

    std::println("Connecting to SQLite database: {}", dbPath);
    auto connRes = DAL::SQLiteConnection::connect(dbPath);
    if (!connRes)
    {
        std::println(std::cerr, "Failed to connect to SQLite: {}", connRes.error().to_string());
        return 1;
    }

    // 1. Initialize DbContext and reset/auto-create schema from reflected DbSet members
    AppDbContext db(connRes.value());

    if (shouldReset)
    {
        std::println("Reset: Dropping existing database tables via db.ensure_deleted()...");
        auto dropRes = db.ensure_deleted();
        if (!dropRes)
        {
            std::println(std::cerr, "Failed to drop database schema: {}", dropRes.error().to_string());
            return 1;
        }
    }

    std::println("Ensuring database schema and foreign keys...");
    auto schemaRes = db.ensure_created();
    if (!schemaRes)
    {
        std::println(std::cerr, "Failed to create database schema: {}", schemaRes.error().to_string());
        return 1;
    }

    // 2. Seed initial entities if database is empty (--no-reset safe)
    std::optional<Core::Person> person1Opt;
    if (db.people.count() == 0)
    {
        std::println("Seeding initial entities...");
        auto p1Res = db.people.add_returning(Core::Person(Core::PersonId{ 0 }, "Rz", "D"));
        if (!p1Res)
        {
            std::println(std::cerr, "Failed to add person1: {}", p1Res.error().to_string());
            return 1;
        }
        auto p2Res = db.people.add_returning(Core::Person(Core::PersonId{ 0 }, "R First", "D Last"));
        if (!p2Res)
        {
            std::println(std::cerr, "Failed to add person2: {}", p2Res.error().to_string());
            return 1;
        }

        auto s1Res = db.somethings.add_returning(
            Core::Something(Core::SomethingId{ 0 }, "Laptop", "Electronics", "Developer Workstation"));
        if (!s1Res)
        {
            std::println(std::cerr, "Failed to add something1: {}", s1Res.error().to_string());
            return 1;
        }
        auto s2Res = db.somethings.add_returning(
            Core::Something(Core::SomethingId{ 0 }, "Coffee Machine", "Appliances", "Espresso Maker"));
        if (!s2Res)
        {
            std::println(std::cerr, "Failed to add something2: {}", s2Res.error().to_string());
            return 1;
        }

        auto link1Res = db.personSomethings.add(
            Core::Person_Something(p1Res->getId(), s1Res->getId(), "Owner"));
        if (!link1Res)
        {
            std::println(std::cerr, "Failed to add link1: {}", link1Res.error().to_string());
            return 1;
        }
        auto link2Res = db.personSomethings.add(
            Core::Person_Something(p2Res->getId(), s2Res->getId(), "User"));
        if (!link2Res)
        {
            std::println(std::cerr, "Failed to add link2: {}", link2Res.error().to_string());
            return 1;
        }

        person1Opt = *p1Res;
        std::println("Database setup complete. Created tables and persisted domain records.");
    }
    else
    {
        std::println("Existing database detected (--no-reset); skipping seed.");
        auto existingPeople = db.people.to_vector();
        if (!existingPeople.empty())
        {
            person1Opt = existingPeople.front();
        }
    }

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

    // 4. Demonstrate server-side DbQuery filtering & compile-time FK navigation
    auto devItems = db.somethings
        .where_equal<"category">("Electronics")
        .to_vector();
    std::println("\nQuery: Found {} Electronics item(s):", devItems.size());
    for (const auto& item : devItems)
    {
        std::println(" -> {}", item.getName());
    }

    if (person1Opt)
    {
        auto ownedByPerson1 = db.query_many_to_many<Core::Something, Core::Person_Something>(*person1Opt);
        if (ownedByPerson1)
        {
            std::println("\nFK Navigation: Person {} owns {} item(s):", person1Opt->getId(), ownedByPerson1->size());
            for (const auto& item : *ownedByPerson1)
            {
                std::println(" -> {}", item.getName());
            }
        }
    }

    std::println("\nDatabase successfully saved to: {}", std::filesystem::absolute(dbPath).string());
    return 0;
}
