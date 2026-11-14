export module core:key;

import std;
import :id;

export namespace Core
{
    // =========================================================================
    // SingleKey<KeyType>
    // Wraps a single primary key (e.g. PersonId, std::int32_t, std::string).
    // Provides compile-time metadata indicating arity = 1 and is_composite = false.
    // =========================================================================
    template<typename KeyType>
    struct SingleKey
    {
        using key_type = KeyType;
        static constexpr bool is_composite = false;
        static constexpr std::size_t arity = 1;

        KeyType value{};

        constexpr SingleKey() = default;
        constexpr explicit SingleKey(KeyType v) noexcept(std::is_nothrow_move_constructible_v<KeyType>)
            : value(std::move(v))
        {
        }

        [[nodiscard]] constexpr const KeyType& get() const noexcept
        {
            return value;
        }

        [[nodiscard]] constexpr KeyType& get() noexcept
        {
            return value;
        }

        // Implicit conversion to underlying KeyType for ergonomic interoperability
        constexpr operator const KeyType&() const noexcept
        {
            return value;
        }

        constexpr operator KeyType&() noexcept
        {
            return value;
        }

        constexpr bool operator==(const SingleKey&) const noexcept = default;
        constexpr auto operator<=>(const SingleKey&) const noexcept = default;
    };

    // Deduction guide for SingleKey
    template<typename KeyType>
    SingleKey(KeyType) -> SingleKey<KeyType>;

    // =========================================================================
    // CompositeKey<KeyTypes...>
    // Models multi-column primary keys (e.g. junction tables like Person_Something).
    // Provides compile-time metadata indicating arity >= 2 and is_composite = true.
    // =========================================================================
    template<typename... KeyTypes>
        requires (sizeof...(KeyTypes) >= 2)
    struct CompositeKey
    {
        static constexpr bool is_composite = true;
        static constexpr std::size_t arity = sizeof...(KeyTypes);
        using tuple_type = std::tuple<KeyTypes...>;

        std::tuple<KeyTypes...> values{};

        constexpr CompositeKey() = default;

        constexpr explicit CompositeKey(KeyTypes... args) noexcept(
            (std::is_nothrow_move_constructible_v<KeyTypes> && ...))
            : values(std::move(args)...)
        {
        }

        constexpr explicit CompositeKey(std::tuple<KeyTypes...> t) noexcept(
            std::is_nothrow_move_constructible_v<std::tuple<KeyTypes...>>)
            : values(std::move(t))
        {
        }

        // Indexed element access: key.get<0>()
        template<std::size_t Index>
            requires (Index < sizeof...(KeyTypes))
        [[nodiscard]] constexpr decltype(auto) get() const noexcept
        {
            return std::get<Index>(values);
        }

        template<std::size_t Index>
            requires (Index < sizeof...(KeyTypes))
        [[nodiscard]] constexpr decltype(auto) get() noexcept
        {
            return std::get<Index>(values);
        }

        // Type-based element access: key.get<PersonId>()
        template<typename T>
        [[nodiscard]] constexpr decltype(auto) get() const noexcept
        {
            return std::get<T>(values);
        }

        template<typename T>
        [[nodiscard]] constexpr decltype(auto) get() noexcept
        {
            return std::get<T>(values);
        }

        [[nodiscard]] constexpr const std::tuple<KeyTypes...>& tuple() const noexcept
        {
            return values;
        }

        [[nodiscard]] constexpr std::tuple<KeyTypes...>& tuple() noexcept
        {
            return values;
        }

        constexpr bool operator==(const CompositeKey&) const noexcept = default;
        constexpr auto operator<=>(const CompositeKey&) const noexcept = default;
    };

    // Deduction guide for CompositeKey
    template<typename... KeyTypes>
    CompositeKey(KeyTypes...) -> CompositeKey<KeyTypes...>;

    // =========================================================================
    // Type Traits & Concepts
    // =========================================================================
    template<typename T>
    struct is_single_key : std::false_type {};

    template<typename KeyType>
    struct is_single_key<SingleKey<KeyType>> : std::true_type {};

    template<typename T>
    inline constexpr bool is_single_key_v = is_single_key<T>::value;

    template<typename T>
    struct is_composite_key : std::false_type {};

    template<typename... KeyTypes>
    struct is_composite_key<CompositeKey<KeyTypes...>> : std::true_type {};

    template<typename T>
    inline constexpr bool is_composite_key_v = is_composite_key<T>::value;

    template<typename T>
    concept SingleKeyType = is_single_key_v<T>;

    template<typename T>
    concept CompositeKeyType = is_composite_key_v<T>;

    template<typename T>
    concept Key = SingleKeyType<T> || CompositeKeyType<T>;

    // =========================================================================
    // Helper Factories
    // =========================================================================
    template<typename KeyType>
    [[nodiscard]] constexpr auto make_single_key(KeyType&& val)
    {
        return SingleKey<std::decay_t<KeyType>>(std::forward<KeyType>(val));
    }

    template<typename... KeyTypes>
    [[nodiscard]] constexpr auto make_composite_key(KeyTypes&&... vals)
    {
        return CompositeKey<std::decay_t<KeyTypes>...>(std::forward<KeyTypes>(vals)...);
    }

    // ADL get overloads for structured binding support on CompositeKey
    template<std::size_t Index, typename... KeyTypes>
    [[nodiscard]] constexpr decltype(auto) get(const CompositeKey<KeyTypes...>& key) noexcept
    {
        return key.template get<Index>();
    }

    template<std::size_t Index, typename... KeyTypes>
    [[nodiscard]] constexpr decltype(auto) get(CompositeKey<KeyTypes...>& key) noexcept
    {
        return key.template get<Index>();
    }

} // namespace Core

// =========================================================================
// Structured Binding Support for Core::CompositeKey
// =========================================================================
template<typename... KeyTypes>
struct std::tuple_size<Core::CompositeKey<KeyTypes...>>
    : std::integral_constant<std::size_t, sizeof...(KeyTypes)> {};

template<std::size_t Index, typename... KeyTypes>
struct std::tuple_element<Index, Core::CompositeKey<KeyTypes...>>
{
    using type = std::tuple_element_t<Index, std::tuple<KeyTypes...>>;
};

// =========================================================================
// std::formatter Specializations
// =========================================================================
template<typename KeyType>
struct std::formatter<Core::SingleKey<KeyType>> : std::formatter<std::string>
{
    template<typename FormatContext>
    auto format(const Core::SingleKey<KeyType>& key, FormatContext& ctx) const
    {
        return std::formatter<std::string>::format(std::format("SingleKey({})", key.value), ctx);
    }
};

template<typename... KeyTypes>
struct std::formatter<Core::CompositeKey<KeyTypes...>> : std::formatter<std::string>
{
    template<typename FormatContext>
    auto format(const Core::CompositeKey<KeyTypes...>& key, FormatContext& ctx) const
    {
        std::string str = "CompositeKey(";
        std::apply([&str](const auto&... args) {
            std::size_t idx = 0;
            ((str += (idx++ > 0 ? ", " : "") + std::format("{}", args)), ...);
        }, key.values);
        str += ")";
        return std::formatter<std::string>::format(str, ctx);
    }
};
