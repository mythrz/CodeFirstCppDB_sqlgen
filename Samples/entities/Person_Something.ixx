export module codefirst.samples:person_something;

import std;
import codefirst.core;
import :ids;

export namespace Core
{
    class Person_Something
    {
    public:
        using Key = CompositeKey<PersonId, SomethingId>;

        Person_Something() = default;
        Person_Something(PersonId personId, SomethingId somethingId, std::string associationType)
            : personId_(personId)
            , somethingId_(somethingId)
            , associationType_(std::move(associationType))
        {
        }

        // Composite key components (immutable identity)
        [[nodiscard]] PersonId getPersonId() const noexcept
        {
            return personId_;
        }

        [[nodiscard]] SomethingId getSomethingId() const noexcept
        {
            return somethingId_;
        }

        [[nodiscard]] Key getKey() const noexcept
        {
            return Key{ personId_, somethingId_ };
        }

        [[nodiscard]] const std::string& getAssociationType() const noexcept
        {
            return associationType_;
        }
        void setAssociationType(std::string associationType) noexcept
        {
            associationType_ = std::move(associationType);
        }

    private:
        PersonId personId_{};
        SomethingId somethingId_{};
        std::string associationType_;
    };

} // namespace Core
