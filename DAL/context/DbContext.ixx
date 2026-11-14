module;
#include <meta>

export module dal:db_context;

import std;
import core;
import :schema_traits;
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

    template<typename T>
    concept IsDbSetMember = requires {
        typename T::DomainType;
        typename T::DTOType;
        requires Schema::RelationalEntity<typename T::DTOType>;
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

        // Type alias for zero-boilerplate DbSet declarations inside derived DbContext classes
        template<typename Domain,
                 typename DTO = Schema::SchemaOf_t<Domain>,
                 typename KeyType = typename Domain::Key>
        using Set = DbSet<Domain, DTO, ConnectionHandle, KeyType>;

        // Factory helper to construct a DbSet bound to this context's connection
        template<typename Domain,
                 typename DTO = Schema::SchemaOf_t<Domain>,
                 typename KeyType = typename Domain::Key>
        [[nodiscard]] Set<Domain, DTO, KeyType> create_set() const
        {
            return Set<Domain, DTO, KeyType>(conn_);
        }

        // =====================================================================
        // Reflection-Driven Schema Creation (db.ensure_created())
        // =====================================================================

        template<typename Self>
        std::expected<void, Core::DbError> ensure_created(this Self&& self)
        {
            if (!self.is_connected())
            {
                return std::unexpected<Core::DbError>{
                    Core::DbError::connection_error("Cannot create schema: database connection is closed")
                };
            }

            using DerivedType = std::remove_cvref_t<Self>;
            static constexpr auto members = std::define_static_array(
                std::meta::nonstatic_data_members_of(^^DerivedType, std::meta::access_context::unchecked()));

            std::expected<void, Core::DbError> result{};
            template for (constexpr auto m : members)
            {
                using RawMemberType = [:std::meta::type_of(m):];
                using MemberType = std::remove_cvref_t<RawMemberType>;
                if constexpr (IsDbSetMember<MemberType>)
                {
                    if (result)
                    {
                        result = self.conn_->template ensure_table_created<typename MemberType::DTOType>();
                    }
                }
            }

            if (!result)
            {
                return result;
            }

            return self.conn_->enable_foreign_keys();
        }

        template<typename... Domains>
        std::expected<void, Core::DbError> ensure_created_for()
        {
            if (!is_connected())
            {
                return std::unexpected<Core::DbError>{
                    Core::DbError::connection_error("Cannot create schema: database connection is closed")
                };
            }

            std::expected<void, Core::DbError> result{};
            (([&] {
                if (result)
                {
                    result = conn_->template ensure_table_created<Schema::SchemaOf_t<Domains>>();
                }
            }()), ...);

            if (!result)
            {
                return result;
            }

            return conn_->enable_foreign_keys();
        }

        // =====================================================================
        // Unit of Work Staging & Transaction Controls
        // =====================================================================

        void defer_changes(bool enabled = true) noexcept
        {
            defer_changes_ = enabled;
        }

        [[nodiscard]] bool is_deferred() const noexcept
        {
            return defer_changes_;
        }

        void enqueue_op(std::function<std::expected<void, Core::DbError>()> op)
        {
            pending_ops_.push_back(std::move(op));
        }

        [[nodiscard]] std::size_t pending_changes_count() const noexcept
        {
            return pending_ops_.size();
        }

        void discard_changes() noexcept
        {
            pending_ops_.clear();
        }

        std::expected<void, Core::DbError> begin_transaction()
        {
            if (!is_connected())
            {
                return std::unexpected<Core::DbError>{
                    Core::DbError::connection_error("Cannot begin transaction: database connection is closed")
                };
            }
            return conn_->begin_transaction();
        }

        std::expected<void, Core::DbError> commit()
        {
            if (!is_connected())
            {
                return std::unexpected<Core::DbError>{
                    Core::DbError::connection_error("Cannot commit transaction: database connection is closed")
                };
            }
            return conn_->commit();
        }

        std::expected<void, Core::DbError> rollback()
        {
            if (!is_connected())
            {
                return std::unexpected<Core::DbError>{
                    Core::DbError::connection_error("Cannot rollback transaction: database connection is closed")
                };
            }
            return conn_->rollback();
        }

        [[nodiscard]] bool in_transaction() const noexcept
        {
            return conn_ && conn_->in_transaction();
        }

        // Unit of Work save_changes: executes staged operations atomically in a transaction
        // with automatic rollback on failure, or commits an active explicit transaction.
        virtual std::expected<void, Core::DbError> save_changes()
        {
            if (!is_connected())
            {
                return std::unexpected<Core::DbError>{
                    Core::DbError::connection_error("Cannot save changes: database connection is closed")
                };
            }

            if (!pending_ops_.empty())
            {
                const bool already_in_tx = in_transaction();
                if (!already_in_tx)
                {
                    auto tx_res = begin_transaction();
                    if (!tx_res)
                    {
                        return tx_res;
                    }
                }

                for (auto& op : pending_ops_)
                {
                    auto op_res = op();
                    if (!op_res)
                    {
                        if (in_transaction())
                        {
                            (void)rollback();
                        }
                        pending_ops_.clear();
                        return op_res;
                    }
                }

                pending_ops_.clear();
                if (in_transaction())
                {
                    return commit();
                }
                return {};
            }

            if (in_transaction())
            {
                return commit();
            }

            return {};
        }

        // Scoped transaction helper: begins a transaction, runs fn(), flushes staged changes,
        // and commits on success or automatically rolls back on failure.
        template<typename Fn>
        std::expected<void, Core::DbError> transaction(Fn&& fn)
        {
            if (!is_connected())
            {
                return std::unexpected<Core::DbError>{
                    Core::DbError::connection_error("Cannot start transaction: database connection is closed")
                };
            }

            auto tx_res = begin_transaction();
            if (!tx_res)
            {
                return tx_res;
            }

            try
            {
                using FnResult = std::invoke_result_t<Fn>;
                if constexpr (std::is_void_v<FnResult>)
                {
                    std::invoke(std::forward<Fn>(fn));
                }
                else
                {
                    auto fn_res = std::invoke(std::forward<Fn>(fn));
                    if (!fn_res)
                    {
                        if (in_transaction())
                        {
                            (void)rollback();
                        }
                        pending_ops_.clear();
                        return fn_res;
                    }
                }

                if (!pending_ops_.empty())
                {
                    return save_changes();
                }

                if (in_transaction())
                {
                    return commit();
                }
                return {};
            }
            catch (...)
            {
                if (in_transaction())
                {
                    (void)rollback();
                }
                pending_ops_.clear();
                throw;
            }
        }

    protected:
        std::shared_ptr<ConnectionHandle> conn_;
        std::vector<std::function<std::expected<void, Core::DbError>()>> pending_ops_;
        bool defer_changes_{ false };
    };

} // namespace DAL
