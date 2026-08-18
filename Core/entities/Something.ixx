export module core:something;

import std;
import :id;

export namespace Core
{
    class Something
    {
    public:
        Something() = default;
        Something(SomethingId id, std::string name, std::string category, std::optional<std::string> description = std::nullopt)
            : id_(id)
            , name_(std::move(name))
            , category_(std::move(category))
            , description_(std::move(description))
        {
        }

        [[nodiscard]] SomethingId getId() const noexcept
        {
            return id_;
        }
        void setId(SomethingId id) noexcept
        {
            id_ = id;
        }

        [[nodiscard]] const std::string& getName() const noexcept
        {
            return name_;
        }
        void setName(std::string name) noexcept
        {
            name_ = std::move(name);
        }

        [[nodiscard]] const std::string& getCategory() const noexcept
        {
            return category_;
        }
        void setCategory(std::string category) noexcept
        {
            category_ = std::move(category);
        }

        [[nodiscard]] const std::optional<std::string>& getDescription() const noexcept
        {
            return description_;
        }
        void setDescription(std::optional<std::string> description) noexcept
        {
            description_ = std::move(description);
        }

    private:
        SomethingId id_{};
        std::string name_;
        std::string category_;
        std::optional<std::string> description_;
    };

} // namespace Core