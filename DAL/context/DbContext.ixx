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

        // Non-copyable and non-movable so Set<T>{ this } member closures never dangle
        DbContextBase(const DbContextBase&) = delete;
        DbContextBase& operator=(const DbContextBase&) = delete;
        DbContextBase(DbContextBase&&) = delete;
        DbContextBase& operator=(DbContextBase&&) = delete;

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
                 typename KeyType = Schema::EntityKey_t<Domain, DTO>>
        using Set = DbSet<Domain, DTO, ConnectionHandle, KeyType>;

        // Factory helper to construct a DbSet bound to this context's connection
        template<typename Domain,
                 typename DTO = Schema::SchemaOf_t<Domain>,
                 typename KeyType = Schema::EntityKey_t<Domain, DTO>>
        [[nodiscard]] Set<Domain, DTO, KeyType> create_set() const
        {
            return Set<Domain, DTO, KeyType>(conn_);
        }

        // =====================================================================
        // Reflection-Driven Schema Creation & Deletion (Topologically Sorted)
        // =====================================================================

        template<typename DerivedType>
        static consteval std::size_t dbset_member_count()
        {
            static constexpr auto members = std::define_static_array(
                std::meta::nonstatic_data_members_of(^^DerivedType, std::meta::access_context::unchecked()));
            std::size_t count = 0;
            template for (constexpr auto m : members)
            {
                using RawMemberType = [:std::meta::type_of(m):];
                using MemberType = std::remove_cvref_t<RawMemberType>;
                if constexpr (IsDbSetMember<MemberType>)
                {
                    ++count;
                }
            }
            return count;
        }

        template<typename DerivedType>
        static consteval auto dbset_members_of()
        {
            static constexpr auto members = std::define_static_array(
                std::meta::nonstatic_data_members_of(^^DerivedType, std::meta::access_context::unchecked()));
            constexpr std::size_t K = dbset_member_count<DerivedType>();
            std::array<std::meta::info, K> result{};
            std::size_t idx = 0;
            template for (constexpr auto m : members)
            {
                using RawMemberType = [:std::meta::type_of(m):];
                using MemberType = std::remove_cvref_t<RawMemberType>;
                if constexpr (IsDbSetMember<MemberType>)
                {
                    result[idx++] = m;
                }
            }
            return result;
        }

        template<typename DerivedType>
        static consteval auto topological_dbset_order()
        {
            constexpr std::size_t K = dbset_member_count<DerivedType>();
            static constexpr auto dbset_members = dbset_members_of<DerivedType>();

            std::array<std::meta::info, K> dto_types{};
            for (std::size_t i = 0; i < K; ++i)
            {
                dto_types[i] = dbset_members[i];
            }

            std::array<std::meta::info, K> resolved_dtos{};
            std::size_t idx = 0;
            template for (constexpr auto m : dbset_members)
            {
                using RawMemberType = [:std::meta::type_of(m):];
                using MemberType = std::remove_cvref_t<RawMemberType>;
                resolved_dtos[idx++] = std::meta::dealias(^^typename MemberType::DTOType);
            }

            std::array<std::array<bool, K>, K> depends_on{};
            std::size_t child_idx = 0;
            template for (constexpr auto m : dbset_members)
            {
                using RawMemberType = [:std::meta::type_of(m):];
                using MemberType = std::remove_cvref_t<RawMemberType>;
                constexpr auto targets = Schema::foreign_table_types_of<typename MemberType::DTOType>();
                for (std::meta::info target : targets)
                {
                    for (std::size_t parent_idx = 0; parent_idx < K; ++parent_idx)
                    {
                        if (parent_idx != child_idx && resolved_dtos[parent_idx] == target)
                        {
                            depends_on[child_idx][parent_idx] = true;
                        }
                    }
                }
                ++child_idx;
            }

            return Schema::kahn_topological_sort<K>(depends_on);
        }

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
            static constexpr auto dbset_members = dbset_members_of<DerivedType>();
            static constexpr auto order = topological_dbset_order<DerivedType>();

            std::expected<void, Core::DbError> result{};
            template for (constexpr std::size_t idx : order)
            {
                constexpr auto m = dbset_members[idx];
                using RawMemberType = [:std::meta::type_of(m):];
                using MemberType = std::remove_cvref_t<RawMemberType>;
                if (result)
                {
                    result = self.conn_->template ensure_table_created<typename MemberType::DTOType>();
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

            static constexpr auto order = Schema::topological_dto_order<Schema::SchemaOf_t<Domains>...>();
            std::array<std::function<std::expected<void, Core::DbError>()>, sizeof...(Domains)> creators{
                [this]() { return conn_->template ensure_table_created<Schema::SchemaOf_t<Domains>>(); }...
            };

            for (std::size_t idx : order)
            {
                auto res = creators[idx]();
                if (!res)
                {
                    return res;
                }
            }

            return conn_->enable_foreign_keys();
        }

        template<typename Self>
        std::expected<void, Core::DbError> ensure_deleted(this Self&& self)
        {
            if (!self.is_connected())
            {
                return std::unexpected<Core::DbError>{
                    Core::DbError::connection_error("Cannot drop schema: database connection is closed")
                };
            }

            using DerivedType = std::remove_cvref_t<Self>;
            static constexpr auto dbset_members = dbset_members_of<DerivedType>();
            static constexpr auto order = Schema::reverse_index_array(topological_dbset_order<DerivedType>());

            std::expected<void, Core::DbError> result{};
            template for (constexpr std::size_t idx : order)
            {
                constexpr auto m = dbset_members[idx];
                using RawMemberType = [:std::meta::type_of(m):];
                using MemberType = std::remove_cvref_t<RawMemberType>;
                if (result)
                {
                    result = self.conn_->template ensure_table_dropped<typename MemberType::DTOType>();
                }
            }

            return result;
        }

        template<typename... Domains>
        std::expected<void, Core::DbError> ensure_deleted_for()
        {
            if (!is_connected())
            {
                return std::unexpected<Core::DbError>{
                    Core::DbError::connection_error("Cannot drop schema: database connection is closed")
                };
            }

            static constexpr auto order = Schema::reverse_index_array(
                Schema::topological_dto_order<Schema::SchemaOf_t<Domains>...>());
            std::array<std::function<std::expected<void, Core::DbError>()>, sizeof...(Domains)> droppers{
                [this]() { return conn_->template ensure_table_dropped<Schema::SchemaOf_t<Domains>>(); }...
            };

            for (std::size_t idx : order)
            {
                auto res = droppers[idx]();
                if (!res)
                {
                    return res;
                }
            }

            return {};
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

    // RAII transaction guard that automatically rolls back on scope exit if not committed
    template<typename ConnectionHandle>
        requires Repositories::DatabaseConnection<ConnectionHandle>
    class TransactionScope
    {
    public:
        explicit TransactionScope(DbContextBase<ConnectionHandle>& ctx)
            : ctx_(&ctx)
        {
            begin_result_ = ctx_->begin_transaction();
        }

        ~TransactionScope()
        {
            if (!committed_ && ctx_ && ctx_->in_transaction())
            {
                (void)ctx_->rollback();
            }
        }

        TransactionScope(const TransactionScope&) = delete;
        TransactionScope& operator=(const TransactionScope&) = delete;
        TransactionScope(TransactionScope&&) = delete;
        TransactionScope& operator=(TransactionScope&&) = delete;

        [[nodiscard]] bool is_active() const noexcept
        {
            return begin_result_.has_value() && !committed_ && ctx_ && ctx_->in_transaction();
        }

        [[nodiscard]] const std::expected<void, Core::DbError>& begin_result() const noexcept
        {
            return begin_result_;
        }

        std::expected<void, Core::DbError> commit()
        {
            if (!begin_result_)
            {
                return begin_result_;
            }
            if (committed_)
            {
                return std::unexpected<Core::DbError>{
                    Core::DbError::connection_error("TransactionScope already committed")
                };
            }
            auto res = ctx_->save_changes();
            if (res)
            {
                committed_ = true;
            }
            return res;
        }

        std::expected<void, Core::DbError> rollback()
        {
            if (committed_)
            {
                return std::unexpected<Core::DbError>{
                    Core::DbError::connection_error("Cannot rollback a committed TransactionScope")
                };
            }
            if (ctx_ && ctx_->in_transaction())
            {
                return ctx_->rollback();
            }
            return {};
        }

    private:
        DbContextBase<ConnectionHandle>* ctx_{ nullptr };
        std::expected<void, Core::DbError> begin_result_{};
        bool committed_{ false };
    };

} // namespace DAL
