export module core:person;

import std;
import :id;

export namespace Core
{
    class Person
    {
    public:
        Person() = default;
        Person(PersonId id, const std::string& firstName, const std::string& lastName)
            : id_(id)
            , firstName_(firstName)
            , lastName_(lastName)
        {
        }

        [[nodiscard]] PersonId getId() const noexcept
        {
            return id_;
        }
        void setId(PersonId id) noexcept
        {
            id_ = id;
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