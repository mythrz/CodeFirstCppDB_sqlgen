export module core:id;

import std;

export namespace Core
{
    // Generic zero-overhead strongly-typed ID wrapper.
    // Usage:
    //   using PersonId = Id<struct PersonTag>;
    template<typename Tag>
    struct Id
    {
        std::int32_t value{ 0 };

        constexpr explicit Id(std::int32_t v = 0) noexcept
            : value(v)
        {
        }

        [[nodiscard]] constexpr std::int32_t get() const noexcept
        {
            return value;
        }

        // Implicit conversion to underlying type allows static_cast in reflection
        // mappers and interop with sqlgen/SQLite (which use signed integers).
        constexpr operator std::int32_t() const noexcept
        {
            return value;
        }

        constexpr bool operator==(const Id&) const noexcept = default;
        constexpr auto operator<=>(const Id&) const noexcept = default;
    };

    // Domain-specific ID aliases — all entity IDs live here so
    // any partition can import :id and use them.
    using PersonId = Id<struct PersonTag>;
    using SomethingId = Id<struct SomethingTag>;

} // namespace Core

// std::formatter specialisation — allows Id<Tag> to be used directly
// in std::format / std::println as if it were a plain std::int32_t.
template<typename Tag>
struct std::formatter<Core::Id<Tag>> : std::formatter<std::int32_t>
{
    template<typename FormatContext>
    auto format(const Core::Id<Tag>& id, FormatContext& ctx) const
    {
        return std::formatter<std::int32_t>::format(id.value, ctx);
    }
};
