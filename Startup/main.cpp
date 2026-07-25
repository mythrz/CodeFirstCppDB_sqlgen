import std;
import core;
import dal;

int main() {
    std::println("Initializing Code-First C++ 23 Database Generation...");

    // Dependency Injection: Core interface wrapping a specific DAL implementation
    std::unique_ptr<Core::IPersonRepository> repository = 
        std::make_unique<DAL::SQLitePersonRepository>("relationalDB.db");

    Core::Person developer(1, "Arch", "User");
    repository->insert_one(developer);

    if (auto fetched = repository->get_by_id(1)) {
        std::println("Successfully retrieved entity: {} {}", 
            fetched->getFirstName(), fetched->getLastName());
    } else {
        std::println("Failed to retrieve entity.");
    }

    return 0;
}