module;
#include <meta>
#include <rfl/internal/StringLiteral.hpp>

export module codefirst.dal:mappers;

import std;
import codefirst.core;
import :schema_traits;

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

        consteval std::string_view strip_member_affixes(std::string_view name)
        {
            if (name.size() > 2 && (name.starts_with("m_") || name.starts_with("M_")))
            {
                name.remove_prefix(2);
            }
            if (name.ends_with("_") && name.size() > 1)
            {
                name.remove_suffix(1);
            }
            return name;
        }

        consteval bool matches_snake_to_camel(std::string_view snake, std::string_view camel)
        {
            snake = strip_member_affixes(snake);
            camel = strip_member_affixes(camel);
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
            return c_idx == camel.size();
        }

        consteval std::optional<std::string_view> annotated_column_name(std::meta::info m)
        {
            auto col_anns = std::meta::annotations_of_with_type(m, ^^DAL::Schema::column);
            if (col_anns.size() > 1)
            {
                throw std::invalid_argument("MapperTraits: field cannot have multiple [[=DAL::Schema::column]] annotations");
            }
            if (!col_anns.empty())
            {
                std::string_view sv{ std::meta::extract<DAL::Schema::column>(col_anns[0]).name };
                if (sv.empty() || sv.find('"') != std::string_view::npos)
                {
                    throw std::invalid_argument(
                        "MapperTraits: [[=DAL::Schema::column]] name must be non-empty and contain no double quotes");
                }
                return sv;
            }
            return std::nullopt;
        }

        // Given a DTO member, find the unique matching domain member.
        // Strategies are tried in order: [[=column{"..."}]], exact name, stripped affixes (m_/M_/_), snake→camel.
        // Evaluating a throw inside a consteval function is ill-formed, so any
        // no-match or ambiguous-match is a compile-time error at the call site.
        consteval std::meta::info find_matching_domain_member(std::meta::info dto_m, std::span<const std::meta::info> domain_members)
        {
            std::string_view dto_name = std::meta::identifier_of(dto_m);

            // [[=column{"..."}]] annotation match.
            std::meta::info col_hit{};
            int col_count = 0;
            for (auto m : domain_members)
            {
                if (auto ann_col = annotated_column_name(m); ann_col && *ann_col == dto_name)
                {
                    col_hit = m;
                    ++col_count;
                }
            }
            if (col_count == 1)
                return col_hit;
            if (col_count > 1)
                throw std::invalid_argument("MapperTraits: ambiguous [[=column]] match for DTO field");

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

            // stripped member affixes (e.g. DTO "id" matches domain "id_", "m_id", "m_id_").
            std::string_view stripped_dto = strip_member_affixes(dto_name);
            std::meta::info affix_hit{};
            int affix_count = 0;
            for (auto m : domain_members)
            {
                if (strip_member_affixes(std::meta::identifier_of(m)) == stripped_dto)
                {
                    affix_hit = m;
                    ++affix_count;
                }
            }
            if (affix_count == 1)
                return affix_hit;
            if (affix_count > 1)
                throw std::invalid_argument("MapperTraits: ambiguous affix-stripped match for DTO field");

            // DTO name is snake_case, domain name is camelCase (with optional m_/M_ prefix or trailing _).
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
            if (auto ann_col = annotated_column_name(dom_m))
            {
                std::meta::info col_hit{};
                int col_count = 0;
                for (auto m : dto_members)
                {
                    if (std::meta::identifier_of(m) == *ann_col)
                    {
                        col_hit = m;
                        ++col_count;
                    }
                }
                if (col_count == 1)
                    return col_hit;
                if (col_count > 1)
                    throw std::invalid_argument("MapperTraits: ambiguous [[=column]] match for domain member");
                throw std::invalid_argument("MapperTraits: [[=column]] name not found in DTO members");
            }

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

            // stripped member affixes (e.g. domain "id_", "m_id", "m_id_" matches DTO "id").
            std::string_view stripped_dom = strip_member_affixes(dom_name);
            std::meta::info affix_hit{};
            int affix_count = 0;
            for (auto m : dto_members)
            {
                if (strip_member_affixes(std::meta::identifier_of(m)) == stripped_dom)
                {
                    affix_hit = m;
                    ++affix_count;
                }
            }
            if (affix_count == 1)
                return affix_hit;
            if (affix_count > 1)
                throw std::invalid_argument("MapperTraits: ambiguous affix-stripped match for domain member");

            // DTO name is snake_case, domain name is camelCase (with optional m_/M_ prefix or trailing _).
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

        template<typename ParamT, typename RawDtoFieldT>
        consteval bool is_hydratable_from()
        {
            using CleanDtoT = std::remove_cvref_t<RawDtoFieldT>;
            if constexpr (is_optional_v<CleanDtoT>)
            {
                return std::is_constructible_v<ParamT, CleanDtoT>;
            }
            else if constexpr (requires(const CleanDtoT& v) { v.value(); })
            {
                using UnwrappedT = std::remove_cvref_t<decltype(std::declval<const CleanDtoT&>().value())>;
                return std::is_constructible_v<ParamT, UnwrappedT>;
            }
            else
            {
                return std::is_constructible_v<ParamT, CleanDtoT>;
            }
        }

        template<typename ParamT, typename RawDtoFieldT>
        inline constexpr bool is_hydratable_from_v = is_hydratable_from<ParamT, RawDtoFieldT>();

        template<typename Domain>
        consteval std::optional<std::size_t> try_find_matching_dto_member_for_param(
            std::meta::info param, std::span<const std::meta::info> dto_members)
        {
            if (!std::meta::has_identifier(param))
            {
                return std::nullopt;
            }

            auto find_by_exact_name = [&](std::string_view name) -> std::optional<std::size_t>
            {
                std::optional<std::size_t> hit;
                int count = 0;
                for (std::size_t i = 0; i < dto_members.size(); ++i)
                {
                    if (std::meta::identifier_of(dto_members[i]) == name)
                    {
                        hit = i;
                        ++count;
                    }
                }
                return count == 1 ? hit : std::nullopt;
            };

            if (auto ann_col = annotated_column_name(param))
            {
                return find_by_exact_name(*ann_col);
            }

            std::string_view pname = std::meta::identifier_of(param);

            // Check whether the constructor parameter corresponds uniquely to a domain
            // member carrying [[=DAL::Schema::column{"..."}]], using tiered matching
            // (exact -> affix-stripped -> snake/camel).
            static constexpr auto& domain_members = DAL::Schema::unchecked_members_v<Domain>;
            auto find_domain_member = [&](auto&& predicate) -> std::pair<std::meta::info, int>
            {
                std::meta::info hit{};
                int count = 0;
                for (auto dm : domain_members)
                {
                    if (predicate(std::meta::identifier_of(dm)))
                    {
                        hit = dm;
                        ++count;
                    }
                }
                return { hit, count };
            };

            std::meta::info matched_dom{};
            if (auto [hit, count] = find_domain_member(
                    [&](std::string_view dm_name)
                    {
                        return dm_name == pname;
                    });
                count == 1)
            {
                matched_dom = hit;
            }
            else if (count == 0)
            {
                const std::string_view stripped_param = strip_member_affixes(pname);
                if (auto [ahit, acount] = find_domain_member(
                        [&](std::string_view dm_name)
                        {
                            return strip_member_affixes(dm_name) == stripped_param;
                        });
                    acount == 1)
                {
                    matched_dom = ahit;
                }
                else if (acount == 0)
                {
                    if (auto [chit, ccount] = find_domain_member(
                            [&](std::string_view dm_name)
                            {
                                return matches_snake_to_camel(pname, dm_name) || matches_snake_to_camel(dm_name, pname);
                            });
                        ccount == 1)
                    {
                        matched_dom = chit;
                    }
                }
            }

            if (matched_dom != std::meta::info{})
            {
                if (auto ann_col = annotated_column_name(matched_dom))
                {
                    return find_by_exact_name(*ann_col);
                }
            }

            // Tiered name matching directly against DTO members (exact -> affix-stripped -> snake_to_camel).
            if (auto exact = find_by_exact_name(pname))
            {
                return exact;
            }

            const std::string_view stripped_param = strip_member_affixes(pname);
            std::optional<std::size_t> affix_hit;
            int affix_count = 0;
            for (std::size_t i = 0; i < dto_members.size(); ++i)
            {
                if (strip_member_affixes(std::meta::identifier_of(dto_members[i])) == stripped_param)
                {
                    affix_hit = i;
                    ++affix_count;
                }
            }
            if (affix_count == 1)
            {
                return affix_hit;
            }
            if (affix_count > 1)
            {
                return std::nullopt;
            }

            std::optional<std::size_t> camel_hit;
            int camel_count = 0;
            for (std::size_t i = 0; i < dto_members.size(); ++i)
            {
                if (matches_snake_to_camel(std::meta::identifier_of(dto_members[i]), pname))
                {
                    camel_hit = i;
                    ++camel_count;
                }
            }
            if (camel_count == 1)
            {
                return camel_hit;
            }
            return std::nullopt;
        }

        template<typename Domain>
        consteval std::meta::info find_matching_dto_member_for_param(std::meta::info param, std::span<const std::meta::info> dto_members)
        {
            if (auto idx = try_find_matching_dto_member_for_param<Domain>(param, dto_members))
            {
                return dto_members[*idx];
            }
            throw std::invalid_argument("MapperTraits: no matching DTO member found for constructor parameter");
        }

        template<typename Domain>
        consteval std::meta::info find_hydrating_constructor(std::span<const std::meta::info> dto_members)
        {
            if constexpr (std::is_aggregate_v<Domain>)
            {
                return std::meta::info{};
            }
            for (auto mem : std::meta::members_of(^^Domain, std::meta::access_context::current()))
            {
                if (std::meta::is_constructor(mem) && !std::meta::is_copy_constructor(mem) && !std::meta::is_move_constructor(mem) &&
                    !std::meta::is_default_constructor(mem))
                {
                    auto params = std::meta::parameters_of(mem);
                    if (params.size() == dto_members.size() && !params.empty())
                    {
                        std::vector<bool> used_dto(dto_members.size(), false);
                        bool all_match = true;
                        for (auto p : params)
                        {
                            auto idx_opt = try_find_matching_dto_member_for_param<Domain>(p, dto_members);
                            if (!idx_opt || used_dto[*idx_opt])
                            {
                                all_match = false;
                                break;
                            }
                            used_dto[*idx_opt] = true;

                            const std::meta::info matched_dm = dto_members[*idx_opt];
                            auto param_type = std::meta::remove_cvref(std::meta::type_of(p));
                            auto dto_field_type = std::meta::remove_cvref(std::meta::type_of(matched_dm));
                            auto check_var = std::meta::substitute(
                                ^^is_hydratable_from_v,
                                {
                                    param_type, dto_field_type });
                            if (!std::meta::extract<bool>(check_var))
                            {
                                all_match = false;
                                break;
                            }
                        }
                        if (all_match)
                        {
                            return mem;
                        }
                    }
                }
            }
            return std::meta::info{};
        }
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
            if constexpr (std::is_same_v<Domain, DTO>)
            {
                return dto;
            }
            else
            {
                static constexpr auto& dto_members = DAL::Schema::members_v<DTO>;
                static constexpr auto& domain_members = DAL::Schema::unchecked_members_v<Domain>;

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

                constexpr auto ctor = detail::find_hydrating_constructor<Domain>(dto_members);
                if constexpr (ctor != std::meta::info{})
                {
                    static constexpr auto params = std::define_static_array(std::meta::parameters_of(ctor));
                    return [&]<std::size_t... Is>(std::index_sequence<Is...>)
                    {
                        return Domain(
                            static_cast<[:std::meta::remove_cvref(std::meta::type_of(params[Is])):]>(
                                extract_value(dto.[:detail::find_matching_dto_member_for_param<Domain>(params[Is], dto_members):]))...);
                    }(std::make_index_sequence<params.size()>{});
                }
                else
                {
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
            }
        }

        [[nodiscard]] static DTO to_dto(const Domain& domain)
        {
            if constexpr (std::is_same_v<Domain, DTO>)
            {
                return domain;
            }
            else
            {
                DTO dto{};
                static constexpr auto& dto_members = DAL::Schema::members_v<DTO>;
                static constexpr auto& domain_members = DAL::Schema::unchecked_members_v<Domain>;

                template for (constexpr auto dto_m : dto_members)
                {
                    constexpr auto dom_m = detail::find_matching_domain_member(dto_m, domain_members);
                    using TargetT = std::remove_cvref_t<decltype(dto.[:dto_m:])>;
                    const auto& src = domain.[:dom_m:];
                    using SrcT = std::remove_cvref_t<decltype(src)>;
                    if constexpr (Core::StrongIdType<SrcT> || Core::SingleKeyType<SrcT>)
                    {
                        dto.[:dto_m:] = static_cast<TargetT>(src.get());
                    }
                    else
                    {
                        dto.[:dto_m:] = static_cast<TargetT>(src);
                    }
                }
                return dto;
            }
        }

        template<typename PkVal>
        [[nodiscard]] static std::expected<Domain, Core::DbError> try_with_primary_key(const Domain& domain, const PkVal& new_pk_val)
            requires DAL::Schema::SinglePrimaryKey<DTO>
        {
            DTO dto = to_dto(domain);
            constexpr auto pk_m = DAL::Schema::primary_key_member<DTO>();
            using PkFieldT = [:std::meta::type_of(pk_m):];
            using CleanPkT = std::remove_cvref_t<PkFieldT>;

            auto check_integral_range = [&]<typename TargetScalarT>() -> std::expected<void, Core::DbError>
            {
                using CleanTargetT = std::remove_cvref_t<TargetScalarT>;
                using CleanSrcT = std::remove_cvref_t<PkVal>;
                constexpr bool is_target_int = std::is_integral_v<CleanTargetT> && !std::is_same_v<CleanTargetT, bool> &&
                    !std::is_same_v<CleanTargetT, char> && !std::is_same_v<CleanTargetT, wchar_t> &&
                    !std::is_same_v<CleanTargetT, char8_t> && !std::is_same_v<CleanTargetT, char16_t> &&
                    !std::is_same_v<CleanTargetT, char32_t>;
                constexpr bool is_src_int = std::is_integral_v<CleanSrcT> && !std::is_same_v<CleanSrcT, bool> &&
                    !std::is_same_v<CleanSrcT, char> && !std::is_same_v<CleanSrcT, wchar_t> && !std::is_same_v<CleanSrcT, char8_t> &&
                    !std::is_same_v<CleanSrcT, char16_t> && !std::is_same_v<CleanSrcT, char32_t>;
                if constexpr (is_target_int && is_src_int)
                {
                    if (!std::in_range<CleanTargetT>(new_pk_val))
                    {
                        return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation(
                            std::format("Generated primary key value {} overflows target key type", new_pk_val)) };
                    }
                }
                return {};
            };

            if constexpr (DAL::Schema::SqlgenPrimaryKeyField<CleanPkT>)
            {
                using InnerT = typename CleanPkT::ReflectionType;
                if constexpr (Core::StrongIdType<InnerT>)
                {
                    if (auto chk = check_integral_range.template operator()<typename InnerT::value_type>(); !chk)
                    {
                        return std::unexpected<Core::DbError>{ chk.error() };
                    }
                    dto.[:pk_m:] = CleanPkT{ InnerT{ static_cast<typename InnerT::value_type>(new_pk_val) } };
                }
                else
                {
                    if (auto chk = check_integral_range.template operator()<InnerT>(); !chk)
                    {
                        return std::unexpected<Core::DbError>{ chk.error() };
                    }
                    dto.[:pk_m:] = CleanPkT{ static_cast<InnerT>(new_pk_val) };
                }
            }
            else if constexpr (Core::StrongIdType<CleanPkT>)
            {
                if (auto chk = check_integral_range.template operator()<typename CleanPkT::value_type>(); !chk)
                {
                    return std::unexpected<Core::DbError>{ chk.error() };
                }
                dto.[:pk_m:] = CleanPkT{ static_cast<typename CleanPkT::value_type>(new_pk_val) };
            }
            else
            {
                if (auto chk = check_integral_range.template operator()<CleanPkT>(); !chk)
                {
                    return std::unexpected<Core::DbError>{ chk.error() };
                }
                dto.[:pk_m:] = static_cast<CleanPkT>(new_pk_val);
            }
            return to_domain(dto);
        }

        template<typename PkVal>
        [[nodiscard]] static Domain with_primary_key(const Domain& domain, const PkVal& new_pk_val)
            requires DAL::Schema::SinglePrimaryKey<DTO>
        {
            auto res = try_with_primary_key(domain, new_pk_val);
            if (!res)
            {
                throw std::overflow_error(res.error().message);
            }
            return std::move(*res);
        }
    };

    template<typename Domain, typename DTO>
    concept Mappable = requires(const Domain& domain, const DTO& dto) {
        { MapperTraits<Domain, DTO>::to_domain(dto) } -> std::same_as<Domain>;
        { MapperTraits<Domain, DTO>::to_dto(domain) } -> std::same_as<DTO>;
    };

    // =========================================================================
    // Phase 16: Compile-Time Domain-to-DTO Column Resolution & Validation
    // =========================================================================

    template<typename Domain, typename DTO, rfl::internal::StringLiteral ColOrField>
    consteval std::optional<std::meta::info> try_resolve_dto_member()
    {
        constexpr std::string_view name = ColOrField.string_view();
        static constexpr auto& dto_members = DAL::Schema::members_v<DTO>;

        // Tier 1: exact DTO column name match.
        {
            std::optional<std::meta::info> hit;
            int count = 0;
            for (auto m : dto_members)
            {
                if (std::meta::identifier_of(m) == name)
                {
                    hit = m;
                    ++count;
                }
            }
            if (count == 1)
            {
                return hit;
            }
            if (count > 1)
            {
                return std::nullopt;
            }
        }

        // Tier 2: snake_case DTO column matched by camelCase/affixed query name.
        {
            std::optional<std::meta::info> hit;
            int count = 0;
            for (auto m : dto_members)
            {
                if (detail::matches_snake_to_camel(std::meta::identifier_of(m), name))
                {
                    hit = m;
                    ++count;
                }
            }
            if (count == 1)
            {
                return hit;
            }
            if (count > 1)
            {
                return std::nullopt;
            }
        }

        // Tier 3: Domain field name (exact or affix-stripped) resolved through MapperTraits (supports [[=column]]).
        if constexpr (!std::is_same_v<Domain, DTO>)
        {
            static constexpr auto& dom_members = DAL::Schema::unchecked_members_v<Domain>;
            std::optional<std::meta::info> exact_dom;
            int exact_count = 0;
            for (auto dm : dom_members)
            {
                if (std::meta::identifier_of(dm) == name)
                {
                    exact_dom = dm;
                    ++exact_count;
                }
            }
            if (exact_count == 1)
            {
                return detail::find_matching_dto_member(*exact_dom, dto_members);
            }
            if (exact_count > 1)
            {
                return std::nullopt;
            }

            std::optional<std::meta::info> affix_dom;
            int affix_count = 0;
            const std::string_view stripped = detail::strip_member_affixes(name);
            for (auto dm : dom_members)
            {
                if (detail::strip_member_affixes(std::meta::identifier_of(dm)) == stripped)
                {
                    affix_dom = dm;
                    ++affix_count;
                }
            }
            if (affix_count == 1)
            {
                return detail::find_matching_dto_member(*affix_dom, dto_members);
            }
            if (affix_count > 1)
            {
                return std::nullopt;
            }
        }
        return std::nullopt;
    }

    template<typename Domain, typename DTO, rfl::internal::StringLiteral ColOrField>
    consteval bool is_valid_query_column()
    {
        return try_resolve_dto_member<Domain, DTO, ColOrField>().has_value();
    }

    template<typename Domain, typename DTO, rfl::internal::StringLiteral ColOrField>
    concept ValidQueryColumn = is_valid_query_column<Domain, DTO, ColOrField>();

    template<typename Domain, typename DTO, rfl::internal::StringLiteral ColOrField>
        requires ValidQueryColumn<Domain, DTO, ColOrField>
    consteval std::meta::info resolve_dto_member()
    {
        if (auto resolved = try_resolve_dto_member<Domain, DTO, ColOrField>())
        {
            return *resolved;
        }
        throw std::invalid_argument("Invalid or ambiguous query column or domain field name");
    }

} // namespace DAL::Mappers
