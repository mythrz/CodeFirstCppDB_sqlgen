module;
#include <meta>
#include <sqlgen.hpp>
#include <sqlgen/sqlite.hpp>
#include <sqlite3.h>

export module dal:sqlite_connection;

import std;
import core;
import :schema_traits;

export namespace DAL
{
    struct SQLiteConnectionOptions
    {
        bool foreign_keys = true;
        std::chrono::milliseconds busy_timeout{ 0 };
        bool wal_mode = false;
    };

    struct RowCount
    {
        std::size_t count = 0;
    };

    struct TableColumnInfo
    {
        std::int64_t cid = 0;
        std::string name;
        std::string type;
        bool not_null = false;
        std::optional<std::string> default_value;
        std::int64_t pk_index = 0;
    };

    using DynamicCell = std::variant<std::monostate, std::int64_t, double, std::string, std::vector<std::byte>>;

    struct DynamicRow
    {
        std::vector<DynamicCell> cells;
    };

    struct DynamicQueryResult
    {
        std::vector<std::string> columns;
        std::vector<DynamicRow> rows;
    };

    // SQLiteConnection adapts a raw sqlgen SQLite connection to satisfy
    // the generic DAL::Repositories::DatabaseConnection concepts.
    class SQLiteConnection
    {
    public:
        explicit SQLiteConnection(sqlgen::Ref<sqlgen::sqlite::Connection> conn)
            : conn_(std::move(conn))
        {
        }

        SQLiteConnection(const SQLiteConnection&) = delete;
        SQLiteConnection& operator=(const SQLiteConnection&) = delete;
        SQLiteConnection(SQLiteConnection&&) = delete;
        SQLiteConnection& operator=(SQLiteConnection&&) = delete;

        ~SQLiteConnection() noexcept
        {
            close();
        }

        void close() noexcept
        {
            if (!is_open_)
            {
                return;
            }
            if (in_transaction() && conn_)
            {
                try
                {
                    (void)(*conn_)->rollback();
                } catch (...)
                {
                }
                in_transaction_ = false;
            }
            is_open_ = false;
            conn_.reset();
        }

        [[nodiscard]] static std::expected<std::shared_ptr<SQLiteConnection>, Core::DbError> connect(const std::string& dbPath)
        {
            return connect(dbPath, SQLiteConnectionOptions{});
        }

        [[nodiscard]] static std::expected<std::shared_ptr<SQLiteConnection>, Core::DbError> connect(
            const std::string& dbPath, const SQLiteConnectionOptions& options)
        {
            auto res = sqlgen::sqlite::connect(dbPath);
            if (!res)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error(res.error().what()) };
            }
            auto handle = std::make_shared<SQLiteConnection>(std::move(res.value()));
            if (options.foreign_keys)
            {
                if (auto fk_res = handle->enable_foreign_keys(); !fk_res)
                {
                    return std::unexpected<Core::DbError>{ fk_res.error() };
                }
            }
            if (options.busy_timeout.count() > 0)
            {
                if (auto bt_res = handle->set_busy_timeout(options.busy_timeout); !bt_res)
                {
                    return std::unexpected<Core::DbError>{ bt_res.error() };
                }
            }
            if (options.wal_mode)
            {
                if (auto wal_res = handle->enable_wal_mode(); !wal_res)
                {
                    return std::unexpected<Core::DbError>{ wal_res.error() };
                }
            }
            return handle;
        }

        template<typename DTO>
        [[nodiscard]] std::expected<void, Core::DbError> ensure_table_created()
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error(
                    "Cannot create table: database connection is closed") };
            }
            if (auto id_chk = validate_dto_ddl_identifiers<DTO>(); !id_chk)
            {
                return id_chk;
            }

            constexpr bool has_unique_flags = requires { DTO::unique_field_flags; };
            constexpr bool has_pk_flags = requires { DTO::pk_field_flags; };

            if constexpr (Schema::CompositePrimaryKey<DTO> || has_unique_flags || has_pk_flags)
            {
                auto stmt = sqlgen::transpilation::to_create_table<DTO>(true);
                for (std::size_t i = 0; i < stmt.columns.size(); ++i)
                {
                    stmt.columns[i].type.visit(
                        [&](auto& t)
                        {
                            if constexpr (Schema::CompositePrimaryKey<DTO>)
                            {
                                // Clear per-column PRIMARY KEY markers so multi-PrimaryKey and
                                // composite FK junction DTOs emit one table-level PRIMARY KEY.
                                t.properties.primary = false;
                                t.properties.auto_incr = false;
                            }
                            else if constexpr (has_pk_flags)
                            {
                                if (i < DTO::pk_field_flags.size() && DTO::pk_field_flags[i])
                                {
                                    t.properties.primary = true;
                                }
                            }
                            if constexpr (has_unique_flags)
                            {
                                if (i < DTO::unique_field_flags.size() && DTO::unique_field_flags[i])
                                {
                                    t.properties.unique = true;
                                }
                            }
                        });
                }

                std::string create_sql = (*conn_)->to_sql(stmt);
                if constexpr (Schema::CompositePrimaryKey<DTO>)
                {
                    static constexpr auto key_members = Schema::key_members_of<DTO>();
                    std::string cols;
                    std::size_t idx = 0;
                    template for (constexpr auto m : key_members)
                    {
                        if (idx > 0)
                        {
                            cols += ", ";
                        }
                        cols += std::format("\"{}\"", std::meta::identifier_of(m));
                        ++idx;
                    }
                    if (const auto close_pos = create_sql.rfind(");"); close_pos != std::string::npos)
                    {
                        create_sql.insert(close_pos, std::format(", PRIMARY KEY ({})", cols));
                    }
                    if (auto res = execute_prepared(create_sql); !res)
                    {
                        return res;
                    }
                    const std::string_view table_name = DTO::tablename;
                    const std::string index_sql = std::format(
                        "CREATE UNIQUE INDEX IF NOT EXISTS \"ux_{}_composite_pk\" ON \"{}\" ({});", table_name, table_name, cols);
                    if (auto idx_res = execute_prepared(index_sql); !idx_res)
                    {
                        return idx_res;
                    }
                }
                else
                {
                    if (auto res = execute_prepared(create_sql); !res)
                    {
                        return res;
                    }
                }
            }
            else
            {
                const std::string create_sql = sqlgen::sqlite::to_sql(sqlgen::CreateTable<DTO>{ .if_not_exists_ = true });
                if (auto res = execute_prepared(create_sql); !res)
                {
                    return res;
                }
            }
            return {};
        }

        template<rfl::internal::StringLiteral IndexName, typename DTO, typename... ColTypes>
            requires(sizeof...(ColTypes) >= 1)
        [[nodiscard]] std::expected<void, Core::DbError> ensure_index_created(bool unique, const ColTypes&...)
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error(
                    "Cannot create index: database connection is closed") };
            }
            if (auto idx_chk = validate_ddl_identifier(IndexName.string_view()); !idx_chk)
            {
                return idx_chk;
            }
            if (auto dto_chk = validate_dto_ddl_identifiers<DTO>(); !dto_chk)
            {
                return dto_chk;
            }
            const std::string sql = sqlgen::sqlite::to_sql(
                sqlgen::CreateIndex<IndexName, DTO, sqlgen::Nothing, ColTypes...>{
                    .unique_ = unique, .if_not_exists_ = true, .where_ = sqlgen::Nothing{} });
            return execute_prepared(sql);
        }

        template<rfl::internal::StringLiteral IndexName, typename DTO, typename WhereCond, typename... ColTypes>
            requires(sizeof...(ColTypes) >= 1)
        [[nodiscard]] std::expected<void, Core::DbError> ensure_filtered_index_created(
            bool unique, const WhereCond& where_cond, const ColTypes&...)
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error(
                    "Cannot create index: database connection is closed") };
            }
            if (auto idx_chk = validate_ddl_identifier(IndexName.string_view()); !idx_chk)
            {
                return idx_chk;
            }
            if (auto dto_chk = validate_dto_ddl_identifiers<DTO>(); !dto_chk)
            {
                return dto_chk;
            }
            const std::string raw_sql = sqlgen::sqlite::to_sql(
                sqlgen::CreateIndex<IndexName, DTO, WhereCond, ColTypes...>{
                    .unique_ = unique, .if_not_exists_ = true, .where_ = where_cond });
            auto safe_sql = encode_sqlgen_embedded_nuls(raw_sql);
            if (!safe_sql)
            {
                return std::unexpected<Core::DbError>{ safe_sql.error() };
            }
            return execute_prepared(*safe_sql);
        }

        template<typename DTO>
        [[nodiscard]] std::expected<void, Core::DbError> ensure_table_dropped()
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error(
                    "Cannot drop table: database connection is closed") };
            }
            if (auto dto_chk = validate_dto_ddl_identifiers<DTO>(); !dto_chk)
            {
                return dto_chk;
            }
            const std::string sql = sqlgen::sqlite::to_sql(
                sqlgen::Drop<DTO>{ .what_ = sqlgen::transpilation::get_table_or_view<DTO>(), .cascade_ = false, .if_exists_ = true });
            return execute_prepared(sql);
        }

        [[nodiscard]] std::expected<void, Core::DbError> enable_foreign_keys()
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error(
                    "Cannot enable foreign keys: database connection is closed") };
            }
            auto res = (*conn_)->execute("PRAGMA foreign_keys = ON;");
            if (!res)
            {
                return std::unexpected<Core::DbError>{ map_sqlite_error(res.error().what()) };
            }
            return {};
        }

        [[nodiscard]] std::expected<void, Core::DbError> begin_transaction()
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error(
                    "Cannot begin transaction: database connection is closed") };
            }
            if (in_transaction())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Transaction is already active") };
            }
            auto res = (*conn_)->begin_transaction();
            if (!res)
            {
                return std::unexpected<Core::DbError>{ map_sqlite_error(res.error().what()) };
            }
            in_transaction_ = true;
            return {};
        }

        [[nodiscard]] std::expected<void, Core::DbError> commit()
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error(
                    "Cannot commit transaction: database connection is closed") };
            }
            if (!in_transaction())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("No active transaction to commit") };
            }
            auto res = (*conn_)->commit();
            if (!res)
            {
                return std::unexpected<Core::DbError>{ map_sqlite_error(res.error().what()) };
            }
            in_transaction_ = in_transaction();
            return {};
        }

        [[nodiscard]] std::expected<void, Core::DbError> rollback()
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error(
                    "Cannot rollback transaction: database connection is closed") };
            }
            if (!in_transaction())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("No active transaction to rollback") };
            }
            auto res = (*conn_)->rollback();
            in_transaction_ = in_transaction();
            if (!res)
            {
                return std::unexpected<Core::DbError>{ map_sqlite_error(res.error().what()) };
            }
            return {};
        }

        [[nodiscard]] bool in_transaction() const noexcept
        {
            if (!is_open())
            {
                return false;
            }
            sqlite3* db = native_sqlite_handle();
            if (db != nullptr)
            {
                return sqlite3_get_autocommit(db) == 0;
            }
            return in_transaction_;
        }

        [[nodiscard]] bool is_open() const noexcept
        {
            return is_open_ && conn_.has_value();
        }

        [[nodiscard]] sqlgen::Ref<sqlgen::sqlite::Connection>& raw_connection() noexcept pre(is_open())
        {
            return (*conn_);
        }

        [[nodiscard]] const sqlgen::Ref<sqlgen::sqlite::Connection>& raw_connection() const noexcept pre(is_open())
        {
            return (*conn_);
        }

        template<typename DTO>
        [[nodiscard]] std::expected<std::vector<DTO>, Core::DbError> try_fetch_all() const
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is closed") };
            }
            const std::string sql = sqlgen::sqlite::to_sql(sqlgen::read<std::vector<DTO>>);
            return execute_typed_sql<DTO>(sql);
        }

        template<typename DTO>
        [[nodiscard]] std::vector<DTO> fetch_all() const
        {
            auto res = try_fetch_all<DTO>();
            if (res)
            {
                return std::move(res.value());
            }
            return {};
        }

        template<typename DTO, typename IdType>
        [[nodiscard]] std::expected<std::optional<DTO>, Core::DbError> try_get_by_id(const IdType& id) const
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is closed") };
            }
            if (auto id_chk = validate_dto_ddl_identifiers<DTO>(); !id_chk)
            {
                return std::unexpected<Core::DbError>{ id_chk.error() };
            }
            constexpr auto pk_m = DAL::Schema::primary_key_member<DTO>();
            const std::string sql = std::format(
                "SELECT {} FROM \"{}\" WHERE \"{}\" = ? LIMIT 1;",
                build_select_columns_sql<DTO>(),
                std::string_view{ DTO::tablename },
                std::meta::identifier_of(pk_m));
            auto res = execute_typed_sql_bound<DTO>(
                sql,
                [&](sqlite3* db, sqlite3_stmt* stmt) -> std::expected<void, Core::DbError>
                {
                    return bind_typed_field(db, stmt, 1, id, false);
                });
            if (!res)
            {
                return std::unexpected<Core::DbError>{ res.error() };
            }
            if (res.value().empty())
            {
                return std::optional<DTO>{ std::nullopt };
            }
            return std::optional<DTO>{ std::move(res.value().front()) };
        }

        template<typename DTO, typename IdType>
        [[nodiscard]] std::optional<DTO> get_by_id(const IdType& id) const
        {
            auto res = try_get_by_id<DTO>(id);
            if (res)
            {
                return std::move(res.value());
            }
            return std::nullopt;
        }

        template<typename DTO, typename... Parts>
        [[nodiscard]] std::expected<std::optional<DTO>, Core::DbError> try_get_by_composite_id(const Parts&... parts) const
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is closed") };
            }
            if (auto id_chk = validate_dto_ddl_identifiers<DTO>(); !id_chk)
            {
                return std::unexpected<Core::DbError>{ id_chk.error() };
            }
            const std::string sql = std::format(
                "SELECT {} FROM \"{}\" WHERE {} LIMIT 1;",
                build_select_columns_sql<DTO>(),
                std::string_view{ DTO::tablename },
                build_key_where_sql<DTO>());
            auto res = execute_typed_sql_bound<DTO>(
                sql,
                [&](sqlite3* db, sqlite3_stmt* stmt) -> std::expected<void, Core::DbError>
                {
                    return bind_variadic_fields(db, stmt, 1, parts...);
                });
            if (!res)
            {
                return std::unexpected<Core::DbError>{ res.error() };
            }
            if (res.value().empty())
            {
                return std::optional<DTO>{ std::nullopt };
            }
            return std::optional<DTO>{ std::move(res.value().front()) };
        }

        template<typename DTO, typename... Parts>
        [[nodiscard]] std::optional<DTO> get_by_composite_id(const Parts&... parts) const
        {
            auto res = try_get_by_composite_id<DTO>(parts...);
            if (res)
            {
                return std::move(res.value());
            }
            return std::nullopt;
        }

        template<typename DTO, rfl::internal::StringLiteral ColLiteral, typename Val>
        [[nodiscard]] std::vector<DTO> fetch_where_equal(const Val& val) const
        {
            if (!is_open())
            {
                return {};
            }
            if (!validate_dto_ddl_identifiers<DTO>().has_value() || !validate_ddl_identifier(ColLiteral.string_view()).has_value())
            {
                return {};
            }
            const std::string sql = std::format(
                "SELECT {} FROM \"{}\" WHERE \"{}\" = ?;",
                build_select_columns_sql<DTO>(),
                std::string_view{ DTO::tablename },
                ColLiteral.string_view());
            auto res = execute_typed_sql_bound<DTO>(
                sql,
                [&](sqlite3* db, sqlite3_stmt* stmt) -> std::expected<void, Core::DbError>
                {
                    return bind_typed_field(db, stmt, 1, val, false);
                });
            if (res)
            {
                return std::move(res.value());
            }
            return {};
        }

        template<typename DTO, typename Query>
        [[nodiscard]] std::expected<std::vector<DTO>, Core::DbError> try_execute_read(Query&& query) const
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is closed") };
            }
            auto safe_sql = encode_sqlgen_embedded_nuls(sqlgen::sqlite::to_sql(std::forward<Query>(query)));
            if (!safe_sql)
            {
                return std::unexpected<Core::DbError>{ safe_sql.error() };
            }
            return execute_typed_sql<DTO>(*safe_sql);
        }

        template<typename RowTuple, typename Query>
        [[nodiscard]] std::expected<std::vector<RowTuple>, Core::DbError> try_execute_tuple_read(Query&& query) const
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is closed") };
            }
            auto safe_sql = encode_sqlgen_embedded_nuls(sqlgen::sqlite::to_sql(std::forward<Query>(query)));
            if (!safe_sql)
            {
                return std::unexpected<Core::DbError>{ safe_sql.error() };
            }
            return execute_typed_tuple_sql<RowTuple>(*safe_sql);
        }

        template<typename DTO, typename Query>
        [[nodiscard]] std::vector<DTO> execute_read(Query&& query) const
        {
            auto res = try_execute_read<DTO>(std::forward<Query>(query));
            if (res)
            {
                return std::move(res.value());
            }
            return {};
        }

        template<typename DTO>
        [[nodiscard]] auto insert(const DTO& item)
        {
            if (!is_open())
            {
                return sqlgen::Result<sqlgen::Ref<sqlgen::sqlite::Connection>>{ sqlgen::error("Database connection is closed") };
            }
            const std::span<const DTO> one{ &item, 1 };
            auto res = execute_insert_batch<DTO>(one, false);
            if (!res)
            {
                return sqlgen::Result<sqlgen::Ref<sqlgen::sqlite::Connection>>{ sqlgen::error(res.error().message) };
            }
            return sqlgen::Result<sqlgen::Ref<sqlgen::sqlite::Connection>>{ *conn_ };
        }

        template<typename DTO>
        [[nodiscard]] auto insert_many(const std::vector<DTO>& items)
        {
            if (!is_open())
            {
                return sqlgen::Result<sqlgen::Ref<sqlgen::sqlite::Connection>>{ sqlgen::error("Database connection is closed") };
            }
            auto res = execute_insert_batch<DTO>(std::span<const DTO>{ items }, false);
            if (!res)
            {
                return sqlgen::Result<sqlgen::Ref<sqlgen::sqlite::Connection>>{ sqlgen::error(res.error().message) };
            }
            return sqlgen::Result<sqlgen::Ref<sqlgen::sqlite::Connection>>{ *conn_ };
        }

        template<typename DTO>
        [[nodiscard]] auto update(const DTO& item)
        {
            if (!is_open())
            {
                return sqlgen::Result<sqlgen::Ref<sqlgen::sqlite::Connection>>{ sqlgen::error("Database connection is closed") };
            }
            if (auto id_chk = validate_dto_ddl_identifiers<DTO>(); !id_chk)
            {
                return sqlgen::Result<sqlgen::Ref<sqlgen::sqlite::Connection>>{ sqlgen::error(id_chk.error().message) };
            }

            static constexpr auto km = DAL::Schema::key_members_of<DTO>();
            static constexpr auto nk_members = DAL::Schema::non_key_members_of<DTO>();
            if constexpr (nk_members.empty())
            {
                const std::string sql =
                    std::format("SELECT 1 FROM \"{}\" WHERE {} LIMIT 1;", std::string_view{ DTO::tablename }, build_key_where_sql<DTO>());
                auto exists_res = execute_raw_text_rows_bound(
                    sql,
                    [&](sqlite3* db, sqlite3_stmt* stmt) -> std::expected<void, Core::DbError>
                    {
                        int idx = 1;
                        std::expected<void, Core::DbError> bind_res{};
                        template for (constexpr auto m : km)
                        {
                            if (bind_res)
                            {
                                bind_res = bind_typed_field(db, stmt, idx++, item.[:m:], false);
                            }
                        }
                        return bind_res;
                    });
                if (!exists_res)
                {
                    return sqlgen::Result<sqlgen::Ref<sqlgen::sqlite::Connection>>{ sqlgen::error(exists_res.error().message) };
                }
                if (exists_res.value().empty())
                {
                    return sqlgen::Result<sqlgen::Ref<sqlgen::sqlite::Connection>>{ sqlgen::error("Update failed: item not found") };
                }
                return sqlgen::Result<sqlgen::Ref<sqlgen::sqlite::Connection>>{ *conn_ };
            }
            else
            {
                std::string set_clause;
                std::size_t s_idx = 0;
                template for (constexpr auto m : nk_members)
                {
                    if (s_idx > 0)
                    {
                        set_clause += ", ";
                    }
                    set_clause += std::format("\"{}\" = ?", std::meta::identifier_of(m));
                    ++s_idx;
                }
                const std::string sql = std::format(
                    "UPDATE \"{}\" SET {} WHERE {};", std::string_view{ DTO::tablename }, set_clause, build_key_where_sql<DTO>());
                auto upd_res = execute_dml_bound(
                    sql,
                    [&](sqlite3* db, sqlite3_stmt* stmt) -> std::expected<void, Core::DbError>
                    {
                        int idx = 1;
                        std::expected<void, Core::DbError> bind_res{};
                        template for (constexpr auto m : nk_members)
                        {
                            if (bind_res)
                            {
                                bind_res = bind_typed_field(db, stmt, idx++, item.[:m:], false);
                            }
                        }
                        template for (constexpr auto m : km)
                        {
                            if (bind_res)
                            {
                                bind_res = bind_typed_field(db, stmt, idx++, item.[:m:], false);
                            }
                        }
                        return bind_res;
                    });
                if (!upd_res)
                {
                    return sqlgen::Result<sqlgen::Ref<sqlgen::sqlite::Connection>>{ sqlgen::error(upd_res.error().message) };
                }

                sqlite3* db = native_sqlite_handle();
                if (db == nullptr || sqlite3_changes(db) == 0)
                {
                    return sqlgen::Result<sqlgen::Ref<sqlgen::sqlite::Connection>>{ sqlgen::error("Update failed: item not found") };
                }
                return sqlgen::Result<sqlgen::Ref<sqlgen::sqlite::Connection>>{ *conn_ };
            }
        }

        template<typename DTO, typename IdType>
        [[nodiscard]] auto delete_by_id(const IdType& id)
        {
            if (!is_open())
            {
                return sqlgen::Result<sqlgen::Ref<sqlgen::sqlite::Connection>>{ sqlgen::error("Database connection is closed") };
            }
            if (auto id_chk = validate_dto_ddl_identifiers<DTO>(); !id_chk)
            {
                return sqlgen::Result<sqlgen::Ref<sqlgen::sqlite::Connection>>{ sqlgen::error(id_chk.error().message) };
            }
            constexpr auto pk_m = DAL::Schema::primary_key_member<DTO>();
            const std::string sql =
                std::format("DELETE FROM \"{}\" WHERE \"{}\" = ?;", std::string_view{ DTO::tablename }, std::meta::identifier_of(pk_m));
            auto del_res = execute_dml_bound(
                sql,
                [&](sqlite3* db, sqlite3_stmt* stmt) -> std::expected<void, Core::DbError>
                {
                    return bind_typed_field(db, stmt, 1, id, false);
                });
            if (!del_res)
            {
                return sqlgen::Result<sqlgen::Ref<sqlgen::sqlite::Connection>>{ sqlgen::error(del_res.error().message) };
            }
            return sqlgen::Result<sqlgen::Ref<sqlgen::sqlite::Connection>>{ *conn_ };
        }

        template<typename DTO, typename... Parts>
        [[nodiscard]] auto delete_by_composite_id(const Parts&... parts)
        {
            if (!is_open())
            {
                return sqlgen::Result<sqlgen::Ref<sqlgen::sqlite::Connection>>{ sqlgen::error("Database connection is closed") };
            }
            if (auto id_chk = validate_dto_ddl_identifiers<DTO>(); !id_chk)
            {
                return sqlgen::Result<sqlgen::Ref<sqlgen::sqlite::Connection>>{ sqlgen::error(id_chk.error().message) };
            }
            const std::string sql =
                std::format("DELETE FROM \"{}\" WHERE {};", std::string_view{ DTO::tablename }, build_key_where_sql<DTO>());
            auto del_res = execute_dml_bound(
                sql,
                [&](sqlite3* db, sqlite3_stmt* stmt) -> std::expected<void, Core::DbError>
                {
                    return bind_variadic_fields(db, stmt, 1, parts...);
                });
            if (!del_res)
            {
                return sqlgen::Result<sqlgen::Ref<sqlgen::sqlite::Connection>>{ sqlgen::error(del_res.error().message) };
            }
            return sqlgen::Result<sqlgen::Ref<sqlgen::sqlite::Connection>>{ *conn_ };
        }

        template<typename DTO, typename IdType>
        [[nodiscard]] bool exists(const IdType& id) const
        {
            if (!is_open() || !validate_dto_ddl_identifiers<DTO>().has_value())
            {
                return false;
            }
            constexpr auto pk_m = DAL::Schema::primary_key_member<DTO>();
            const std::string sql = std::format(
                "SELECT 1 FROM \"{}\" WHERE \"{}\" = ? LIMIT 1;", std::string_view{ DTO::tablename }, std::meta::identifier_of(pk_m));
            auto res = execute_raw_text_rows_bound(
                sql,
                [&](sqlite3* db, sqlite3_stmt* stmt) -> std::expected<void, Core::DbError>
                {
                    return bind_typed_field(db, stmt, 1, id, false);
                });
            return res.has_value() && !res->empty();
        }

        template<typename DTO, typename... Parts>
        [[nodiscard]] bool exists_by_composite_id(const Parts&... parts) const
        {
            if (!is_open() || !validate_dto_ddl_identifiers<DTO>().has_value())
            {
                return false;
            }
            const std::string sql =
                std::format("SELECT 1 FROM \"{}\" WHERE {} LIMIT 1;", std::string_view{ DTO::tablename }, build_key_where_sql<DTO>());
            auto res = execute_raw_text_rows_bound(
                sql,
                [&](sqlite3* db, sqlite3_stmt* stmt) -> std::expected<void, Core::DbError>
                {
                    return bind_variadic_fields(db, stmt, 1, parts...);
                });
            return res.has_value() && !res->empty();
        }

        template<typename DTO>
        [[nodiscard]] std::expected<std::size_t, Core::DbError> try_count() const
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is closed") };
            }
            auto query = sqlgen::select_from<DTO>(sqlgen::count() | sqlgen::as<"count">) | sqlgen::to<RowCount>;
            auto res = execute_typed_sql<RowCount>(sqlgen::sqlite::to_sql(query));
            if (!res)
            {
                return std::unexpected<Core::DbError>{ res.error() };
            }
            if (res->size() != 1)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::unknown(
                    std::format("Expected 1 row for count query, got {}", res->size())) };
            }
            return res->front().count;
        }

        template<typename DTO>
        [[nodiscard]] std::size_t count() const
        {
            auto res = try_count<DTO>();
            if (res)
            {
                return res.value();
            }
            return 0;
        }

        template<typename DTO, typename Condition>
        [[nodiscard]] std::expected<std::size_t, Core::DbError> try_count_where(const Condition& cond) const
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is closed") };
            }
            auto query = sqlgen::select_from<DTO>(sqlgen::count() | sqlgen::as<"count">) | sqlgen::where(cond) | sqlgen::to<RowCount>;
            auto safe_sql = encode_sqlgen_embedded_nuls(sqlgen::sqlite::to_sql(query));
            if (!safe_sql)
            {
                return std::unexpected<Core::DbError>{ safe_sql.error() };
            }
            auto res = execute_typed_sql<RowCount>(*safe_sql);
            if (!res)
            {
                return std::unexpected<Core::DbError>{ res.error() };
            }
            if (res->size() != 1)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::unknown(
                    std::format("Expected 1 row for count query, got {}", res->size())) };
            }
            return res->front().count;
        }

        template<typename DTO, typename ResultT, typename AggExpr>
        [[nodiscard]] std::expected<std::optional<ResultT>, Core::DbError> try_aggregate(const AggExpr& agg_expr) const
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is closed") };
            }
            struct AggRow
            {
                std::optional<ResultT> val;
            };
            auto query = sqlgen::select_from<DTO>(agg_expr | sqlgen::as<"val">) | sqlgen::to<AggRow>;
            auto res = execute_typed_sql<AggRow>(sqlgen::sqlite::to_sql(query));
            if (!res)
            {
                return std::unexpected<Core::DbError>{ res.error() };
            }
            if (res->size() != 1)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::unknown(
                    std::format("Expected 1 row for aggregate query, got {}", res->size())) };
            }
            return res->front().val;
        }

        template<typename DTO, typename ResultT, typename AggExpr, typename Condition>
        [[nodiscard]] std::expected<std::optional<ResultT>, Core::DbError> try_aggregate_where(
            const AggExpr& agg_expr, const Condition& cond) const
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is closed") };
            }
            struct AggRow
            {
                std::optional<ResultT> val;
            };
            auto query = sqlgen::select_from<DTO>(agg_expr | sqlgen::as<"val">) | sqlgen::where(cond) | sqlgen::to<AggRow>;
            auto safe_sql = encode_sqlgen_embedded_nuls(sqlgen::sqlite::to_sql(query));
            if (!safe_sql)
            {
                return std::unexpected<Core::DbError>{ safe_sql.error() };
            }
            auto res = execute_typed_sql<AggRow>(*safe_sql);
            if (!res)
            {
                return std::unexpected<Core::DbError>{ res.error() };
            }
            if (res->size() != 1)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::unknown(
                    std::format("Expected 1 row for aggregate query, got {}", res->size())) };
            }
            return res->front().val;
        }

        [[nodiscard]] std::expected<std::int64_t, Core::DbError> try_last_insert_rowid() const
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is closed") };
            }
            sqlite3* db = native_sqlite_handle();
            if (db == nullptr)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection handle is null") };
            }
            return static_cast<std::int64_t>(sqlite3_last_insert_rowid(db));
        }

        template<typename DTO>
            requires DAL::Schema::RelationalEntity<DTO>
        [[nodiscard]] auto upsert(const DTO& item)
        {
            if (!is_open())
            {
                return sqlgen::Result<sqlgen::Ref<sqlgen::sqlite::Connection>>{ sqlgen::error("Database connection is closed") };
            }
            const std::span<const DTO> one{ &item, 1 };
            auto res = execute_insert_batch<DTO>(one, true);
            if (!res)
            {
                return sqlgen::Result<sqlgen::Ref<sqlgen::sqlite::Connection>>{ sqlgen::error(res.error().message) };
            }
            return sqlgen::Result<sqlgen::Ref<sqlgen::sqlite::Connection>>{ *conn_ };
        }

        template<typename DTO>
            requires DAL::Schema::RelationalEntity<DTO>
        [[nodiscard]] auto upsert_many(const std::vector<DTO>& items)
        {
            if (!is_open())
            {
                return sqlgen::Result<sqlgen::Ref<sqlgen::sqlite::Connection>>{ sqlgen::error("Database connection is closed") };
            }
            auto res = execute_insert_batch<DTO>(std::span<const DTO>{ items }, true);
            if (!res)
            {
                return sqlgen::Result<sqlgen::Ref<sqlgen::sqlite::Connection>>{ sqlgen::error(res.error().message) };
            }
            return sqlgen::Result<sqlgen::Ref<sqlgen::sqlite::Connection>>{ *conn_ };
        }

        template<typename DTO, typename Condition>
        [[nodiscard]] std::expected<void, Core::DbError> delete_where(const Condition& cond)
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is closed") };
            }
            auto query = sqlgen::delete_from<DTO> | sqlgen::where(cond);
            auto safe_sql = encode_sqlgen_embedded_nuls(sqlgen::sqlite::to_sql(query));
            if (!safe_sql)
            {
                return std::unexpected<Core::DbError>{ safe_sql.error() };
            }
            return execute_prepared(*safe_sql);
        }

        template<typename DTO, typename SetExpr, typename Condition>
        [[nodiscard]] std::expected<void, Core::DbError> update_where(const SetExpr& set_expr, const Condition& cond)
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is closed") };
            }
            auto query = sqlgen::update<DTO>(set_expr) | sqlgen::where(cond);
            auto safe_sql = encode_sqlgen_embedded_nuls(sqlgen::sqlite::to_sql(query));
            if (!safe_sql)
            {
                return std::unexpected<Core::DbError>{ safe_sql.error() };
            }
            return execute_prepared(*safe_sql);
        }

        template<typename DTO, typename Condition, typename... SetExprs>
            requires(sizeof...(SetExprs) >= 2)
        [[nodiscard]] std::expected<void, Core::DbError> update_where_many(const Condition& cond, const SetExprs&... set_exprs)
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is closed") };
            }
            auto query = sqlgen::update<DTO>(set_exprs...) | sqlgen::where(cond);
            auto safe_sql = encode_sqlgen_embedded_nuls(sqlgen::sqlite::to_sql(query));
            if (!safe_sql)
            {
                return std::unexpected<Core::DbError>{ safe_sql.error() };
            }
            return execute_prepared(*safe_sql);
        }

        [[nodiscard]] std::expected<void, Core::DbError> savepoint(std::string_view name)
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is closed") };
            }
            if (!is_valid_sql_identifier(name))
            {
                return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation("Invalid savepoint identifier") };
            }
            auto res = (*conn_)->execute(std::format("SAVEPOINT \"{}\";", name));
            if (!res)
            {
                return std::unexpected<Core::DbError>{ map_sqlite_error(res.error().what()) };
            }
            return {};
        }

        [[nodiscard]] std::expected<void, Core::DbError> release_savepoint(std::string_view name)
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is closed") };
            }
            if (!is_valid_sql_identifier(name))
            {
                return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation("Invalid savepoint identifier") };
            }
            auto res = (*conn_)->execute(std::format("RELEASE SAVEPOINT \"{}\";", name));
            if (!res)
            {
                return std::unexpected<Core::DbError>{ map_sqlite_error(res.error().what()) };
            }
            return {};
        }

        [[nodiscard]] std::expected<void, Core::DbError> rollback_to_savepoint(std::string_view name)
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is closed") };
            }
            if (!is_valid_sql_identifier(name))
            {
                return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation("Invalid savepoint identifier") };
            }
            auto res = (*conn_)->execute(std::format("ROLLBACK TO SAVEPOINT \"{}\"; RELEASE SAVEPOINT \"{}\";", name, name));
            if (!res)
            {
                return std::unexpected<Core::DbError>{ map_sqlite_error(res.error().what()) };
            }
            return {};
        }

        [[nodiscard]] std::expected<void, Core::DbError> set_busy_timeout(std::chrono::milliseconds timeout)
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is closed") };
            }
            auto res = (*conn_)->execute(std::format("PRAGMA busy_timeout = {};", timeout.count()));
            if (!res)
            {
                return std::unexpected<Core::DbError>{ map_sqlite_error(res.error().what()) };
            }
            return {};
        }

        [[nodiscard]] std::expected<void, Core::DbError> enable_wal_mode()
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is closed") };
            }
            auto res = query_rows("PRAGMA journal_mode = WAL;");
            if (!res)
            {
                return std::unexpected<Core::DbError>{ res.error() };
            }
            if (res->rows.empty() || res->rows[0].cells.empty() || !std::holds_alternative<std::string>(res->rows[0].cells[0]))
            {
                return std::unexpected<Core::DbError>{ Core::DbError::unknown(
                    "PRAGMA journal_mode = WAL did not return an effective journal mode") };
            }
            const std::string& effective = std::get<std::string>(res->rows[0].cells[0]);
            std::string lower;
            lower.reserve(effective.size());
            for (const char raw_ch : effective)
            {
                lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(raw_ch))));
            }
            if (lower != "wal")
            {
                return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation(
                    std::format("Failed to activate WAL journal mode (effective mode: '{}')", effective)) };
            }
            return {};
        }

        [[nodiscard]] std::expected<std::int32_t, Core::DbError> try_user_version() const
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is closed") };
            }
            auto res = query_rows("PRAGMA user_version;");
            if (!res)
            {
                return std::unexpected<Core::DbError>{ res.error() };
            }
            if (res->rows.empty() || res->rows[0].cells.empty() || !std::holds_alternative<std::int64_t>(res->rows[0].cells[0]))
            {
                return std::unexpected<Core::DbError>{ Core::DbError::unknown("PRAGMA user_version did not return an integer") };
            }
            const std::int64_t val = std::get<std::int64_t>(res->rows[0].cells[0]);
            if (!std::in_range<std::int32_t>(val))
            {
                return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation("PRAGMA user_version exceeds int32_t range") };
            }
            return static_cast<std::int32_t>(val);
        }

        [[nodiscard]] std::expected<void, Core::DbError> set_user_version(std::int32_t version)
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is closed") };
            }
            if (version < 0)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation("Schema user_version cannot be negative") };
            }
            if (auto res = execute_raw(std::format("PRAGMA user_version = {};", version)); !res)
            {
                return res;
            }
            auto readback = try_user_version();
            if (!readback)
            {
                return std::unexpected<Core::DbError>{ readback.error() };
            }
            if (*readback != version)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation(
                    std::format("Failed to persist PRAGMA user_version = {} (read back {})", version, *readback)) };
            }
            return {};
        }

        [[nodiscard]] std::expected<std::vector<std::string>, Core::DbError> list_tables() const
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is closed") };
            }
            auto res = query_rows("SELECT name FROM sqlite_master WHERE type = 'table' AND name NOT LIKE 'sqlite_%' ORDER BY name;");
            if (!res)
            {
                return std::unexpected<Core::DbError>{ res.error() };
            }
            std::vector<std::string> tables;
            tables.reserve(res->rows.size());
            for (const auto& row : res->rows)
            {
                if (!row.cells.empty() && std::holds_alternative<std::string>(row.cells[0]))
                {
                    tables.push_back(std::get<std::string>(row.cells[0]));
                }
            }
            return tables;
        }

        [[nodiscard]] std::expected<std::vector<TableColumnInfo>, Core::DbError> table_info(std::string_view table_name) const
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is closed") };
            }
            if (table_name.empty() || table_name.find('\0') != std::string_view::npos ||
                table_name.find_first_of("'\";\r\n") != std::string_view::npos)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::unknown("Invalid table name for table_info()") };
            }
            const std::array<DynamicCell, 1> params{ std::string{ table_name } };
            auto res = query_rows(
                "SELECT cid, name, type, \"notnull\", dflt_value, pk FROM pragma_table_info(?);", std::span<const DynamicCell>{ params });
            if (!res)
            {
                return std::unexpected<Core::DbError>{ res.error() };
            }
            std::vector<TableColumnInfo> cols;
            cols.reserve(res->rows.size());
            for (const auto& row : res->rows)
            {
                if (row.cells.size() < 6)
                {
                    continue;
                }
                TableColumnInfo info;
                if (const auto* v = std::get_if<std::int64_t>(&row.cells[0]))
                {
                    info.cid = *v;
                }
                if (const auto* v = std::get_if<std::string>(&row.cells[1]))
                {
                    info.name = *v;
                }
                if (const auto* v = std::get_if<std::string>(&row.cells[2]))
                {
                    info.type = *v;
                }
                if (const auto* v = std::get_if<std::int64_t>(&row.cells[3]))
                {
                    info.not_null = (*v != 0);
                }
                if (const auto* v = std::get_if<std::string>(&row.cells[4]))
                {
                    info.default_value = *v;
                }
                if (const auto* v = std::get_if<std::int64_t>(&row.cells[5]))
                {
                    info.pk_index = *v;
                }
                cols.push_back(std::move(info));
            }
            return cols;
        }

        [[nodiscard]] std::expected<void, Core::DbError> execute_raw(std::string_view sql)
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is closed") };
            }
            if (sql.empty())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::unknown("SQL statement cannot be empty") };
            }
            if (sql.find('\0') != std::string_view::npos)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation("SQL statement contains embedded NUL") };
            }
            auto res = (*conn_)->execute(std::string{ sql });
            if (!res)
            {
                return std::unexpected<Core::DbError>{ map_sqlite_error(res.error().what()) };
            }
            return {};
        }

        [[nodiscard]] std::expected<DynamicQueryResult, Core::DbError> query_rows(
            std::string_view sql, std::span<const DynamicCell> params = {}) const
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is closed") };
            }
            if (sql.empty())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::unknown("SQL statement cannot be empty") };
            }
            if (sql.find('\0') != std::string_view::npos)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation("SQL statement contains embedded NUL") };
            }
            if (sql.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
            {
                return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation("SQL statement exceeds length limit") };
            }
            sqlite3* db = native_sqlite_handle();
            if (db == nullptr)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Native SQLite handle is unavailable") };
            }

            sqlite3_stmt* raw_stmt = nullptr;
            const char* pz_tail = nullptr;
            const int prep_rc = sqlite3_prepare_v2(db, sql.data(), static_cast<int>(sql.size()), &raw_stmt, &pz_tail);
            if (prep_rc != SQLITE_OK)
            {
                return std::unexpected<Core::DbError>{ map_sqlite_error(sqlite3_errmsg(db)) };
            }
            auto stmt_guard = std::unique_ptr<sqlite3_stmt, decltype([](sqlite3_stmt* s) {
                if (s != nullptr)
                {
                    sqlite3_finalize(s);
                }
            })>(raw_stmt);

            if (raw_stmt == nullptr)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::unknown("SQL statement contains no executable command") };
            }
            const char* const sql_end = sql.data() + sql.size();
            const std::string_view tail = (pz_tail != nullptr && pz_tail <= sql_end)
                ? std::string_view(pz_tail, static_cast<std::size_t>(sql_end - pz_tail))
                : std::string_view{};
            if (has_executable_sql_tail(tail))
            {
                return std::unexpected<Core::DbError>{ Core::DbError::unknown(
                    "Multi-statement SQL is not supported in prepared execution") };
            }

            if (auto bind_res = bind_dynamic_params(db, raw_stmt, params); !bind_res)
            {
                return std::unexpected<Core::DbError>{ bind_res.error() };
            }

            DynamicQueryResult result;
            const int col_count = sqlite3_column_count(raw_stmt);
            result.columns.reserve(static_cast<std::size_t>(col_count));
            for (int c = 0; c < col_count; ++c)
            {
                const char* col_name = sqlite3_column_name(raw_stmt, c);
                result.columns.emplace_back(col_name != nullptr ? col_name : "");
            }

            while (true)
            {
                const int step_rc = sqlite3_step(raw_stmt);
                if (step_rc == SQLITE_DONE)
                {
                    break;
                }
                if (step_rc != SQLITE_ROW)
                {
                    return std::unexpected<Core::DbError>{ map_sqlite_error(sqlite3_errmsg(db)) };
                }

                DynamicRow row;
                row.cells.reserve(static_cast<std::size_t>(col_count));
                for (int c = 0; c < col_count; ++c)
                {
                    switch (sqlite3_column_type(raw_stmt, c))
                    {
                    case SQLITE_NULL:
                        row.cells.emplace_back(std::monostate{});
                        break;
                    case SQLITE_INTEGER:
                        row.cells.emplace_back(static_cast<std::int64_t>(sqlite3_column_int64(raw_stmt, c)));
                        break;
                    case SQLITE_FLOAT:
                        row.cells.emplace_back(sqlite3_column_double(raw_stmt, c));
                        break;
                    case SQLITE_TEXT:
                        {
                            const auto* txt = reinterpret_cast<const char*>(sqlite3_column_text(raw_stmt, c));
                            const int len = sqlite3_column_bytes(raw_stmt, c);
                            row.cells.emplace_back(txt != nullptr ? std::string(txt, static_cast<std::size_t>(len)) : std::string{});
                            break;
                        }
                    case SQLITE_BLOB:
                        {
                            const auto* blob = static_cast<const std::byte*>(sqlite3_column_blob(raw_stmt, c));
                            const int len = sqlite3_column_bytes(raw_stmt, c);
                            if (blob != nullptr && len > 0)
                            {
                                row.cells.emplace_back(std::vector<std::byte>(blob, blob + len));
                            }
                            else
                            {
                                row.cells.emplace_back(std::vector<std::byte>{});
                            }
                            break;
                        }
                    default:
                        row.cells.emplace_back(std::monostate{});
                        break;
                    }
                }
                result.rows.push_back(std::move(row));
            }
            return result;
        }

        [[nodiscard]] std::expected<void, Core::DbError> execute_prepared(std::string_view sql, std::span<const DynamicCell> params = {})
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is closed") };
            }
            if (sql.empty())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::unknown("SQL statement cannot be empty") };
            }
            if (sql.find('\0') != std::string_view::npos)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation("SQL statement contains embedded NUL") };
            }
            if (sql.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
            {
                return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation("SQL statement exceeds length limit") };
            }
            sqlite3* db = native_sqlite_handle();
            if (db == nullptr)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Native SQLite handle is unavailable") };
            }

            sqlite3_stmt* raw_stmt = nullptr;
            const char* pz_tail = nullptr;
            const int prep_rc = sqlite3_prepare_v2(db, sql.data(), static_cast<int>(sql.size()), &raw_stmt, &pz_tail);
            if (prep_rc != SQLITE_OK)
            {
                return std::unexpected<Core::DbError>{ map_sqlite_error(sqlite3_errmsg(db)) };
            }
            auto stmt_guard = std::unique_ptr<sqlite3_stmt, decltype([](sqlite3_stmt* s) {
                if (s != nullptr)
                {
                    sqlite3_finalize(s);
                }
            })>(raw_stmt);

            if (raw_stmt == nullptr)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::unknown("SQL statement contains no executable command") };
            }
            const char* const sql_end = sql.data() + sql.size();
            const std::string_view tail = (pz_tail != nullptr && pz_tail <= sql_end)
                ? std::string_view(pz_tail, static_cast<std::size_t>(sql_end - pz_tail))
                : std::string_view{};
            if (has_executable_sql_tail(tail))
            {
                return std::unexpected<Core::DbError>{ Core::DbError::unknown(
                    "Multi-statement SQL is not supported in prepared execution") };
            }

            if (auto bind_res = bind_dynamic_params(db, raw_stmt, params); !bind_res)
            {
                return std::unexpected<Core::DbError>{ bind_res.error() };
            }

            int step_rc = SQLITE_OK;
            while ((step_rc = sqlite3_step(raw_stmt)) == SQLITE_ROW)
            {
            }
            if (step_rc != SQLITE_DONE)
            {
                return std::unexpected<Core::DbError>{ map_sqlite_error(sqlite3_errmsg(db)) };
            }
            return {};
        }

        [[nodiscard]] Core::DbError map_error(std::string_view msg) const
        {
            return map_sqlite_error(msg);
        }

    private:
        [[nodiscard]] static constexpr bool is_valid_sql_identifier(std::string_view name) noexcept
        {
            if (name.empty() || name.size() > 128)
            {
                return false;
            }
            const unsigned char first = static_cast<unsigned char>(name.front());
            const bool first_ok = (first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z') || first == '_';
            if (!first_ok)
            {
                return false;
            }
            for (const char raw_ch : name)
            {
                const unsigned char ch = static_cast<unsigned char>(raw_ch);
                const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_';
                if (!ok)
                {
                    return false;
                }
            }
            return true;
        }

        [[nodiscard]] static constexpr bool has_executable_sql_tail(std::string_view tail) noexcept
        {
            std::size_t i = 0;
            while (i < tail.size())
            {
                const char ch = tail[i];
                if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '\f' || ch == '\v')
                {
                    ++i;
                    continue;
                }
                if (ch == '-' && (i + 1) < tail.size() && tail[i + 1] == '-')
                {
                    i += 2;
                    while (i < tail.size() && tail[i] != '\n')
                    {
                        ++i;
                    }
                    continue;
                }
                if (ch == '/' && (i + 1) < tail.size() && tail[i + 1] == '*')
                {
                    i += 2;
                    bool closed = false;
                    while ((i + 1) < tail.size())
                    {
                        if (tail[i] == '*' && tail[i + 1] == '/')
                        {
                            i += 2;
                            closed = true;
                            break;
                        }
                        ++i;
                    }
                    if (!closed)
                    {
                        return true;
                    }
                    continue;
                }
                return true;
            }
            return false;
        }

        [[nodiscard]] std::expected<void, Core::DbError> bind_dynamic_params(
            sqlite3* db, sqlite3_stmt* stmt, std::span<const DynamicCell> params) const
        {
            const int expected_params = sqlite3_bind_parameter_count(stmt);
            if (expected_params < 0 || params.size() != static_cast<std::size_t>(expected_params))
            {
                return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation(
                    std::format("Prepared statement expects {} parameters, but {} were provided", expected_params, params.size())) };
            }

            for (std::size_t i = 0; i < params.size(); ++i)
            {
                const int idx = static_cast<int>(i + 1);
                int rc = SQLITE_OK;
                std::expected<void, Core::DbError> size_err{};
                std::visit(
                    [&](const auto& val)
                    {
                        using T = std::decay_t<decltype(val)>;
                        if constexpr (std::is_same_v<T, std::monostate>)
                        {
                            rc = sqlite3_bind_null(stmt, idx);
                        }
                        else if constexpr (std::is_same_v<T, std::int64_t>)
                        {
                            rc = sqlite3_bind_int64(stmt, idx, val);
                        }
                        else if constexpr (std::is_same_v<T, double>)
                        {
                            rc = sqlite3_bind_double(stmt, idx, val);
                        }
                        else if constexpr (std::is_same_v<T, std::string>)
                        {
                            if (val.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
                            {
                                size_err = std::unexpected<Core::DbError>{ Core::DbError::constraint_violation(
                                    "Bound string parameter exceeds SQLite length limit") };
                                return;
                            }
                            rc =
                                sqlite3_bind_text(stmt, idx, val.empty() ? "" : val.data(), static_cast<int>(val.size()), SQLITE_TRANSIENT);
                        }
                        else if constexpr (std::is_same_v<T, std::vector<std::byte>>)
                        {
                            if (val.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
                            {
                                size_err = std::unexpected<Core::DbError>{ Core::DbError::constraint_violation(
                                    "Bound BLOB parameter exceeds SQLite length limit") };
                                return;
                            }
                            if (val.empty())
                            {
                                rc = sqlite3_bind_zeroblob(stmt, idx, 0);
                            }
                            else
                            {
                                rc = sqlite3_bind_blob(stmt, idx, val.data(), static_cast<int>(val.size()), SQLITE_TRANSIENT);
                            }
                        }
                    },
                    params[i]);
                if (!size_err)
                {
                    return size_err;
                }
                if (rc != SQLITE_OK)
                {
                    return std::unexpected<Core::DbError>{ map_sqlite_error(sqlite3_errmsg(db)) };
                }
            }
            return {};
        }

        template<typename F>
        static constexpr bool is_sqlgen_unique_v = false;
        template<typename T>
        static constexpr bool is_sqlgen_unique_v<sqlgen::Unique<T>> = true;

        [[nodiscard]] static std::expected<void, Core::DbError> validate_ddl_identifier(std::string_view name)
        {
            if (name.empty())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation("SQL identifier cannot be empty") };
            }
            if (name.find('\0') != std::string_view::npos)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation("SQL identifier contains embedded NUL") };
            }
            if (name.find('"') != std::string_view::npos)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation("SQL identifier cannot contain double quotes") };
            }
            return {};
        }

        template<typename DTO>
        [[nodiscard]] static std::expected<void, Core::DbError> validate_dto_ddl_identifiers()
        {
            if (auto res = validate_ddl_identifier(std::string_view{ DTO::tablename }); !res)
            {
                return res;
            }
            std::expected<void, Core::DbError> status{};
            template for (constexpr auto m : DAL::Schema::members_v<DTO>)
            {
                if (status)
                {
                    status = validate_ddl_identifier(std::meta::identifier_of(m));
                }
                using RawFieldT = [:std::meta::type_of(m):];
                using FieldT = std::remove_cvref_t<RawFieldT>;
                if constexpr (DAL::Schema::SqlgenForeignKeyField<FieldT>)
                {
                    if (status)
                    {
                        status = validate_ddl_identifier(std::string_view{ FieldT::ForeignTableType::tablename });
                    }
                }
            }
            return status;
        }

        template<typename DTO>
        [[nodiscard]] static std::string build_select_columns_sql()
        {
            std::string cols;
            std::size_t idx = 0;
            template for (constexpr auto m : DAL::Schema::members_v<DTO>)
            {
                if (idx > 0)
                {
                    cols += ", ";
                }
                cols += std::format("\"{}\"", std::meta::identifier_of(m));
                ++idx;
            }
            return cols;
        }

        template<typename DTO>
        [[nodiscard]] static std::string build_key_where_sql()
        {
            static constexpr auto km = DAL::Schema::key_members_of<DTO>();
            std::string clause;
            std::size_t idx = 0;
            template for (constexpr auto m : km)
            {
                if (idx > 0)
                {
                    clause += " AND ";
                }
                clause += std::format("\"{}\" = ?", std::meta::identifier_of(m));
                ++idx;
            }
            return clause;
        }

        // Rewrites embedded NUL bytes inside single-quoted string literals emitted by sqlgen::sqlite::to_sql
        // into SQLite `'...' || char(0) || '...'` expressions while rejecting NUL bytes outside string literals.
        [[nodiscard]] static std::expected<std::string, Core::DbError> encode_sqlgen_embedded_nuls(std::string_view sql)
        {
            if (sql.find('\0') == std::string_view::npos)
            {
                return std::string{ sql };
            }
            std::string out;
            out.reserve(sql.size() + 32);
            bool in_string = false;
            for (std::size_t i = 0; i < sql.size(); ++i)
            {
                const char ch = sql[i];
                if (!in_string)
                {
                    if (ch == '\0')
                    {
                        return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation(
                            "SQL statement contains embedded NUL outside string literal") };
                    }
                    if (ch == '\'')
                    {
                        in_string = true;
                    }
                    out.push_back(ch);
                }
                else
                {
                    if (ch == '\0')
                    {
                        out += "' || char(0) || '";
                    }
                    else if (ch == '\'')
                    {
                        out.push_back('\'');
                        if ((i + 1) < sql.size() && sql[i + 1] == '\'')
                        {
                            out.push_back('\'');
                            ++i;
                        }
                        else
                        {
                            in_string = false;
                        }
                    }
                    else
                    {
                        out.push_back(ch);
                    }
                }
            }
            return out;
        }

        template<typename FieldT>
        [[nodiscard]] std::expected<void, Core::DbError> bind_typed_field(
            sqlite3* db, sqlite3_stmt* stmt, int idx, const FieldT& field, bool treat_zero_auto_incr_as_null = false) const
        {
            using CleanT = std::remove_cvref_t<FieldT>;
            if constexpr (Schema::SqlgenPrimaryKeyField<CleanT>)
            {
                if (treat_zero_auto_incr_as_null)
                {
                    if constexpr (CleanT::auto_incr)
                    {
                        using InnerT = std::remove_cvref_t<decltype(field.value())>;
                        if constexpr (std::is_integral_v<InnerT> && !std::is_same_v<InnerT, bool>)
                        {
                            if (field.value() == 0)
                            {
                                if (sqlite3_bind_null(stmt, idx) != SQLITE_OK)
                                {
                                    return std::unexpected<Core::DbError>{ map_sqlite_error(sqlite3_errmsg(db)) };
                                }
                                return {};
                            }
                        }
                    }
                }
                return bind_typed_field(db, stmt, idx, field.value(), false);
            }
            else if constexpr (Schema::SqlgenForeignKeyField<CleanT> || is_sqlgen_unique_v<CleanT>)
            {
                return bind_typed_field(db, stmt, idx, field.value(), false);
            }
            else if constexpr (Schema::detail::is_optional_v<CleanT>)
            {
                if (!field.has_value())
                {
                    if (sqlite3_bind_null(stmt, idx) != SQLITE_OK)
                    {
                        return std::unexpected<Core::DbError>{ map_sqlite_error(sqlite3_errmsg(db)) };
                    }
                    return {};
                }
                return bind_typed_field(db, stmt, idx, *field, false);
            }
            else if constexpr (Core::StrongIdType<CleanT> || Core::SingleKeyType<CleanT>)
            {
                return bind_typed_field(db, stmt, idx, field.get(), false);
            }
            else if constexpr (std::is_same_v<CleanT, bool>)
            {
                if (sqlite3_bind_int64(stmt, idx, field ? 1 : 0) != SQLITE_OK)
                {
                    return std::unexpected<Core::DbError>{ map_sqlite_error(sqlite3_errmsg(db)) };
                }
                return {};
            }
            else if constexpr (std::is_integral_v<CleanT>)
            {
                if constexpr (std::is_signed_v<CleanT> || sizeof(CleanT) < sizeof(std::int64_t))
                {
                    if (sqlite3_bind_int64(stmt, idx, static_cast<sqlite3_int64>(field)) != SQLITE_OK)
                    {
                        return std::unexpected<Core::DbError>{ map_sqlite_error(sqlite3_errmsg(db)) };
                    }
                    return {};
                }
                else
                {
                    if (field <= static_cast<CleanT>(std::numeric_limits<std::int64_t>::max()))
                    {
                        if (sqlite3_bind_int64(stmt, idx, static_cast<sqlite3_int64>(field)) != SQLITE_OK)
                        {
                            return std::unexpected<Core::DbError>{ map_sqlite_error(sqlite3_errmsg(db)) };
                        }
                        return {};
                    }
                    const std::string rendered = std::to_string(field);
                    if (sqlite3_bind_text(stmt, idx, rendered.data(), static_cast<int>(rendered.size()), SQLITE_TRANSIENT) != SQLITE_OK)
                    {
                        return std::unexpected<Core::DbError>{ map_sqlite_error(sqlite3_errmsg(db)) };
                    }
                    return {};
                }
            }
            else if constexpr (std::is_floating_point_v<CleanT>)
            {
                if (sqlite3_bind_double(stmt, idx, static_cast<double>(field)) != SQLITE_OK)
                {
                    return std::unexpected<Core::DbError>{ map_sqlite_error(sqlite3_errmsg(db)) };
                }
                return {};
            }
            else if constexpr (std::is_same_v<CleanT, std::string> || std::is_same_v<CleanT, std::string_view>)
            {
                if (field.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
                {
                    return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation(
                        "Bound string parameter exceeds SQLite length limit") };
                }
                if (sqlite3_bind_text(stmt, idx, field.empty() ? "" : field.data(), static_cast<int>(field.size()), SQLITE_TRANSIENT) !=
                    SQLITE_OK)
                {
                    return std::unexpected<Core::DbError>{ map_sqlite_error(sqlite3_errmsg(db)) };
                }
                return {};
            }
            else if constexpr (std::is_same_v<CleanT, const char*> || std::is_same_v<CleanT, char*>)
            {
                if (field == nullptr)
                {
                    if (sqlite3_bind_null(stmt, idx) != SQLITE_OK)
                    {
                        return std::unexpected<Core::DbError>{ map_sqlite_error(sqlite3_errmsg(db)) };
                    }
                    return {};
                }
                const std::string_view sv{ field };
                return bind_typed_field(db, stmt, idx, sv, false);
            }
            else
            {
                const std::optional<std::string> encoded = sqlgen::parsing::Parser<CleanT>::write(field);
                if (!encoded.has_value())
                {
                    if (sqlite3_bind_null(stmt, idx) != SQLITE_OK)
                    {
                        return std::unexpected<Core::DbError>{ map_sqlite_error(sqlite3_errmsg(db)) };
                    }
                    return {};
                }
                if (encoded->size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
                {
                    return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation(
                        "Bound encoded parameter exceeds SQLite length limit") };
                }
                if (sqlite3_bind_text(
                        stmt, idx, encoded->empty() ? "" : encoded->data(), static_cast<int>(encoded->size()), SQLITE_TRANSIENT) !=
                    SQLITE_OK)
                {
                    return std::unexpected<Core::DbError>{ map_sqlite_error(sqlite3_errmsg(db)) };
                }
                return {};
            }
        }

        template<typename... Parts>
        [[nodiscard]] std::expected<void, Core::DbError> bind_variadic_fields(
            sqlite3* db, sqlite3_stmt* stmt, int start_idx, const Parts&... parts) const
        {
            std::expected<void, Core::DbError> res{};
            int idx = start_idx;
            (
                [&]()
                {
                    if (res)
                    {
                        res = bind_typed_field(db, stmt, idx++, parts, false);
                    }
                }(),
                ...);
            return res;
        }

        template<typename BindFn>
        [[nodiscard]] std::expected<void, Core::DbError> execute_dml_bound(std::string_view sql, BindFn&& bind_fn) const
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is closed") };
            }
            if (sql.empty())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::unknown("SQL statement cannot be empty") };
            }
            if (sql.find('\0') != std::string_view::npos)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation("SQL statement contains embedded NUL") };
            }
            if (sql.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
            {
                return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation("SQL statement exceeds length limit") };
            }
            sqlite3* db = native_sqlite_handle();
            if (db == nullptr)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Native SQLite handle is unavailable") };
            }

            sqlite3_stmt* raw_stmt = nullptr;
            const char* pz_tail = nullptr;
            if (sqlite3_prepare_v2(db, sql.data(), static_cast<int>(sql.size()), &raw_stmt, &pz_tail) != SQLITE_OK)
            {
                return std::unexpected<Core::DbError>{ map_sqlite_error(sqlite3_errmsg(db)) };
            }
            auto stmt_guard = std::unique_ptr<sqlite3_stmt, decltype([](sqlite3_stmt* s) {
                if (s != nullptr)
                {
                    sqlite3_finalize(s);
                }
            })>(raw_stmt);

            if (raw_stmt == nullptr)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::unknown("SQL statement contains no executable command") };
            }
            const char* const sql_end = sql.data() + sql.size();
            const std::string_view tail = (pz_tail != nullptr && pz_tail <= sql_end)
                ? std::string_view(pz_tail, static_cast<std::size_t>(sql_end - pz_tail))
                : std::string_view{};
            if (has_executable_sql_tail(tail))
            {
                return std::unexpected<Core::DbError>{ Core::DbError::unknown(
                    "Multi-statement SQL is not supported in prepared execution") };
            }

            if (auto bind_res = std::forward<BindFn>(bind_fn)(db, raw_stmt); !bind_res)
            {
                return bind_res;
            }

            int step_rc = SQLITE_OK;
            while ((step_rc = sqlite3_step(raw_stmt)) == SQLITE_ROW)
            {
            }
            if (step_rc != SQLITE_DONE)
            {
                return std::unexpected<Core::DbError>{ map_sqlite_error(sqlite3_errmsg(db)) };
            }
            return {};
        }

        template<typename DTO>
        [[nodiscard]] std::expected<void, Core::DbError> execute_insert_batch(std::span<const DTO> items, bool on_conflict_update) const
        {
            if (items.empty())
            {
                return {};
            }
            if (auto id_chk = validate_dto_ddl_identifiers<DTO>(); !id_chk)
            {
                return id_chk;
            }

            sqlite3* db = native_sqlite_handle();
            if (db == nullptr)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Native SQLite handle is unavailable") };
            }

            std::string cols;
            std::string placeholders;
            std::size_t col_idx = 0;
            template for (constexpr auto m : DAL::Schema::members_v<DTO>)
            {
                if (col_idx > 0)
                {
                    cols += ", ";
                    placeholders += ", ";
                }
                cols += std::format("\"{}\"", std::meta::identifier_of(m));
                placeholders += "?";
                ++col_idx;
            }

            std::string sql = std::format("INSERT INTO \"{}\" ({}) VALUES ({})", std::string_view{ DTO::tablename }, cols, placeholders);
            if (on_conflict_update)
            {
                static constexpr auto km = DAL::Schema::key_members_of<DTO>();
                static constexpr auto nk_members = DAL::Schema::non_key_members_of<DTO>();
                std::string conflict_cols;
                std::size_t k_idx = 0;
                template for (constexpr auto m : km)
                {
                    if (k_idx > 0)
                    {
                        conflict_cols += ", ";
                    }
                    conflict_cols += std::format("\"{}\"", std::meta::identifier_of(m));
                    ++k_idx;
                }
                sql += std::format(" ON CONFLICT ({})", conflict_cols);
                if constexpr (nk_members.empty())
                {
                    sql += " DO NOTHING";
                }
                else
                {
                    sql += " DO UPDATE SET ";
                    std::size_t nk_idx = 0;
                    template for (constexpr auto m : nk_members)
                    {
                        if (nk_idx > 0)
                        {
                            sql += ", ";
                        }
                        const std::string_view name = std::meta::identifier_of(m);
                        sql += std::format("\"{}\" = excluded.\"{}\"", name, name);
                        ++nk_idx;
                    }
                }
            }
            sql += ";";

            sqlite3_stmt* raw_stmt = nullptr;
            const char* pz_tail = nullptr;
            if (sqlite3_prepare_v2(db, sql.data(), static_cast<int>(sql.size()), &raw_stmt, &pz_tail) != SQLITE_OK)
            {
                return std::unexpected<Core::DbError>{ map_sqlite_error(sqlite3_errmsg(db)) };
            }
            auto stmt_guard = std::unique_ptr<sqlite3_stmt, decltype([](sqlite3_stmt* s) {
                if (s != nullptr)
                {
                    sqlite3_finalize(s);
                }
            })>(raw_stmt);

            if (raw_stmt == nullptr)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::unknown("SQL insert statement could not be prepared") };
            }

            for (const DTO& item : items)
            {
                int bind_idx = 1;
                std::expected<void, Core::DbError> bind_res{};
                template for (constexpr auto m : DAL::Schema::members_v<DTO>)
                {
                    if (bind_res)
                    {
                        bind_res = bind_typed_field(db, raw_stmt, bind_idx++, item.[:m:], true);
                    }
                }
                if (!bind_res)
                {
                    return bind_res;
                }

                const int step_rc = sqlite3_step(raw_stmt);
                if (step_rc != SQLITE_DONE)
                {
                    return std::unexpected<Core::DbError>{ map_sqlite_error(sqlite3_errmsg(db)) };
                }
                if (sqlite3_reset(raw_stmt) != SQLITE_OK || sqlite3_clear_bindings(raw_stmt) != SQLITE_OK)
                {
                    return std::unexpected<Core::DbError>{ map_sqlite_error(sqlite3_errmsg(db)) };
                }
            }
            return {};
        }

        template<typename BindFn>
        [[nodiscard]] std::expected<std::vector<std::vector<std::optional<std::string>>>, Core::DbError> execute_raw_text_rows_bound(
            std::string_view sql, BindFn&& bind_fn) const
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is closed") };
            }
            if (sql.empty())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::unknown("SQL statement cannot be empty") };
            }
            if (sql.find('\0') != std::string_view::npos)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation("SQL statement contains embedded NUL") };
            }
            if (sql.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
            {
                return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation("SQL statement exceeds length limit") };
            }

            sqlite3* db = native_sqlite_handle();
            if (db == nullptr)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Native SQLite handle is unavailable") };
            }

            sqlite3_stmt* raw_stmt = nullptr;
            const char* pz_tail = nullptr;
            const int prep_rc = sqlite3_prepare_v2(db, sql.data(), static_cast<int>(sql.size()), &raw_stmt, &pz_tail);
            if (prep_rc != SQLITE_OK)
            {
                return std::unexpected<Core::DbError>{ map_sqlite_error(sqlite3_errmsg(db)) };
            }
            auto stmt_guard = std::unique_ptr<sqlite3_stmt, decltype([](sqlite3_stmt* s) {
                if (s != nullptr)
                {
                    sqlite3_finalize(s);
                }
            })>(raw_stmt);

            if (raw_stmt == nullptr)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::unknown("SQL statement contains no executable command") };
            }
            const char* const sql_end = sql.data() + sql.size();
            const std::string_view tail = (pz_tail != nullptr && pz_tail <= sql_end)
                ? std::string_view(pz_tail, static_cast<std::size_t>(sql_end - pz_tail))
                : std::string_view{};
            if (has_executable_sql_tail(tail))
            {
                return std::unexpected<Core::DbError>{ Core::DbError::unknown(
                    "Multi-statement SQL is not supported in prepared execution") };
            }

            if (auto bind_res = std::forward<BindFn>(bind_fn)(db, raw_stmt); !bind_res)
            {
                return std::unexpected<Core::DbError>{ bind_res.error() };
            }

            const int col_count = sqlite3_column_count(raw_stmt);
            std::vector<std::vector<std::optional<std::string>>> rows;

            while (true)
            {
                const int step_rc = sqlite3_step(raw_stmt);
                if (step_rc == SQLITE_DONE)
                {
                    break;
                }
                if (step_rc != SQLITE_ROW)
                {
                    return std::unexpected<Core::DbError>{ map_sqlite_error(sqlite3_errmsg(db)) };
                }

                std::vector<std::optional<std::string>> row;
                row.reserve(static_cast<std::size_t>(col_count));
                for (int c = 0; c < col_count; ++c)
                {
                    if (sqlite3_column_type(raw_stmt, c) == SQLITE_NULL)
                    {
                        row.emplace_back(std::nullopt);
                        continue;
                    }
                    const auto* txt = reinterpret_cast<const char*>(sqlite3_column_text(raw_stmt, c));
                    const int len = sqlite3_column_bytes(raw_stmt, c);
                    if (txt == nullptr)
                    {
                        if (sqlite3_errcode(db) == SQLITE_NOMEM)
                        {
                            return std::unexpected<Core::DbError>{ map_sqlite_error(sqlite3_errmsg(db)) };
                        }
                        row.emplace_back(std::in_place);
                    }
                    else
                    {
                        row.emplace_back(std::in_place, txt, static_cast<std::size_t>(len));
                    }
                }
                rows.push_back(std::move(row));
            }
            return rows;
        }

        [[nodiscard]] std::expected<std::vector<std::vector<std::optional<std::string>>>, Core::DbError> execute_raw_text_rows(
            std::string_view sql) const
        {
            return execute_raw_text_rows_bound(
                sql,
                [](sqlite3*, sqlite3_stmt*) -> std::expected<void, Core::DbError>
                {
                    return {};
                });
        }

        template<typename RowT, typename BindFn>
        [[nodiscard]] std::expected<std::vector<RowT>, Core::DbError> execute_typed_sql_bound(std::string_view sql, BindFn&& bind_fn) const
        {
            auto raw_rows = execute_raw_text_rows_bound(sql, std::forward<BindFn>(bind_fn));
            if (!raw_rows)
            {
                return std::unexpected<Core::DbError>{ raw_rows.error() };
            }
            std::vector<RowT> result;
            result.reserve(raw_rows->size());
            for (const auto& row : *raw_rows)
            {
                auto parsed = sqlgen::internal::from_str_vec<RowT>(row);
                if (!parsed)
                {
                    return std::unexpected<Core::DbError>{ map_sqlite_error(parsed.error().what()) };
                }
                result.push_back(std::move(*parsed));
            }
            return result;
        }

        template<typename RowT>
        [[nodiscard]] std::expected<std::vector<RowT>, Core::DbError> execute_typed_sql(std::string_view sql) const
        {
            return execute_typed_sql_bound<RowT>(
                sql,
                [](sqlite3*, sqlite3_stmt*) -> std::expected<void, Core::DbError>
                {
                    return {};
                });
        }

        template<typename RowTuple>
        [[nodiscard]] std::expected<std::vector<RowTuple>, Core::DbError> execute_typed_tuple_sql(std::string_view sql) const
        {
            auto raw_rows = execute_raw_text_rows(sql);
            if (!raw_rows)
            {
                return std::unexpected<Core::DbError>{ raw_rows.error() };
            }
            constexpr std::size_t tuple_arity = std::tuple_size_v<RowTuple>;
            std::vector<RowTuple> result;
            result.reserve(raw_rows->size());
            for (const auto& row : *raw_rows)
            {
                if (row.size() != tuple_arity)
                {
                    return std::unexpected<Core::DbError>{ Core::DbError::unknown(
                        std::format("Expected {} columns in tuple projection, got {}", tuple_arity, row.size())) };
                }
                std::optional<Core::DbError> parse_err;
                auto parsed_tuple = [&]<std::size_t... Is>(std::index_sequence<Is...>) -> std::optional<RowTuple>
                {
                    auto parse_one = [&](auto idx_c)
                    {
                        constexpr std::size_t I = decltype(idx_c)::value;
                        using ElemT = std::tuple_element_t<I, RowTuple>;
                        auto parsed = sqlgen::parsing::Parser<ElemT>::read(row[I]);
                        if (!parsed && !parse_err)
                        {
                            parse_err =
                                Core::DbError::unknown(std::format("Failed to parse tuple column {}: {}", I, parsed.error().what()));
                        }
                        return parsed;
                    };
                    auto results = std::make_tuple(parse_one(std::integral_constant<std::size_t, Is>{})...);
                    if (parse_err)
                    {
                        return std::nullopt;
                    }
                    return RowTuple{ std::move(*std::get<Is>(results))... };
                }(std::make_index_sequence<tuple_arity>{});

                if (parse_err)
                {
                    return std::unexpected<Core::DbError>{ std::move(*parse_err) };
                }
                result.push_back(std::move(*parsed_tuple));
            }
            return result;
        }

        static consteval std::meta::info find_sqlite_conn_member()
        {
            for (auto member : std::meta::nonstatic_data_members_of(^^sqlgen::sqlite::Connection, std::meta::access_context::unchecked()))
            {
                if (std::meta::identifier_of(member) == "conn_")
                {
                    return member;
                }
            }
            return {};
        }

        [[nodiscard]] sqlite3* native_sqlite_handle() const noexcept
        {
            if (!conn_)
            {
                return nullptr;
            }
            static constexpr auto conn_member = find_sqlite_conn_member();
            static_assert(conn_member != std::meta::info{});
            return (*conn_)->[:conn_member:].get();
        }

        [[nodiscard]] Core::DbError map_sqlite_error(std::string_view msg) const
        {
            sqlite3* db = native_sqlite_handle();
            if (db != nullptr)
            {
                const int ext_code = sqlite3_extended_errcode(db);
                if (ext_code != SQLITE_OK && ext_code != SQLITE_ROW && ext_code != SQLITE_DONE)
                {
                    Core::DbError err = Core::DbError::unknown(std::string(msg));
                    switch (ext_code)
                    {
                    case SQLITE_CONSTRAINT_UNIQUE:
                    case SQLITE_CONSTRAINT_PRIMARYKEY:
                        err = Core::DbError::duplicate_key(std::string(msg));
                        break;
                    case SQLITE_CONSTRAINT_FOREIGNKEY:
                    case SQLITE_CONSTRAINT_CHECK:
                    case SQLITE_CONSTRAINT_NOTNULL:
                        err = Core::DbError::constraint_violation(std::string(msg));
                        break;
                    case SQLITE_BUSY:
                    case SQLITE_LOCKED:
                    case SQLITE_CANTOPEN:
                        err = Core::DbError::connection_error(std::string(msg));
                        break;
                    default:
                        err = Core::DbError::map_from_message(msg);
                        err.trace = Core::DbError::capture_debug_trace();
                        break;
                    }
                    err.native_code = ext_code;
                    return err;
                }
            }
            auto err = Core::DbError::map_from_message(msg);
            err.trace = Core::DbError::capture_debug_trace();
            return err;
        }

        std::optional<sqlgen::Ref<sqlgen::sqlite::Connection>> conn_;
        bool is_open_{ true };
        bool in_transaction_{ false };
    };

} // namespace DAL
