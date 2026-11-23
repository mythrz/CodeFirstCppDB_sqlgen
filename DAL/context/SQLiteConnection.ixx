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

                // Clear per-column PRIMARY KEY markers so multi-PrimaryKey and multi-ForeignKey DTOs emit one table-level PRIMARY KEY
                // constraint.
                auto stmt = sqlgen::transpilation::to_create_table<DTO>(true);
                for (auto& col : stmt.columns)
                {
                    col.type.visit(
                        [](auto& t)
                        {
                            t.properties.primary = false;
                            t.properties.auto_incr = false;
                        });
                }
                std::string create_sql = (*conn_)->to_sql(stmt);
                if (const auto close_pos = create_sql.rfind(");"); close_pos != std::string::npos)
                {
                    create_sql.insert(close_pos, std::format(", PRIMARY KEY ({})", cols));
                }
                if (auto res = (*conn_)->execute(create_sql); !res)
                {
                    return std::unexpected<Core::DbError>{ map_sqlite_error(res.error().what()) };
                }

                const std::string_view table_name = DTO::tablename;
                const std::string index_sql =
                    std::format("CREATE UNIQUE INDEX IF NOT EXISTS \"ux_{}_composite_pk\" ON \"{}\" ({});", table_name, table_name, cols);
                if (auto idx_res = (*conn_)->execute(index_sql); !idx_res)
                {
                    return std::unexpected<Core::DbError>{ map_sqlite_error(idx_res.error().what()) };
                }
            }
            else
            {
                auto res = sqlgen::CreateTable<DTO>{ .if_not_exists_ = true }((*conn_));
                if (!res)
                {
                    return std::unexpected<Core::DbError>{ map_sqlite_error(res.error().what()) };
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
            auto res = sqlgen::CreateIndex<IndexName, DTO, sqlgen::Nothing, ColTypes...>{ .unique_ = unique,
                                                                                          .if_not_exists_ = true,
                                                                                          .where_ = sqlgen::Nothing{} }((*conn_));
            if (!res)
            {
                return std::unexpected<Core::DbError>{ map_sqlite_error(res.error().what()) };
            }
            return {};
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
            auto res = sqlgen::CreateIndex<IndexName, DTO, WhereCond, ColTypes...>{ .unique_ = unique,
                                                                                    .if_not_exists_ = true,
                                                                                    .where_ = where_cond }((*conn_));
            if (!res)
            {
                return std::unexpected<Core::DbError>{ map_sqlite_error(res.error().what()) };
            }
            return {};
        }

        template<typename DTO>
        [[nodiscard]] std::expected<void, Core::DbError> ensure_table_dropped()
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error(
                    "Cannot drop table: database connection is closed") };
            }
            auto res = sqlgen::Drop<DTO>{ .what_ = sqlgen::transpilation::get_table_or_view<DTO>(), .cascade_ = false, .if_exists_ = true }(
                (*conn_));
            if (!res)
            {
                return std::unexpected<Core::DbError>{ map_sqlite_error(res.error().what()) };
            }
            return {};
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
            auto query = sqlgen::read<std::vector<DTO>> | sqlgen::where(DAL::Schema::make_pk_where<DTO>(id)) | sqlgen::limit(1);
            auto res = execute_typed_sql<DTO>(sqlgen::sqlite::to_sql(query));
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
            auto query =
                sqlgen::read<std::vector<DTO>> | sqlgen::where(DAL::Schema::make_composite_where<DTO>(parts...)) | sqlgen::limit(1);
            auto res = execute_typed_sql<DTO>(sqlgen::sqlite::to_sql(query));
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
            auto query = sqlgen::read<std::vector<DTO>> | sqlgen::where(sqlgen::col<ColLiteral> == val);
            auto res = execute_typed_sql<DTO>(sqlgen::sqlite::to_sql(query));
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
            const std::string sql = sqlgen::sqlite::to_sql(std::forward<Query>(query));
            return execute_typed_sql<DTO>(sql);
        }

        template<typename RowTuple, typename Query>
        [[nodiscard]] std::expected<std::vector<RowTuple>, Core::DbError> try_execute_tuple_read(Query&& query) const
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is closed") };
            }
            const std::string sql = sqlgen::sqlite::to_sql(std::forward<Query>(query));
            return execute_typed_tuple_sql<RowTuple>(sql);
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
            std::vector<DTO> vec{ item };
            return sqlgen::insert(std::ref(vec))((*conn_));
        }

        template<typename DTO>
        [[nodiscard]] auto insert_many(const std::vector<DTO>& items)
        {
            if (!is_open())
            {
                return sqlgen::Result<sqlgen::Ref<sqlgen::sqlite::Connection>>{ sqlgen::error("Database connection is closed") };
            }
            return sqlgen::insert(std::ref(items))((*conn_));
        }

        template<typename DTO>
        [[nodiscard]] auto update(const DTO& item)
        {
            if (!is_open())
            {
                return sqlgen::Result<sqlgen::Ref<sqlgen::sqlite::Connection>>{ sqlgen::error("Database connection is closed") };
            }

            static constexpr auto nk_members = DAL::Schema::non_key_members_of<DTO>();
            if constexpr (nk_members.empty())
            {
                auto exists_query =
                    sqlgen::read<std::vector<DTO>> | sqlgen::where(DAL::Schema::make_dto_key_where<DTO>(item)) | sqlgen::limit(1);
                auto exists_res = execute_typed_sql<DTO>(sqlgen::sqlite::to_sql(exists_query));
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
                auto upd_res = [&]<std::size_t... Is>(std::index_sequence<Is...>)
                {
                    auto query = sqlgen::update<DTO>(
                                     (DAL::Schema::col_of<nk_members[Is]>.set(DAL::Schema::unwrap_field(item.[:nk_members[Is]:])))...) |
                        sqlgen::where(DAL::Schema::make_dto_key_where<DTO>(item));
                    return query(*conn_);
                }(std::make_index_sequence<nk_members.size()>{});
                if (!upd_res)
                {
                    return upd_res;
                }

                sqlite3* db = native_sqlite_handle();
                if (db == nullptr || sqlite3_changes(db) == 0)
                {
                    return sqlgen::Result<sqlgen::Ref<sqlgen::sqlite::Connection>>{ sqlgen::error("Update failed: item not found") };
                }
                return upd_res;
            }
        }

        template<typename DTO, typename IdType>
        [[nodiscard]] auto delete_by_id(const IdType& id)
        {
            if (!is_open())
            {
                return sqlgen::Result<sqlgen::Ref<sqlgen::sqlite::Connection>>{ sqlgen::error("Database connection is closed") };
            }
            return (sqlgen::delete_from<DTO> | sqlgen::where(DAL::Schema::make_pk_where<DTO>(id)))((*conn_));
        }

        template<typename DTO, typename... Parts>
        [[nodiscard]] auto delete_by_composite_id(const Parts&... parts)
        {
            if (!is_open())
            {
                return sqlgen::Result<sqlgen::Ref<sqlgen::sqlite::Connection>>{ sqlgen::error("Database connection is closed") };
            }
            return (sqlgen::delete_from<DTO> | sqlgen::where(DAL::Schema::make_composite_where<DTO>(parts...)))((*conn_));
        }

        template<typename DTO, typename IdType>
        [[nodiscard]] bool exists(const IdType& id) const
        {
            auto res = try_count_where<DTO>(DAL::Schema::make_pk_where<DTO>(id));
            return res.has_value() && *res > 0;
        }

        template<typename DTO, typename... Parts>
        [[nodiscard]] bool exists_by_composite_id(const Parts&... parts) const
        {
            auto res = try_count_where<DTO>(DAL::Schema::make_composite_where<DTO>(parts...));
            return res.has_value() && *res > 0;
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
            requires sqlgen::internal::has_constraint_v<DTO>
        [[nodiscard]] auto upsert(const DTO& item)
        {
            if (!is_open())
            {
                return sqlgen::Result<sqlgen::Ref<sqlgen::sqlite::Connection>>{ sqlgen::error("Database connection is closed") };
            }
            std::vector<DTO> vec{ item };
            return sqlgen::insert_or_replace(std::ref(vec))((*conn_));
        }

        template<typename DTO>
            requires sqlgen::internal::has_constraint_v<DTO>
        [[nodiscard]] auto upsert_many(const std::vector<DTO>& items)
        {
            if (!is_open())
            {
                return sqlgen::Result<sqlgen::Ref<sqlgen::sqlite::Connection>>{ sqlgen::error("Database connection is closed") };
            }
            return sqlgen::insert_or_replace(std::ref(items))((*conn_));
        }

        template<typename DTO, typename Condition>
        [[nodiscard]] std::expected<void, Core::DbError> delete_where(const Condition& cond)
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is closed") };
            }
            auto query = sqlgen::delete_from<DTO> | sqlgen::where(cond);
            auto res = query((*conn_));
            if (!res)
            {
                return std::unexpected<Core::DbError>{ map_sqlite_error(res.error().what()) };
            }
            return {};
        }

        template<typename DTO, typename SetExpr, typename Condition>
        [[nodiscard]] std::expected<void, Core::DbError> update_where(const SetExpr& set_expr, const Condition& cond)
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is closed") };
            }
            auto query = sqlgen::update<DTO>(set_expr) | sqlgen::where(cond);
            auto res = query((*conn_));
            if (!res)
            {
                return std::unexpected<Core::DbError>{ map_sqlite_error(res.error().what()) };
            }
            return {};
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
            auto res = query((*conn_));
            if (!res)
            {
                return std::unexpected<Core::DbError>{ map_sqlite_error(res.error().what()) };
            }
            return {};
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
            for (unsigned char ch : effective)
            {
                lower.push_back(static_cast<char>(std::tolower(ch)));
            }
            if (lower != "wal")
            {
                return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation(
                    std::format("Failed to activate WAL journal mode (effective mode: '{}')", effective)) };
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
            for (unsigned char ch : name)
            {
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

        [[nodiscard]] std::expected<std::vector<std::vector<std::optional<std::string>>>, Core::DbError> execute_raw_text_rows(
            std::string_view sql) const
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

        template<typename RowT>
        [[nodiscard]] std::expected<std::vector<RowT>, Core::DbError> execute_typed_sql(std::string_view sql) const
        {
            auto raw_rows = execute_raw_text_rows(sql);
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
