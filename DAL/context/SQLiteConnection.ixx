module;
#include <sqlgen.hpp>
#include <sqlgen/sqlite.hpp>

export module dal:sqlite_connection;

import std;
import core;
import :schema_traits;
import :schema_person_something;

export namespace DAL
{
    // SQLiteConnection adapts a raw sqlgen SQLite connection to satisfy
    // the generic DAL::Repositories::DatabaseConnection concepts.
    class SQLiteConnection
    {
    public:
        explicit SQLiteConnection(sqlgen::Ref<sqlgen::sqlite::Connection> conn)
            : conn_(std::move(conn))
        {
        }

        [[nodiscard]] static std::expected<std::shared_ptr<SQLiteConnection>, Core::DbError> connect(
            const std::string& dbPath)
        {
            auto res = sqlgen::sqlite::connect(dbPath);
            if (!res)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error(res.error().what()) };
            }
            return std::make_shared<SQLiteConnection>(std::move(res.value()));
        }

        template<typename DTO>
        std::expected<void, Core::DbError> ensure_table_created()
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{
                    Core::DbError::connection_error("Cannot create table: database connection is closed")
                };
            }
            auto res = sqlgen::CreateTable<DTO>{ .if_not_exists_ = true }(conn_);
            if (!res)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::map_from_message(res.error().what()) };
            }
            return {};
        }

        std::expected<void, Core::DbError> enable_foreign_keys()
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{
                    Core::DbError::connection_error("Cannot enable foreign keys: database connection is closed")
                };
            }
            auto res = conn_->execute("PRAGMA foreign_keys = ON;");
            if (!res)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::map_from_message(res.error().what()) };
            }
            return {};
        }

        std::expected<void, Core::DbError> begin_transaction()
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{
                    Core::DbError::connection_error("Cannot begin transaction: database connection is closed")
                };
            }
            if (in_transaction_)
            {
                return std::unexpected<Core::DbError>{
                    Core::DbError::connection_error("Transaction is already active")
                };
            }
            auto res = conn_->begin_transaction();
            if (!res)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::map_from_message(res.error().what()) };
            }
            in_transaction_ = true;
            return {};
        }

        std::expected<void, Core::DbError> commit()
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{
                    Core::DbError::connection_error("Cannot commit transaction: database connection is closed")
                };
            }
            if (!in_transaction_)
            {
                return std::unexpected<Core::DbError>{
                    Core::DbError::connection_error("No active transaction to commit")
                };
            }
            auto res = conn_->commit();
            if (!res)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::map_from_message(res.error().what()) };
            }
            in_transaction_ = false;
            return {};
        }

        std::expected<void, Core::DbError> rollback()
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{
                    Core::DbError::connection_error("Cannot rollback transaction: database connection is closed")
                };
            }
            if (!in_transaction_)
            {
                return std::unexpected<Core::DbError>{
                    Core::DbError::connection_error("No active transaction to rollback")
                };
            }
            auto res = conn_->rollback();
            in_transaction_ = false;
            if (!res)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::map_from_message(res.error().what()) };
            }
            return {};
        }

        [[nodiscard]] bool in_transaction() const noexcept
        {
            return in_transaction_;
        }

        [[nodiscard]] bool is_open() const noexcept
        {
            return true;
        }

        [[nodiscard]] sqlgen::Ref<sqlgen::sqlite::Connection>& raw_connection() noexcept
        {
            return conn_;
        }

        [[nodiscard]] const sqlgen::Ref<sqlgen::sqlite::Connection>& raw_connection() const noexcept
        {
            return conn_;
        }

        template<typename DTO>
        [[nodiscard]] std::expected<std::vector<DTO>, Core::DbError> try_fetch_all() const
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{
                    Core::DbError::connection_error("Database connection is closed")
                };
            }
            auto res = sqlgen::read<std::vector<DTO>>(conn_);
            if (!res)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::map_from_message(res.error().what()) };
            }
            return res.value();
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

        template<typename DTO>
        [[nodiscard]] std::expected<std::optional<DTO>, Core::DbError> try_get_by_id(std::int32_t id) const
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{
                    Core::DbError::connection_error("Database connection is closed")
                };
            }
            auto query = sqlgen::read<std::vector<DTO>>
                       | sqlgen::where(DAL::Schema::make_pk_where<DTO>(id))
                       | sqlgen::limit(1);
            auto res = query(conn_);
            if (!res)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::map_from_message(res.error().what()) };
            }
            if (res.value().empty())
            {
                return std::optional<DTO>{ std::nullopt };
            }
            return std::optional<DTO>{ std::move(res.value().front()) };
        }

        template<typename DTO>
        [[nodiscard]] std::optional<DTO> get_by_id(std::int32_t id) const
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
                return std::unexpected<Core::DbError>{
                    Core::DbError::connection_error("Database connection is closed")
                };
            }
            auto query = sqlgen::read<std::vector<DTO>>
                       | sqlgen::where(DAL::Schema::make_composite_where<DTO>(parts...))
                       | sqlgen::limit(1);
            auto res = query(conn_);
            if (!res)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::map_from_message(res.error().what()) };
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
            auto query = sqlgen::read<std::vector<DTO>>
                       | sqlgen::where(sqlgen::Col<ColLiteral>{} == val);
            auto res = query(conn_);
            if (res)
            {
                return std::move(res.value());
            }
            return {};
        }

        template<typename DTO>
        auto insert(const DTO& item)
        {
            std::vector<DTO> vec{ item };
            return sqlgen::insert(std::ref(vec))(conn_);
        }

        template<typename DTO>
        auto insert_many(const std::vector<DTO>& items)
        {
            return sqlgen::insert(std::ref(items))(conn_);
        }

        template<typename DTO>
        auto update(const DTO& item)
        {
            auto exists_query = sqlgen::read<std::vector<DTO>>
                              | sqlgen::where(DAL::Schema::make_dto_key_where<DTO>(item))
                              | sqlgen::limit(1);
            auto exists_res = exists_query(conn_);
            if (!exists_res)
            {
                return sqlgen::Result<sqlgen::Ref<sqlgen::sqlite::Connection>>{
                    sqlgen::error(exists_res.error().what())
                };
            }
            if (exists_res.value().empty())
            {
                return sqlgen::Result<sqlgen::Ref<sqlgen::sqlite::Connection>>{
                    sqlgen::error("Update failed: item not found")
                };
            }

            static constexpr auto nk_members = DAL::Schema::non_key_members_of<DTO>();
            if constexpr (nk_members.empty())
            {
                return sqlgen::Result<sqlgen::Ref<sqlgen::sqlite::Connection>>{ conn_ };
            }
            else
            {
                return [&]<std::size_t... Is>(std::index_sequence<Is...>) {
                    auto query = sqlgen::update<DTO>(
                                     (DAL::Schema::col_of<nk_members[Is]>.set(
                                         DAL::Schema::unwrap_field(item.[:nk_members[Is]:])))...)
                               | sqlgen::where(DAL::Schema::make_dto_key_where<DTO>(item));
                    return query(conn_);
                }(std::make_index_sequence<nk_members.size()>{});
            }
        }

        template<typename DTO>
        auto delete_by_id(std::int32_t id)
        {
            return (sqlgen::delete_from<DTO>
                  | sqlgen::where(DAL::Schema::make_pk_where<DTO>(id)))(conn_);
        }

        template<typename DTO = DAL::Schema::Person_SomethingDTO, typename... Parts>
        [[nodiscard]] auto delete_by_composite_id(const Parts&... parts)
        {
            return (sqlgen::delete_from<DTO>
                  | sqlgen::where(DAL::Schema::make_composite_where<DTO>(parts...)))(conn_);
        }

        template<typename DTO>
        [[nodiscard]] bool exists(std::int32_t id) const
        {
            return get_by_id<DTO>(id).has_value();
        }

        template<typename DTO, typename... Parts>
        [[nodiscard]] bool exists_by_composite_id(const Parts&... parts) const
        {
            return get_by_composite_id<DTO>(parts...).has_value();
        }

        template<typename DTO>
        [[nodiscard]] std::expected<std::size_t, Core::DbError> try_count() const
        {
            if (!is_open())
            {
                return std::unexpected<Core::DbError>{
                    Core::DbError::connection_error("Database connection is closed")
                };
            }
            auto query = sqlgen::select_from<DTO>(sqlgen::count() | sqlgen::as<"count">)
                       | sqlgen::to<RowCount>;
            auto res = query(conn_);
            if (!res)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::map_from_message(res.error().what()) };
            }
            return res.value().count;
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

    private:
        struct RowCount
        {
            std::size_t count = 0;
        };

        sqlgen::Ref<sqlgen::sqlite::Connection> conn_;
        bool in_transaction_{ false };
    };

} // namespace DAL
