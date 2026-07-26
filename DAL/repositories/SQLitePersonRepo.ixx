export module dal:sqlite_person_repo;

import std;
import core;
// import :schema_person;
// import :mappers;
import :generic_repo;

export namespace DAL::Repositories {

template <typename ConnectionHandle>
    requires DatabaseConnection<ConnectionHandle>
class SQLitePersonRepo 
    : public GenericRepository<Core::Person, DAL::Schema::PersonDTO, ConnectionHandle>,
      public Core::IPersonRepository {
    
    using Base = GenericRepository<Core::Person, DAL::Schema::PersonDTO, ConnectionHandle>;

public:
    explicit SQLitePersonRepo(ConnectionHandle& conn) noexcept 
        : Base(conn) {}

    [[nodiscard]] std::vector<Core::Person> get_all() const override { 
        return Base::get_all(); 
    }

    [[nodiscard]] std::optional<Core::Person> find_by_id(int id) const override {
        // Direct template invocation handles the dependent name lookup correctly
        auto dto_opt = this->conn_.template find_by_id<DAL::Schema::PersonDTO>(id);
        if (!dto_opt) {
            return std::nullopt;
        }
        return DAL::Mappers::MapperTraits<Core::Person, DAL::Schema::PersonDTO>::to_domain(*dto_opt);
    }
    
    [[nodiscard]] std::optional<Core::Person> get_by_id(int id) const override {
        auto dto_opt = this->conn_.template find_by_id<DAL::Schema::PersonDTO>(id);
        if (!dto_opt) {
            return std::nullopt;
        }
        return DAL::Mappers::MapperTraits<Core::Person, DAL::Schema::PersonDTO>::to_domain(*dto_opt);
    }

    bool insert_one(const Core::Person& item) override { 
        auto dto = DAL::Mappers::MapperTraits<Core::Person, DAL::Schema::PersonDTO>::to_dto(item);
        return this->conn_.insert(dto); 
    }

    bool insert_many(const std::vector<Core::Person>& items) override {
        if (items.empty()) {
            return true;
        }

        // Lazy view pipeline transformation avoids heap allocations
        auto dto_view = items | std::views::transform(
            DAL::Mappers::MapperTraits<Core::Person, DAL::Schema::PersonDTO>::to_dto
        );

        bool success = true;
        for (const auto& dto : dto_view) {
            if (!this->conn_.insert(dto)) {
                success = false;
            }
        }
        return success;
    }

    bool update_one(const Core::Person& item) override { 
        auto dto = DAL::Mappers::MapperTraits<Core::Person, DAL::Schema::PersonDTO>::to_dto(item);
        return this->conn_.update(dto); 
    }

    void delete_by_id(int id) override { 
        this->conn_.template delete_by_id<DAL::Schema::PersonDTO>(id); 
    }

    [[nodiscard]] bool exists_by_id(int id) const override { 
        return this->conn_.template exists<DAL::Schema::PersonDTO>(id); 
    }

    [[nodiscard]] std::size_t count() const override { 
        return this->conn_.template count<DAL::Schema::PersonDTO>(); 
    }

    // --- Domain-Specific Query Operations ---

    [[nodiscard]] std::vector<Core::Person> find_by_last_name(const std::string& lastName) const override {
        auto dtos = this->conn_.template query<DAL::Schema::PersonDTO>()
                              .where("last_name", lastName)
                              .execute();

        auto domain_view = dtos | std::views::transform(
            DAL::Mappers::MapperTraits<Core::Person, DAL::Schema::PersonDTO>::to_domain
        );
        return std::vector<Core::Person>(domain_view.begin(), domain_view.end());
    }
};

} // namespace DAL::Repositories

// export module dal:sqlite_person_repo;

// import std;
// import core;
// // import :schema_person;
// // import :mappers;
// import :generic_repo;

// export namespace DAL::Repositories {

// template <typename ConnectionHandle>
// class SQLitePersonRepo 
//     : public GenericRepository<Core::Person, DAL::Schema::PersonDTO, ConnectionHandle>,
//       public Core::IPersonRepository {
    
//     using Base = GenericRepository<Core::Person, DAL::Schema::PersonDTO, ConnectionHandle>;

// public:
//     explicit SQLitePersonRepo(ConnectionHandle& conn) noexcept : Base(conn) {}

//     // --- Core::IPersonRepository Interface Contract Fulfillment ---

//     [[nodiscard]] std::vector<Core::Person> get_all() const override { 
//         return Base::get_all(); 
//     }

//     // Fixed: Fully implements find_by_id with exact const qualifier and return type
//     [[nodiscard]] std::optional<Core::Person> find_by_id(int id) const override {
//         // Safe query construction using C++20 std::format
//         auto query_str = std::format("SELECT id, first_name, last_name FROM person WHERE id = {};", id);
        
//         auto dto_opt = this->conn_.get().template find_by_id<DAL::Schema::PersonDTO>(id);
//         if (!dto_opt) {
//             return std::nullopt;
//         }
//         return DAL::Mappers::MapperTraits<Core::Person, DAL::Schema::PersonDTO>::to_domain(*dto_opt);
//     }

//     bool insert_one(const Core::Person& item) override { 
//         auto dto = DAL::Mappers::MapperTraits<Core::Person, DAL::Schema::PersonDTO>::to_dto(item);
//         return this->conn_.get().insert(dto); 
//     }

//     // Fixed: Added missing insert_many implementation using C++20 ranges
//     bool insert_many(const std::vector<Core::Person>& items) override {
//         if (items.empty()) {
//             return true;
//         }

//         // Project domain entities into DTOs using std::views::transform
//         auto dto_view = items | std::views::transform(
//             DAL::Mappers::MapperTraits<Core::Person, DAL::Schema::PersonDTO>::to_dto
//         );

//         bool success = true;
//         for (const auto& dto : dto_view) {
//             if (!this->conn_.get().insert(dto)) {
//                 success = false;
//             }
//         }
//         return success;
//     }

//     bool update_one(const Core::Person& item) override { 
//         auto dto = DAL::Mappers::MapperTraits<Core::Person, DAL::Schema::PersonDTO>::to_dto(item);
//         return this->conn_.get().update(dto); 
//     }

//     void delete_by_id(int id) override { 
//         this->conn_.get().template delete_by_id<DAL::Schema::PersonDTO>(id); 
//     }

//     [[nodiscard]] bool exists_by_id(int id) const override { 
//         return this->conn_.get().template exists<DAL::Schema::PersonDTO>(id); 
//     }

//     [[nodiscard]] std::size_t count() const override { 
//         return this->conn_.get().template count<DAL::Schema::PersonDTO>(); 
//     }

//     // --- Specialized Domain Queries ---

//     [[nodiscard]] std::vector<Core::Person> find_by_last_name(const std::string& lastName) const override {
//         auto dtos = this->conn_.get().template query<DAL::Schema::PersonDTO>()
//                               .where("last_name", lastName)
//                               .execute();

//         auto domain_view = dtos | std::views::transform(
//             DAL::Mappers::MapperTraits<Core::Person, DAL::Schema::PersonDTO>::to_domain
//         );
//         return std::vector<Core::Person>(domain_view.begin(), domain_view.end());
//     }
// };

// } // namespace DAL::Repositories

// export module dal:sqlite_person_repo;

// import std;
// import core;
// // import dal:schema_person;
// // import dal:generic_repo;
// // import :schema_person;
// import :generic_repo;
// // import :mappers;

// export namespace DAL::Repositories {

// template <typename ConnectionHandle>
// class SQLitePersonRepo 
//     : public GenericRepository<Core::Person, DAL::Schema::PersonDTO, ConnectionHandle>,
//       public Core::IPersonRepository {
    
//     using Base = GenericRepository<Core::Person, DAL::Schema::PersonDTO, ConnectionHandle>;

// public:
//     explicit SQLitePersonRepo(ConnectionHandle& conn) : Base(conn) {}

//     // IPersonRepository Abstract Contract Fulfillment
//     [[nodiscard]] std::vector<Core::Person> get_all() const override { return Base::get_all(); }
//     std::optional<Core::Person> get_by_id(int id) const override { return Base::get_by_id(id); }
//     /// TODO: this cannot stay like this. No raw queries...
//     [[nodiscard]] std::optional<Core::Person> find_by_id(int id) const override {
//         auto query_str = std::format("SELECT id, first_name, last_name FROM person WHERE id = {};", id);
        
//         auto dto_opt = this->conn_.get().template find_by_id<DAL::Schema::PersonDTO>(id);
//         if (!dto_opt) {
//             return std::nullopt;
//         }
//         return DAL::Mappers::MapperTraits<Core::Person, DAL::Schema::PersonDTO>::to_domain(*dto_opt);
//     }
//     bool insert_one(const Core::Person& item) override { return Base::insert_one(item); }
//     bool update_one(const Core::Person& item) override { return Base::update_one(item); }
//     void delete_by_id(int id) override { Base::delete_by_id(id); }
//     bool exists_by_id(int id) const override { return Base::exists_by_id(id); }
//     std::size_t count() const override { return Base::count(); }

//     // Specialized Domain Queries
//     std::vector<Core::Person> find_by_last_name(const std::string& lastName) const override {
//         auto dtos = this->conn_.template query<DAL::Schema::PersonDTO>()
//                               .where("last_name", lastName)
//                               .execute();

//         auto domain_view = dtos | std::views::transform(
//             DAL::Mappers::MapperTraits<Core::Person, DAL::Schema::PersonDTO>::to_domain
//         );
//         return std::vector<Core::Person>(domain_view.begin(), domain_view.end());
//     }
// };

// } // namespace DAL::Repositories

// export module dal:sqlite_repo;

// import std;
// import core;
// import :schema;

// export namespace DAL {

//     class SQLitePersonRepository final : public Core::IPersonRepository {
//     private:
//         std::string m_dbPath;

//     public:
//         explicit SQLitePersonRepository(std::string dbPath) : m_dbPath(std::move(dbPath)) {}

//         ~SQLitePersonRepository() override = default;

//         [[nodiscard]] std::vector<Core::Person> get_all() const override;
//         [[nodiscard]] std::optional<Core::Person> get_by_id(int id) const override;
//         [[nodiscard]] std::optional<Core::Person> find_by_id(int id) const override;
//         [[nodiscard]] std::vector<Core::Person> find_by_last_name(const std::string& lastName) const override;
        
//         bool insert_one(const Core::Person& item) override;
//         bool insert_many(const std::vector<Core::Person>& items) override;
//         bool update_one(const Core::Person& item) override;
//         void delete_by_id(int id) override;
        
//         [[nodiscard]] bool exists_by_id(int id) const override;
//         [[nodiscard]] std::size_t count() const override;
//     };
// }