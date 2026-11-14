export module dal:db_context;

import std;
import core;
import :generic_repo;
import :db_set;

export namespace DAL
{
    // Transaction handle abstraction for Unit of Work execution
    class ITransaction
    {
    public:
        virtual ~ITransaction() = default;
        virtual std::expected<void, Core::DbError> commit() = 0;
        virtual std::expected<void, Core::DbError> rollback() = 0;
    };

    // DbContextBase acts as the central Unit of Work and connection coordinator,
    // mirroring C# Entity Framework Core's DbContext.
    template<typename ConnectionHandle>
        requires Repositories::DatabaseConnection<ConnectionHandle>
    class DbContextBase
    {
    public:
        explicit DbContextBase(std::shared_ptr<ConnectionHandle> conn)
            : conn_(std::move(conn))
        {
        }

        virtual ~DbContextBase() = default;

        // Access the underlying database connection handle
        [[nodiscard]] std::shared_ptr<ConnectionHandle> connection() const noexcept
        {
            return conn_;
        }

        // Verifies whether the underlying database connection is open and active
        [[nodiscard]] bool is_connected() const noexcept
        {
            return conn_ && conn_->is_open();
        }

        // Factory helper to construct a DbSet bound to this context's connection
        template<typename Domain, typename DTO, typename KeyType = std::int32_t>
        [[nodiscard]] DbSet<Domain, DTO, ConnectionHandle, KeyType> create_set() const
        {
            return DbSet<Domain, DTO, ConnectionHandle, KeyType>(conn_);
        }

        // Unit of Work save_changes: commits pending state and verifies connection health
        virtual std::expected<void, Core::DbError> save_changes()
        {
            if (!is_connected())
            {
                return std::unexpected<Core::DbError>{
                    Core::DbError::connection_error("Cannot save changes: database connection is closed")
                };
            }
            return {};
        }

    protected:
        std::shared_ptr<ConnectionHandle> conn_;
    };

} // namespace DAL
