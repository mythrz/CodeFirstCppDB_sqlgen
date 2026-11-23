#pragma once

namespace
{
    // In-Memory Database Connection mock simulating a database driver satisfying DAL::Repositories::DatabaseConnection concept
    class TestDatabaseConnection
    {
    public:
        [[nodiscard]] bool is_open() const noexcept
        {
            return open_;
        }

        void set_open(bool open) noexcept
        {
            open_ = open;
        }

        template<typename DTO>
        std::expected<void, Core::DbError> ensure_table_created()
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("TestDatabaseConnection is closed") };
            }
            return {};
        }

        template<typename DTO>
        std::expected<void, Core::DbError> ensure_table_dropped()
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("TestDatabaseConnection is closed") };
            }
            get_storage<DTO>().clear();
            return {};
        }

        std::expected<void, Core::DbError> enable_foreign_keys()
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("TestDatabaseConnection is closed") };
            }
            return {};
        }

        std::expected<void, Core::DbError> begin_transaction()
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("TestDatabaseConnection is closed") };
            }
            if (in_transaction_)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Transaction is already active") };
            }
            snapshot_ = std::make_tuple(persons_, somethings_, person_somethings_);
            in_transaction_ = true;
            return {};
        }

        std::expected<void, Core::DbError> commit()
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("TestDatabaseConnection is closed") };
            }
            if (!in_transaction_)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("No active transaction to commit") };
            }
            in_transaction_ = false;
            return {};
        }

        std::expected<void, Core::DbError> rollback()
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("TestDatabaseConnection is closed") };
            }
            if (!in_transaction_)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("No active transaction to rollback") };
            }
            persons_ = std::move(std::get<0>(snapshot_));
            somethings_ = std::move(std::get<1>(snapshot_));
            person_somethings_ = std::move(std::get<2>(snapshot_));
            in_transaction_ = false;
            return {};
        }

        [[nodiscard]] bool in_transaction() const noexcept
        {
            return in_transaction_;
        }

        template<typename DTO>
        std::vector<DTO>& get_storage()
        {
            if constexpr (std::is_same_v<DTO, DAL::Schema::PersonDTO>)
            {
                return persons_;
            }
            else if constexpr (std::is_same_v<DTO, DAL::Schema::SomethingDTO>)
            {
                return somethings_;
            }
            else if constexpr (std::is_same_v<DTO, DAL::Schema::Person_SomethingDTO>)
            {
                return person_somethings_;
            }
        }

        template<typename DTO>
        const std::vector<DTO>& get_storage() const
        {
            if constexpr (std::is_same_v<DTO, DAL::Schema::PersonDTO>)
            {
                return persons_;
            }
            else if constexpr (std::is_same_v<DTO, DAL::Schema::SomethingDTO>)
            {
                return somethings_;
            }
            else if constexpr (std::is_same_v<DTO, DAL::Schema::Person_SomethingDTO>)
            {
                return person_somethings_;
            }
        }

        template<typename DTO>
        std::expected<std::vector<DTO>, Core::DbError> try_fetch_all() const
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("TestDatabaseConnection is closed") };
            }
            return get_storage<DTO>();
        }

        template<typename DTO>
        std::vector<DTO> fetch_all() const
        {
            return get_storage<DTO>();
        }

        template<typename DTO>
        std::expected<std::optional<DTO>, Core::DbError> try_get_by_id(std::int32_t id) const
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("TestDatabaseConnection is closed") };
            }
            return std::optional<DTO>{ get_by_id<DTO>(id) };
        }

        template<typename DTO>
        std::optional<DTO> get_by_id(std::int32_t id) const
        {
            const auto& storage = get_storage<DTO>();
            for (const auto& item : storage)
            {
                if constexpr (requires { item.id; })
                {
                    if (item.id.value() == id)
                        return item;
                }
            }
            return std::nullopt;
        }

        template<typename DTO, typename... Parts>
        std::expected<std::optional<DTO>, Core::DbError> try_get_by_composite_id(const Parts&... parts) const
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("TestDatabaseConnection is closed") };
            }
            return std::optional<DTO>{ get_by_composite_id<DTO>(parts...) };
        }

        template<typename DTO, typename... Parts>
        std::optional<DTO> get_by_composite_id(const Parts&... parts) const
        {
            const auto& storage = get_storage<DTO>();
            auto key_tuple = std::make_tuple(static_cast<std::int32_t>(parts)...);
            for (const auto& item : storage)
            {
                if constexpr (std::is_same_v<DTO, DAL::Schema::Person_SomethingDTO>)
                {
                    if (item.person_id.value() == std::get<0>(key_tuple) && item.something_id.value() == std::get<1>(key_tuple))
                    {
                        return item;
                    }
                }
            }
            return std::nullopt;
        }

        template<typename DTO, typename... Parts>
        bool exists_by_composite_id(const Parts&... parts) const
        {
            return get_by_composite_id<DTO>(parts...).has_value();
        }

        template<typename DTO, rfl::internal::StringLiteral ColLiteral, typename Val>
        std::vector<DTO> fetch_where_equal(const Val& val) const
        {
            std::vector<DTO> result;
            for (const auto& item : get_storage<DTO>())
            {
                if constexpr (std::is_same_v<DTO, DAL::Schema::PersonDTO> && ColLiteral.string_view() == "last_name")
                {
                    if (item.last_name == val)
                    {
                        result.push_back(item);
                    }
                }
                else if constexpr (std::is_same_v<DTO, DAL::Schema::Person_SomethingDTO> && ColLiteral.string_view() == "person_id")
                {
                    if (item.person_id.value() == val)
                    {
                        result.push_back(item);
                    }
                }
                else if constexpr (std::is_same_v<DTO, DAL::Schema::Person_SomethingDTO> && ColLiteral.string_view() == "something_id")
                {
                    if (item.something_id.value() == val)
                    {
                        result.push_back(item);
                    }
                }
            }
            return result;
        }

        // Updated to return sqlgen::Result<sqlgen::Nothing> preserving error information

        template<typename DTO>
        sqlgen::Result<sqlgen::Nothing> insert(const DTO& item)
        {
            auto& storage = get_storage<DTO>();
            if constexpr (requires { item.id; })
            {
                for (const auto& existing : storage)
                {
                    if (existing.id.value() == item.id.value())
                    {
                        return sqlgen::error("UNIQUE constraint failed: duplicate primary key");
                    }
                }
            }
            else if constexpr (std::is_same_v<DTO, DAL::Schema::Person_SomethingDTO>)
            {
                for (const auto& existing : storage)
                {
                    if (existing.person_id.value() == item.person_id.value() && existing.something_id.value() == item.something_id.value())
                    {
                        return sqlgen::error("UNIQUE constraint failed: duplicate composite key");
                    }
                }
            }
            storage.push_back(item);
            // success -> default-constructed Result (has value)
            return sqlgen::Result<sqlgen::Nothing>{ sqlgen::Nothing{} };
        }

        template<typename DTO>
        sqlgen::Result<sqlgen::Nothing> insert_many(const std::vector<DTO>& items)
        {
            auto& storage = get_storage<DTO>();
            storage.insert(storage.end(), items.begin(), items.end());
            return sqlgen::Result<sqlgen::Nothing>{ sqlgen::Nothing{} };
        }

        template<typename DTO>
        sqlgen::Result<sqlgen::Nothing> update(const DTO& item)
        {
            auto& storage = get_storage<DTO>();
            for (auto& elem : storage)
            {
                if constexpr (requires { elem.id; })
                {
                    if (elem.id.value() == item.id.value())
                    {
                        elem = item;
                        return sqlgen::Result<sqlgen::Nothing>{ sqlgen::Nothing{} };
                    }
                }
                else if constexpr (std::is_same_v<DTO, DAL::Schema::Person_SomethingDTO>)
                {
                    if (elem.person_id.value() == item.person_id.value() && elem.something_id.value() == item.something_id.value())
                    {
                        elem = item;
                        return sqlgen::Result<sqlgen::Nothing>{ sqlgen::Nothing{} };
                    }
                }
            }
            // Return an error if the item was not found
            return sqlgen::error("Update failed: item not found");
        }

        template<typename DTO>
        sqlgen::Result<sqlgen::Nothing> delete_by_id(std::int32_t id)
        {
            auto& storage = get_storage<DTO>();
            std::erase_if(
                storage,
                [id](const DTO& item)
                {
                    if constexpr (requires { item.id; })
                    {
                        return item.id.value() == id;
                    }
                    return false;
                });
            return sqlgen::Result<sqlgen::Nothing>{ sqlgen::Nothing{} };
        }

        template<typename DTO = DAL::Schema::Person_SomethingDTO, typename... Parts>
        sqlgen::Result<sqlgen::Nothing> delete_by_composite_id(const Parts&... parts)
        {
            auto& storage = get_storage<DTO>();
            auto key_tuple = std::make_tuple(static_cast<std::int32_t>(parts)...);
            std::erase_if(
                storage,
                [&key_tuple](const DTO& item)
                {
                    if constexpr (std::is_same_v<DTO, DAL::Schema::Person_SomethingDTO>)
                    {
                        return item.person_id.value() == std::get<0>(key_tuple) && item.something_id.value() == std::get<1>(key_tuple);
                    }
                    return false;
                });
            return sqlgen::Result<sqlgen::Nothing>{ sqlgen::Nothing{} };
        }

        template<typename DTO>
        bool exists(std::int32_t id) const
        {
            return get_by_id<DTO>(id).has_value();
        }

        template<typename DTO>
        std::size_t count() const
        {
            return get_storage<DTO>().size();
        }

        template<typename DTO>
        struct QueryBuilder
        {
            const TestDatabaseConnection& conn;
            std::string column;
            std::string value;

            QueryBuilder& where(std::string_view col, std::string_view val)
            {
                column = col;
                value = val;
                return *this;
            }

            std::vector<DTO> execute() const
            {
                std::vector<DTO> result;
                const auto& storage = conn.get_storage<DTO>();
                for (const auto& item : storage)
                {
                    if constexpr (std::is_same_v<DTO, DAL::Schema::PersonDTO>)
                    {
                        if (column == "last_name" && item.last_name == value)
                        {
                            result.push_back(item);
                        }
                    }
                }
                return result;
            }
        };

        template<typename DTO>
        QueryBuilder<DTO> query() const
        {
            return QueryBuilder<DTO>{ *this, "", "" };
        }

    private:
        bool open_{ true };
        bool in_transaction_{ false };
        std::vector<DAL::Schema::PersonDTO> persons_;
        std::vector<DAL::Schema::SomethingDTO> somethings_;
        std::vector<DAL::Schema::Person_SomethingDTO> person_somethings_;
        std::tuple<
            std::vector<DAL::Schema::PersonDTO>,
            std::vector<DAL::Schema::SomethingDTO>,
            std::vector<DAL::Schema::Person_SomethingDTO>>
            snapshot_;
    };

    class Phase6AppDbContext : public DAL::DbContextBase<DAL::SQLiteConnection>
    {
    public:
        Set<Core::Person> people{ this };
        Set<Core::Something> somethings{ this };
        Set<Core::Person_Something> personSomethings{ this };

        explicit Phase6AppDbContext(std::shared_ptr<DAL::SQLiteConnection> conn)
            : DAL::DbContextBase<DAL::SQLiteConnection>(std::move(conn))
        {
        }
    };

    struct PocoUser
    {
        static constexpr auto tablename = "PocoUsers";
        sqlgen::PrimaryKey<std::int32_t, true> user_id{ 0 };
        std::string username;
    };

    struct PocoRole
    {
        static constexpr auto tablename = "PocoRoles";
        sqlgen::PrimaryKey<std::int32_t, true> role_id{ 0 };
        std::string role_name;
    };

    struct PocoUserRole
    {
        static constexpr auto tablename = "PocoUserRoles";
        static constexpr bool composite_primary_key = true;
        sqlgen::ForeignKey<std::int32_t, PocoUser, "user_id"> user_id{ 0 };
        sqlgen::ForeignKey<std::int32_t, PocoRole, "role_id"> role_id{ 0 };
        std::string assigned_by;
    };

    class PocoDbContext : public DAL::DbContextBase<DAL::SQLiteConnection>
    {
    public:
        Set<PocoUser> users{ this };
        Set<PocoRole> roles{ this };
        Set<PocoUserRole> userRoles{ this };

        explicit PocoDbContext(std::shared_ptr<DAL::SQLiteConnection> conn)
            : DAL::DbContextBase<DAL::SQLiteConnection>(std::move(conn))
        {
        }
    };

    struct Phase30DbContext : DAL::DbContextBase<DAL::SQLiteConnection>
    {
        DAL::DbSet<Core::Person, DAL::Schema::PersonDTO, DAL::SQLiteConnection> people{ this };
        DAL::DbSet<Core::Something, DAL::Schema::SomethingDTO, DAL::SQLiteConnection> somethings{ this };
        DAL::DbSet<Core::Person_Something, DAL::Schema::Person_SomethingDTO, DAL::SQLiteConnection> personSomethings{ this };

        explicit Phase30DbContext(std::shared_ptr<DAL::SQLiteConnection> c)
            : DAL::DbContextBase<DAL::SQLiteConnection>(std::move(c))
        {
        }
    };

    struct Phase25Author
    {
        static constexpr const char* tablename = "phase25_authors";
        sqlgen::PrimaryKey<std::int32_t, true> author_id{ 0 };
        std::string name{};
    };

    struct Phase25Book
    {
        static constexpr const char* tablename = "phase25_books";
        sqlgen::PrimaryKey<std::int32_t, true> book_id{ 0 };
        sqlgen::ForeignKey<std::int32_t, Phase25Author, "author_id"> author_id{ 0 };
        std::string title{};
        std::string genre{};
    };

    struct Phase25Context : DAL::DbContextBase<DAL::SQLiteConnection>
    {
        DAL::DbSet<Phase25Author, Phase25Author, DAL::SQLiteConnection> authors{ this };
        DAL::DbSet<Phase25Book, Phase25Book, DAL::SQLiteConnection> books{ this };

        explicit Phase25Context(std::shared_ptr<DAL::SQLiteConnection> c)
            : DAL::DbContextBase<DAL::SQLiteConnection>(std::move(c))
        {
        }
    };
} // namespace
