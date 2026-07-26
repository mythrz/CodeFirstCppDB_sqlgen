export module core:person;

import std;

export namespace Core {

    class Person {
    public:
        Person() = default;
        Person(std::uint32_t id, const std::string& firstName, const std::string& lastName)
            : id_(id), firstName_(firstName), lastName_(lastName) {}
        
        [[nodiscard]] std::uint32_t getId() const noexcept { return id_; }
        void setId(std::uint32_t id) noexcept { id_ = id; }
        
        [[nodiscard]] const std::string& getFirstName() const noexcept { return firstName_; }
        void setFirstName(std::string firstName) noexcept { firstName_ = std::move(firstName); }
        
        [[nodiscard]] const std::string& getLastName() const noexcept { return lastName_; }
        void setLastName(std::string lastName) noexcept { lastName_ = std::move(lastName); }

    private:
        std::uint32_t id_ = 0;
        std::string firstName_;
        std::string lastName_;
    };

} // namespace Core