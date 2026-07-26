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

    // [[nodiscard]] std::vector<Core::Person> get_all() const override { 
    //     return Base::get_all(); 
    // }

    // [[nodiscard]] std::optional<Core::Person> find_by_id(int id) const override 
    // {
    //     auto dto_opt = this->conn_.template find_by_id<DAL::Schema::PersonDTO>(id);
    //     if (!dto_opt) {
    //         return std::nullopt;
    //     }
    //     return DAL::Mappers::MapperTraits<Core::Person, DAL::Schema::PersonDTO>::to_domain(*dto_opt);
    // }

    // [[nodiscard]] std::optional<Core::Person> get_by_id(int id) const override 
    // {
    //     auto dto_opt = this->conn_.template find_by_id<DAL::Schema::PersonDTO>(id);
    //     if (!dto_opt) {
    //         return std::nullopt;
    //     }
    //     return DAL::Mappers::MapperTraits<Core::Person, DAL::Schema::PersonDTO>::to_domain(*dto_opt);
    // }

    // bool insert_one(const Core::Person& item) override { 
    //     auto dto = DAL::Mappers::MapperTraits<Core::Person, DAL::Schema::PersonDTO>::to_dto(item);
    //     return this->conn_.insert(dto); 
    // }

    // bool insert_many(const std::vector<Core::Person>& items) override 
    // {
    //     if (items.empty()) 
    //     {
    //         return true;
    //     }

    //     // Lazy view pipeline transformation avoids heap allocations
    //     auto dto_view = items | std::views::transform
    //     (
    //         DAL::Mappers::MapperTraits<Core::Person, DAL::Schema::PersonDTO>::to_dto
    //     );

    //     bool success = true;
    //     for (const auto& dto : dto_view) 
    //     {
    //         if (!this->conn_.insert(dto)) 
    //         {
    //             success = false;
    //         }
    //     }
    //     return success;
    // }

    // bool update_one(const Core::Person& item) override { 
    //     auto dto = DAL::Mappers::MapperTraits<Core::Person, DAL::Schema::PersonDTO>::to_dto(item);
    //     return this->conn_.update(dto); 
    // }

    // void delete_by_id(int id) override { 
    //     this->conn_.template delete_by_id<DAL::Schema::PersonDTO>(id); 
    // }

    // [[nodiscard]] bool exists_by_id(int id) const override { 
    //     return this->conn_.template exists<DAL::Schema::PersonDTO>(id); 
    // }

    // [[nodiscard]] std::size_t count() const override { 
    //     return this->conn_.template count<DAL::Schema::PersonDTO>(); 
    // }

    // --- Domain-Specific Query Operations ---

    [[nodiscard]] std::vector<Core::Person> find_by_last_name(const std::string& lastName) const override 
    {
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