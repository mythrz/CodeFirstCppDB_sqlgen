import std;
import core;
import dal;

namespace {

// Moved to namespace scope: C++ forbids member templates inside local classes
struct MockConnection {
    template <typename DTO> std::vector<DTO> fetch_all() { return {}; }
    template <typename DTO> std::optional<DTO> find_by_id(std::int32_t) { return std::nullopt; }
    template <typename DTO> bool insert(const DTO&) { return true; }
    template <typename DTO> bool update(const DTO&) { return true; }
    template <typename DTO> void delete_by_id(std::int32_t) {}
    template <typename DTO> bool exists(std::int32_t) { return false; }
    template <typename DTO> std::size_t count() { return 0; }

    template <typename DTO>
    struct QueryBuilder {
        QueryBuilder& where(std::string_view, std::string_view) { return *this; }
        std::vector<DTO> execute() { return {}; }
    };

    template <typename DTO> 
    QueryBuilder<DTO> query() { return {}; }
};

} // namespace

int main() {
    std::println("Initializing Code-First Modular Engine...");

    MockConnection connection;

    std::unique_ptr<Core::IPersonRepository> personRepo = 
        std::make_unique<DAL::Repositories::SQLitePersonRepo<MockConnection>>(connection);

    Core::Person newPerson(1, "Ada", "Lovelace");
    if (personRepo->insert_one(newPerson)) {
        std::println("Successfully persisted entity: {}", newPerson.getFirstName());
    }

    return 0;
}

// import std;
// import core;
// import dal;

// int main() {
//     std::println("Initializing Code-First Modular Engine...");

//     // Simulated database connection context handle
//     struct MockConnection {
//         template <typename DTO> std::vector<DTO> fetch_all() { return {}; }
//         template <typename DTO> std::optional<DTO> find_by_id(std::int32_t) { return std::nullopt; }
//         template <typename DTO> bool insert(const DTO&) { return true; }
//         template <typename DTO> bool update(const DTO&) { return true; }
//         template <typename DTO> void delete_by_id(std::int32_t) {}
//         template <typename DTO> bool exists(std::int32_t) { return false; }
//         template <typename DTO> std::size_t count() { return 0; }
        
//         template <typename DTO>
//         struct QueryBuilder {
//             QueryBuilder& where(std::string_view, std::string_view) { return *this; }
//             std::vector<DTO> execute() { return {}; }
//         };
//         template <typename DTO> QueryBuilder<DTO> query() { return {}; }
//     } connection;

//     // Direct instantiation using IPersonRepository interface
//     std::unique_ptr<Core::IPersonRepository> personRepo = 
//         std::make_unique<DAL::Repositories::SQLitePersonRepo<MockConnection>>(connection);

//     Core::Person newPerson(1, "Ada", "Lovelace");
//     if (personRepo->insert_one(newPerson)) {
//         std::println("Successfully persisted entity: {}", newPerson.getFirstName());
//     }

//     return 0;
// }

// import std;
// import core;
// import dal;

// int main() {
//     std::println("Initializing Code-First C++ 23 Database Generation...");

//     // Dependency Injection: Core interface wrapping a specific DAL implementation
//     std::unique_ptr<Core::IPersonRepository> repository = 
//         std::make_unique<DAL::SQLitePersonRepository>("relationalDB.db");

//     Core::Person developer(1, "Arch", "User");
//     repository->insert_one(developer);

//     if (auto fetched = repository->get_by_id(1)) {
//         std::println("Successfully retrieved entity: {} {}", 
//             fetched->getFirstName(), fetched->getLastName());
//     } else {
//         std::println("Failed to retrieve entity.");
//     }

//     return 0;
// }