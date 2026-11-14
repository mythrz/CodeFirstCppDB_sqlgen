export module core:person;

import std;
import :id;

export namespace Core
{
    class Person
    {
    public:
        Person() = default;
        Person(PersonId id, std::string firstName, std::string lastName)
            : id_(id)
            , firstName_(std::move(firstName))
            , lastName_(std::move(lastName))
        {
        }

        // Immutable identity: Primary key can only be initialized, not mutated arbitrarily.
        [[nodiscard]] PersonId getId() const noexcept
        {
            return id_;
        }

        [[nodiscard]] const std::string& getFirstName() const noexcept
        {
            return firstName_;
        }
        void setFirstName(std::string firstName) noexcept
        {
            firstName_ = std::move(firstName);
        }

        [[nodiscard]] const std::string& getLastName() const noexcept
        {
            return lastName_;
        }
        void setLastName(std::string lastName) noexcept
        {
            lastName_ = std::move(lastName);
        }

    private:
        PersonId id_{};
        std::string firstName_;
        std::string lastName_;
    };
} // namespace Core
