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

        consteval std::meta::info find_matching_domain_member(std::meta::info dto_m, std::span<const std::meta::info> domain_members)
        {
            std::string_view dto_name = std::meta::identifier_of(dto_m);
            for (auto m : domain_members)
            {
                std::string_view d_name = std::meta::identifier_of(m);
                if (d_name == dto_name)
                    return m;
                if (d_name.size() == dto_name.size() + 1 && d_name.starts_with(dto_name) && d_name.ends_with("_"))
                    return m;
                if (matches_snake_to_camel(dto_name, d_name))
                    return m;
            }
            return domain_members[0];
        }
    } // namespace detail

    template<typename Domain, typename DTO>
    struct MapperTraits
    {
        [[nodiscard]] static Domain to_domain(const DTO& dto)
        {
            static constexpr auto members =
                std::define_static_array(std::meta::nonstatic_data_members_of(^^DTO, std::meta::access_context::current()));
            return [&]<std::size_t... Is>(std::index_sequence<Is...>)
            {
                return Domain(static_cast<std::remove_cvref_t<decltype(dto.[:members[Is]:])>>(dto.[:members[Is]:])...);
            }(std::make_index_sequence<members.size()>{});
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
