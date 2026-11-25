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
        DbContextBase(const DbContextBase&) = delete ("DbContextBase is non-copyable because DbSet members bind 'this'");
        DbContextBase& operator=(const DbContextBase&) = delete ("DbContextBase is non-copyable because DbSet members bind 'this'");
        DbContextBase(DbContextBase&&) = delete ("DbContextBase is non-movable because DbSet members bind 'this'");
        DbContextBase& operator=(DbContextBase&&) = delete ("DbContextBase is non-movable because DbSet members bind 'this'");

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

        // Lists user tables in the underlying database (Phase 38)
        [[nodiscard]] auto list_tables() const
            requires requires(const ConnectionHandle& c) { c.list_tables(); }
        {
            return conn_->list_tables();
        }

        // Returns column metadata for the specified table (Phase 38)
        [[nodiscard]] auto table_info(std::string_view table_name) const
            requires requires(const ConnectionHandle& c, std::string_view t) { c.table_info(t); }
        {
            return conn_->table_info(table_name);
        }

        // Executes a raw SQL statement against the underlying connection (Phase 38)
        [[nodiscard]] auto execute_raw(std::string_view sql)
            requires requires(ConnectionHandle& c, std::string_view s) { c.execute_raw(s); }
        {
            return conn_->execute_raw(sql);
        }

        // Executes a parameterized SQL query returning dynamic rows (Phase 41)
        template<typename CellT>
        [[nodiscard]] auto query_rows(std::string_view sql, std::span<const CellT> params) const
            requires requires(const ConnectionHandle& c, std::string_view s, std::span<const CellT> p) { c.query_rows(s, p); }
        {
            return conn_->query_rows(sql, params);
        }

        [[nodiscard]] auto query_rows(std::string_view sql) const
            requires requires(const ConnectionHandle& c, std::string_view s) { c.query_rows(s); }
        {
            return conn_->query_rows(sql);
        }

        // Executes a parameterized SQL statement without result rows (Phase 41)
        template<typename CellT>
        [[nodiscard]] auto execute_prepared(std::string_view sql, std::span<const CellT> params)
            requires requires(ConnectionHandle& c, std::string_view s, std::span<const CellT> p) { c.execute_prepared(s, p); }
        {
            return conn_->execute_prepared(sql, params);
        }

        [[nodiscard]] auto execute_prepared(std::string_view sql)
            requires requires(ConnectionHandle& c, std::string_view s) { c.execute_prepared(s); }
        {
            return conn_->execute_prepared(sql);
        }

        // Type alias for zero-boilerplate DbSet declarations inside derived DbContext classes
        template<typename Domain, typename DTO = Schema::SchemaOf_t<Domain>, typename KeyType = Schema::EntityKey_t<Domain, DTO>>
        using Set = DbSet<Domain, DTO, ConnectionHandle, KeyType>;

        // Factory helper to construct a DbSet bound to this context (or standalone for const contexts)
        template<typename Domain, typename DTO = Schema::SchemaOf_t<Domain>, typename KeyType = Schema::EntityKey_t<Domain, DTO>>
        [[nodiscard]] Set<Domain, DTO, KeyType> create_set()
        {
            return Set<Domain, DTO, KeyType>(this);
        }

        template<typename Domain, typename DTO = Schema::SchemaOf_t<Domain>, typename KeyType = Schema::EntityKey_t<Domain, DTO>>
        [[nodiscard]] Set<Domain, DTO, KeyType> create_set() const
        {
            return Set<Domain, DTO, KeyType>(conn_);
        }

        [[nodiscard]] std::weak_ptr<detail::ContextState> weak_context_state() const noexcept
        {
            return state_;
        }

        [[nodiscard]] detail::BaselineCheckpoint capture_checkpoint() const
        {
            return state_->capture_checkpoint();
        }

        void restore_checkpoint_baselines(const detail::BaselineCheckpoint& cp)
        {
            state_->restore_baselines(cp);
        }

        // =====================================================================
        // Reflection-Driven Schema Creation & Deletion (Topologically Sorted)
        // =====================================================================

        template<typename DerivedType>
        static consteval std::size_t dbset_member_count()
        {
            static constexpr auto members =
                std::define_static_array(std::meta::nonstatic_data_members_of(^^DerivedType, std::meta::access_context::unchecked()));
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
            static constexpr auto members =
                std::define_static_array(std::meta::nonstatic_data_members_of(^^DerivedType, std::meta::access_context::unchecked()));
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
            return []<std::size_t... Is>(std::index_sequence<Is...>) consteval
            {
                return Schema::topological_dto_order<
                    typename std::remove_cvref_t<typename[:std::meta::type_of(dbset_members[Is]):]>::DTOType...>();
            }(std::make_index_sequence<K>{});
        }

        template<typename Self>
        [[nodiscard]] std::expected<void, Core::DbError> ensure_created(this Self&& self)
        {
            if (!self.is_connected())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error(
                    "Cannot create schema: database connection is closed") };
            }

            using DerivedType = std::remove_cvref_t<Self>;
            static constexpr auto dbset_members = dbset_members_of<DerivedType>();
            static constexpr auto order = topological_dbset_order<DerivedType>();

            {
                detail::WriteBatchTxGuard<ConnectionHandle> tx_guard(self.conn_);
                if (auto begin_res = tx_guard.begin(); !begin_res)
                {
                    return begin_res;
                }

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

                if (auto commit_res = tx_guard.commit(); !commit_res)
                {
                    return commit_res;
                }
            }

            return self.conn_->enable_foreign_keys();
        }

        template<typename... Domains>
        [[nodiscard]] std::expected<void, Core::DbError> ensure_created_for()
        {
            if (!is_connected())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error(
                    "Cannot create schema: database connection is closed") };
            }

            static constexpr auto order = Schema::topological_dto_order<Schema::SchemaOf_t<Domains>...>();
            std::array<std::function<std::expected<void, Core::DbError>()>, sizeof...(Domains)> creators{
                [this]()
                {
                    return conn_->template ensure_table_created<Schema::SchemaOf_t<Domains>>();
                }...
            };

            {
                detail::WriteBatchTxGuard<ConnectionHandle> tx_guard(conn_);
                if (auto begin_res = tx_guard.begin(); !begin_res)
                {
                    return begin_res;
                }

                for (std::size_t idx : order)
                {
                    auto res = creators[idx]();
                    if (!res)
                    {
                        return res;
                    }
                }

                if (auto commit_res = tx_guard.commit(); !commit_res)
                {
                    return commit_res;
                }
            }

            return conn_->enable_foreign_keys();
        }

        [[nodiscard]] std::expected<std::int32_t, Core::DbError> try_user_version() const
        {
            if (!is_connected())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error(
                    "Cannot read schema version: database connection is closed") };
            }
            if constexpr (requires(const ConnectionHandle& c) { c.try_user_version(); })
            {
                return conn_->try_user_version();
            }
            return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Connection does not support PRAGMA user_version") };
        }

        [[nodiscard]] std::expected<void, Core::DbError> set_user_version(std::int32_t version)
        {
            if (!is_connected())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error(
                    "Cannot update schema version: database connection is closed") };
            }
            if constexpr (requires(ConnectionHandle& c, std::int32_t v) { c.set_user_version(v); })
            {
                return conn_->set_user_version(version);
            }
            return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Connection does not support PRAGMA user_version") };
        }

        // Transactional PRAGMA user_version schema-versioning / migration hook.
        // Ensures base tables exist, invokes migration_fn(current_version, target_version) when
        // current_version < target_version, and persists target_version atomically on commit.
        template<typename Self, typename Fn>
        [[nodiscard]] std::expected<void, Core::DbError> migrate_to(this Self&& self, std::int32_t target_version, Fn&& migration_fn)
        {
            if (!self.is_connected())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error(
                    "Cannot migrate schema: database connection is closed") };
            }
            if (target_version < 0)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation(
                    "Target schema user_version cannot be negative") };
            }

            auto current_res = self.try_user_version();
            if (!current_res)
            {
                return std::unexpected<Core::DbError>{ current_res.error() };
            }
            const std::int32_t current_version = *current_res;
            if (current_version > target_version)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation(
                    std::format(
                        "Database schema version {} is newer than target version {} (downgrade not supported)",
                        current_version,
                        target_version)) };
            }
            if (current_version == target_version)
            {
                return self.ensure_created();
            }

            {
                detail::WriteBatchTxGuard<ConnectionHandle> tx_guard(self.conn_);
                if (auto begin_res = tx_guard.begin(); !begin_res)
                {
                    return begin_res;
                }

                if (auto ec_res = self.ensure_created(); !ec_res)
                {
                    return ec_res;
                }

                using FnResult = std::invoke_result_t<Fn, std::int32_t, std::int32_t>;
                if constexpr (std::is_void_v<FnResult>)
                {
                    std::invoke(std::forward<Fn>(migration_fn), current_version, target_version);
                }
                else
                {
                    auto mig_res = std::invoke(std::forward<Fn>(migration_fn), current_version, target_version);
                    if (!mig_res)
                    {
                        return mig_res;
                    }
                }

                if (auto ver_res = self.set_user_version(target_version); !ver_res)
                {
                    return ver_res;
                }

                if (auto commit_res = tx_guard.commit(); !commit_res)
                {
                    return commit_res;
                }
            }

            return self.conn_->enable_foreign_keys();
        }

        template<typename Self>
        [[nodiscard]] std::expected<void, Core::DbError> ensure_deleted(this Self&& self)
        {
            if (!self.is_connected())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error(
                    "Cannot drop schema: database connection is closed") };
            }

            using DerivedType = std::remove_cvref_t<Self>;
            static constexpr auto dbset_members = dbset_members_of<DerivedType>();
            static constexpr auto order = Schema::reverse_index_array(topological_dbset_order<DerivedType>());

            detail::WriteBatchTxGuard<ConnectionHandle> tx_guard(self.conn_);
            if (auto begin_res = tx_guard.begin(); !begin_res)
            {
                return begin_res;
            }

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

            if (!result)
            {
                return result;
            }
            return tx_guard.commit();
        }

        template<typename... Domains>
        [[nodiscard]] std::expected<void, Core::DbError> ensure_deleted_for()
        {
            if (!is_connected())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error(
                    "Cannot drop schema: database connection is closed") };
            }

            static constexpr auto order = Schema::reverse_index_array(Schema::topological_dto_order<Schema::SchemaOf_t<Domains>...>());
            std::array<std::function<std::expected<void, Core::DbError>()>, sizeof...(Domains)> droppers{
                [this]()
                {
                    return conn_->template ensure_table_dropped<Schema::SchemaOf_t<Domains>>();
                }...
            };

            detail::WriteBatchTxGuard<ConnectionHandle> tx_guard(conn_);
            if (auto begin_res = tx_guard.begin(); !begin_res)
            {
                return begin_res;
            }

            for (std::size_t idx : order)
            {
                auto res = droppers[idx]();
                if (!res)
                {
                    return res;
                }
            }

            return tx_guard.commit();
        }

        // =====================================================================
        // Unit of Work Staging & Transaction Controls
        // =====================================================================

        void defer_changes(bool enabled = true) noexcept
        {
            state_->defer_changes_ = enabled;
        }

        [[nodiscard]] bool is_deferred() const noexcept
        {
            return state_->defer_changes_;
        }

        void enqueue_op(std::move_only_function<std::expected<void, Core::DbError>()> op)
        {
            state_->pending_ops_.push_back(std::move(op));
        }

        [[nodiscard]] std::size_t pending_changes_count() const noexcept
        {
            return state_->pending_ops_.size();
        }

        void discard_changes() noexcept
        {
            state_->pending_ops_.clear();
        }

        // =====================================================================
        // Optional Identity Map & Snapshot ChangeTracker (Task 10.2)
        // =====================================================================

        using TrackedEntityEntry = detail::TrackedEntityEntry;

        void enable_change_tracking(bool enabled = true) noexcept
        {
            state_->change_tracking_enabled_ = enabled;
        }

        [[nodiscard]] bool is_change_tracking_enabled() const noexcept
        {
            return state_->change_tracking_enabled_;
        }

        [[nodiscard]] std::size_t tracked_entities_count() const noexcept
        {
            return state_->tracked_entries_.size();
        }

        void clear_change_tracker() noexcept
        {
            state_->tracked_entries_.clear();
        }

        void untrack_key(std::type_index type, const std::string& key_repr) noexcept
        {
            state_->untrack_key(type, key_repr);
        }

        void untrack_type(std::type_index type) noexcept
        {
            state_->untrack_type(type);
        }

        void upsert_tracked_entry(
            std::type_index type,
            std::string key_repr,
            bool refresh_baseline,
            std::shared_ptr<void> owned,
            std::function<std::optional<std::function<std::expected<void, Core::DbError>()>>(const std::shared_ptr<void>&)> detect_fn,
            std::function<std::shared_ptr<void>()> snapshot_baseline_fn = {},
            std::function<void(const std::shared_ptr<void>&)> restore_baseline_fn = {})
        {
            state_->upsert_tracked_entry(
                type,
                std::move(key_repr),
                refresh_baseline,
                std::move(owned),
                std::move(detect_fn),
                std::move(snapshot_baseline_fn),
                std::move(restore_baseline_fn));
        }

        [[nodiscard]] std::shared_ptr<void> find_tracked_instance(std::type_index type, const std::string& key_repr) const
        {
            return state_->find_tracked_instance(type, key_repr);
        }

        std::size_t detect_changes()
        {
            std::size_t dirty_count = 0;
            for (auto& [key, entry] : state_->tracked_entries_)
            {
                if (!entry.detect_fn)
                {
                    continue;
                }
                if (auto op = entry.detect_fn(entry.owned_instance))
                {
                    state_->pending_ops_.push_back(std::move(*op));
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
                (
                    []()
                    {
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
        [[nodiscard]] std::expected<std::vector<ChildDomain>, Core::DbError> query_children(this Self&& self, const ParentDomain& parent)
        {
            auto& child_set = self.template find_dbset<ChildDomain>();
            return child_set.template where_foreign_key<ParentDomain>(parent).try_to_vector();
        }

        template<typename ParentDomain, typename ChildDomain, typename Self>
        [[nodiscard]] std::expected<std::optional<ParentDomain>, Core::DbError> query_parent(this Self&& self, const ChildDomain& child)
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
            this Self&& self, const SourceDomain& source)
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
                using FkValueType =
                    std::remove_cvref_t<decltype(Schema::extract_fk_value<JunctionDTO, TargetDTO>(std::declval<const JunctionDTO&>()))>;
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
            this Self&& self, const std::vector<ParentDomain>& parents)
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
            using FkValueType =
                std::remove_cvref_t<decltype(Schema::extract_fk_value<ChildDTO, ParentDTO>(std::declval<const ChildDTO&>()))>;

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
            this Self&& self, const std::vector<ChildDomain>& children)
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
            using FkValueType =
                std::remove_cvref_t<decltype(Schema::extract_fk_value<ChildDTO, ParentDTO>(std::declval<const ChildDTO&>()))>;

            std::vector<FkValueType> fk_vals;
            fk_vals.reserve(children.size());
            for (const auto& child : children)
            {
                const ChildDTO c_dto = DAL::Mappers::MapperTraits<ChildDomain, ChildDTO>::to_dto(child);
                fk_vals.push_back(Schema::extract_fk_value<ChildDTO, ParentDTO>(c_dto));
            }
            return parent_set.where_sql(Schema::col_of<pk_m>.in(fk_vals)).try_to_vector();
        }

        template<typename ChildDomain, typename ParentDomain, typename Self>
        [[nodiscard]] std::expected<std::vector<std::pair<ParentDomain, std::vector<ChildDomain>>>, Core::DbError> query_children_grouped(
            this Self&& self, const std::vector<ParentDomain>& parents)
        {
            using PairType = std::pair<ParentDomain, std::vector<ChildDomain>>;
            std::vector<PairType> grouped;
            grouped.reserve(parents.size());
            if (parents.empty())
            {
                return grouped;
            }

            auto children_res = self.template query_children_batch<ChildDomain>(parents);
            if (!children_res)
            {
                return std::unexpected<Core::DbError>{ children_res.error() };
            }

            auto& child_set = self.template find_dbset<ChildDomain>();
            auto& parent_set = self.template find_dbset<ParentDomain>();
            using ChildDTO = typename std::remove_cvref_t<decltype(child_set)>::DTOType;
            using ParentDTO = typename std::remove_cvref_t<decltype(parent_set)>::DTOType;
            using FkValueType =
                std::remove_cvref_t<decltype(Schema::extract_fk_value<ChildDTO, ParentDTO>(std::declval<const ChildDTO&>()))>;

            std::map<FkValueType, std::vector<ChildDomain>> buckets;
            for (auto& child : *children_res)
            {
                const ChildDTO c_dto = DAL::Mappers::MapperTraits<ChildDomain, ChildDTO>::to_dto(child);
                const auto fk_val = Schema::extract_fk_value<ChildDTO, ParentDTO>(c_dto);
                buckets[fk_val].push_back(std::move(child));
            }

            for (const auto& parent : parents)
            {
                const auto parent_key = Schema::EntityMetadata<ParentDomain, ParentDTO>::extract_key(parent);
                const auto pk_val = static_cast<FkValueType>(detail::normalize_query_val(parent_key));
                auto it = buckets.find(pk_val);
                if (it != buckets.end())
                {
                    grouped.emplace_back(parent, std::move(it->second));
                }
                else
                {
                    grouped.emplace_back(parent, std::vector<ChildDomain>{});
                }
            }
            return grouped;
        }

        template<typename TargetDomain, typename JunctionDomain, typename SourceDomain, typename Self>
        [[nodiscard]] std::expected<std::vector<TargetDomain>, Core::DbError> query_many_to_many_batch(
            this Self&& self, const std::vector<SourceDomain>& sources)
        {
            if (sources.empty())
            {
                return std::vector<TargetDomain>{};
            }

            auto junctions_res = self.template query_children_batch<JunctionDomain>(sources);
            if (!junctions_res)
            {
                return std::unexpected<Core::DbError>{ junctions_res.error() };
            }
            if (junctions_res->empty())
            {
                return std::vector<TargetDomain>{};
            }

            auto& junction_set = self.template find_dbset<JunctionDomain>();
            auto& target_set = self.template find_dbset<TargetDomain>();
            using JunctionDTO = typename std::remove_cvref_t<decltype(junction_set)>::DTOType;
            using TargetDTO = typename std::remove_cvref_t<decltype(target_set)>::DTOType;

            if constexpr (Schema::SinglePrimaryKey<TargetDTO>)
            {
                constexpr auto pk_m = Schema::primary_key_member<TargetDTO>();
                using FkValueType =
                    std::remove_cvref_t<decltype(Schema::extract_fk_value<JunctionDTO, TargetDTO>(std::declval<const JunctionDTO&>()))>;
                std::vector<FkValueType> target_fks;
                target_fks.reserve(junctions_res->size());
                for (const auto& junction : *junctions_res)
                {
                    const JunctionDTO j_dto = DAL::Mappers::MapperTraits<JunctionDomain, JunctionDTO>::to_dto(junction);
                    target_fks.push_back(Schema::extract_fk_value<JunctionDTO, TargetDTO>(j_dto));
                }
                return target_set.where_sql(Schema::col_of<pk_m>.in(target_fks)).try_to_vector();
            }
            else
            {
                using FkValueType =
                    std::remove_cvref_t<decltype(Schema::extract_fk_value<JunctionDTO, TargetDTO>(std::declval<const JunctionDTO&>()))>;
                std::set<FkValueType> seen_fks;
                std::vector<TargetDomain> results;
                results.reserve(junctions_res->size());
                for (const auto& junction : *junctions_res)
                {
                    const JunctionDTO j_dto = DAL::Mappers::MapperTraits<JunctionDomain, JunctionDTO>::to_dto(junction);
                    const auto target_fk = Schema::extract_fk_value<JunctionDTO, TargetDTO>(j_dto);
                    if (!seen_fks.insert(target_fk).second)
                    {
                        continue;
                    }
                    auto target_res = target_set.try_find(target_fk);
                    if (!target_res)
                    {
                        return std::unexpected<Core::DbError>{ target_res.error() };
                    }
                    if (target_res->has_value())
                    {
                        results.push_back(std::move(*target_res.value()));
                    }
                }
                return results;
            }
        }

        template<typename TargetDomain, typename JunctionDomain, typename SourceDomain, typename Self>
        [[nodiscard]] std::expected<std::vector<std::pair<SourceDomain, std::vector<TargetDomain>>>, Core::DbError>
        query_many_to_many_grouped(this Self&& self, const std::vector<SourceDomain>& sources)
        {
            using PairType = std::pair<SourceDomain, std::vector<TargetDomain>>;
            std::vector<PairType> grouped;
            grouped.reserve(sources.size());
            if (sources.empty())
            {
                return grouped;
            }

            auto junctions_res = self.template query_children_batch<JunctionDomain>(sources);
            if (!junctions_res)
            {
                return std::unexpected<Core::DbError>{ junctions_res.error() };
            }

            auto& source_set = self.template find_dbset<SourceDomain>();
            auto& junction_set = self.template find_dbset<JunctionDomain>();
            auto& target_set = self.template find_dbset<TargetDomain>();
            using SourceDTO = typename std::remove_cvref_t<decltype(source_set)>::DTOType;
            using JunctionDTO = typename std::remove_cvref_t<decltype(junction_set)>::DTOType;
            using TargetDTO = typename std::remove_cvref_t<decltype(target_set)>::DTOType;

            using SourceFkType =
                std::remove_cvref_t<decltype(Schema::extract_fk_value<JunctionDTO, SourceDTO>(std::declval<const JunctionDTO&>()))>;
            using TargetFkType =
                std::remove_cvref_t<decltype(Schema::extract_fk_value<JunctionDTO, TargetDTO>(std::declval<const JunctionDTO&>()))>;

            std::map<TargetFkType, TargetDomain> target_map;
            if (!junctions_res->empty())
            {
                if constexpr (Schema::SinglePrimaryKey<TargetDTO>)
                {
                    constexpr auto pk_m = Schema::primary_key_member<TargetDTO>();
                    std::set<TargetFkType> seen_fks;
                    std::vector<TargetFkType> target_fks;
                    target_fks.reserve(junctions_res->size());
                    for (const auto& junction : *junctions_res)
                    {
                        const JunctionDTO j_dto = DAL::Mappers::MapperTraits<JunctionDomain, JunctionDTO>::to_dto(junction);
                        const auto tfk = Schema::extract_fk_value<JunctionDTO, TargetDTO>(j_dto);
                        if (seen_fks.insert(tfk).second)
                        {
                            target_fks.push_back(tfk);
                        }
                    }
                    auto targets_res = target_set.where_sql(Schema::col_of<pk_m>.in(target_fks)).try_to_vector();
                    if (!targets_res)
                    {
                        return std::unexpected<Core::DbError>{ targets_res.error() };
                    }
                    for (auto& t : *targets_res)
                    {
                        const auto tkey = Schema::EntityMetadata<TargetDomain, TargetDTO>::extract_key(t);
                        const auto raw_pk = static_cast<TargetFkType>(detail::normalize_query_val(tkey));
                        target_map.emplace(raw_pk, std::move(t));
                    }
                }
                else
                {
                    for (const auto& junction : *junctions_res)
                    {
                        const JunctionDTO j_dto = DAL::Mappers::MapperTraits<JunctionDomain, JunctionDTO>::to_dto(junction);
                        const auto target_fk = Schema::extract_fk_value<JunctionDTO, TargetDTO>(j_dto);
                        if (target_map.contains(target_fk))
                        {
                            continue;
                        }
                        auto target_res = target_set.try_find(target_fk);
                        if (!target_res)
                        {
                            return std::unexpected<Core::DbError>{ target_res.error() };
                        }
                        if (target_res->has_value())
                        {
                            target_map.emplace(target_fk, std::move(*target_res.value()));
                        }
                    }
                }
            }

            std::map<SourceFkType, std::vector<TargetDomain>> buckets;
            for (const auto& junction : *junctions_res)
            {
                const JunctionDTO j_dto = DAL::Mappers::MapperTraits<JunctionDomain, JunctionDTO>::to_dto(junction);
                const auto src_fk = Schema::extract_fk_value<JunctionDTO, SourceDTO>(j_dto);
                const auto tgt_fk = Schema::extract_fk_value<JunctionDTO, TargetDTO>(j_dto);
                auto it = target_map.find(tgt_fk);
                if (it != target_map.end())
                {
                    buckets[src_fk].push_back(it->second);
                }
            }

            for (const auto& source : sources)
            {
                const auto src_key = Schema::EntityMetadata<SourceDomain, SourceDTO>::extract_key(source);
                const auto raw_pk = static_cast<SourceFkType>(detail::normalize_query_val(src_key));
                auto it = buckets.find(raw_pk);
                if (it != buckets.end())
                {
                    grouped.emplace_back(source, std::move(it->second));
                }
                else
                {
                    grouped.emplace_back(source, std::vector<TargetDomain>{});
                }
            }
            return grouped;
        }

        [[nodiscard]] std::expected<void, Core::DbError> begin_transaction()
        {
            if (!is_connected())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error(
                    "Cannot begin transaction: database connection is closed") };
            }
            auto cp = state_->capture_checkpoint();
            auto res = conn_->begin_transaction();
            if (res)
            {
                state_->outer_tx_checkpoint_ = std::move(cp);
            }
            return res;
        }

        [[nodiscard]] std::expected<void, Core::DbError> commit()
        {
            if (!is_connected())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error(
                    "Cannot commit transaction: database connection is closed") };
            }
            auto res = conn_->commit();
            if (res)
            {
                state_->outer_tx_checkpoint_.reset();
            }
            return res;
        }

        [[nodiscard]] std::expected<void, Core::DbError> rollback()
        {
            if (!is_connected())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error(
                    "Cannot rollback transaction: database connection is closed") };
            }
            auto res = conn_->rollback();
            if (state_->outer_tx_checkpoint_.has_value())
            {
                state_->restore_baselines(*state_->outer_tx_checkpoint_);
                truncate_pending_changes(state_->outer_tx_checkpoint_->saved_pending_size);
                state_->outer_tx_checkpoint_.reset();
            }
            return res;
        }

        [[nodiscard]] bool in_transaction() const noexcept
        {
            return conn_ && conn_->in_transaction();
        }

        // Unit of Work save_changes: executes staged operations atomically in a transaction
        // (or a savepoint when already inside an outer transaction) with automatic rollback
        // of both database state and ChangeTracker baselines on failure or exception.
        [[nodiscard]] virtual std::expected<void, Core::DbError> save_changes()
        {
            if (!is_connected())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error(
                    "Cannot save changes: database connection is closed") };
            }

            const std::size_t base_idx = state_->current_pending_base();
            const auto save_cp = state_->capture_checkpoint();
            const bool had_staged_ops = (state_->pending_ops_.size() > base_idx);
            if (!had_staged_ops)
            {
                (void)detect_changes();
                if (state_->pending_ops_.size() <= base_idx)
                {
                    return {};
                }
            }

            auto run_pending_range = [&](std::size_t from_idx) -> std::expected<void, Core::DbError>
            {
                for (std::size_t i = from_idx; i < state_->pending_ops_.size(); ++i)
                {
                    auto& op = state_->pending_ops_[i];
                    if (!op)
                    {
                        continue;
                    }
                    auto op_res = op();
                    if (!op_res)
                    {
                        return op_res;
                    }
                }
                return {};
            };

            auto run_all_ops = [&]() -> std::expected<void, Core::DbError>
            {
                if (auto res = run_pending_range(base_idx); !res)
                {
                    return res;
                }
                if (had_staged_ops)
                {
                    const std::size_t after_staged_idx = state_->pending_ops_.size();
                    (void)detect_changes();
                    if (auto res = run_pending_range(after_staged_idx); !res)
                    {
                        return res;
                    }
                }
                return {};
            };

            const bool started_tx = !in_transaction();
            if (started_tx)
            {
                auto tx_res = begin_transaction();
                if (!tx_res)
                {
                    state_->restore_baselines(save_cp);
                    truncate_pending_changes(base_idx);
                    return tx_res;
                }

                try
                {
                    if (auto op_res = run_all_ops(); !op_res)
                    {
                        if (in_transaction())
                        {
                            (void)rollback();
                        }
                        state_->restore_baselines(save_cp);
                        truncate_pending_changes(base_idx);
                        return op_res;
                    }

                    truncate_pending_changes(base_idx);
                    if (in_transaction())
                    {
                        auto commit_res = commit();
                        if (!commit_res)
                        {
                            if (in_transaction())
                            {
                                (void)rollback();
                            }
                            state_->restore_baselines(save_cp);
                            return commit_res;
                        }
                    }
                    return {};
                } catch (...)
                {
                    if (in_transaction())
                    {
                        (void)rollback();
                    }
                    state_->restore_baselines(save_cp);
                    truncate_pending_changes(base_idx);
                    throw;
                }
            }

            // Already inside an outer transaction: flush only operations at or after the active
            // savepoint base so an inner save_changes() never consumes outer staged operations.
            auto sp_res = acquire_savepoint();
            if (!sp_res)
            {
                state_->restore_baselines(save_cp);
                truncate_pending_changes(base_idx);
                return std::unexpected<Core::DbError>{ sp_res.error() };
            }
            const std::string& sp_name = *sp_res;

            try
            {
                if (auto op_res = run_all_ops(); !op_res)
                {
                    (void)rollback_to_savepoint(sp_name);
                    release_savepoint_depth();
                    state_->restore_baselines(save_cp);
                    truncate_pending_changes(base_idx);
                    return op_res;
                }

                truncate_pending_changes(base_idx);
                auto rel_res = release_savepoint(sp_name);
                if (!rel_res)
                {
                    (void)rollback_to_savepoint(sp_name);
                    release_savepoint_depth();
                    state_->restore_baselines(save_cp);
                    return rel_res;
                }
                release_savepoint_depth();
                return {};
            } catch (...)
            {
                (void)rollback_to_savepoint(sp_name);
                release_savepoint_depth();
                state_->restore_baselines(save_cp);
                truncate_pending_changes(base_idx);
                throw;
            }
        }

        // Scoped transaction helper: begins a transaction, runs fn(), flushes staged changes,
        // Executes fn inside a transaction (or a SQL SAVEPOINT if already in a transaction),
        // committing/releasing on success or rolling back on failure.
        [[nodiscard]] std::expected<void, Core::DbError> savepoint(std::string_view name)
        {
            if (!is_connected())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error(
                    "Cannot create savepoint: database connection is closed") };
            }
            if constexpr (requires(ConnectionHandle& c) { c.savepoint(name); })
            {
                return conn_->savepoint(name);
            }
            return {};
        }

        [[nodiscard]] std::expected<void, Core::DbError> release_savepoint(std::string_view name)
        {
            if (!is_connected())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error(
                    "Cannot release savepoint: database connection is closed") };
            }
            if constexpr (requires(ConnectionHandle& c) { c.release_savepoint(name); })
            {
                return conn_->release_savepoint(name);
            }
            return {};
        }

        [[nodiscard]] std::expected<void, Core::DbError> rollback_to_savepoint(std::string_view name)
        {
            if (!is_connected())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error(
                    "Cannot rollback savepoint: database connection is closed") };
            }
            if constexpr (requires(ConnectionHandle& c) { c.rollback_to_savepoint(name); })
            {
                return conn_->rollback_to_savepoint(name);
            }
            return {};
        }

        [[nodiscard]] std::expected<void, Core::DbError> flush_pending_since(std::size_t start_idx)
        {
            if (start_idx >= state_->pending_ops_.size())
            {
                return {};
            }
            for (std::size_t i = start_idx; i < state_->pending_ops_.size(); ++i)
            {
                auto& op = state_->pending_ops_[i];
                if (!op)
                {
                    continue;
                }
                auto res = op();
                if (!res)
                {
                    truncate_pending_changes(start_idx);
                    return res;
                }
            }
            truncate_pending_changes(start_idx);
            return {};
        }

        void push_pending_base(std::size_t base_idx)
        {
            state_->pending_base_stack_.push_back(base_idx);
        }

        void pop_pending_base() noexcept
        {
            if (!state_->pending_base_stack_.empty())
            {
                state_->pending_base_stack_.pop_back();
            }
        }

        [[nodiscard]] std::expected<std::string, Core::DbError> acquire_savepoint()
        {
            const std::string sp_name = std::format("_codefirst_sp_{}", ++detail::g_context_sp_counter);
            ++savepoint_depth_;
            auto res = savepoint(sp_name);
            if (!res)
            {
                --savepoint_depth_;
                return std::unexpected<Core::DbError>{ res.error() };
            }
            return sp_name;
        }

        void release_savepoint_depth() noexcept
        {
            if (savepoint_depth_ > 0)
            {
                --savepoint_depth_;
            }
        }

        void truncate_pending_changes(std::size_t size) noexcept
        {
            if (size < state_->pending_ops_.size())
            {
                state_->pending_ops_.resize(size);
            }
        }

        [[nodiscard]] static constexpr bool supports_savepoints() noexcept
        {
            return requires(ConnectionHandle& c, std::string_view n) {
                c.savepoint(n);
                c.release_savepoint(n);
                c.rollback_to_savepoint(n);
            };
        }

        template<typename Fn>
        [[nodiscard]] std::expected<void, Core::DbError> transaction(Fn&& fn)
        {
            if (!is_connected())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error(
                    "Cannot start transaction: database connection is closed") };
            }

            if (in_transaction())
            {
                if constexpr (supports_savepoints())
                {
                    const auto sp_cp = state_->capture_checkpoint();
                    const std::string sp_name = std::format("_codefirst_sp_{}", ++detail::g_context_sp_counter);
                    ++savepoint_depth_;
                    const std::size_t saved_pending_size = sp_cp.saved_pending_size;
                    auto sp_res = conn_->savepoint(sp_name);
                    if (!sp_res)
                    {
                        --savepoint_depth_;
                        return sp_res;
                    }

                    push_pending_base(saved_pending_size);
                    struct PendingBasePopGuard
                    {
                        DbContextBase* ctx;
                        ~PendingBasePopGuard() noexcept
                        {
                            ctx->pop_pending_base();
                        }
                    } base_guard{ this };

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
                                truncate_pending_changes(saved_pending_size);
                                (void)conn_->rollback_to_savepoint(sp_name);
                                --savepoint_depth_;
                                state_->restore_baselines(sp_cp);
                                return fn_res;
                            }
                        }

                        if (state_->pending_ops_.size() > saved_pending_size)
                        {
                            auto flush_res = flush_pending_since(saved_pending_size);
                            if (!flush_res)
                            {
                                (void)conn_->rollback_to_savepoint(sp_name);
                                --savepoint_depth_;
                                state_->restore_baselines(sp_cp);
                                return flush_res;
                            }
                        }

                        (void)detect_changes();
                        if (state_->pending_ops_.size() > saved_pending_size)
                        {
                            auto flush_res = flush_pending_since(saved_pending_size);
                            if (!flush_res)
                            {
                                (void)conn_->rollback_to_savepoint(sp_name);
                                --savepoint_depth_;
                                state_->restore_baselines(sp_cp);
                                return flush_res;
                            }
                        }

                        auto rel_res = conn_->release_savepoint(sp_name);
                        if (!rel_res)
                        {
                            (void)conn_->rollback_to_savepoint(sp_name);
                            --savepoint_depth_;
                            state_->restore_baselines(sp_cp);
                            return rel_res;
                        }
                        --savepoint_depth_;
                        return {};
                    } catch (...)
                    {
                        truncate_pending_changes(saved_pending_size);
                        (void)conn_->rollback_to_savepoint(sp_name);
                        --savepoint_depth_;
                        state_->restore_baselines(sp_cp);
                        throw;
                    }
                }
            }

            const auto tx_cp = state_->capture_checkpoint();
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
                        state_->restore_baselines(tx_cp);
                        truncate_pending_changes(tx_cp.saved_pending_size);
                        return fn_res;
                    }
                }

                auto sc_res = save_changes();
                if (!sc_res)
                {
                    if (in_transaction())
                    {
                        (void)rollback();
                    }
                    state_->restore_baselines(tx_cp);
                    truncate_pending_changes(tx_cp.saved_pending_size);
                    return sc_res;
                }

                if (in_transaction())
                {
                    auto commit_res = commit();
                    if (!commit_res)
                    {
                        if (in_transaction())
                        {
                            (void)rollback();
                        }
                        state_->restore_baselines(tx_cp);
                        truncate_pending_changes(tx_cp.saved_pending_size);
                        return commit_res;
                    }
                }
                return {};
            } catch (...)
            {
                if (in_transaction())
                {
                    (void)rollback();
                }
                state_->restore_baselines(tx_cp);
                truncate_pending_changes(tx_cp.saved_pending_size);
                throw;
            }
        }

    protected:
        std::shared_ptr<ConnectionHandle> conn_;
        std::shared_ptr<detail::ContextState> state_{ std::make_shared<detail::ContextState>() };
        std::size_t savepoint_depth_{ 0 };
    };

    // RAII transaction guard that automatically rolls back on scope exit if not committed
    template<typename ConnectionHandle>
        requires Repositories::DatabaseConnection<ConnectionHandle>
    class TransactionScope
    {
    public:
        explicit TransactionScope(DbContextBase<ConnectionHandle>& ctx)
            : ctx_(&ctx)
            , saved_pending_size_(ctx.pending_changes_count())
            , scope_checkpoint_(ctx.capture_checkpoint())
        {
            if (ctx_->in_transaction() && DbContextBase<ConnectionHandle>::supports_savepoints())
            {
                is_savepoint_ = true;
                auto sp_res = ctx_->acquire_savepoint();
                if (sp_res)
                {
                    savepoint_name_ = std::move(*sp_res);
                    ctx_->push_pending_base(saved_pending_size_);
                    pushed_pending_base_ = true;
                    begin_result_ = {};
                }
                else
                {
                    begin_result_ = std::unexpected<Core::DbError>{ sp_res.error() };
                }
            }
            else
            {
                begin_result_ = ctx_->begin_transaction();
            }
        }

        ~TransactionScope()
        {
            if (!committed_ && !rolled_back_ && begin_result_.has_value() && ctx_ && ctx_->in_transaction())
            {
                ctx_->truncate_pending_changes(saved_pending_size_);
                if (is_savepoint_)
                {
                    (void)ctx_->rollback_to_savepoint(savepoint_name_);
                    ctx_->release_savepoint_depth();
                }
                else
                {
                    (void)ctx_->rollback();
                }
                ctx_->restore_checkpoint_baselines(scope_checkpoint_);
            }
            if (pushed_pending_base_ && ctx_)
            {
                ctx_->pop_pending_base();
                pushed_pending_base_ = false;
            }
        }

        TransactionScope(const TransactionScope&) = delete;
        TransactionScope& operator=(const TransactionScope&) = delete;
        TransactionScope(TransactionScope&&) = delete;
        TransactionScope& operator=(TransactionScope&&) = delete;

        [[nodiscard]] bool is_active() const noexcept
        {
            return begin_result_.has_value() && !committed_ && !rolled_back_ && ctx_ && ctx_->in_transaction();
        }

        [[nodiscard]] const std::expected<void, Core::DbError>& begin_result() const noexcept
        {
            return begin_result_;
        }

        [[nodiscard]] std::expected<void, Core::DbError> commit()
        {
            if (!begin_result_)
            {
                return begin_result_;
            }
            if (committed_)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("TransactionScope already committed") };
            }
            if (rolled_back_)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Cannot commit a rolled-back TransactionScope") };
            }
            if (is_savepoint_)
            {
                auto pop_base = [this]() noexcept
                {
                    if (pushed_pending_base_ && ctx_)
                    {
                        ctx_->pop_pending_base();
                        pushed_pending_base_ = false;
                    }
                };
                if (ctx_->pending_changes_count() > saved_pending_size_)
                {
                    auto flush_res = ctx_->flush_pending_since(saved_pending_size_);
                    if (!flush_res)
                    {
                        (void)ctx_->rollback_to_savepoint(savepoint_name_);
                        ctx_->release_savepoint_depth();
                        ctx_->restore_checkpoint_baselines(scope_checkpoint_);
                        pop_base();
                        rolled_back_ = true;
                        return flush_res;
                    }
                }
                (void)ctx_->detect_changes();
                if (ctx_->pending_changes_count() > saved_pending_size_)
                {
                    auto flush_res = ctx_->flush_pending_since(saved_pending_size_);
                    if (!flush_res)
                    {
                        (void)ctx_->rollback_to_savepoint(savepoint_name_);
                        ctx_->release_savepoint_depth();
                        ctx_->restore_checkpoint_baselines(scope_checkpoint_);
                        pop_base();
                        rolled_back_ = true;
                        return flush_res;
                    }
                }
                auto rel_res = ctx_->release_savepoint(savepoint_name_);
                if (!rel_res)
                {
                    (void)ctx_->rollback_to_savepoint(savepoint_name_);
                    ctx_->release_savepoint_depth();
                    ctx_->restore_checkpoint_baselines(scope_checkpoint_);
                    pop_base();
                    rolled_back_ = true;
                    return rel_res;
                }
                ctx_->release_savepoint_depth();
                pop_base();
                committed_ = true;
                return rel_res;
            }
            auto res = ctx_->save_changes();
            if (!res)
            {
                ctx_->truncate_pending_changes(saved_pending_size_);
                if (ctx_->in_transaction())
                {
                    (void)ctx_->rollback();
                }
                ctx_->restore_checkpoint_baselines(scope_checkpoint_);
                rolled_back_ = true;
                return res;
            }
            if (ctx_->in_transaction())
            {
                res = ctx_->commit();
            }
            if (res)
            {
                committed_ = true;
            }
            else
            {
                ctx_->truncate_pending_changes(saved_pending_size_);
                if (ctx_->in_transaction())
                {
                    (void)ctx_->rollback();
                }
                ctx_->restore_checkpoint_baselines(scope_checkpoint_);
                rolled_back_ = true;
            }
            return res;
        }

        [[nodiscard]] std::expected<void, Core::DbError> rollback()
        {
            if (committed_)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Cannot rollback a committed TransactionScope") };
            }
            if (rolled_back_)
            {
                return {};
            }
            if (ctx_ && ctx_->in_transaction())
            {
                rolled_back_ = true;
                ctx_->truncate_pending_changes(saved_pending_size_);
                std::expected<void, Core::DbError> res{};
                if (is_savepoint_)
                {
                    res = ctx_->rollback_to_savepoint(savepoint_name_);
                    ctx_->release_savepoint_depth();
                }
                else
                {
                    res = ctx_->rollback();
                }
                ctx_->restore_checkpoint_baselines(scope_checkpoint_);
                if (pushed_pending_base_)
                {
                    ctx_->pop_pending_base();
                    pushed_pending_base_ = false;
                }
                return res;
            }
            return {};
        }

    private:
        DbContextBase<ConnectionHandle>* ctx_{ nullptr };
        std::expected<void, Core::DbError> begin_result_{};
        std::size_t saved_pending_size_{ 0 };
        detail::BaselineCheckpoint scope_checkpoint_{};
        bool is_savepoint_{ false };
        bool pushed_pending_base_{ false };
        std::string savepoint_name_{};
        bool committed_{ false };
        bool rolled_back_{ false };
    };

} // namespace DAL
