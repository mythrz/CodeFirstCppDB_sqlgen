module;
#include <meta>

export module codefirst.dal:entity_metadata;

import std;
import codefirst.core;
import :schema_traits;
import :mappers;

export namespace DAL::Schema
{
    template<typename Domain, typename DTO = SchemaOf_t<Domain>>
        requires RelationalEntity<DTO> && Core::Key<EntityKey_t<Domain, DTO>> && DAL::Mappers::Mappable<Domain, DTO>
    struct EntityMetadata
    {
        using DomainType = Domain;
        using DTOType = DTO;
        using KeyType = EntityKey_t<Domain, DTO>;

        static constexpr std::string_view table_name = DTO::tablename;
        static constexpr bool is_composite_key = Core::CompositeKeyType<KeyType>;
        static constexpr std::size_t key_arity = KeyType::arity;

        static consteval auto column_names()
        {
            std::array<std::string_view, members_v<DTO>.size()> names{};
            for (std::size_t i = 0; i < members_v<DTO>.size(); ++i)
            {
                names[i] = std::meta::identifier_of(members_v<DTO>[i]);
            }
            return names;
        }

        static consteval auto key_members()
        {
            static constexpr auto km = key_members_of<DTO>();
            if constexpr (km.size() != key_arity)
            {
                throw std::invalid_argument("EntityMetadata: DTO key field count does not match Domain::Key::arity");
            }
            return km;
        }

        static consteval auto key_column_names()
        {
            static constexpr auto km = key_members();
            std::array<std::string_view, key_arity> names{};
            for (std::size_t i = 0; i < key_arity; ++i)
            {
                names[i] = std::meta::identifier_of(km[i]);
            }
            return names;
        }

        [[nodiscard]] static constexpr KeyType extract_dto_key(const DTO& dto)
        {
            static constexpr auto km = key_members();
            if constexpr (is_composite_key)
            {
                return [&]<std::size_t... Is>(std::index_sequence<Is...>)
                {
                    return KeyType{ static_cast<std::tuple_element_t<Is, typename KeyType::tuple_type>>(dto.[:km[Is]:].value())... };
                }(std::make_index_sequence<key_arity>{});
            }
            else
            {
                return KeyType{ static_cast<typename KeyType::key_type>(dto.[:km[0]:].value()) };
            }
        }

        [[nodiscard]] static constexpr KeyType extract_key(const Domain& domain)
        {
            if constexpr (requires {
                              { domain.getKey() } -> std::convertible_to<KeyType>;
                          })
            {
                return domain.getKey();
            }
            else
            {
                return extract_dto_key(DAL::Mappers::MapperTraits<Domain, DTO>::to_dto(domain));
            }
        }

        [[nodiscard]] static constexpr bool matches_key(const DTO& dto, const KeyType& key)
        {
            return extract_dto_key(dto) == key;
        }
    };

} // namespace DAL::Schema
