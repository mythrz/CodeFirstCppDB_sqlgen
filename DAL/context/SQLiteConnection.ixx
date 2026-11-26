module;
#include <meta>
#include <sqlgen.hpp>
#include <sqlgen/sqlite/to_sql.hpp>
#include <sqlite3.h>

export module dal:sqlite_connection;

import std;
import core;
import :schema_traits;
export import :sqlite_statement;

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

    // SQLiteConnection owns a native sqlite3* handle directly via RAII and satisfies
    // the DAL::Repositories::DatabaseConnection / TransactionalConnection / SavepointConnection concepts.
    class SQLiteConnection
    {
    public:
        explicit SQLiteConnection(sqlite3* db) noexcept
            : db_(db)
            , is_open_(db != nullptr)
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
            if (!is_open_ && db_ == nullptr)
            {
                return;
            }
            if (db_ != nullptr)
            {
                if (sqlite3_get_autocommit(db_) == 0)
                {
                    char* errmsg = nullptr;
                    (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, &errmsg);
                    if (errmsg != nullptr)
                    {
                        sqlite3_free(errmsg);
                    }
                }
                sqlite3_close_v2(db_);
                db_ = nullptr;
            }
            is_open_ = false;
        }

        [[nodiscard]] static std::expected<std::shared_ptr<SQLiteConnection>, Core::DbError> connect(const std::string& dbPath)
        {
            return connect(dbPath, SQLiteConnectionOptions{});
        }

        [[nodiscard]] static std::expected<std::shared_ptr<SQLiteConnection>, Core::DbError> connect(
            const std::string& dbPath, const SQLiteConnectionOptions& options)
        {
            if (dbPath.empty() || dbPath.find('\0') != std::string::npos)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Invalid SQLite database path") };
            }

            sqlite3* raw_db = nullptr;
            const int open_rc = sqlite3_open_v2(dbPath.c_str(), &raw_db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
            if (open_rc != SQLITE_OK || raw_db == nullptr)
            {
                const std::string msg = raw_db != nullptr ? sqlite3_errmsg(raw_db) : "Failed to open SQLite database";
                const int ext = raw_db != nullptr ? sqlite3_extended_errcode(raw_db) : open_rc;
                if (raw_db != nullptr)
                {
                    sqlite3_close_v2(raw_db);
                }
                return std::unexpected<Core::DbError>{ Core::DbError::create(Core::DbErrorCode::ConnectionError, msg, ext) };
            }
            (void)sqlite3_extended_result_codes(raw_db, 1);

            auto handle = std::make_shared<SQLiteConnection>(raw_db);
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

        [[nodiscard]] sqlite3* native_handle() const noexcept
        {
            return is_open() ? db_ : nullptr;
        }

        template<typename DTO>
        [[nodiscard]] std::expected<void, Core::DbError> ensure_table_created()
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error(
                    "Cannot create table: database connection is closed") };
            }
            if (auto id_chk = detail::validate_dto_ddl_identifiers<DTO>(); !id_chk)
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

                std::string create_sql = sqlgen::sqlite::to_sql_impl(sqlgen::dynamic::Statement{ stmt });
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
            if (auto idx_chk = detail::validate_ddl_identifier(IndexName.string_view()); !idx_chk)
            {
                return idx_chk;
            }
            if (auto dto_chk = detail::validate_dto_ddl_identifiers<DTO>(); !dto_chk)
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
            if (auto idx_chk = detail::validate_ddl_identifier(IndexName.string_view()); !idx_chk)
            {
                return idx_chk;
            }
            if (auto dto_chk = detail::validate_dto_ddl_identifiers<DTO>(); !dto_chk)
            {
                return dto_chk;
            }
            const std::string raw_sql = sqlgen::sqlite::to_sql(
                sqlgen::CreateIndex<IndexName, DTO, WhereCond, ColTypes...>{
                    .unique_ = unique, .if_not_exists_ = true, .where_ = where_cond });
            auto safe_sql = detail::encode_sqlgen_embedded_nuls(raw_sql);
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
            if (auto dto_chk = detail::validate_dto_ddl_identifiers<DTO>(); !dto_chk)
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
            return exec_cstr("PRAGMA foreign_keys = ON;");
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
            return exec_cstr("BEGIN TRANSACTION;");
        }

        [[nodiscard]] std::expected<void, Core::DbError> begin_immediate_transaction()
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
            return exec_cstr("BEGIN IMMEDIATE;");
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
            return exec_cstr("COMMIT;");
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
            return exec_cstr("ROLLBACK;");
        }

        [[nodiscard]] bool in_transaction() const noexcept
        {
            if (!is_open())
            {
                return false;
            }
            return sqlite3_get_autocommit(db_) == 0;
        }

        [[nodiscard]] bool is_open() const noexcept
        {
            return is_open_ && db_ != nullptr;
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
            if (auto id_chk = detail::validate_dto_ddl_identifiers<DTO>(); !id_chk)
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
                    return detail::bind_typed_field(db, stmt, 1, id, false);
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
            if (auto id_chk = detail::validate_dto_ddl_identifiers<DTO>(); !id_chk)
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
                    return detail::bind_variadic_fields(db, stmt, 1, parts...);
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
            if (!detail::validate_dto_ddl_identifiers<DTO>().has_value() ||
                !detail::validate_ddl_identifier(ColLiteral.string_view()).has_value())
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
                    return detail::bind_typed_field(db, stmt, 1, val, false);
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
            auto safe_sql = detail::encode_sqlgen_embedded_nuls(sqlgen::sqlite::to_sql(std::forward<Query>(query)));
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
            auto safe_sql = detail::encode_sqlgen_embedded_nuls(sqlgen::sqlite::to_sql(std::forward<Query>(query)));
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
        [[nodiscard]] std::expected<void, Core::DbError> try_insert(const DTO& item)
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is closed") };
            }
            const std::span<const DTO> one{ &item, 1 };
            return execute_insert_batch<DTO>(one, false);
        }

        template<typename DTO>
        [[nodiscard]] auto insert(const DTO& item)
        {
            auto res = try_insert(item);
            if (!res)
            {
                return sqlgen::Result<sqlgen::Nothing>{ sqlgen::error(res.error().message) };
            }
            return sqlgen::Result<sqlgen::Nothing>{ sqlgen::Nothing{} };
        }

        template<typename DTO>
        [[nodiscard]] std::expected<void, Core::DbError> try_insert_many(const std::vector<DTO>& items)
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is closed") };
            }
            return execute_insert_batch<DTO>(std::span<const DTO>{ items }, false);
        }

        template<typename DTO>
        [[nodiscard]] auto insert_many(const std::vector<DTO>& items)
        {
            auto res = try_insert_many(items);
            if (!res)
            {
                return sqlgen::Result<sqlgen::Nothing>{ sqlgen::error(res.error().message) };
            }
            return sqlgen::Result<sqlgen::Nothing>{ sqlgen::Nothing{} };
        }

        template<typename DTO>
        [[nodiscard]] std::expected<void, Core::DbError> try_update(const DTO& item)
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is closed") };
            }
            if (auto id_chk = detail::validate_dto_ddl_identifiers<DTO>(); !id_chk)
            {
                return id_chk;
            }

            static constexpr auto km = DAL::Schema::key_members_of<DTO>();
            static constexpr auto nk_members = DAL::Schema::non_key_members_of<DTO>();
            if constexpr (nk_members.empty())
            {
                const std::string sql =
                    std::format("SELECT 1 FROM \"{}\" WHERE {} LIMIT 1;", std::string_view{ DTO::tablename }, build_key_where_sql<DTO>());
                auto exists_res = execute_exists_bound(
                    sql,
                    [&](sqlite3* db, sqlite3_stmt* stmt) -> std::expected<void, Core::DbError>
                    {
                        int idx = 1;
                        std::expected<void, Core::DbError> bind_res{};
                        template for (constexpr auto m : km)
                        {
                            if (bind_res)
                            {
                                bind_res = detail::bind_typed_field(db, stmt, idx++, item.[:m:], false);
                            }
                        }
                        return bind_res;
                    });
                if (!exists_res)
                {
                    return std::unexpected<Core::DbError>{ exists_res.error() };
                }
                if (!*exists_res)
                {
                    return std::unexpected<Core::DbError>{ Core::DbError::not_found("Update failed: item not found") };
                }
                return {};
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
                                bind_res = detail::bind_typed_field(db, stmt, idx++, item.[:m:], false);
                            }
                        }
                        template for (constexpr auto m : km)
                        {
                            if (bind_res)
                            {
                                bind_res = detail::bind_typed_field(db, stmt, idx++, item.[:m:], false);
                            }
                        }
                        return bind_res;
                    });
                if (!upd_res)
                {
                    return upd_res;
                }
                if (sqlite3_changes(db_) == 0)
                {
                    return std::unexpected<Core::DbError>{ Core::DbError::not_found("Update failed: item not found") };
                }
                return {};
            }
        }

        template<typename DTO>
        [[nodiscard]] auto update(const DTO& item)
        {
            auto res = try_update(item);
            if (!res)
            {
                return sqlgen::Result<sqlgen::Nothing>{ sqlgen::error(res.error().message) };
            }
            return sqlgen::Result<sqlgen::Nothing>{ sqlgen::Nothing{} };
        }

        template<typename DTO, typename IdType>
        [[nodiscard]] std::expected<void, Core::DbError> try_delete_by_id(const IdType& id)
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is closed") };
            }
            if (auto id_chk = detail::validate_dto_ddl_identifiers<DTO>(); !id_chk)
            {
                return id_chk;
            }
            constexpr auto pk_m = DAL::Schema::primary_key_member<DTO>();
            const std::string sql =
                std::format("DELETE FROM \"{}\" WHERE \"{}\" = ?;", std::string_view{ DTO::tablename }, std::meta::identifier_of(pk_m));
            return execute_dml_bound(
                sql,
                [&](sqlite3* db, sqlite3_stmt* stmt) -> std::expected<void, Core::DbError>
                {
                    return detail::bind_typed_field(db, stmt, 1, id, false);
                });
        }

        template<typename DTO, typename IdType>
        [[nodiscard]] auto delete_by_id(const IdType& id)
        {
            auto del_res = try_delete_by_id<DTO>(id);
            if (!del_res)
            {
                return sqlgen::Result<sqlgen::Nothing>{ sqlgen::error(del_res.error().message) };
            }
            return sqlgen::Result<sqlgen::Nothing>{ sqlgen::Nothing{} };
        }

        template<typename DTO, typename... Parts>
        [[nodiscard]] std::expected<void, Core::DbError> try_delete_by_composite_id(const Parts&... parts)
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is closed") };
            }
            if (auto id_chk = detail::validate_dto_ddl_identifiers<DTO>(); !id_chk)
            {
                return id_chk;
            }
            const std::string sql =
                std::format("DELETE FROM \"{}\" WHERE {};", std::string_view{ DTO::tablename }, build_key_where_sql<DTO>());
            return execute_dml_bound(
                sql,
                [&](sqlite3* db, sqlite3_stmt* stmt) -> std::expected<void, Core::DbError>
                {
                    return detail::bind_variadic_fields(db, stmt, 1, parts...);
                });
        }

        template<typename DTO, typename... Parts>
        [[nodiscard]] auto delete_by_composite_id(const Parts&... parts)
        {
            auto del_res = try_delete_by_composite_id<DTO>(parts...);
            if (!del_res)
            {
                return sqlgen::Result<sqlgen::Nothing>{ sqlgen::error(del_res.error().message) };
            }
            return sqlgen::Result<sqlgen::Nothing>{ sqlgen::Nothing{} };
        }

        template<typename DTO, typename IdType>
        [[nodiscard]] bool exists(const IdType& id) const
        {
            if (!is_open() || !detail::validate_dto_ddl_identifiers<DTO>().has_value())
            {
                return false;
            }
            constexpr auto pk_m = DAL::Schema::primary_key_member<DTO>();
            const std::string sql = std::format(
                "SELECT 1 FROM \"{}\" WHERE \"{}\" = ? LIMIT 1;", std::string_view{ DTO::tablename }, std::meta::identifier_of(pk_m));
            auto res = execute_exists_bound(
                sql,
                [&](sqlite3* db, sqlite3_stmt* stmt) -> std::expected<void, Core::DbError>
                {
                    return detail::bind_typed_field(db, stmt, 1, id, false);
                });
            return res.has_value() && *res;
        }

        template<typename DTO, typename... Parts>
        [[nodiscard]] bool exists_by_composite_id(const Parts&... parts) const
        {
            if (!is_open() || !detail::validate_dto_ddl_identifiers<DTO>().has_value())
            {
                return false;
            }
            const std::string sql =
                std::format("SELECT 1 FROM \"{}\" WHERE {} LIMIT 1;", std::string_view{ DTO::tablename }, build_key_where_sql<DTO>());
            auto res = execute_exists_bound(
                sql,
                [&](sqlite3* db, sqlite3_stmt* stmt) -> std::expected<void, Core::DbError>
                {
                    return detail::bind_variadic_fields(db, stmt, 1, parts...);
                });
            return res.has_value() && *res;
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
            auto safe_sql = detail::encode_sqlgen_embedded_nuls(sqlgen::sqlite::to_sql(query));
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
            using AggRow = detail::SingleValueRow<std::optional<ResultT>>;
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
            using AggRow = detail::SingleValueRow<std::optional<ResultT>>;
            auto query = sqlgen::select_from<DTO>(agg_expr | sqlgen::as<"val">) | sqlgen::where(cond) | sqlgen::to<AggRow>;
            auto safe_sql = detail::encode_sqlgen_embedded_nuls(sqlgen::sqlite::to_sql(query));
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
            return static_cast<std::int64_t>(sqlite3_last_insert_rowid(db_));
        }

        template<typename DTO>
            requires DAL::Schema::RelationalEntity<DTO>
        [[nodiscard]] std::expected<void, Core::DbError> try_upsert(const DTO& item)
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is closed") };
            }
            const std::span<const DTO> one{ &item, 1 };
            return execute_insert_batch<DTO>(one, true);
        }

        template<typename DTO>
            requires DAL::Schema::RelationalEntity<DTO>
        [[nodiscard]] auto upsert(const DTO& item)
        {
            auto res = try_upsert(item);
            if (!res)
            {
                return sqlgen::Result<sqlgen::Nothing>{ sqlgen::error(res.error().message) };
            }
            return sqlgen::Result<sqlgen::Nothing>{ sqlgen::Nothing{} };
        }

        template<typename DTO>
            requires DAL::Schema::RelationalEntity<DTO>
        [[nodiscard]] std::expected<void, Core::DbError> try_upsert_many(const std::vector<DTO>& items)
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is closed") };
            }
            return execute_insert_batch<DTO>(std::span<const DTO>{ items }, true);
        }

        template<typename DTO>
            requires DAL::Schema::RelationalEntity<DTO>
        [[nodiscard]] auto upsert_many(const std::vector<DTO>& items)
        {
            auto res = try_upsert_many(items);
            if (!res)
            {
                return sqlgen::Result<sqlgen::Nothing>{ sqlgen::error(res.error().message) };
            }
            return sqlgen::Result<sqlgen::Nothing>{ sqlgen::Nothing{} };
        }

        template<typename DTO, typename Condition>
        [[nodiscard]] std::expected<void, Core::DbError> delete_where(const Condition& cond)
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is closed") };
            }
            auto query = sqlgen::delete_from<DTO> | sqlgen::where(cond);
            auto safe_sql = detail::encode_sqlgen_embedded_nuls(sqlgen::sqlite::to_sql(query));
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
            auto safe_sql = detail::encode_sqlgen_embedded_nuls(sqlgen::sqlite::to_sql(query));
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
            auto safe_sql = detail::encode_sqlgen_embedded_nuls(sqlgen::sqlite::to_sql(query));
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
            if (!detail::is_valid_sql_identifier(name))
            {
                return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation("Invalid savepoint identifier") };
            }
            return exec_cstr(std::format("SAVEPOINT \"{}\";", name));
        }

        [[nodiscard]] std::expected<void, Core::DbError> release_savepoint(std::string_view name)
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is closed") };
            }
            if (!detail::is_valid_sql_identifier(name))
            {
                return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation("Invalid savepoint identifier") };
            }
            return exec_cstr(std::format("RELEASE SAVEPOINT \"{}\";", name));
        }

        [[nodiscard]] std::expected<void, Core::DbError> rollback_to_savepoint(std::string_view name)
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is closed") };
            }
            if (!detail::is_valid_sql_identifier(name))
            {
                return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation("Invalid savepoint identifier") };
            }
            return exec_cstr(std::format("ROLLBACK TO SAVEPOINT \"{}\"; RELEASE SAVEPOINT \"{}\";", name, name));
        }

        [[nodiscard]] std::expected<void, Core::DbError> set_busy_timeout(std::chrono::milliseconds timeout)
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is closed") };
            }
            if (timeout.count() < 0 || timeout.count() > std::numeric_limits<int>::max())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation(
                    "SQLite busy_timeout must be between 0 and INT_MAX milliseconds") };
            }
            if (sqlite3_busy_timeout(db_, static_cast<int>(timeout.count())) != SQLITE_OK)
            {
                return std::unexpected<Core::DbError>{ map_sqlite_error(sqlite3_errmsg(db_)) };
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
            return exec_cstr(std::string{ sql });
        }

        [[nodiscard]] std::expected<DynamicQueryResult, Core::DbError> query_rows(
            std::string_view sql, std::span<const DynamicCell> params = {}) const
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is closed") };
            }
            auto stmt_res = detail::SQLiteStatement::prepare_single(db_, sql);
            if (!stmt_res)
            {
                return std::unexpected<Core::DbError>{ stmt_res.error() };
            }
            auto& stmt = *stmt_res;
            if (auto bind_res = detail::bind_dynamic_params(db_, stmt.raw(), params); !bind_res)
            {
                return std::unexpected<Core::DbError>{ bind_res.error() };
            }

            DynamicQueryResult result;
            const int col_count = stmt.column_count();
            result.columns.reserve(static_cast<std::size_t>(col_count));
            for (int c = 0; c < col_count; ++c)
            {
                const char* col_name = sqlite3_column_name(stmt.raw(), c);
                result.columns.emplace_back(col_name != nullptr ? col_name : "");
            }

            while (true)
            {
                const int step_rc = stmt.step();
                if (step_rc == SQLITE_DONE)
                {
                    break;
                }
                if (step_rc != SQLITE_ROW)
                {
                    return std::unexpected<Core::DbError>{ map_sqlite_error(sqlite3_errmsg(db_)) };
                }

                DynamicRow row;
                row.cells.reserve(static_cast<std::size_t>(col_count));
                for (int c = 0; c < col_count; ++c)
                {
                    switch (sqlite3_column_type(stmt.raw(), c))
                    {
                    case SQLITE_NULL:
                        row.cells.emplace_back(std::monostate{});
                        break;
                    case SQLITE_INTEGER:
                        row.cells.emplace_back(static_cast<std::int64_t>(sqlite3_column_int64(stmt.raw(), c)));
                        break;
                    case SQLITE_FLOAT:
                        row.cells.emplace_back(sqlite3_column_double(stmt.raw(), c));
                        break;
                    case SQLITE_TEXT:
                        {
                            const auto* txt = reinterpret_cast<const char*>(sqlite3_column_text(stmt.raw(), c));
                            const int len = sqlite3_column_bytes(stmt.raw(), c);
                            row.cells.emplace_back(txt != nullptr ? std::string(txt, static_cast<std::size_t>(len)) : std::string{});
                            break;
                        }
                    case SQLITE_BLOB:
                        {
                            const auto* blob = static_cast<const std::byte*>(sqlite3_column_blob(stmt.raw(), c));
                            const int len = sqlite3_column_bytes(stmt.raw(), c);
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
            auto stmt_res = detail::SQLiteStatement::prepare_single(db_, sql);
            if (!stmt_res)
            {
                return std::unexpected<Core::DbError>{ stmt_res.error() };
            }
            auto& stmt = *stmt_res;
            if (auto bind_res = detail::bind_dynamic_params(db_, stmt.raw(), params); !bind_res)
            {
                return std::unexpected<Core::DbError>{ bind_res.error() };
            }
            int step_rc = SQLITE_OK;
            while ((step_rc = stmt.step()) == SQLITE_ROW)
            {
            }
            if (step_rc != SQLITE_DONE)
            {
                return std::unexpected<Core::DbError>{ map_sqlite_error(sqlite3_errmsg(db_)) };
            }
            return {};
        }

        [[nodiscard]] Core::DbError map_error(std::string_view msg) const
        {
            return map_sqlite_error(msg);
        }

    private:
        [[nodiscard]] std::expected<void, Core::DbError> exec_cstr(const std::string& sql) const
        {
            char* errmsg = nullptr;
            const int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &errmsg);
            if (rc != SQLITE_OK)
            {
                const std::string msg = errmsg != nullptr ? std::string(errmsg) : std::string(sqlite3_errmsg(db_));
                if (errmsg != nullptr)
                {
                    sqlite3_free(errmsg);
                }
                return std::unexpected<Core::DbError>{ map_sqlite_error(msg) };
            }
            if (errmsg != nullptr)
            {
                sqlite3_free(errmsg);
            }
            return {};
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

        template<typename BindFn>
        [[nodiscard]] std::expected<void, Core::DbError> execute_dml_bound(std::string_view sql, BindFn&& bind_fn) const
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is closed") };
            }
            auto stmt_res = detail::SQLiteStatement::prepare_single(db_, sql);
            if (!stmt_res)
            {
                return std::unexpected<Core::DbError>{ stmt_res.error() };
            }
            auto& stmt = *stmt_res;
            if (auto bind_res = std::forward<BindFn>(bind_fn)(db_, stmt.raw()); !bind_res)
            {
                return bind_res;
            }
            int step_rc = SQLITE_OK;
            while ((step_rc = stmt.step()) == SQLITE_ROW)
            {
            }
            if (step_rc != SQLITE_DONE)
            {
                return std::unexpected<Core::DbError>{ map_sqlite_error(sqlite3_errmsg(db_)) };
            }
            return {};
        }

        template<typename BindFn>
        [[nodiscard]] std::expected<bool, Core::DbError> execute_exists_bound(std::string_view sql, BindFn&& bind_fn) const
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is closed") };
            }
            auto stmt_res = detail::SQLiteStatement::prepare_single(db_, sql);
            if (!stmt_res)
            {
                return std::unexpected<Core::DbError>{ stmt_res.error() };
            }
            auto& stmt = *stmt_res;
            if (auto bind_res = std::forward<BindFn>(bind_fn)(db_, stmt.raw()); !bind_res)
            {
                return std::unexpected<Core::DbError>{ bind_res.error() };
            }
            const int step_rc = stmt.step();
            if (step_rc == SQLITE_ROW)
            {
                return true;
            }
            if (step_rc == SQLITE_DONE)
            {
                return false;
            }
            return std::unexpected<Core::DbError>{ map_sqlite_error(sqlite3_errmsg(db_)) };
        }

        template<typename DTO>
        [[nodiscard]] std::expected<void, Core::DbError> execute_insert_batch(std::span<const DTO> items, bool on_conflict_update) const
        {
            if (items.empty())
            {
                return {};
            }
            if (auto id_chk = detail::validate_dto_ddl_identifiers<DTO>(); !id_chk)
            {
                return id_chk;
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

            auto stmt_res = detail::SQLiteStatement::prepare_single(db_, sql);
            if (!stmt_res)
            {
                return std::unexpected<Core::DbError>{ stmt_res.error() };
            }
            auto& stmt = *stmt_res;

            for (const DTO& item : items)
            {
                int bind_idx = 1;
                std::expected<void, Core::DbError> bind_res{};
                template for (constexpr auto m : DAL::Schema::members_v<DTO>)
                {
                    if (bind_res)
                    {
                        bind_res = detail::bind_typed_field(db_, stmt.raw(), bind_idx++, item.[:m:], true);
                    }
                }
                if (!bind_res)
                {
                    return bind_res;
                }

                const int step_rc = stmt.step();
                if (step_rc != SQLITE_DONE)
                {
                    return std::unexpected<Core::DbError>{ map_sqlite_error(sqlite3_errmsg(db_)) };
                }
                if (auto reset_res = stmt.reset_and_clear(); !reset_res)
                {
                    return reset_res;
                }
            }
            return {};
        }

        template<typename RowT, typename BindFn>
        [[nodiscard]] std::expected<std::vector<RowT>, Core::DbError> execute_typed_sql_bound(std::string_view sql, BindFn&& bind_fn) const
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is closed") };
            }
            auto stmt_res = detail::SQLiteStatement::prepare_single(db_, sql);
            if (!stmt_res)
            {
                return std::unexpected<Core::DbError>{ stmt_res.error() };
            }
            auto& stmt = *stmt_res;
            if (auto bind_res = std::forward<BindFn>(bind_fn)(db_, stmt.raw()); !bind_res)
            {
                return std::unexpected<Core::DbError>{ bind_res.error() };
            }

            std::vector<RowT> result;
            while (true)
            {
                const int step_rc = stmt.step();
                if (step_rc == SQLITE_DONE)
                {
                    break;
                }
                if (step_rc != SQLITE_ROW)
                {
                    return std::unexpected<Core::DbError>{ map_sqlite_error(sqlite3_errmsg(db_)) };
                }
                auto row_res = detail::decode_row<RowT>(db_, stmt.raw());
                if (!row_res)
                {
                    return std::unexpected<Core::DbError>{ row_res.error() };
                }
                result.push_back(std::move(*row_res));
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
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is closed") };
            }
            auto stmt_res = detail::SQLiteStatement::prepare_single(db_, sql);
            if (!stmt_res)
            {
                return std::unexpected<Core::DbError>{ stmt_res.error() };
            }
            auto& stmt = *stmt_res;

            std::vector<RowTuple> result;
            while (true)
            {
                const int step_rc = stmt.step();
                if (step_rc == SQLITE_DONE)
                {
                    break;
                }
                if (step_rc != SQLITE_ROW)
                {
                    return std::unexpected<Core::DbError>{ map_sqlite_error(sqlite3_errmsg(db_)) };
                }
                auto tuple_res = detail::decode_tuple<RowTuple>(db_, stmt.raw());
                if (!tuple_res)
                {
                    return std::unexpected<Core::DbError>{ tuple_res.error() };
                }
                result.push_back(std::move(*tuple_res));
            }
            return result;
        }

        [[nodiscard]] Core::DbError map_sqlite_error(std::string_view msg) const
        {
            return detail::map_sqlite_db_error(db_, msg);
        }

        sqlite3* db_{ nullptr };
        bool is_open_{ false };
    };

} // namespace DAL
