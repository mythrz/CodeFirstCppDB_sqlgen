module;
#include <meta>

export module dal:mappers;

import std;
import core;
import :schema_person;
import :schema_something;
import :schema_person_something;

export namespace DAL::Mappers
{
    namespace detail
    {
        consteval bool chars_equal_ignore_case(char a, char b)
        {
            auto to_lower = [](char c)
            {
                return (c >= 'A' && c <= 'Z') ? (c - 'A' + 'a') : c;
            };
            return to_lower(a) == to_lower(b);
        }

        consteval bool matches_snake_to_camel(std::string_view snake, std::string_view camel)
        {
            std::size_t c_idx = 0;
            for (std::size_t s_idx = 0; s_idx < snake.size(); ++s_idx)
            {
                if (snake[s_idx] == '_')
                {
                    continue;
                }
                if (c_idx >= camel.size())
                    return false;
                if (!chars_equal_ignore_case(snake[s_idx], camel[c_idx]))
                    return false;
                c_idx++;
            }
            if (c_idx < camel.size() && camel[c_idx] == '_' && c_idx + 1 == camel.size())
            {
                return true;
            }
            return false;
        }

        // Given a DTO member, find the unique matching domain member.
        // Strategies are tried in order: exact name, trailing underscore, snake→camel.
        // Evaluating a throw inside a consteval function is ill-formed, so any
        // no-match or ambiguous-match is a compile-time error at the call site.
        consteval std::meta::info find_matching_domain_member(std::meta::info dto_m, std::span<const std::meta::info> domain_members)
        {
            std::string_view dto_name = std::meta::identifier_of(dto_m);

            // exact name match.
            std::meta::info exact_hit{};
            int exact_count = 0;
            for (auto m : domain_members)
            {
                if (std::meta::identifier_of(m) == dto_name)
                {
                    exact_hit = m;
                    ++exact_count;
                }
            }
            if (exact_count == 1)
                return exact_hit;
            if (exact_count > 1)
                throw std::invalid_argument("MapperTraits: ambiguous exact match for DTO field");

            // domain member name is DTO name plus a trailing underscore
            //              (e.g. DTO "id" matches domain "id_").
            std::meta::info underscore_hit{};
            int underscore_count = 0;
            for (auto m : domain_members)
            {
                std::string_view d_name = std::meta::identifier_of(m);
                if (d_name.size() == dto_name.size() + 1 && d_name.starts_with(dto_name) && d_name.ends_with("_"))
                {
                    underscore_hit = m;
                    ++underscore_count;
                }
            }
            if (underscore_count == 1)
                return underscore_hit;
            if (underscore_count > 1)
                throw std::invalid_argument("MapperTraits: ambiguous trailing-underscore match for DTO field");

            // DTO name is snake_case, domain name is camelCase (with optional trailing underscore).
            std::meta::info camel_hit{};
            int camel_count = 0;
            for (auto m : domain_members)
            {
                if (matches_snake_to_camel(dto_name, std::meta::identifier_of(m)))
                {
                    camel_hit = m;
                    ++camel_count;
                }
            }
            if (camel_count == 1)
                return camel_hit;
            if (camel_count > 1)
                throw std::invalid_argument("MapperTraits: ambiguous naming-convention match for DTO field");

            // No strategy produced a match — fail at compile time.
            throw std::invalid_argument("MapperTraits: no matching domain member found for DTO field");
        }

        // Inverse of find_matching_domain_member: given a domain member, locate the
        // matching DTO member.  Uses the same three strategies and the same fail-fast
        // semantics — ambiguous or missing matches are ill-formed at compile time.
        consteval std::meta::info find_matching_dto_member(std::meta::info dom_m, std::span<const std::meta::info> dto_members)
        {
            std::string_view dom_name = std::meta::identifier_of(dom_m);

            // exact name match.
            std::meta::info exact_hit{};
            int exact_count = 0;
            for (auto m : dto_members)
            {
                if (std::meta::identifier_of(m) == dom_name)
                {
                    exact_hit = m;
                    ++exact_count;
                }
            }
            if (exact_count == 1)
                return exact_hit;
            if (exact_count > 1)
                throw std::invalid_argument("MapperTraits: ambiguous exact match for domain member");

            // domain member name is DTO name plus a trailing underscore
            //              (e.g. domain "id_" matches DTO "id").
            std::meta::info underscore_hit{};
            int underscore_count = 0;
            for (auto m : dto_members)
            {
                std::string_view dto_name = std::meta::identifier_of(m);
                if (dom_name.size() == dto_name.size() + 1 && dom_name.starts_with(dto_name) && dom_name.ends_with("_"))
                {
                    underscore_hit = m;
                    ++underscore_count;
                }
            }
            if (underscore_count == 1)
                return underscore_hit;
            if (underscore_count > 1)
                throw std::invalid_argument("MapperTraits: ambiguous trailing-underscore match for domain member");

            // DTO name is snake_case, domain name is camelCase (with optional trailing underscore).
            std::meta::info camel_hit{};
            int camel_count = 0;
            for (auto m : dto_members)
            {
                if (matches_snake_to_camel(std::meta::identifier_of(m), dom_name))
                {
                    camel_hit = m;
                    ++camel_count;
                }
            }
            if (camel_count == 1)
                return camel_hit;
            if (camel_count > 1)
                throw std::invalid_argument("MapperTraits: ambiguous naming-convention match for domain member");

            // No strategy produced a match — fail at compile time.
            throw std::invalid_argument("MapperTraits: no matching DTO member found for domain member");
        }

        // Detects std::optional<T> without touching ::value_type on arbitrary types.
        template<typename>
        inline constexpr bool is_optional_v = false;
        template<typename T>
        inline constexpr bool is_optional_v<std::optional<T>> = true;
    } // namespace detail

    template<typename Domain, typename DTO>
    struct MapperTraits
    {
        // default-construct Domain, then iterate its private members in
        // domain-declaration order and assign each from the matching DTO member.
        // This removes any dependency on constructor parameter order — mapping is
        // driven entirely by member names via find_matching_dto_member.
        [[nodiscard]] static Domain to_domain(const DTO& dto)
        {
            static constexpr auto dto_members =
                std::define_static_array(std::meta::nonstatic_data_members_of(^^DTO, std::meta::access_context::current()));
            static constexpr auto domain_members =
                std::define_static_array(std::meta::nonstatic_data_members_of(^^Domain, std::meta::access_context::unchecked()));

            // Extract the raw value from sqlgen wrapper types (e.g. PrimaryKey, ForeignKey)
            // that expose a .value() accessor, or pass through plain types unchanged.
            // std::optional is detected via is_optional_v (partial specialisation) so
            // that ::value_type is never accessed on non-optional types such as ForeignKey.
            // Return type is auto (value semantics) to avoid decltype(auto) deduction
            // conflicts across branches that return different reference categories.
            auto extract_value = []<typename T>(const T& val) -> auto
            {
                if constexpr (detail::is_optional_v<std::remove_cvref_t<T>>)
                    return val; // keep std::optional<X> as-is
                else if constexpr (requires { val.value(); })
                    return val.value(); // unwrap sqlgen PrimaryKey / ForeignKey
                else
                    return val;
            };

            Domain result{};
            template for (constexpr auto dom_m : domain_members)
            {
                // Locate the DTO member whose name matches this domain member.
                // Ill-formed at compile time if no unique match is found.
                constexpr auto dto_m = detail::find_matching_dto_member(dom_m, dto_members);

                // Write directly into the private domain member via unchecked splice.
                // Cast to the domain member's type so that strong ID types
                // (e.g. PersonId) are constructed from their int32_t DTO counterpart.
                result.[:dom_m:] = static_cast<[:std::meta::type_of(dom_m):]>(extract_value(dto.[:dto_m:]));
            }
            return result;
        }

        [[nodiscard]] static DTO to_dto(const Domain& domain)
        {
            DTO dto{};
            static constexpr auto dto_members =
                std::define_static_array(std::meta::nonstatic_data_members_of(^^DTO, std::meta::access_context::current()));
            static constexpr auto domain_members =
                std::define_static_array(std::meta::nonstatic_data_members_of(^^Domain, std::meta::access_context::unchecked()));

            template for (constexpr auto dto_m : dto_members)
            {
                constexpr auto dom_m = detail::find_matching_domain_member(dto_m, domain_members);
                dto.[:dto_m:] = static_cast<std::remove_cvref_t<decltype(dto.[:dto_m:])>>(domain.[:dom_m:]);
            }
            return dto;
        }
    };

    template<typename Domain, typename DTO>
    concept Mappable = requires(const Domain& domain, const DTO& dto) {
        { MapperTraits<Domain, DTO>::to_domain(dto) } -> std::same_as<Domain>;
        { MapperTraits<Domain, DTO>::to_dto(domain) } -> std::same_as<DTO>;
    };

} // namespace DAL::Mappers
