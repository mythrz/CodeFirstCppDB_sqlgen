export module core:id;

import std;

export namespace Core
{
    // Generic zero-overhead strongly-typed ID wrapper.
    // Usage:
    //   using PersonId = Id<struct PersonTag>;
    //   using DocId = Id<struct DocTag, std::string>;
    template<typename Tag, typename ValueType = std::int32_t>
    struct Id
    {
        using value_type = ValueType;
        ValueType value{};

        constexpr Id() = default;
        constexpr explicit Id(ValueType v) noexcept(std::is_nothrow_move_constructible_v<ValueType>)
            : value(std::move(v))
        {
        }

        [[nodiscard]] constexpr const ValueType& get() const noexcept
        {
            return value;
        }

        // Implicit conversion to underlying type allows static_cast in reflection
        // mappers and interop with sqlgen/SQLite.
        constexpr operator const ValueType&() const noexcept
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

// std::formatter specialisation — allows Id<Tag, ValueType> to be used directly
// in std::format / std::println as if it were ValueType.
template<typename Tag, typename ValueType>
struct std::formatter<Core::Id<Tag, ValueType>> : std::formatter<ValueType>
{
    template<typename FormatContext>
    auto format(const Core::Id<Tag, ValueType>& id, FormatContext& ctx) const
    {
        return std::formatter<ValueType>::format(id.value, ctx);
    }
};
