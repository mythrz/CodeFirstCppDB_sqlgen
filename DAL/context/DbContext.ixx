module;
#include <meta>

export module dal:db_context;

import std;
import core;
import :schema_traits;
import :mappers;
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

        // =====================================================================
        // Optional Identity Map & Snapshot ChangeTracker (Task 10.2)
        // =====================================================================

        struct TrackedEntityEntry
        {
            std::shared_ptr<void> owned_instance{};
            const void* borrowed_instance{ nullptr };
            std::function<std::optional<std::function<std::expected<void, Core::DbError>()>>(
                const std::shared_ptr<void>&, const void*)> detect_fn{};
        };

        void enable_change_tracking(bool enabled = true) noexcept
        {
            change_tracking_enabled_ = enabled;
        }

        [[nodiscard]] bool is_change_tracking_enabled() const noexcept
        {
            return change_tracking_enabled_;
        }

        [[nodiscard]] std::size_t tracked_entities_count() const noexcept
        {
            return tracked_entries_.size();
        }

        void clear_change_tracker() noexcept
        {
            tracked_entries_.clear();
        }

        void untrack_key(std::type_index type, const std::string& key_repr) noexcept
        {
            tracked_entries_.erase(std::pair<std::type_index, std::string>{ type, key_repr });
        }

        void untrack_type(std::type_index type) noexcept
        {
            std::erase_if(tracked_entries_, [type](const auto& kv) {
                return kv.first.first == type;
            });
        }

        void upsert_tracked_entry(
            std::type_index type,
            std::string key_repr,
            bool refresh_baseline,
            std::shared_ptr<void> owned,
            const void* borrowed,
            std::function<std::optional<std::function<std::expected<void, Core::DbError>()>>(
                const std::shared_ptr<void>&, const void*)> detect_fn)
        {
            auto map_key = std::pair<std::type_index, std::string>{ type, std::move(key_repr) };
            auto it = tracked_entries_.find(map_key);
            if (it == tracked_entries_.end())
            {
                tracked_entries_.emplace(
                    std::move(map_key),
                    TrackedEntityEntry{
                        std::move(owned),
                        borrowed,
                        std::move(detect_fn)
                    });
                return;
            }

            if (owned)
            {
                it->second.owned_instance = std::move(owned);
            }
            if (borrowed != nullptr)
            {
                it->second.borrowed_instance = borrowed;
            }
            if (refresh_baseline && detect_fn)
            {
                it->second.detect_fn = std::move(detect_fn);
            }
            else if (!it->second.detect_fn && detect_fn)
            {
                it->second.detect_fn = std::move(detect_fn);
            }
        }

        [[nodiscard]] std::shared_ptr<void> find_tracked_instance(
            std::type_index type,
            const std::string& key_repr) const
        {
            auto it = tracked_entries_.find(std::pair<std::type_index, std::string>{ type, key_repr });
            if (it != tracked_entries_.end())
            {
                return it->second.owned_instance;
            }
            return nullptr;
        }

        std::size_t detect_changes()
        {
            std::size_t dirty_count = 0;
            for (auto& [key, entry] : tracked_entries_)
            {
                if (!entry.detect_fn)
                {
                    continue;
                }
                if (auto op = entry.detect_fn(entry.owned_instance, entry.borrowed_instance))
                {
                    pending_ops_.push_back(std::move(*op));
                    ++dirty_count;
                }
            }
            return dirty_count;
        }

        // =====================================================================
        // Phase 14: Compile-Time Foreign-Key Relationship Navigation
        // =====================================================================

        template<typename TargetDomain, typename Self>
        [[nodiscard]] auto& find_dbset(this Self&& self)
        {
            using Derived = std::remove_cvref_t<Self>;
            static constexpr auto members = dbset_members_of<Derived>();

            template for (constexpr auto m : members)
            {
                using RawMemberType = [:std::meta::type_of(m):];
                using MemberType = std::remove_cvref_t<RawMemberType>;
                if constexpr (std::same_as<typename MemberType::DomainType, TargetDomain>)
                {
                    return self.[:m:];
                }
            }
            static_assert(
                ([]() {
                    template for (constexpr auto m : dbset_members_of<Derived>())
                    {
                        using RawMemberType = [:std::meta::type_of(m):];
                using MemberType = std::remove_cvref_t<RawMemberType>;
                        if constexpr (std::same_as<typename MemberType::DomainType, TargetDomain>)
                        {
                            return true;
                        }
                    }
                    return false;
                })(),
                "DbContext does not contain a Set<TargetDomain> member");
        }

        template<typename ChildDomain, typename ParentDomain, typename Self>
        [[nodiscard]] std::expected<std::vector<ChildDomain>, Core::DbError> query_children(
            this Self&& self,
            const ParentDomain& parent)
        {
            auto& child_set = self.template find_dbset<ChildDomain>();
            return child_set.template where_foreign_key<ParentDomain>(parent).try_to_vector();
        }

        template<typename ParentDomain, typename ChildDomain, typename Self>
        [[nodiscard]] std::expected<std::optional<ParentDomain>, Core::DbError> query_parent(
            this Self&& self,
            const ChildDomain& child)
        {
            auto& child_set = self.template find_dbset<ChildDomain>();
            auto& parent_set = self.template find_dbset<ParentDomain>();
            using ChildSetType = std::remove_cvref_t<decltype(child_set)>;
            using ParentSetType = std::remove_cvref_t<decltype(parent_set)>;
            using ChildDTO = typename ChildSetType::DTOType;
            using ParentDTO = typename ParentSetType::DTOType;

            const ChildDTO child_dto = DAL::Mappers::MapperTraits<ChildDomain, ChildDTO>::to_dto(child);
            const auto fk_val = Schema::extract_fk_value<ChildDTO, ParentDTO>(child_dto);
            return parent_set.try_find(fk_val);
        }

        template<typename TargetDomain, typename JunctionDomain, typename SourceDomain, typename Self>
        [[nodiscard]] std::expected<std::vector<TargetDomain>, Core::DbError> query_many_to_many(
            this Self&& self,
            const SourceDomain& source)
        {
            auto junctions_res = self.template query_children<JunctionDomain>(source);
            if (!junctions_res)
            {
                return std::unexpected<Core::DbError>{ junctions_res.error() };
            }

            if (junctions_res.value().empty())
            {
                return std::vector<TargetDomain>{};
            }

            auto& junction_set = self.template find_dbset<JunctionDomain>();
            auto& target_set = self.template find_dbset<TargetDomain>();
            using JunctionSetType = std::remove_cvref_t<decltype(junction_set)>;
            using TargetSetType = std::remove_cvref_t<decltype(target_set)>;
            using JunctionDTO = typename JunctionSetType::DTOType;
            using TargetDTO = typename TargetSetType::DTOType;

            if constexpr (Schema::SinglePrimaryKey<TargetDTO>)
            {
                constexpr auto pk_m = Schema::primary_key_member<TargetDTO>();
                using FkValueType = std::remove_cvref_t<decltype(
                    Schema::extract_fk_value<JunctionDTO, TargetDTO>(std::declval<const JunctionDTO&>()))>;
                std::vector<FkValueType> target_fks;
                target_fks.reserve(junctions_res.value().size());
                for (const auto& junction : junctions_res.value())
                {
                    const JunctionDTO j_dto = DAL::Mappers::MapperTraits<JunctionDomain, JunctionDTO>::to_dto(junction);
                    target_fks.push_back(Schema::extract_fk_value<JunctionDTO, TargetDTO>(j_dto));
                }
                return target_set.where_sql(Schema::col_of<pk_m>.in(target_fks)).try_to_vector();
            }
            else
            {
                std::vector<TargetDomain> results;
                results.reserve(junctions_res.value().size());
                for (const auto& junction : junctions_res.value())
                {
                    const JunctionDTO j_dto = DAL::Mappers::MapperTraits<JunctionDomain, JunctionDTO>::to_dto(junction);
                    const auto target_fk = Schema::extract_fk_value<JunctionDTO, TargetDTO>(j_dto);
                    auto target_res = target_set.try_find(target_fk);
                    if (!target_res)
                    {
                        return std::unexpected<Core::DbError>{ target_res.error() };
                    }
                    if (target_res.value().has_value())
                    {
                        results.push_back(std::move(*target_res.value()));
                    }
                }
                return results;
            }
        }

        template<typename ChildDomain, typename ParentDomain, typename Self>
        [[nodiscard]] std::expected<std::vector<ChildDomain>, Core::DbError> query_children_batch(
            this Self&& self,
            const std::vector<ParentDomain>& parents)
        {
            if (parents.empty())
            {
                return std::vector<ChildDomain>{};
            }
            auto& child_set = self.template find_dbset<ChildDomain>();
            auto& parent_set = self.template find_dbset<ParentDomain>();
            using ChildDTO = typename std::remove_cvref_t<decltype(child_set)>::DTOType;
            using ParentDTO = typename std::remove_cvref_t<decltype(parent_set)>::DTOType;

            constexpr auto fk_m = Schema::find_fk_member_referencing<ChildDTO, ParentDTO>();
            using FkValueType = std::remove_cvref_t<decltype(
                Schema::extract_fk_value<ChildDTO, ParentDTO>(std::declval<const ChildDTO&>()))>;

            std::vector<FkValueType> pks;
            pks.reserve(parents.size());
            for (const auto& parent : parents)
            {
                const auto parent_key = Schema::EntityMetadata<ParentDomain, ParentDTO>::extract_key(parent);
                pks.push_back(static_cast<FkValueType>(detail::normalize_query_val(parent_key)));
            }
            return child_set.where_sql(Schema::col_of<fk_m>.in(pks)).try_to_vector();
        }

        template<typename ParentDomain, typename ChildDomain, typename Self>
        [[nodiscard]] std::expected<std::vector<ParentDomain>, Core::DbError> query_parents_batch(
            this Self&& self,
            const std::vector<ChildDomain>& children)
        {
            if (children.empty())
            {
                return std::vector<ParentDomain>{};
            }
            auto& child_set = self.template find_dbset<ChildDomain>();
            auto& parent_set = self.template find_dbset<ParentDomain>();
            using ChildDTO = typename std::remove_cvref_t<decltype(child_set)>::DTOType;
            using ParentDTO = typename std::remove_cvref_t<decltype(parent_set)>::DTOType;

            constexpr auto pk_m = Schema::primary_key_member<ParentDTO>();
            using FkValueType = std::remove_cvref_t<decltype(
                Schema::extract_fk_value<ChildDTO, ParentDTO>(std::declval<const ChildDTO&>()))>;

            std::vector<FkValueType> fk_vals;
            fk_vals.reserve(children.size());
            for (const auto& child : children)
            {
                const ChildDTO c_dto = DAL::Mappers::MapperTraits<ChildDomain, ChildDTO>::to_dto(child);
                fk_vals.push_back(Schema::extract_fk_value<ChildDTO, ParentDTO>(c_dto));
            }
            return parent_set.where_sql(Schema::col_of<pk_m>.in(fk_vals)).try_to_vector();
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

            (void)detect_changes();

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
        // Executes fn inside a transaction (or a SQL SAVEPOINT if already in a transaction),
        // committing/releasing on success or rolling back on failure.
        std::expected<void, Core::DbError> savepoint(std::string_view name)
        {
            if (!is_connected())
            {
                return std::unexpected<Core::DbError>{
                    Core::DbError::connection_error("Cannot create savepoint: database connection is closed")
                };
            }
            if constexpr (requires(ConnectionHandle& c) { c.savepoint(name); })
            {
                return conn_->savepoint(name);
            }
            return {};
        }

        std::expected<void, Core::DbError> release_savepoint(std::string_view name)
        {
            if (!is_connected())
            {
                return std::unexpected<Core::DbError>{
                    Core::DbError::connection_error("Cannot release savepoint: database connection is closed")
                };
            }
            if constexpr (requires(ConnectionHandle& c) { c.release_savepoint(name); })
            {
                return conn_->release_savepoint(name);
            }
            return {};
        }

        std::expected<void, Core::DbError> rollback_to_savepoint(std::string_view name)
        {
            if (!is_connected())
            {
                return std::unexpected<Core::DbError>{
                    Core::DbError::connection_error("Cannot rollback savepoint: database connection is closed")
                };
            }
            if constexpr (requires(ConnectionHandle& c) { c.rollback_to_savepoint(name); })
            {
                return conn_->rollback_to_savepoint(name);
            }
            return {};
        }

        template<typename Fn>
        std::expected<void, Core::DbError> transaction(Fn&& fn)
        {
            if (!is_connected())
            {
                return std::unexpected<Core::DbError>{
                    Core::DbError::connection_error("Cannot start transaction: database connection is closed")
                };
            }

            if (in_transaction())
            {
                if constexpr (requires(ConnectionHandle& c, std::string_view n) {
                    c.savepoint(n);
                    c.release_savepoint(n);
                    c.rollback_to_savepoint(n);
                })
                {
                    const std::string sp_name = std::format("_codefirst_sp_{}", ++savepoint_depth_);
                    auto sp_res = conn_->savepoint(sp_name);
                    if (!sp_res)
                    {
                        --savepoint_depth_;
                        return sp_res;
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
                                (void)conn_->rollback_to_savepoint(sp_name);
                                --savepoint_depth_;
                                return fn_res;
                            }
                        }

                        auto rel_res = conn_->release_savepoint(sp_name);
                        --savepoint_depth_;
                        return rel_res;
                    }
                    catch (...)
                    {
                        (void)conn_->rollback_to_savepoint(sp_name);
                        --savepoint_depth_;
                        throw;
                    }
                }
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
        bool change_tracking_enabled_{ false };
        std::size_t savepoint_depth_{ 0 };
        std::map<std::pair<std::type_index, std::string>, TrackedEntityEntry> tracked_entries_{};
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
