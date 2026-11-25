export module dal:generic_repo;

import std;
import core;
import :mappers;
import :schema_traits;
import :entity_metadata;

export namespace DAL::Repositories
{
    // Minimal connection concept verifying basic liveness
    template<typename T>
    concept DatabaseConnection = requires(T& conn) {
        { conn.is_open() } -> std::same_as<bool>;
    };

    // Full contract concept for reading DTOs from connection
    template<typename Conn, typename DTO>
    concept ReadableConnection = DatabaseConnection<Conn> && requires(Conn& conn, std::int32_t id) {
        { conn.template fetch_all<DTO>() } -> std::same_as<std::vector<DTO>>;
        { conn.template count<DTO>() } -> std::convertible_to<std::size_t>;
    };

    // Full contract concept for writing DTOs to connection
    template<typename Conn, typename DTO>
    concept WritableConnection = DatabaseConnection<Conn> && requires(Conn& conn, const DTO& dto, const std::vector<DTO>& dtos) {
        conn.insert(dto);
        conn.insert_many(dtos);
        conn.update(dto);
    };

    template<typename Domain, typename DTO, typename ConnectionHandle, typename KeyType = std::int32_t>
        requires DAL::Mappers::Mappable<Domain, DTO> && DatabaseConnection<ConnectionHandle>
    class GenericRepository : public virtual Core::IGenericRepository<Domain, KeyType>
    {
    public:
        explicit GenericRepository(std::shared_ptr<ConnectionHandle> conn) pre(conn != nullptr)
            : conn_(std::move(conn))
        {
        }

        [[nodiscard]] std::vector<Domain> get_all() const override
        {
            return conn_->template fetch_all<DTO>() | std::views::transform(DAL::Mappers::MapperTraits<Domain, DTO>::to_domain) |
                std::ranges::to<std::vector>();
        }

        [[nodiscard]] std::expected<std::vector<Domain>, Core::DbError> try_get_all() const override
        {
            if constexpr (requires { conn_->template try_fetch_all<DTO>(); })
            {
                auto res = conn_->template try_fetch_all<DTO>();
                if (!res)
                {
                    return std::unexpected<Core::DbError>{ res.error() };
                }
                return res.value() | std::views::transform(DAL::Mappers::MapperTraits<Domain, DTO>::to_domain) |
                    std::ranges::to<std::vector>();
            }
            else
            {
                if (!conn_->is_open())
                {
                    return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is closed") };
                }
                return get_all();
            }
        }

        [[nodiscard]] std::optional<Domain> get_by_id(KeyType id) const override
        {
            if constexpr (Core::CompositeKeyType<KeyType>)
            {
                const auto& [... parts] = id.tuple();
                if constexpr (requires { conn_->template get_by_composite_id<DTO>(unwrap_key_part(parts)...); })
                {
                    return conn_->template get_by_composite_id<DTO>(unwrap_key_part(parts)...)
                        .transform(DAL::Mappers::MapperTraits<Domain, DTO>::to_domain);
                }
                else
                {
                    auto dtos = conn_->template fetch_all<DTO>();
                    for (const auto& dto : dtos)
                    {
                        if (DAL::Schema::EntityMetadata<Domain, DTO>::matches_key(dto, id))
                        {
                            return DAL::Mappers::MapperTraits<Domain, DTO>::to_domain(dto);
                        }
                    }
                    return std::nullopt;
                }
            }
            else
            {
                return conn_->template get_by_id<DTO>(unwrap_key_part(id)).transform(DAL::Mappers::MapperTraits<Domain, DTO>::to_domain);
            }
        }

        [[nodiscard]] std::expected<std::optional<Domain>, Core::DbError> try_get_by_id(KeyType id) const override
        {
            if constexpr (Core::CompositeKeyType<KeyType>)
            {
                const auto& [... parts] = id.tuple();
                if constexpr (requires { conn_->template try_get_by_composite_id<DTO>(unwrap_key_part(parts)...); })
                {
                    auto res = conn_->template try_get_by_composite_id<DTO>(unwrap_key_part(parts)...);
                    if (!res)
                    {
                        return std::unexpected<Core::DbError>{ res.error() };
                    }
                    return res.value().transform(DAL::Mappers::MapperTraits<Domain, DTO>::to_domain);
                }
                else
                {
                    auto dtos_res = [this]() -> std::expected<std::vector<DTO>, Core::DbError>
                    {
                        if constexpr (requires { conn_->template try_fetch_all<DTO>(); })
                        {
                            return conn_->template try_fetch_all<DTO>();
                        }
                        else
                        {
                            if (!conn_->is_open())
                            {
                                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is closed") };
                            }
                            return conn_->template fetch_all<DTO>();
                        }
                    }();

                    if (!dtos_res)
                    {
                        return std::unexpected<Core::DbError>{ dtos_res.error() };
                    }

                    for (const auto& dto : dtos_res.value())
                    {
                        if (DAL::Schema::EntityMetadata<Domain, DTO>::matches_key(dto, id))
                        {
                            return std::optional<Domain>{ DAL::Mappers::MapperTraits<Domain, DTO>::to_domain(dto) };
                        }
                    }
                    return std::optional<Domain>{ std::nullopt };
                }
            }
            else
            {
                if constexpr (requires { conn_->template try_get_by_id<DTO>(unwrap_key_part(id)); })
                {
                    auto res = conn_->template try_get_by_id<DTO>(unwrap_key_part(id));
                    if (!res)
                    {
                        return std::unexpected<Core::DbError>{ res.error() };
                    }
                    return res.value().transform(DAL::Mappers::MapperTraits<Domain, DTO>::to_domain);
                }
                else
                {
                    if (!conn_->is_open())
                    {
                        return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is closed") };
                    }
                    return get_by_id(id);
                }
            }
        }

        [[nodiscard]] std::expected<void, Core::DbError> insert_one(const Domain& item) override
        {
            auto dto = DAL::Mappers::MapperTraits<Domain, DTO>::to_dto(item);
            auto res = conn_->insert(dto);
            if (!res)
            {
                return std::unexpected<Core::DbError>{ map_conn_error(res.error().what()) };
            }
            return {};
        }

        [[nodiscard]] std::expected<void, Core::DbError> insert_many(const std::vector<Domain>& items) override
        {
            auto dtos = items | std::views::transform(DAL::Mappers::MapperTraits<Domain, DTO>::to_dto) | std::ranges::to<std::vector>();
            auto res = conn_->insert_many(dtos);
            if (!res)
            {
                return std::unexpected<Core::DbError>{ map_conn_error(res.error().what()) };
            }
            return {};
        }

        [[nodiscard]] std::expected<void, Core::DbError> update_one(const Domain& item) override
        {
            auto dto = DAL::Mappers::MapperTraits<Domain, DTO>::to_dto(item);
            auto res = conn_->update(dto);
            if (!res)
            {
                return std::unexpected<Core::DbError>{ map_conn_error(res.error().what()) };
            }
            return {};
        }

        [[nodiscard]] std::expected<void, Core::DbError> delete_by_id(KeyType id) override
        {
            if constexpr (Core::CompositeKeyType<KeyType>)
            {
                const auto& [... parts] = id.tuple();
                auto res = [&]()
                {
                    if constexpr (requires { conn_->template delete_by_composite_id<DTO>(unwrap_key_part(parts)...); })
                    {
                        return conn_->template delete_by_composite_id<DTO>(unwrap_key_part(parts)...);
                    }
                    else
                    {
                        return conn_->delete_by_composite_id(unwrap_key_part(parts)...);
                    }
                }();
                if (!res)
                {
                    return std::unexpected<Core::DbError>{ map_conn_error(res.error().what()) };
                }
                return {};
            }
            else
            {
                auto res = conn_->template delete_by_id<DTO>(unwrap_key_part(id));
                if (!res)
                {
                    return std::unexpected<Core::DbError>{ map_conn_error(res.error().what()) };
                }
                return {};
            }
        }

        [[nodiscard]] bool exists_by_id(KeyType id) const override
        {
            if constexpr (Core::CompositeKeyType<KeyType>)
            {
                const auto& [... parts] = id.tuple();
                if constexpr (requires { conn_->template exists_by_composite_id<DTO>(unwrap_key_part(parts)...); })
                {
                    return conn_->template exists_by_composite_id<DTO>(unwrap_key_part(parts)...);
                }
                else
                {
                    return get_by_id(id).has_value();
                }
            }
            else
            {
                return conn_->template exists<DTO>(unwrap_key_part(id));
            }
        }

        [[nodiscard]] std::size_t count() const override
        {
            return conn_->template count<DTO>();
        }

    protected:
        [[nodiscard]] Core::DbError map_conn_error(std::string_view msg) const
        {
            if constexpr (requires { conn_->map_error(msg); })
            {
                return conn_->map_error(msg);
            }
            else
            {
                return Core::DbError::map_from_message(msg);
            }
        }

        template<typename Part>
        [[nodiscard]] static constexpr decltype(auto) unwrap_key_part(const Part& part) noexcept
        {
            using CleanPart = std::remove_cvref_t<Part>;
            if constexpr (Core::SingleKeyType<CleanPart>)
            {
                return unwrap_key_part(part.get());
            }
            else if constexpr (Core::StrongIdType<CleanPart>)
            {
                return part.get();
            }
            else
            {
                return part;
            }
        }

        std::shared_ptr<ConnectionHandle> conn_;
    };

} // namespace DAL::Repositories
