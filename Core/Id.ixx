export module codefirst.core:id;

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

        // Explicit conversion to underlying type prevents accidental cross-entity
        // comparisons/arithmetic while allowing static_cast or .get() in mappers.
        constexpr explicit operator const ValueType&() const noexcept
        {
            return value;
        }

        constexpr bool operator==(const Id&) const noexcept = default;
        constexpr auto operator<=>(const Id&) const noexcept = default;
    };

    template<typename T>
    struct is_strong_id : std::false_type
    {
    };

    template<typename Tag, typename ValueType>
    struct is_strong_id<Id<Tag, ValueType>> : std::true_type
    {
    };

    template<typename T>
    inline constexpr bool is_strong_id_v = is_strong_id<std::remove_cvref_t<T>>::value;

    template<typename T>
    concept StrongIdType = is_strong_id_v<T>;

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
