export module core:person_something;

import std;

export namespace Core 
{

    class Person_Something 
    {
    public:
        Person_Something() = default;
        Person_Something(std::uint32_t personId, std::uint32_t somethingId, std::string associationType)
            : personId_(personId)
            , somethingId_(somethingId)
            , associationType_(std::move(associationType)) {}

        [[nodiscard]] std::uint32_t getPersonId() const noexcept { return personId_; }
        void setPersonId(std::uint32_t personId) noexcept { personId_ = personId; }

        [[nodiscard]] std::uint32_t getSomethingId() const noexcept { return somethingId_; }
        void setSomethingId(std::uint32_t somethingId) noexcept { somethingId_ = somethingId; }

        [[nodiscard]] const std::string& getAssociationType() const noexcept { return associationType_; }
        void setAssociationType(std::string associationType) noexcept { associationType_ = std::move(associationType); }

    private:
        std::uint32_t personId_{0};
        std::uint32_t somethingId_{0};
        std::string associationType_;
    };

} // namespace Core