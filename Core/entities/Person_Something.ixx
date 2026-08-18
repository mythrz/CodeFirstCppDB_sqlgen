export module core:person_something;

import std;
import :id;

export namespace Core
{
    class Person_Something
    {
    public:
        Person_Something() = default;
        Person_Something(PersonId personId, SomethingId somethingId, std::string associationType)
            : personId_(personId)
            , somethingId_(somethingId)
            , associationType_(std::move(associationType))
        {
        }

        [[nodiscard]] PersonId getPersonId() const noexcept
        {
            return personId_;
        }
        void setPersonId(PersonId personId) noexcept
        {
            personId_ = personId;
        }

        [[nodiscard]] SomethingId getSomethingId() const noexcept
        {
            return somethingId_;
        }
        void setSomethingId(SomethingId somethingId) noexcept
        {
            somethingId_ = somethingId;
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