module;
#include <sqlgen.hpp>

export module dal:db_set;

import std;
import core;
import :schema_traits;
import :mappers;
import :generic_repo;
import :sqlite_connection;

export namespace DAL
{
    namespace detail
    {
        template<typename Val>
        [[nodiscard]] constexpr auto normalize_query_val(const Val& val)
        {
            using T = std::remove_cvref_t<Val>;
            if constexpr (Core::SingleKeyType<T>)
            {
                return normalize_query_val(val.value);
            }
            else if constexpr (Core::StrongIdType<T>)
            {
                return val.get();
            }
            else if constexpr (std::is_convertible_v<T, std::string_view> && !std::same_as<T, std::string>)
            {
                return std::string(val);
            }
            else
            {
                return val;
            }
        }

        template<typename T>
        struct UnwrapColumnScalar
        {
            using type = T;
        };

        template<typename T>
        struct UnwrapColumnScalar<std::optional<T>>
        {
            using type = typename UnwrapColumnScalar<T>::type;
        };

        template<typename T>
            requires requires { typename T::ReflectionType; }
        struct UnwrapColumnScalar<T>
        {
            using type = typename UnwrapColumnScalar<typename T::ReflectionType>::type;
        };

        template<typename T>
        struct IsOptionalField : std::false_type
        {
        };

        template<typename T>
        struct IsOptionalField<std::optional<T>> : std::true_type
        {
        };

        template<typename T>
            requires requires { typename T::ReflectionType; }
        struct IsOptionalField<T> : IsOptionalField<typename T::ReflectionType>
        {
        };

        template<typename RawFieldT>
        using ProjectedColumn_t = std::conditional_t<
            IsOptionalField<std::remove_cvref_t<RawFieldT>>::value,
            std::optional<typename UnwrapColumnScalar<std::remove_cvref_t<RawFieldT>>::type>,
            typename UnwrapColumnScalar<std::remove_cvref_t<RawFieldT>>::type>;

        inline constexpr std::size_t kSqliteUnboundedLimit = static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max());

        template<typename OrderByVariant, typename NewAscCol, typename NewDescCol>
        struct AppendOrderByVariant;

        template<typename... PrevOrderBys, typename NewAscCol, typename NewDescCol>
        struct AppendOrderByVariant<std::variant<PrevOrderBys...>, NewAscCol, NewDescCol>
        {
            template<typename PrevOB, typename NextCol>
            struct AppendOne;

            template<typename... Cols, typename NextCol>
            struct AppendOne<sqlgen::OrderBy<Cols...>, NextCol>
            {
                using type = sqlgen::OrderBy<Cols..., NextCol>;
            };

            using type =
                std::variant<typename AppendOne<PrevOrderBys, NewAscCol>::type..., typename AppendOne<PrevOrderBys, NewDescCol>::type...>;
        };

        inline std::atomic<std::uint64_t> g_write_guard_sp_counter{ 0 };
        inline std::atomic<std::uint64_t> g_context_sp_counter{ 0 };

        // RAII transaction/savepoint guard for atomic single- and multi-row write operations.
        // Begins a transaction when not already in one, or acquires a uniquely named savepoint
        // when nested inside an active transaction, rolling back on every non-committed exit
        // (returned errors or thrown exceptions).
        template<typename ConnectionHandle>
        class WriteBatchTxGuard
        {
        public:
            explicit WriteBatchTxGuard(const std::shared_ptr<ConnectionHandle>& conn) noexcept
                : conn_(conn)
            {
            }

            WriteBatchTxGuard(const WriteBatchTxGuard&) = delete;
            WriteBatchTxGuard& operator=(const WriteBatchTxGuard&) = delete;

            [[nodiscard]] std::expected<void, Core::DbError> begin()
            {
                if (!conn_)
                {
                    return {};
                }
                if constexpr (requires(ConnectionHandle& c) {
                                  c.in_transaction();
                                  c.begin_transaction();
                                  c.commit();
                                  c.rollback();
                              })
                {
                    if (!conn_->in_transaction())
                    {
                        auto tx_res = conn_->begin_transaction();
                        if (!tx_res)
                        {
                            return tx_res;
                        }
                        started_tx_ = true;
                        active_ = true;
                        return {};
                    }
                    if constexpr (requires(ConnectionHandle& c, std::string_view n) {
                                      c.savepoint(n);
                                      c.release_savepoint(n);
                                      c.rollback_to_savepoint(n);
                                  })
                    {
                        sp_name_ = std::format("_codefirst_wg_{}", ++g_write_guard_sp_counter);
                        auto sp_res = conn_->savepoint(sp_name_);
                        if (!sp_res)
                        {
                            return sp_res;
                        }
                        started_sp_ = true;
                        active_ = true;
                    }
                }
                return {};
            }

            [[nodiscard]] std::expected<void, Core::DbError> commit()
            {
                if (!active_ || !conn_)
                {
                    return {};
                }
                if (started_sp_)
                {
                    if constexpr (requires(ConnectionHandle& c, std::string_view n) {
                                      c.release_savepoint(n);
                                      c.rollback_to_savepoint(n);
                                  })
                    {
                        auto rel_res = conn_->release_savepoint(sp_name_);
                        if (!rel_res)
                        {
                            (void)conn_->rollback_to_savepoint(sp_name_);
                            active_ = false;
                            return rel_res;
                        }
                    }
                }
                else if (started_tx_)
                {
                    if constexpr (requires(ConnectionHandle& c) {
                                      c.commit();
                                      c.in_transaction();
                                      c.rollback();
                                  })
                    {
                        auto commit_res = conn_->commit();
                        if (!commit_res)
                        {
                            if (conn_->in_transaction())
                            {
                                (void)conn_->rollback();
                            }
                            active_ = false;
                            return commit_res;
                        }
                    }
                }
                active_ = false;
                return {};
            }

            void rollback_now() noexcept
            {
                if (!active_ || !conn_)
                {
                    return;
                }
                active_ = false;
                if (started_sp_)
                {
                    if constexpr (requires(ConnectionHandle& c, std::string_view n) { c.rollback_to_savepoint(n); })
                    {
                        try
                        {
                            (void)conn_->rollback_to_savepoint(sp_name_);
                        } catch (...)
                        {
                        }
                    }
                }
                else if (started_tx_)
                {
                    if constexpr (requires(ConnectionHandle& c) {
                                      c.in_transaction();
                                      c.rollback();
                                  })
                    {
                        try
                        {
                            if (conn_->in_transaction())
                            {
                                (void)conn_->rollback();
                            }
                        } catch (...)
                        {
                        }
                    }
                }
            }

            ~WriteBatchTxGuard() noexcept
            {
                rollback_now();
            }

        private:
            std::shared_ptr<ConnectionHandle> conn_{};
            std::string sp_name_{};
            bool started_tx_{ false };
            bool started_sp_{ false };
            bool active_{ false };
        };

        struct TrackedEntityEntry
        {
            std::shared_ptr<void> owned_instance{};
            std::function<std::optional<std::function<std::expected<void, Core::DbError>()>>(const std::shared_ptr<void>&)> detect_fn{};
            std::function<std::shared_ptr<void>()> snapshot_baseline_fn{};
            std::function<void(const std::shared_ptr<void>&)> restore_baseline_fn{};
        };

        struct BaselineCheckpoint
        {
            std::size_t saved_pending_size{ 0 };
            std::map<std::pair<std::type_index, std::string>, std::shared_ptr<void>> baselines{};
            std::map<std::pair<std::type_index, std::string>, TrackedEntityEntry> saved_entries{};
        };

        struct ContextState
        {
            std::vector<std::move_only_function<std::expected<void, Core::DbError>()>> pending_ops_{};
            std::vector<std::size_t> pending_base_stack_{};
            bool defer_changes_{ false };
            bool change_tracking_enabled_{ false };
            std::map<std::pair<std::type_index, std::string>, TrackedEntityEntry> tracked_entries_{};
            std::optional<BaselineCheckpoint> outer_tx_checkpoint_{};

            [[nodiscard]] std::size_t current_pending_base() const noexcept
            {
                return pending_base_stack_.empty() ? 0u : pending_base_stack_.back();
            }

            [[nodiscard]] BaselineCheckpoint capture_checkpoint() const
            {
                BaselineCheckpoint cp;
                cp.saved_pending_size = pending_ops_.size();
                cp.saved_entries = tracked_entries_;
                for (const auto& [k, entry] : tracked_entries_)
                {
                    if (entry.snapshot_baseline_fn)
                    {
                        cp.baselines.emplace(k, entry.snapshot_baseline_fn());
                    }
                }
                return cp;
            }

            void restore_baselines(const BaselineCheckpoint& cp)
            {
                // Evict entries that did not exist when the checkpoint was captured so rows
                // inserted and tracked inside a rolled-back transaction/savepoint never linger.
                std::erase_if(
                    tracked_entries_,
                    [&cp](const auto& kv)
                    {
                        return !cp.saved_entries.contains(kv.first);
                    });
                for (const auto& [k, entry] : cp.saved_entries)
                {
                    if (!tracked_entries_.contains(k))
                    {
                        tracked_entries_.emplace(k, entry);
                    }
                }
                for (const auto& [k, snap] : cp.baselines)
                {
                    auto it = tracked_entries_.find(k);
                    if (it != tracked_entries_.end() && it->second.restore_baseline_fn && snap)
                    {
                        it->second.restore_baseline_fn(snap);
                    }
                }
            }

            void untrack_key(std::type_index type, const std::string& key_repr) noexcept
            {
                const auto map_key = std::pair<std::type_index, std::string>{ type, key_repr };
                auto it = tracked_entries_.find(map_key);
                if (it == tracked_entries_.end())
                {
                    return;
                }
                if (outer_tx_checkpoint_.has_value())
                {
                    if (!outer_tx_checkpoint_->saved_entries.contains(map_key))
                    {
                        outer_tx_checkpoint_->saved_entries.emplace(map_key, it->second);
                    }
                    if (it->second.snapshot_baseline_fn && !outer_tx_checkpoint_->baselines.contains(map_key))
                    {
                        outer_tx_checkpoint_->baselines.emplace(map_key, it->second.snapshot_baseline_fn());
                    }
                }
                tracked_entries_.erase(it);
            }

            void untrack_type(std::type_index type) noexcept
            {
                if (outer_tx_checkpoint_.has_value())
                {
                    for (const auto& [k, entry] : tracked_entries_)
                    {
                        if (k.first == type)
                        {
                            if (!outer_tx_checkpoint_->saved_entries.contains(k))
                            {
                                outer_tx_checkpoint_->saved_entries.emplace(k, entry);
                            }
                            if (entry.snapshot_baseline_fn && !outer_tx_checkpoint_->baselines.contains(k))
                            {
                                outer_tx_checkpoint_->baselines.emplace(k, entry.snapshot_baseline_fn());
                            }
                        }
                    }
                }
                std::erase_if(
                    tracked_entries_,
                    [type](const auto& kv)
                    {
                        return kv.first.first == type;
                    });
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
                auto map_key = std::pair<std::type_index, std::string>{ type, std::move(key_repr) };
                auto it = tracked_entries_.find(map_key);
                if (it == tracked_entries_.end())
                {
                    tracked_entries_.emplace(
                        std::move(map_key),
                        TrackedEntityEntry{
                            std::move(owned), std::move(detect_fn), std::move(snapshot_baseline_fn), std::move(restore_baseline_fn) });
                    return;
                }

                if (outer_tx_checkpoint_.has_value() && it->second.snapshot_baseline_fn &&
                    !outer_tx_checkpoint_->baselines.contains(map_key))
                {
                    outer_tx_checkpoint_->baselines.emplace(map_key, it->second.snapshot_baseline_fn());
                }

                if (owned)
                {
                    it->second.owned_instance = std::move(owned);
                }
                if (refresh_baseline && detect_fn)
                {
                    it->second.detect_fn = std::move(detect_fn);
                    if (snapshot_baseline_fn)
                    {
                        it->second.snapshot_baseline_fn = std::move(snapshot_baseline_fn);
                    }
                    if (restore_baseline_fn)
                    {
                        it->second.restore_baseline_fn = std::move(restore_baseline_fn);
                    }
                }
                else if (!it->second.detect_fn && detect_fn)
                {
                    it->second.detect_fn = std::move(detect_fn);
                    it->second.snapshot_baseline_fn = std::move(snapshot_baseline_fn);
                    it->second.restore_baseline_fn = std::move(restore_baseline_fn);
                }
            }

            [[nodiscard]] std::shared_ptr<void> find_tracked_instance(std::type_index type, const std::string& key_repr) const
            {
                auto it = tracked_entries_.find(std::pair<std::type_index, std::string>{ type, key_repr });
                if (it != tracked_entries_.end())
                {
                    return it->second.owned_instance;
                }
                return nullptr;
            }
        };
    } // namespace detail

    // Fluent server-side query builder for DbSet (Phases 10, 12, 13, 14).
    // Translates filtering, sorting, pagination, aggregations, and bulk deletes into sqlgen pipelines.
    template<
        typename Domain,
        typename DTO,
        typename ConnectionHandle,
        typename KeyType = Schema::EntityKey_t<Domain, DTO>,
        typename WhereCondT = rfl::Nothing,
        typename OrderByT = rfl::Nothing>
        requires DAL::Mappers::Mappable<Domain, DTO> && Repositories::DatabaseConnection<ConnectionHandle>
    class DbQuery
    {
    public:
        using RepoType = Repositories::GenericRepository<Domain, DTO, ConnectionHandle, KeyType>;
        using TrackFn = std::function<void(const Domain&)>;
        using ClearTrackFn = std::function<void()>;
        using UntrackKeyFn = std::function<void(const KeyType&)>;

        DbQuery(
            std::shared_ptr<RepoType> repo,
            std::shared_ptr<ConnectionHandle> conn,
            WhereCondT where_cond = {},
            OrderByT order_by = {},
            std::optional<std::size_t> limit = std::nullopt,
            std::optional<std::size_t> offset = std::nullopt,
            TrackFn track_fn = {},
            ClearTrackFn clear_track_fn = {},
            UntrackKeyFn untrack_key_fn = {})
            : repo_(std::move(repo))
            , conn_(std::move(conn))
            , where_cond_(std::move(where_cond))
            , order_by_(std::move(order_by))
            , limit_(limit)
            , offset_(offset)
            , track_fn_(std::move(track_fn))
            , clear_track_fn_(std::move(clear_track_fn))
            , untrack_key_fn_(std::move(untrack_key_fn))
        {
        }

        template<typename Condition>
        [[nodiscard]] auto where_sql(Condition cond) const
        {
            if constexpr (std::same_as<WhereCondT, rfl::Nothing>)
            {
                return DbQuery<Domain, DTO, ConnectionHandle, KeyType, Condition, OrderByT>(
                    repo_, conn_, std::move(cond), order_by_, limit_, offset_, track_fn_, clear_track_fn_, untrack_key_fn_);
            }
            else
            {
                auto combined = where_cond_ && std::move(cond);
                using CombinedT = decltype(combined);
                return DbQuery<Domain, DTO, ConnectionHandle, KeyType, CombinedT, OrderByT>(
                    repo_, conn_, std::move(combined), order_by_, limit_, offset_, track_fn_, clear_track_fn_, untrack_key_fn_);
            }
        }

        template<typename Condition>
        [[nodiscard]] auto or_where_sql(Condition cond) const
        {
            if constexpr (std::same_as<WhereCondT, rfl::Nothing>)
            {
                return where_sql(std::move(cond));
            }
            else
            {
                auto combined = where_cond_ || std::move(cond);
                using CombinedT = decltype(combined);
                return DbQuery<Domain, DTO, ConnectionHandle, KeyType, CombinedT, OrderByT>(
                    repo_, conn_, std::move(combined), order_by_, limit_, offset_, track_fn_, clear_track_fn_, untrack_key_fn_);
            }
        }

        template<rfl::internal::StringLiteral ColLiteral, typename Val>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_equal(const Val& val) const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            return where_sql(Schema::col_of<dto_m> == detail::normalize_query_val(val));
        }

        template<rfl::internal::StringLiteral ColLiteral, typename Val>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto or_where_equal(const Val& val) const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            return or_where_sql(Schema::col_of<dto_m> == detail::normalize_query_val(val));
        }

        template<rfl::internal::StringLiteral ColLiteral, typename Val>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto or_where_not_equal(const Val& val) const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            return or_where_sql(Schema::col_of<dto_m> != detail::normalize_query_val(val));
        }

        template<rfl::internal::StringLiteral ColLiteral, typename Val>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto or_where_lt(const Val& val) const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            return or_where_sql(Schema::col_of<dto_m> < detail::normalize_query_val(val));
        }

        template<rfl::internal::StringLiteral ColLiteral, typename Val>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto or_where_le(const Val& val) const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            return or_where_sql(Schema::col_of<dto_m> <= detail::normalize_query_val(val));
        }

        template<rfl::internal::StringLiteral ColLiteral, typename Val>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto or_where_gt(const Val& val) const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            return or_where_sql(Schema::col_of<dto_m> > detail::normalize_query_val(val));
        }

        template<rfl::internal::StringLiteral ColLiteral, typename Val>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto or_where_ge(const Val& val) const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            return or_where_sql(Schema::col_of<dto_m> >= detail::normalize_query_val(val));
        }

        template<rfl::internal::StringLiteral ColLiteral, typename Val>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto or_where_between(const Val& low, const Val& high) const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            return or_where_sql(
                (Schema::col_of<dto_m> >= detail::normalize_query_val(low)) &&
                (Schema::col_of<dto_m> <= detail::normalize_query_val(high)));
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto or_where_like(std::string_view pattern) const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            return or_where_sql(Schema::col_of<dto_m>.like(std::string(pattern)));
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto or_where_not_like(std::string_view pattern) const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            return or_where_sql(Schema::col_of<dto_m>.not_like(std::string(pattern)));
        }

        template<rfl::internal::StringLiteral ColLiteral, typename... Ts>
            requires(sizeof...(Ts) > 0) && DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto or_where_in(const Ts&... vals) const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            return or_where_sql(Schema::col_of<dto_m>.in(detail::normalize_query_val(vals)...));
        }

        template<rfl::internal::StringLiteral ColLiteral, typename Val>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto or_where_in_vec(const std::vector<Val>& vals) const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            using NormalizedT = std::remove_cvref_t<decltype(detail::normalize_query_val(std::declval<const Val&>()))>;
            std::vector<NormalizedT> normalized;
            normalized.reserve(vals.size());
            for (const auto& v : vals)
            {
                normalized.push_back(detail::normalize_query_val(v));
            }
            return or_where_sql(Schema::col_of<dto_m>.in(normalized));
        }

        template<rfl::internal::StringLiteral ColLiteral, typename... Ts>
            requires(sizeof...(Ts) > 0) && DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto or_where_not_in(const Ts&... vals) const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            return or_where_sql(Schema::col_of<dto_m>.not_in(detail::normalize_query_val(vals)...));
        }

        template<rfl::internal::StringLiteral ColLiteral, typename Val>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto or_where_not_in_vec(const std::vector<Val>& vals) const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            using NormalizedT = std::remove_cvref_t<decltype(detail::normalize_query_val(std::declval<const Val&>()))>;
            std::vector<NormalizedT> normalized;
            normalized.reserve(vals.size());
            for (const auto& v : vals)
            {
                normalized.push_back(detail::normalize_query_val(v));
            }
            return or_where_sql(Schema::col_of<dto_m>.not_in(normalized));
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto or_where_null() const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            return or_where_sql(Schema::col_of<dto_m>.is_null());
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto or_where_not_null() const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            return or_where_sql(Schema::col_of<dto_m>.is_not_null());
        }

        template<rfl::internal::StringLiteral ColLiteral, typename Val>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_not_equal(const Val& val) const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            return where_sql(Schema::col_of<dto_m> != detail::normalize_query_val(val));
        }

        template<rfl::internal::StringLiteral ColLiteral, typename Val>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_lt(const Val& val) const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            return where_sql(Schema::col_of<dto_m> < detail::normalize_query_val(val));
        }

        template<rfl::internal::StringLiteral ColLiteral, typename Val>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_le(const Val& val) const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            return where_sql(Schema::col_of<dto_m> <= detail::normalize_query_val(val));
        }

        template<rfl::internal::StringLiteral ColLiteral, typename Val>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_gt(const Val& val) const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            return where_sql(Schema::col_of<dto_m> > detail::normalize_query_val(val));
        }

        template<rfl::internal::StringLiteral ColLiteral, typename Val>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_ge(const Val& val) const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            return where_sql(Schema::col_of<dto_m> >= detail::normalize_query_val(val));
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_like(std::string_view pattern) const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            return where_sql(Schema::col_of<dto_m>.like(std::string(pattern)));
        }

        template<rfl::internal::StringLiteral ColLiteral, typename... Ts>
            requires(sizeof...(Ts) > 0) && DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_in(const Ts&... vals) const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            return where_sql(Schema::col_of<dto_m>.in(detail::normalize_query_val(vals)...));
        }

        template<rfl::internal::StringLiteral ColLiteral, typename Val>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_in_vec(const std::vector<Val>& vals) const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            using NormalizedT = std::remove_cvref_t<decltype(detail::normalize_query_val(std::declval<const Val&>()))>;
            std::vector<NormalizedT> normalized;
            normalized.reserve(vals.size());
            for (const auto& v : vals)
            {
                normalized.push_back(detail::normalize_query_val(v));
            }
            return where_sql(Schema::col_of<dto_m>.in(normalized));
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_null() const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            return where_sql(Schema::col_of<dto_m>.is_null());
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_not_null() const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            return where_sql(Schema::col_of<dto_m>.is_not_null());
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_not_like(std::string_view pattern) const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            return where_sql(Schema::col_of<dto_m>.not_like(std::string(pattern)));
        }

        template<rfl::internal::StringLiteral ColLiteral, typename... Ts>
            requires(sizeof...(Ts) > 0) && DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_not_in(const Ts&... vals) const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            return where_sql(Schema::col_of<dto_m>.not_in(detail::normalize_query_val(vals)...));
        }

        template<rfl::internal::StringLiteral ColLiteral, typename Val>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_between(const Val& low, const Val& high) const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            return where_sql(
                (Schema::col_of<dto_m> >= detail::normalize_query_val(low)) &&
                (Schema::col_of<dto_m> <= detail::normalize_query_val(high)));
        }

        template<rfl::internal::StringLiteral ColLiteral, typename Val>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_not_in_vec(const std::vector<Val>& vals) const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            using NormalizedT = std::remove_cvref_t<decltype(detail::normalize_query_val(std::declval<const Val&>()))>;
            std::vector<NormalizedT> normalized;
            normalized.reserve(vals.size());
            for (const auto& v : vals)
            {
                normalized.push_back(detail::normalize_query_val(v));
            }
            return where_sql(Schema::col_of<dto_m>.not_in(normalized));
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto order_by(bool desc = false) const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            using AscT = decltype(sqlgen::order_by(Schema::col_of<dto_m>));
            using DescT = decltype(sqlgen::order_by(Schema::col_of<dto_m>.desc()));
            using VarT = std::variant<AscT, DescT>;
            VarT ob = desc ? VarT{ DescT{} } : VarT{ AscT{} };
            return DbQuery<Domain, DTO, ConnectionHandle, KeyType, WhereCondT, VarT>(
                repo_, conn_, where_cond_, std::move(ob), limit_, offset_, track_fn_, clear_track_fn_, untrack_key_fn_);
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires(!std::same_as<OrderByT, rfl::Nothing>) && DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto then_by(bool desc = false) const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            using NewAscCol = std::remove_cvref_t<decltype(Schema::col_of<dto_m>)>;
            using NewDescCol = std::remove_cvref_t<decltype(Schema::col_of<dto_m>.desc())>;
            using Helper = detail::AppendOrderByVariant<OrderByT, NewAscCol, NewDescCol>;
            using NewVarT = typename Helper::type;

            NewVarT combined = std::visit(
                [desc](const auto& prev_ob) -> NewVarT
                {
                    using PrevOB = std::remove_cvref_t<decltype(prev_ob)>;
                    if (desc)
                    {
                        using NextOB = typename Helper::template AppendOne<PrevOB, NewDescCol>::type;
                        return NewVarT{ NextOB{} };
                    }
                    else
                    {
                        using NextOB = typename Helper::template AppendOne<PrevOB, NewAscCol>::type;
                        return NewVarT{ NextOB{} };
                    }
                },
                order_by_);

            return DbQuery<Domain, DTO, ConnectionHandle, KeyType, WhereCondT, NewVarT>(
                repo_, conn_, where_cond_, std::move(combined), limit_, offset_, track_fn_, clear_track_fn_, untrack_key_fn_);
        }

        [[nodiscard]] DbQuery limit(std::size_t limit_count) const pre(limit_count > 0)
        {
            return DbQuery(repo_, conn_, where_cond_, order_by_, limit_count, offset_, track_fn_, clear_track_fn_, untrack_key_fn_);
        }

        [[nodiscard]] DbQuery offset(std::size_t offset_count) const
        {
            return DbQuery(repo_, conn_, where_cond_, order_by_, limit_, offset_count, track_fn_, clear_track_fn_, untrack_key_fn_);
        }

        [[nodiscard]] DbQuery page(std::size_t limit_count, std::size_t offset_count = 0) const pre(limit_count > 0)
        {
            return DbQuery(repo_, conn_, where_cond_, order_by_, limit_count, offset_count, track_fn_, clear_track_fn_, untrack_key_fn_);
        }

        [[nodiscard]] std::expected<std::vector<Domain>, Core::DbError> try_to_vector() const
        {
            if (!conn_)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is null") };
            }
            if (!conn_->is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is closed") };
            }
            if (limit_.has_value() && *limit_ == 0)
            {
                return std::vector<Domain>{};
            }

            if constexpr (requires(const ConnectionHandle& c) { c.template try_execute_read<DTO>(sqlgen::read<std::vector<DTO>>); })
            {
                auto dto_res = execute_query(*conn_);
                if (!dto_res)
                {
                    return std::unexpected<Core::DbError>{ dto_res.error() };
                }

                std::vector<Domain> out;
                out.reserve(dto_res.value().size());
                for (const auto& dto : dto_res.value())
                {
                    Domain entity = DAL::Mappers::MapperTraits<Domain, DTO>::to_domain(dto);
                    if (track_fn_)
                    {
                        track_fn_(entity);
                    }
                    out.push_back(std::move(entity));
                }
                return out;
            }
            else
            {
                if constexpr (!std::same_as<WhereCondT, rfl::Nothing> || !std::same_as<OrderByT, rfl::Nothing>)
                {
                    return std::unexpected<Core::DbError>{ Core::DbError::unknown(
                        "Connection adapter does not support server-side WHERE or ORDER BY queries") };
                }
                else
                {
                    auto all_res = repo_->try_get_all();
                    if (!all_res)
                    {
                        return std::unexpected<Core::DbError>{ all_res.error() };
                    }
                    auto out = std::move(*all_res);
                    if (offset_.has_value())
                    {
                        const std::size_t off = std::min(*offset_, out.size());
                        out.erase(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(off));
                    }
                    if (limit_.has_value() && out.size() > *limit_)
                    {
                        out.resize(*limit_);
                    }
                    if (track_fn_)
                    {
                        for (const auto& entity : out)
                        {
                            track_fn_(entity);
                        }
                    }
                    return out;
                }
            }
        }

        [[nodiscard]] std::vector<Domain> to_vector() const
        {
            auto res = try_to_vector();
            if (res)
            {
                return std::move(res.value());
            }
            return {};
        }

        [[nodiscard]] std::expected<std::optional<Domain>, Core::DbError> try_first_or_default() const
        {
            if (!conn_)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is null") };
            }
            if (!conn_->is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is closed") };
            }
            if (limit_.has_value() && *limit_ == 0)
            {
                return std::optional<Domain>{ std::nullopt };
            }
            auto res = limit(1).try_to_vector();
            if (!res)
            {
                return std::unexpected<Core::DbError>{ res.error() };
            }
            if (res->empty())
            {
                return std::optional<Domain>{ std::nullopt };
            }
            return std::optional<Domain>{ std::move(res->front()) };
        }

        [[nodiscard]] std::optional<Domain> first_or_default() const
        {
            return try_first_or_default().value_or(std::nullopt);
        }

        [[nodiscard]] std::expected<std::size_t, Core::DbError> try_count() const
        {
            if (!conn_)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is null") };
            }
            if (!conn_->is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is closed") };
            }
            if (limit_.has_value() && *limit_ == 0)
            {
                return 0u;
            }

            if (!limit_.has_value() && !offset_.has_value())
            {
                if constexpr (std::same_as<WhereCondT, rfl::Nothing>)
                {
                    if constexpr (requires(const ConnectionHandle& c) { c.template try_count<DTO>(); })
                    {
                        return conn_->template try_count<DTO>();
                    }
                }
                else
                {
                    if constexpr (requires(const ConnectionHandle& c) { c.template try_count_where<DTO>(where_cond_); })
                    {
                        return conn_->template try_count_where<DTO>(where_cond_);
                    }
                }
            }

            if constexpr (requires(const ConnectionHandle& c) { c.template try_execute_read<DTO>(sqlgen::read<std::vector<DTO>>); })
            {
                auto dto_res = execute_query(*conn_);
                if (!dto_res)
                {
                    return std::unexpected<Core::DbError>{ dto_res.error() };
                }
                return dto_res->size();
            }
            else
            {
                if constexpr (!std::same_as<WhereCondT, rfl::Nothing> || !std::same_as<OrderByT, rfl::Nothing>)
                {
                    return std::unexpected<Core::DbError>{ Core::DbError::unknown(
                        "Connection adapter does not support server-side WHERE or ORDER BY queries") };
                }
                else
                {
                    auto all_res = repo_->try_get_all();
                    if (!all_res)
                    {
                        return std::unexpected<Core::DbError>{ all_res.error() };
                    }
                    std::size_t count_val = all_res->size();
                    if (offset_.has_value())
                    {
                        count_val = (*offset_ >= count_val) ? 0u : (count_val - *offset_);
                    }
                    if (limit_.has_value() && count_val > *limit_)
                    {
                        count_val = *limit_;
                    }
                    return count_val;
                }
            }
        }

        [[nodiscard]] std::size_t count() const
        {
            return try_count().value_or(0);
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto try_min() const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            using RawT = [:std::meta::type_of(dto_m):];
            using ScalarT = typename detail::UnwrapColumnScalar<std::remove_cvref_t<RawT>>::type;
            return try_aggregate_impl<ScalarT>(sqlgen::min(Schema::col_of<dto_m>));
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto min() const
        {
            return try_min<ColLiteral>().value_or(std::nullopt);
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto try_max() const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            using RawT = [:std::meta::type_of(dto_m):];
            using ScalarT = typename detail::UnwrapColumnScalar<std::remove_cvref_t<RawT>>::type;
            return try_aggregate_impl<ScalarT>(sqlgen::max(Schema::col_of<dto_m>));
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto max() const
        {
            return try_max<ColLiteral>().value_or(std::nullopt);
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto try_sum() const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            using RawT = [:std::meta::type_of(dto_m):];
            using ScalarT = typename detail::UnwrapColumnScalar<std::remove_cvref_t<RawT>>::type;
            return try_aggregate_impl<ScalarT>(sqlgen::sum(Schema::col_of<dto_m>));
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto sum() const
        {
            return try_sum<ColLiteral>().value_or(std::nullopt);
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] std::expected<std::optional<double>, Core::DbError> try_avg() const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            return try_aggregate_impl<double>(sqlgen::avg(Schema::col_of<dto_m>));
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] std::optional<double> avg() const
        {
            return try_avg<ColLiteral>().value_or(std::nullopt);
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto try_select() const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            using SplicedT = [:std::meta::type_of(dto_m):];
            using RawT = std::remove_cvref_t<SplicedT>;
            using ScalarT = typename detail::UnwrapColumnScalar<RawT>::type;
            using ColValueT = std::conditional_t<detail::IsOptionalField<RawT>::value, std::optional<ScalarT>, ScalarT>;

            if (!conn_)
            {
                return std::expected<std::vector<ColValueT>, Core::DbError>{ std::unexpected<Core::DbError>{
                    Core::DbError::connection_error("Database connection is null") } };
            }
            if (!conn_->is_open())
            {
                return std::expected<std::vector<ColValueT>, Core::DbError>{ std::unexpected<Core::DbError>{
                    Core::DbError::connection_error("Database connection is closed") } };
            }
            if (limit_.has_value() && *limit_ == 0)
            {
                return std::expected<std::vector<ColValueT>, Core::DbError>{ std::vector<ColValueT>{} };
            }

            struct ProjRow
            {
                RawT val{};
            };

            auto with_where = [&]()
            {
                if constexpr (!std::same_as<WhereCondT, rfl::Nothing>)
                {
                    return sqlgen::select_from<DTO>(Schema::col_of<dto_m> | sqlgen::as<"val">) | sqlgen::where(where_cond_);
                }
                else
                {
                    return sqlgen::select_from<DTO>(Schema::col_of<dto_m> | sqlgen::as<"val">);
                }
            }();

            auto rows_res = [&]() -> std::expected<std::vector<ProjRow>, Core::DbError>
            {
                if constexpr (!std::same_as<OrderByT, rfl::Nothing>)
                {
                    return std::visit(
                        [&](const auto& ob)
                        {
                            return execute_projection<ProjRow>(*conn_, with_where | ob);
                        },
                        order_by_);
                }
                else
                {
                    return execute_projection<ProjRow>(*conn_, std::move(with_where));
                }
            }();

            if (!rows_res)
            {
                return std::expected<std::vector<ColValueT>, Core::DbError>{ std::unexpected<Core::DbError>{ rows_res.error() } };
            }

            std::vector<ColValueT> result;
            result.reserve(rows_res->size());
            for (auto& r : *rows_res)
            {
                if constexpr (requires { r.val.value(); })
                {
                    result.push_back(r.val.value());
                }
                else
                {
                    result.push_back(std::move(r.val));
                }
            }
            return std::expected<std::vector<ColValueT>, Core::DbError>{ std::move(result) };
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto select() const
        {
            auto res = try_select<ColLiteral>();
            using VecT = std::remove_cvref_t<decltype(res.value())>;
            if (res)
            {
                return VecT{ std::move(res.value()) };
            }
            return VecT{};
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] std::expected<std::size_t, Core::DbError> try_count_distinct() const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            auto res = try_aggregate_impl<std::size_t>(sqlgen::count_distinct(Schema::col_of<dto_m>));
            if (!res)
            {
                return std::unexpected<Core::DbError>{ res.error() };
            }
            return res->value_or(0);
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] std::size_t count_distinct() const
        {
            return try_count_distinct<ColLiteral>().value_or(0);
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto try_select_distinct() const
        {
            constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiteral>();
            using SplicedT = [:std::meta::type_of(dto_m):];
            using RawT = std::remove_cvref_t<SplicedT>;
            using ScalarT = typename detail::UnwrapColumnScalar<RawT>::type;
            using ColValueT = std::conditional_t<detail::IsOptionalField<RawT>::value, std::optional<ScalarT>, ScalarT>;

            if (!conn_)
            {
                return std::expected<std::vector<ColValueT>, Core::DbError>{ std::unexpected<Core::DbError>{
                    Core::DbError::connection_error("Database connection is null") } };
            }
            if (!conn_->is_open())
            {
                return std::expected<std::vector<ColValueT>, Core::DbError>{ std::unexpected<Core::DbError>{
                    Core::DbError::connection_error("Database connection is closed") } };
            }
            if (limit_.has_value() && *limit_ == 0)
            {
                return std::expected<std::vector<ColValueT>, Core::DbError>{ std::vector<ColValueT>{} };
            }

            struct ProjRow
            {
                RawT val{};
            };

            auto with_group = [&]()
            {
                if constexpr (!std::same_as<WhereCondT, rfl::Nothing>)
                {
                    return sqlgen::select_from<DTO>(Schema::col_of<dto_m> | sqlgen::as<"val">) | sqlgen::where(where_cond_) |
                        sqlgen::group_by(Schema::col_of<dto_m>);
                }
                else
                {
                    return sqlgen::select_from<DTO>(Schema::col_of<dto_m> | sqlgen::as<"val">) | sqlgen::group_by(Schema::col_of<dto_m>);
                }
            }();

            auto rows_res = [&]() -> std::expected<std::vector<ProjRow>, Core::DbError>
            {
                if constexpr (!std::same_as<OrderByT, rfl::Nothing>)
                {
                    return std::visit(
                        [&](const auto& ob)
                        {
                            return execute_projection<ProjRow>(*conn_, with_group | ob);
                        },
                        order_by_);
                }
                else
                {
                    return execute_projection<ProjRow>(*conn_, std::move(with_group));
                }
            }();

            if (!rows_res)
            {
                return std::expected<std::vector<ColValueT>, Core::DbError>{ std::unexpected<Core::DbError>{ rows_res.error() } };
            }

            std::vector<ColValueT> result;
            result.reserve(rows_res->size());
            for (auto& r : *rows_res)
            {
                if constexpr (requires { r.val.value(); })
                {
                    result.push_back(r.val.value());
                }
                else
                {
                    result.push_back(std::move(r.val));
                }
            }
            return std::expected<std::vector<ColValueT>, Core::DbError>{ std::move(result) };
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto select_distinct() const
        {
            auto res = try_select_distinct<ColLiteral>();
            using VecT = std::remove_cvref_t<decltype(res.value())>;
            if (res)
            {
                return VecT{ std::move(res.value()) };
            }
            return VecT{};
        }

        template<rfl::internal::StringLiteral... ColLiterals>
            requires(sizeof...(ColLiterals) >= 2) && (DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiterals> && ...)
        [[nodiscard]] auto try_select_columns() const
        {
            using RowTuple = std::tuple<detail::ProjectedColumn_t<
                decltype(std::declval<const DTO&>().[:DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiterals>():])>...>;

            if (!conn_)
            {
                return std::expected<std::vector<RowTuple>, Core::DbError>{ std::unexpected<Core::DbError>{
                    Core::DbError::connection_error("Database connection is null") } };
            }
            if (!conn_->is_open())
            {
                return std::expected<std::vector<RowTuple>, Core::DbError>{ std::unexpected<Core::DbError>{
                    Core::DbError::connection_error("Database connection is closed") } };
            }
            if (limit_.has_value() && *limit_ == 0)
            {
                return std::expected<std::vector<RowTuple>, Core::DbError>{ std::vector<RowTuple>{} };
            }

            auto with_where = [&]()
            {
                if constexpr (!std::same_as<WhereCondT, rfl::Nothing>)
                {
                    return sqlgen::select_from<DTO>(Schema::col_of<DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiterals>()>...) |
                        sqlgen::where(where_cond_);
                }
                else
                {
                    return sqlgen::select_from<DTO>(Schema::col_of<DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiterals>()>...);
                }
            }();

            if constexpr (!std::same_as<OrderByT, rfl::Nothing>)
            {
                return std::visit(
                    [&](const auto& ob)
                    {
                        return execute_tuple_projection<RowTuple>(*conn_, with_where | ob);
                    },
                    order_by_);
            }
            else
            {
                return execute_tuple_projection<RowTuple>(*conn_, std::move(with_where));
            }
        }

        template<rfl::internal::StringLiteral... ColLiterals>
            requires(sizeof...(ColLiterals) >= 2) && (DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiterals> && ...)
        [[nodiscard]] auto select_columns() const
        {
            auto res = try_select_columns<ColLiterals...>();
            using VecT = std::remove_cvref_t<decltype(res.value())>;
            if (res)
            {
                return VecT{ std::move(res.value()) };
            }
            return VecT{};
        }

        template<rfl::internal::StringLiteral... ColLiterals>
            requires(sizeof...(ColLiterals) >= 2) && (DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiterals> && ...)
        [[nodiscard]] auto try_select_distinct_columns() const
        {
            using RowTuple = std::tuple<detail::ProjectedColumn_t<
                decltype(std::declval<const DTO&>().[:DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiterals>():])>...>;

            if (!conn_)
            {
                return std::expected<std::vector<RowTuple>, Core::DbError>{ std::unexpected<Core::DbError>{
                    Core::DbError::connection_error("Database connection is null") } };
            }
            if (!conn_->is_open())
            {
                return std::expected<std::vector<RowTuple>, Core::DbError>{ std::unexpected<Core::DbError>{
                    Core::DbError::connection_error("Database connection is closed") } };
            }
            if (limit_.has_value() && *limit_ == 0)
            {
                return std::expected<std::vector<RowTuple>, Core::DbError>{ std::vector<RowTuple>{} };
            }

            auto with_group = [&]()
            {
                if constexpr (!std::same_as<WhereCondT, rfl::Nothing>)
                {
                    return sqlgen::select_from<DTO>(Schema::col_of<DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiterals>()>...) |
                        sqlgen::where(where_cond_) |
                        sqlgen::group_by(Schema::col_of<DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiterals>()>...);
                }
                else
                {
                    return sqlgen::select_from<DTO>(Schema::col_of<DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiterals>()>...) |
                        sqlgen::group_by(Schema::col_of<DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiterals>()>...);
                }
            }();

            if constexpr (!std::same_as<OrderByT, rfl::Nothing>)
            {
                return std::visit(
                    [&](const auto& ob)
                    {
                        return execute_tuple_projection<RowTuple>(*conn_, with_group | ob);
                    },
                    order_by_);
            }
            else
            {
                return execute_tuple_projection<RowTuple>(*conn_, std::move(with_group));
            }
        }

        template<rfl::internal::StringLiteral... ColLiterals>
            requires(sizeof...(ColLiterals) >= 2) && (DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiterals> && ...)
        [[nodiscard]] auto select_distinct_columns() const
        {
            auto res = try_select_distinct_columns<ColLiterals...>();
            using VecT = std::remove_cvref_t<decltype(res.value())>;
            if (res)
            {
                return VecT{ std::move(res.value()) };
            }
            return VecT{};
        }

        template<rfl::internal::StringLiteral IndexName, rfl::internal::StringLiteral... ColLiterals>
            requires(sizeof...(ColLiterals) >= 1) && (DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiterals> && ...)
        [[nodiscard]] std::expected<void, Core::DbError> ensure_index_created(bool unique = false) const
        {
            if (!conn_)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is null") };
            }
            if constexpr (!std::same_as<WhereCondT, rfl::Nothing>)
            {
                return conn_->template ensure_filtered_index_created<IndexName, DTO>(
                    unique, where_cond_, Schema::col_of<DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiterals>()>...);
            }
            else
            {
                return conn_->template ensure_index_created<IndexName, DTO>(
                    unique, Schema::col_of<DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiterals>()>...);
            }
        }

        template<rfl::internal::StringLiteral IndexName, rfl::internal::StringLiteral... ColLiterals>
            requires(sizeof...(ColLiterals) >= 1) && (DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiterals> && ...)
        [[nodiscard]] std::expected<void, Core::DbError> ensure_unique_index_created() const
        {
            return ensure_index_created<IndexName, ColLiterals...>(true);
        }

        [[nodiscard]] std::expected<bool, Core::DbError> try_any() const
        {
            if (!conn_)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is null") };
            }
            if (!conn_->is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is closed") };
            }
            if (limit_.has_value() && *limit_ == 0)
            {
                return false;
            }
            if (!limit_.has_value() && !offset_.has_value())
            {
                auto cnt_res = try_count();
                if (!cnt_res)
                {
                    return std::unexpected<Core::DbError>{ cnt_res.error() };
                }
                return *cnt_res > 0;
            }
            if constexpr (requires(const ConnectionHandle& c) { c.template try_execute_read<DTO>(sqlgen::read<std::vector<DTO>>); })
            {
                auto dto_res = limit(1).execute_query(*conn_);
                if (!dto_res)
                {
                    return std::unexpected<Core::DbError>{ dto_res.error() };
                }
                return !dto_res->empty();
            }
            else
            {
                auto cnt_res = try_count();
                if (!cnt_res)
                {
                    return std::unexpected<Core::DbError>{ cnt_res.error() };
                }
                return *cnt_res > 0;
            }
        }

        [[nodiscard]] bool any() const
        {
            return try_any().value_or(false);
        }

        [[nodiscard]] std::expected<void, Core::DbError> remove_where() const
            requires(!std::same_as<WhereCondT, rfl::Nothing>)
        {
            if (!conn_)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is null") };
            }
            if (!conn_->is_open())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is closed") };
            }
            if (limit_.has_value() && *limit_ == 0)
            {
                return {};
            }

            if (!limit_.has_value() && !offset_.has_value())
            {
                if constexpr (requires(ConnectionHandle& c) { c.template delete_where<DTO>(where_cond_); })
                {
                    auto del_res = conn_->template delete_where<DTO>(where_cond_);
                    if (del_res && clear_track_fn_)
                    {
                        clear_track_fn_();
                    }
                    return del_res;
                }
            }

            // Collect keys without hydrating Domain or polluting ChangeTracker, then delete atomically.
            std::vector<KeyType> keys_to_delete;
            if constexpr (requires(const ConnectionHandle& c) { c.template try_execute_read<DTO>(sqlgen::read<std::vector<DTO>>); })
            {
                auto dto_res = execute_query(*conn_);
                if (!dto_res)
                {
                    return std::unexpected<Core::DbError>{ dto_res.error() };
                }
                keys_to_delete.reserve(dto_res->size());
                for (const auto& dto : *dto_res)
                {
                    keys_to_delete.push_back(Schema::EntityMetadata<Domain, DTO>::extract_dto_key(dto));
                }
            }
            else
            {
                return std::unexpected<Core::DbError>{ Core::DbError::unknown(
                    "Connection adapter does not support server-side WHERE queries") };
            }

            if (keys_to_delete.empty())
            {
                return {};
            }

            detail::WriteBatchTxGuard<ConnectionHandle> tx_guard(conn_);
            if (auto begin_res = tx_guard.begin(); !begin_res)
            {
                return begin_res;
            }

            for (const auto& key : keys_to_delete)
            {
                auto del_res = repo_->delete_by_id(key);
                if (!del_res)
                {
                    return del_res;
                }
            }

            if (auto commit_res = tx_guard.commit(); !commit_res)
            {
                return commit_res;
            }

            if (untrack_key_fn_)
            {
                for (const auto& key : keys_to_delete)
                {
                    untrack_key_fn_(key);
                }
            }
            else if (clear_track_fn_)
            {
                clear_track_fn_();
            }
            return {};
        }

        template<rfl::internal::StringLiteral... ColLiterals, typename... Vals>
            requires(sizeof...(ColLiterals) >= 1) && (sizeof...(ColLiterals) == sizeof...(Vals)) &&
            (!std::same_as<WhereCondT, rfl::Nothing>) && (DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiterals> && ...)
        [[nodiscard]] std::expected<void, Core::DbError> update_where(const Vals&... new_vals) const
        {
            if (!conn_)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is null") };
            }
            if (limit_.has_value() || offset_.has_value())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation(
                    "Bulk update_where does not support limit or offset pagination") };
            }
            auto make_set_expr = []<rfl::internal::StringLiteral ColLit, typename ValT>(const ValT& v)
            {
                constexpr auto dto_m = DAL::Mappers::resolve_dto_member<Domain, DTO, ColLit>();
                return Schema::col_of<dto_m>.set(detail::normalize_query_val(v));
            };

            std::expected<void, Core::DbError> res{};
            if constexpr (sizeof...(ColLiterals) == 1)
            {
                res = conn_->template update_where<DTO>(make_set_expr.template operator()<ColLiterals>(new_vals)..., where_cond_);
            }
            else
            {
                res = conn_->template update_where_many<DTO>(where_cond_, make_set_expr.template operator()<ColLiterals>(new_vals)...);
            }
            if (res && clear_track_fn_)
            {
                clear_track_fn_();
            }
            return res;
        }

    private:
        template<typename RowTuple, typename BaseQuery>
        [[nodiscard]] std::expected<std::vector<RowTuple>, Core::DbError> execute_tuple_projection(
            const ConnectionHandle& conn, BaseQuery&& base_query) const
        {
            if (limit_.has_value() && offset_.has_value())
            {
                auto q = std::forward<BaseQuery>(base_query) | sqlgen::limit(*limit_) | sqlgen::offset(*offset_);
                return conn.template try_execute_tuple_read<RowTuple>(std::move(q));
            }
            if (limit_.has_value())
            {
                auto q = std::forward<BaseQuery>(base_query) | sqlgen::limit(*limit_);
                return conn.template try_execute_tuple_read<RowTuple>(std::move(q));
            }
            if (offset_.has_value())
            {
                auto q = std::forward<BaseQuery>(base_query) | sqlgen::limit(detail::kSqliteUnboundedLimit) | sqlgen::offset(*offset_);
                return conn.template try_execute_tuple_read<RowTuple>(std::move(q));
            }
            return conn.template try_execute_tuple_read<RowTuple>(std::forward<BaseQuery>(base_query));
        }

        template<typename OutRowT, typename BaseQuery>
        [[nodiscard]] std::expected<std::vector<OutRowT>, Core::DbError> execute_projection(
            const ConnectionHandle& conn, BaseQuery&& base_query) const
        {
            if (limit_.has_value() && offset_.has_value())
            {
                auto q = std::forward<BaseQuery>(base_query) | sqlgen::limit(*limit_) | sqlgen::offset(*offset_) |
                    sqlgen::to<std::vector<OutRowT>>;
                return conn.template try_execute_read<OutRowT>(std::move(q));
            }
            if (limit_.has_value())
            {
                auto q = std::forward<BaseQuery>(base_query) | sqlgen::limit(*limit_) | sqlgen::to<std::vector<OutRowT>>;
                return conn.template try_execute_read<OutRowT>(std::move(q));
            }
            if (offset_.has_value())
            {
                auto q = std::forward<BaseQuery>(base_query) | sqlgen::limit(detail::kSqliteUnboundedLimit) | sqlgen::offset(*offset_) |
                    sqlgen::to<std::vector<OutRowT>>;
                return conn.template try_execute_read<OutRowT>(std::move(q));
            }
            auto q = std::forward<BaseQuery>(base_query) | sqlgen::to<std::vector<OutRowT>>;
            return conn.template try_execute_read<OutRowT>(std::move(q));
        }

        template<typename ResultT, typename AggExpr>
        [[nodiscard]] std::expected<std::optional<ResultT>, Core::DbError> try_aggregate_impl(const AggExpr& agg_expr) const
        {
            if (!conn_)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is null") };
            }
            if (limit_.has_value() || offset_.has_value())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation(
                    "Aggregate queries do not support limit or offset pagination") };
            }
            if constexpr (std::same_as<WhereCondT, rfl::Nothing>)
            {
                return conn_->template try_aggregate<DTO, ResultT>(agg_expr);
            }
            else
            {
                return conn_->template try_aggregate_where<DTO, ResultT>(agg_expr, where_cond_);
            }
        }

        template<typename BaseQuery>
        [[nodiscard]] std::expected<std::vector<DTO>, Core::DbError> execute_with_limit_offset(
            const ConnectionHandle& conn, BaseQuery&& base_query) const
        {
            if (limit_.has_value() && offset_.has_value())
            {
                auto q = std::forward<BaseQuery>(base_query) | sqlgen::limit(*limit_) | sqlgen::offset(*offset_);
                return conn.template try_execute_read<DTO>(std::move(q));
            }
            if (limit_.has_value())
            {
                auto q = std::forward<BaseQuery>(base_query) | sqlgen::limit(*limit_);
                return conn.template try_execute_read<DTO>(std::move(q));
            }
            if (offset_.has_value())
            {
                auto q = std::forward<BaseQuery>(base_query) | sqlgen::limit(detail::kSqliteUnboundedLimit) | sqlgen::offset(*offset_);
                return conn.template try_execute_read<DTO>(std::move(q));
            }
            return conn.template try_execute_read<DTO>(std::forward<BaseQuery>(base_query));
        }

        [[nodiscard]] std::expected<std::vector<DTO>, Core::DbError> execute_query(const ConnectionHandle& conn) const
        {
            auto with_where = [&]()
            {
                if constexpr (!std::same_as<WhereCondT, rfl::Nothing>)
                {
                    return sqlgen::read<std::vector<DTO>> | sqlgen::where(where_cond_);
                }
                else
                {
                    return sqlgen::read<std::vector<DTO>>;
                }
            }();

            if constexpr (!std::same_as<OrderByT, rfl::Nothing>)
            {
                return std::visit(
                    [&](const auto& ob)
                    {
                        return execute_with_limit_offset(conn, with_where | ob);
                    },
                    order_by_);
            }
            else
            {
                return execute_with_limit_offset(conn, std::move(with_where));
            }
        }

        std::shared_ptr<RepoType> repo_;
        std::shared_ptr<ConnectionHandle> conn_;
        WhereCondT where_cond_{};
        OrderByT order_by_{};
        std::optional<std::size_t> limit_{};
        std::optional<std::size_t> offset_{};
        TrackFn track_fn_{};
        ClearTrackFn clear_track_fn_{};
        UntrackKeyFn untrack_key_fn_{};
    };
    // DbSet<Domain, DTO, ConnectionHandle, KeyType> provides an Entity Framework-style strongly-typed
    // entity set abstraction over a relational table, eliminating manual repository wiring.
    template<
        typename Domain,
        typename DTO = Schema::SchemaOf_t<Domain>,
        typename ConnectionHandle = SQLiteConnection,
        typename KeyType = Schema::EntityKey_t<Domain, DTO>>
        requires DAL::Mappers::Mappable<Domain, DTO> && Repositories::DatabaseConnection<ConnectionHandle>
    class DbSet
    {
    public:
        using DomainType = Domain;
        using DTOType = DTO;
        using KeyValueType = KeyType;
        using RepoType = Repositories::GenericRepository<Domain, DTO, ConnectionHandle, KeyType>;

        explicit DbSet(std::shared_ptr<ConnectionHandle> conn) pre(conn != nullptr)
            : repo_(std::make_shared<RepoType>(conn))
            , conn_(std::move(conn))
            , local_state_(std::make_shared<detail::ContextState>())
        {
            bind_tracking_callbacks(local_state_);
        }

        template<typename Ctx>
            requires requires(Ctx& ctx, std::function<std::expected<void, Core::DbError>()> op) {
                { ctx.connection() } -> std::convertible_to<std::shared_ptr<ConnectionHandle>>;
                { ctx.is_deferred() } -> std::same_as<bool>;
                ctx.enqueue_op(std::move(op));
            }
        explicit DbSet(Ctx* ctx) pre(ctx != nullptr)
            : repo_(std::make_shared<RepoType>(ctx->connection()))
            , conn_(ctx->connection())
        {
            if constexpr (requires(Ctx& c) { c.weak_context_state(); })
            {
                const auto weak_state = ctx->weak_context_state();
                is_deferred_fn_ = [weak_state]() noexcept
                {
                    auto st = weak_state.lock();
                    return st && st->defer_changes_;
                };
                enqueue_op_fn_ = [weak_state](std::move_only_function<std::expected<void, Core::DbError>()> op) -> bool
                {
                    if (auto st = weak_state.lock())
                    {
                        st->pending_ops_.push_back(std::move(op));
                        return true;
                    }
                    return false;
                };
                bind_tracking_callbacks(weak_state);
            }
            else
            {
                is_deferred_fn_ = [ctx]()
                {
                    return ctx->is_deferred();
                };
                enqueue_op_fn_ = [ctx](std::move_only_function<std::expected<void, Core::DbError>()> op) -> bool
                {
                    ctx->enqueue_op(std::function<std::expected<void, Core::DbError>()>(std::move(op)));
                    return true;
                };
                if constexpr (requires(Ctx& c) {
                                  c.is_change_tracking_enabled();
                                  c.clear_change_tracker();
                              })
                {
                    is_tracker_alive_fn_ = []() noexcept
                    {
                        return true;
                    };
                    is_tracking_enabled_fn_ = [ctx]()
                    {
                        return ctx->is_change_tracking_enabled();
                    };
                    upsert_tracked_fn_ =
                        [ctx](
                            std::type_index type,
                            std::string key_repr,
                            bool refresh_baseline,
                            std::shared_ptr<void> owned,
                            std::function<std::optional<std::function<std::expected<void, Core::DbError>()>>(const std::shared_ptr<void>&)>
                                detect_fn,
                            std::function<std::shared_ptr<void>()> snap_fn,
                            std::function<void(const std::shared_ptr<void>&)> restore_fn)
                    {
                        ctx->upsert_tracked_entry(
                            type,
                            std::move(key_repr),
                            refresh_baseline,
                            std::move(owned),
                            std::move(detect_fn),
                            std::move(snap_fn),
                            std::move(restore_fn));
                    };
                    untrack_fn_ = [ctx](std::type_index type, const std::string& key_repr)
                    {
                        ctx->untrack_key(type, key_repr);
                    };
                    untrack_type_fn_ = [ctx](std::type_index type)
                    {
                        ctx->untrack_type(type);
                    };
                    find_tracked_fn_ = [ctx](std::type_index type, const std::string& key_repr)
                    {
                        return ctx->find_tracked_instance(type, key_repr);
                    };
                }
            }
        }

        template<typename Ctx>
            requires requires(const Ctx& ctx) {
                { ctx.connection() } -> std::convertible_to<std::shared_ptr<ConnectionHandle>>;
            } && (!requires(Ctx& ctx) { ctx.is_deferred(); })
        explicit DbSet(const Ctx* ctx) pre(ctx != nullptr)
            : repo_(std::make_shared<RepoType>(ctx->connection()))
            , conn_(ctx->connection())
            , local_state_(std::make_shared<detail::ContextState>())
        {
            bind_tracking_callbacks(local_state_);
        }

        // Retrieves all domain entities from the underlying table
        [[nodiscard]] std::vector<Domain> to_vector() const
        {
            auto items = repo_->get_all();
            if (is_tracking_enabled())
            {
                for (const auto& item : items)
                {
                    record_snapshot(item);
                }
            }
            return items;
        }

        // Monadic read: retrieves all domain entities or returns a structured DbError
        [[nodiscard]] std::expected<std::vector<Domain>, Core::DbError> try_to_vector() const
        {
            auto res = repo_->try_get_all();
            if (res && is_tracking_enabled())
            {
                for (const auto& item : res.value())
                {
                    record_snapshot(item);
                }
            }
            return res;
        }

        // Finds an entity by its primary key (SingleKey, CompositeKey, or scalar)
        [[nodiscard]] std::optional<Domain> find(const KeyType& id) const
        {
            auto item = repo_->get_by_id(id);
            if (item && is_tracking_enabled())
            {
                record_snapshot(*item);
            }
            return item;
        }

        template<typename K>
            requires Core::SingleKeyType<KeyType> && std::constructible_from<typename KeyType::key_type, K> &&
            (!std::same_as<std::remove_cvref_t<K>, KeyType>)
        [[nodiscard]] std::optional<Domain> find(const K& raw_or_id) const
        {
            return find(KeyType{ static_cast<typename KeyType::key_type>(raw_or_id) });
        }

        template<typename... Ks>
            requires Core::CompositeKeyType<KeyType> && (sizeof...(Ks) == KeyType::arity) && std::constructible_from<KeyType, Ks...>
        [[nodiscard]] std::optional<Domain> find(Ks&&... parts) const
        {
            return find(KeyType{ std::forward<Ks>(parts)... });
        }

        // Monadic read: finds an entity by its primary key or returns a structured DbError
        [[nodiscard]] std::expected<std::optional<Domain>, Core::DbError> try_find(const KeyType& id) const
        {
            auto res = repo_->try_get_by_id(id);
            if (res && res.value() && is_tracking_enabled())
            {
                record_snapshot(*res.value());
            }
            return res;
        }

        template<typename K>
            requires Core::SingleKeyType<KeyType> && std::constructible_from<typename KeyType::key_type, K> &&
            (!std::same_as<std::remove_cvref_t<K>, KeyType>)
        [[nodiscard]] std::expected<std::optional<Domain>, Core::DbError> try_find(const K& raw_or_id) const
        {
            return try_find(KeyType{ static_cast<typename KeyType::key_type>(raw_or_id) });
        }

        template<typename... Ks>
            requires Core::CompositeKeyType<KeyType> && (sizeof...(Ks) == KeyType::arity) && std::constructible_from<KeyType, Ks...>
        [[nodiscard]] std::expected<std::optional<Domain>, Core::DbError> try_find(Ks&&... parts) const
        {
            return try_find(KeyType{ std::forward<Ks>(parts)... });
        }

        // Inserts a single entity into the database table (or stages if deferred)
        [[nodiscard]] std::expected<void, Core::DbError> add(const Domain& item)
        {
            if (should_defer())
            {
                return stage_add(item);
            }
            auto res = repo_->insert_one(item);
            if (res && is_tracking_enabled() && !has_unset_auto_incr_key(item))
            {
                record_snapshot(item);
            }
            return res;
        }

        // Inserts a single entity atomically (wrapping insert + rowid lookup + key hydration in a
        // transaction or savepoint with RAII rollback) and returns a copy populated with any
        // database-generated auto-increment primary key.
        [[nodiscard]] std::expected<Domain, Core::DbError> add_returning(const Domain& item)
        {
            return add_returning_impl(item, true);
        }

        // Inserts multiple entities atomically in a transaction (or savepoint if already in one)
        // with RAII rollback on both returned errors and thrown exceptions, updating tracker state
        // only after commit.
        [[nodiscard]] std::expected<std::vector<Domain>, Core::DbError> add_range_returning(const std::vector<Domain>& items)
        {
            std::vector<Domain> results;
            results.reserve(items.size());
            if (items.empty())
            {
                return results;
            }

            detail::WriteBatchTxGuard<ConnectionHandle> tx_guard(conn_);
            if (auto begin_res = tx_guard.begin(); !begin_res)
            {
                return std::unexpected<Core::DbError>{ begin_res.error() };
            }

            for (const auto& item : items)
            {
                auto res = add_returning_impl(item, false);
                if (!res)
                {
                    return std::unexpected<Core::DbError>{ res.error() };
                }
                results.push_back(std::move(*res));
            }

            if (auto commit_res = tx_guard.commit(); !commit_res)
            {
                return std::unexpected<Core::DbError>{ commit_res.error() };
            }

            if (is_tracking_enabled())
            {
                for (const auto& r : results)
                {
                    record_snapshot(r);
                }
            }
            return results;
        }

        // Inserts multiple entities atomically into the database table (or stages if deferred)
        [[nodiscard]] std::expected<void, Core::DbError> add_range(const std::vector<Domain>& items)
        {
            if (should_defer())
            {
                return stage_add_range(items);
            }
            if (items.empty())
            {
                return {};
            }
            detail::WriteBatchTxGuard<ConnectionHandle> tx_guard(conn_);
            if (auto begin_res = tx_guard.begin(); !begin_res)
            {
                return std::unexpected<Core::DbError>{ begin_res.error() };
            }
            auto res = repo_->insert_many(items);
            if (!res)
            {
                return res;
            }
            if (auto commit_res = tx_guard.commit(); !commit_res)
            {
                return commit_res;
            }
            if (is_tracking_enabled())
            {
                for (const auto& item : items)
                {
                    if (!has_unset_auto_incr_key(item))
                    {
                        record_snapshot(item);
                    }
                }
            }
            return {};
        }

        // Updates an existing entity (or stages if deferred)
        [[nodiscard]] std::expected<void, Core::DbError> update(const Domain& item)
        {
            if (should_defer())
            {
                return stage_update(item);
            }
            auto res = repo_->update_one(item);
            if (res && is_tracking_enabled())
            {
                record_snapshot(item);
            }
            return res;
        }

        // Removes an entity by primary key (or stages if deferred).
        // Tracker eviction is deferred until SQLite confirms the delete.
        [[nodiscard]] std::expected<void, Core::DbError> remove(const KeyType& id)
        {
            if (should_defer())
            {
                return stage_remove(id);
            }
            auto res = repo_->delete_by_id(id);
            if (res)
            {
                untrack_key(id);
            }
            return res;
        }

        // Upserts a single entity: uses SQLite ON CONFLICT DO UPDATE for assigned/composite keys,
        // or a transaction/savepoint-guarded exists+update/insert for auto-increment keys.
        [[nodiscard]] std::expected<void, Core::DbError> upsert(const Domain& item)
        {
            if (should_defer())
            {
                return stage_upsert(item);
            }
            return upsert_immediate(item);
        }

        // Atomic batch upsert: wraps the entire range (both assigned-key and auto-increment paths)
        // in a transaction or savepoint so any mid-batch failure rolls back all earlier rows,
        // updating tracker state only after commit.
        [[nodiscard]] std::expected<void, Core::DbError> upsert_range(const std::vector<Domain>& items)
        {
            if (should_defer())
            {
                return stage_upsert_range(items);
            }
            return upsert_many_immediate(items);
        }

        [[nodiscard]] std::expected<void, Core::DbError> stage_upsert(const Domain& item)
        {
            if (!enqueue_op_fn_)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error(
                    "Cannot stage operation: DbSet is not bound to a DbContext") };
            }
            if (!enqueue_op_fn_(
                    [repo = repo_, conn = conn_, upsert_fn = upsert_tracked_fn_, tracking_fn = is_tracking_enabled_fn_, item]()
                    {
                        return upsert_immediate_with(repo, conn, upsert_fn, tracking_fn, item);
                    }))
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error(
                    "Cannot stage operation: owning DbContext has been destroyed") };
            }
            return {};
        }

        [[nodiscard]] std::expected<void, Core::DbError> stage_upsert_range(const std::vector<Domain>& items)
        {
            if (!enqueue_op_fn_)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error(
                    "Cannot stage operation: DbSet is not bound to a DbContext") };
            }
            if (!enqueue_op_fn_(
                    [repo = repo_, conn = conn_, upsert_fn = upsert_tracked_fn_, tracking_fn = is_tracking_enabled_fn_, items]()
                    {
                        return upsert_many_immediate_with(repo, conn, upsert_fn, tracking_fn, items);
                    }))
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error(
                    "Cannot stage operation: owning DbContext has been destroyed") };
            }
            return {};
        }

        // Explicit Unit of Work staging methods (always enqueue into DbContext)
        [[nodiscard]] std::expected<void, Core::DbError> stage_add(const Domain& item)
        {
            if (!enqueue_op_fn_)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error(
                    "Cannot stage operation: DbSet is not bound to a DbContext") };
            }
            if (!enqueue_op_fn_(
                    [repo = repo_, upsert_fn = upsert_tracked_fn_, tracking_fn = is_tracking_enabled_fn_, item]()
                    {
                        auto res = repo->insert_one(item);
                        if (res && tracking_fn && tracking_fn() && !has_unset_auto_incr_key(item))
                        {
                            record_snapshot_with(repo, upsert_fn, item);
                        }
                        return res;
                    }))
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error(
                    "Cannot stage operation: owning DbContext has been destroyed") };
            }
            return {};
        }

        [[nodiscard]] std::expected<void, Core::DbError> stage_add_range(const std::vector<Domain>& items)
        {
            if (!enqueue_op_fn_)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error(
                    "Cannot stage operation: DbSet is not bound to a DbContext") };
            }
            if (!enqueue_op_fn_(
                    [repo = repo_, upsert_fn = upsert_tracked_fn_, tracking_fn = is_tracking_enabled_fn_, items]()
                    {
                        auto res = repo->insert_many(items);
                        if (res && tracking_fn && tracking_fn())
                        {
                            for (const auto& item : items)
                            {
                                if (!has_unset_auto_incr_key(item))
                                {
                                    record_snapshot_with(repo, upsert_fn, item);
                                }
                            }
                        }
                        return res;
                    }))
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error(
                    "Cannot stage operation: owning DbContext has been destroyed") };
            }
            return {};
        }

        [[nodiscard]] std::expected<void, Core::DbError> stage_update(const Domain& item)
        {
            if (!enqueue_op_fn_)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error(
                    "Cannot stage operation: DbSet is not bound to a DbContext") };
            }
            if (!enqueue_op_fn_(
                    [repo = repo_, upsert_fn = upsert_tracked_fn_, tracking_fn = is_tracking_enabled_fn_, item]()
                    {
                        auto res = repo->update_one(item);
                        if (res && tracking_fn && tracking_fn())
                        {
                            record_snapshot_with(repo, upsert_fn, item);
                        }
                        return res;
                    }))
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error(
                    "Cannot stage operation: owning DbContext has been destroyed") };
            }
            return {};
        }

        [[nodiscard]] std::expected<void, Core::DbError> stage_remove(const KeyType& id)
        {
            if (!enqueue_op_fn_)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error(
                    "Cannot stage operation: DbSet is not bound to a DbContext") };
            }
            const std::string key_repr = format_key(id);
            if (!enqueue_op_fn_(
                    [repo = repo_, untrack_fn = untrack_fn_, id, key_repr]()
                    {
                        auto res = repo->delete_by_id(id);
                        if (res && untrack_fn)
                        {
                            untrack_fn(std::type_index(typeid(Domain)), key_repr);
                        }
                        return res;
                    }))
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error(
                    "Cannot stage operation: owning DbContext has been destroyed") };
            }
            return {};
        }

        template<typename K>
            requires Core::SingleKeyType<KeyType> && std::constructible_from<typename KeyType::key_type, K> &&
            (!std::same_as<std::remove_cvref_t<K>, KeyType>)
        [[nodiscard]] std::expected<void, Core::DbError> stage_remove(const K& raw_or_id)
        {
            return stage_remove(KeyType{ static_cast<typename KeyType::key_type>(raw_or_id) });
        }

        template<typename... Ks>
            requires Core::CompositeKeyType<KeyType> && (sizeof...(Ks) == KeyType::arity) && std::constructible_from<KeyType, Ks...>
        [[nodiscard]] std::expected<void, Core::DbError> stage_remove(Ks&&... parts)
        {
            return stage_remove(KeyType{ std::forward<Ks>(parts)... });
        }

        template<typename K>
            requires Core::SingleKeyType<KeyType> && std::constructible_from<typename KeyType::key_type, K> &&
            (!std::same_as<std::remove_cvref_t<K>, KeyType>)
        [[nodiscard]] std::expected<void, Core::DbError> remove(const K& raw_or_id)
        {
            return remove(KeyType{ static_cast<typename KeyType::key_type>(raw_or_id) });
        }

        template<typename... Ks>
            requires Core::CompositeKeyType<KeyType> && (sizeof...(Ks) == KeyType::arity) && std::constructible_from<KeyType, Ks...>
        [[nodiscard]] std::expected<void, Core::DbError> remove(Ks&&... parts)
        {
            return remove(KeyType{ std::forward<Ks>(parts)... });
        }

        // Checks whether an entity with the given primary key exists
        [[nodiscard]] bool contains(const KeyType& id) const
        {
            return repo_->exists_by_id(id);
        }

        template<typename K>
            requires Core::SingleKeyType<KeyType> && std::constructible_from<typename KeyType::key_type, K> &&
            (!std::same_as<std::remove_cvref_t<K>, KeyType>)
        [[nodiscard]] bool contains(const K& raw_or_id) const
        {
            return contains(KeyType{ static_cast<typename KeyType::key_type>(raw_or_id) });
        }

        template<typename... Ks>
            requires Core::CompositeKeyType<KeyType> && (sizeof...(Ks) == KeyType::arity) && std::constructible_from<KeyType, Ks...>
        [[nodiscard]] bool contains(Ks&&... parts) const
        {
            return contains(KeyType{ std::forward<Ks>(parts)... });
        }

        // Returns total row count for this entity table with full error propagation
        [[nodiscard]] std::expected<std::size_t, Core::DbError> try_count() const
        {
            return query().try_count();
        }

        // Returns total row count for this entity table
        [[nodiscard]] std::size_t count() const
        {
            return repo_->count();
        }

        // In-memory LINQ-style filter: db.set.where([](const auto& x) { return ...; })
        template<typename Predicate>
        [[nodiscard]] std::vector<Domain> where(Predicate&& pred) const
        {
            auto all = to_vector();
            std::vector<Domain> filtered;
            for (auto& item : all)
            {
                if (std::invoke(pred, item))
                {
                    filtered.push_back(std::move(item));
                }
            }
            return filtered;
        }

        // First matching element, or std::nullopt
        template<typename Predicate>
        [[nodiscard]] std::optional<Domain> first_or_default(Predicate&& pred) const
        {
            auto all = to_vector();
            for (auto& item : all)
            {
                if (std::invoke(pred, item))
                {
                    return std::move(item);
                }
            }
            return std::nullopt;
        }

        [[nodiscard]] std::expected<std::optional<Domain>, Core::DbError> try_first_or_default() const
        {
            return query().try_first_or_default();
        }

        [[nodiscard]] std::optional<Domain> first_or_default() const
        {
            return query().first_or_default();
        }

        [[nodiscard]] std::expected<bool, Core::DbError> try_any() const
        {
            return query().try_any();
        }

        [[nodiscard]] bool any() const
        {
            return query().any();
        }

        // =====================================================================
        // Task 10.1: Fluent Server-Side Query Pushdown
        // =====================================================================

        [[nodiscard]] auto query() const
        {
            std::function<void(const Domain&)> track_cb{};
            std::function<void()> clear_cb{};
            std::function<void(const KeyType&)> untrack_key_cb{};
            if (is_tracking_enabled())
            {
                track_cb = [repo = repo_, upsert_fn = upsert_tracked_fn_](const Domain& entity)
                {
                    record_snapshot_with(repo, upsert_fn, entity);
                };
                if (untrack_type_fn_)
                {
                    clear_cb = [fn = untrack_type_fn_]()
                    {
                        fn(std::type_index(typeid(Domain)));
                    };
                }
                if (untrack_fn_)
                {
                    untrack_key_cb = [fn = untrack_fn_](const KeyType& key)
                    {
                        fn(std::type_index(typeid(Domain)), Core::encode_key(key));
                    };
                }
            }
            return DbQuery<Domain, DTO, ConnectionHandle, KeyType>(
                repo_,
                conn_,
                rfl::Nothing{},
                rfl::Nothing{},
                std::nullopt,
                std::nullopt,
                std::move(track_cb),
                std::move(clear_cb),
                std::move(untrack_key_cb));
        }

        template<typename Condition>
        [[nodiscard]] auto where_sql(Condition cond) const
        {
            return query().where_sql(std::move(cond));
        }

        template<rfl::internal::StringLiteral ColLiteral, typename Val>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_equal(const Val& val) const
        {
            return query().template where_equal<ColLiteral>(val);
        }

        template<rfl::internal::StringLiteral ColLiteral, typename Val>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_not_equal(const Val& val) const
        {
            return query().template where_not_equal<ColLiteral>(val);
        }

        template<rfl::internal::StringLiteral ColLiteral, typename Val>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_lt(const Val& val) const
        {
            return query().template where_lt<ColLiteral>(val);
        }

        template<rfl::internal::StringLiteral ColLiteral, typename Val>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_le(const Val& val) const
        {
            return query().template where_le<ColLiteral>(val);
        }

        template<rfl::internal::StringLiteral ColLiteral, typename Val>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_gt(const Val& val) const
        {
            return query().template where_gt<ColLiteral>(val);
        }

        template<rfl::internal::StringLiteral ColLiteral, typename Val>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_ge(const Val& val) const
        {
            return query().template where_ge<ColLiteral>(val);
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_like(std::string_view pattern) const
        {
            return query().template where_like<ColLiteral>(pattern);
        }

        template<rfl::internal::StringLiteral ColLiteral, typename... Ts>
            requires(sizeof...(Ts) > 0) && DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_in(const Ts&... vals) const
        {
            return query().template where_in<ColLiteral>(vals...);
        }

        template<rfl::internal::StringLiteral ColLiteral, typename Val>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_in_vec(const std::vector<Val>& vals) const
        {
            return query().template where_in_vec<ColLiteral>(vals);
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_null() const
        {
            return query().template where_null<ColLiteral>();
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_not_null() const
        {
            return query().template where_not_null<ColLiteral>();
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_not_like(std::string_view pattern) const
        {
            return query().template where_not_like<ColLiteral>(pattern);
        }

        template<rfl::internal::StringLiteral ColLiteral, typename... Ts>
            requires(sizeof...(Ts) > 0) && DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_not_in(const Ts&... vals) const
        {
            return query().template where_not_in<ColLiteral>(vals...);
        }

        template<rfl::internal::StringLiteral ColLiteral, typename Val>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_between(const Val& low, const Val& high) const
        {
            return query().template where_between<ColLiteral>(low, high);
        }

        template<rfl::internal::StringLiteral ColLiteral, typename Val>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto where_not_in_vec(const std::vector<Val>& vals) const
        {
            return query().template where_not_in_vec<ColLiteral>(vals);
        }

        template<rfl::internal::StringLiteral ColLiteral, typename Val>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] std::expected<void, Core::DbError> remove_where_equal(const Val& val) const
        {
            return where_equal<ColLiteral>(val).remove_where();
        }

        template<rfl::internal::StringLiteral MatchCol, rfl::internal::StringLiteral... SetCols, typename MatchVal, typename... SetVals>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, MatchCol> && (sizeof...(SetCols) >= 1) &&
            (sizeof...(SetCols) == sizeof...(SetVals)) && (DAL::Mappers::ValidQueryColumn<Domain, DTO, SetCols> && ...)
        [[nodiscard]] std::expected<void, Core::DbError> update_where_equal(const MatchVal& match_val, const SetVals&... new_vals) const
        {
            return where_equal<MatchCol>(match_val).template update_where<SetCols...>(new_vals...);
        }

        template<rfl::internal::StringLiteral IndexName, rfl::internal::StringLiteral... ColLiterals>
            requires(sizeof...(ColLiterals) >= 1) && (DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiterals> && ...)
        [[nodiscard]] std::expected<void, Core::DbError> ensure_index_created(bool unique = false) const
        {
            if (!conn_)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is null") };
            }
            return conn_->template ensure_index_created<IndexName, DTO>(
                unique, Schema::col_of<DAL::Mappers::resolve_dto_member<Domain, DTO, ColLiterals>()>...);
        }

        template<rfl::internal::StringLiteral IndexName, rfl::internal::StringLiteral... ColLiterals>
            requires(sizeof...(ColLiterals) >= 1) && (DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiterals> && ...)
        [[nodiscard]] std::expected<void, Core::DbError> ensure_unique_index_created() const
        {
            return ensure_index_created<IndexName, ColLiterals...>(true);
        }

        template<typename ParentDomain>
        [[nodiscard]] auto where_foreign_key(const ParentDomain& parent) const
        {
            const auto parent_key = Schema::EntityMetadata<ParentDomain>::extract_key(parent);
            const auto raw_pk = detail::normalize_query_val(parent_key);
            return where_sql(Schema::make_fk_where<DTO, Schema::SchemaOf_t<ParentDomain>>(raw_pk));
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto order_by(bool desc = false) const
        {
            return query().template order_by<ColLiteral>(desc);
        }

        [[nodiscard]] auto page(std::size_t limit_count, std::size_t offset_count = 0) const
        {
            return query().page(limit_count, offset_count);
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto try_min() const
        {
            return query().template try_min<ColLiteral>();
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto min() const
        {
            return query().template min<ColLiteral>();
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto try_max() const
        {
            return query().template try_max<ColLiteral>();
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto max() const
        {
            return query().template max<ColLiteral>();
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto try_sum() const
        {
            return query().template try_sum<ColLiteral>();
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto sum() const
        {
            return query().template sum<ColLiteral>();
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] std::expected<std::optional<double>, Core::DbError> try_avg() const
        {
            return query().template try_avg<ColLiteral>();
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] std::optional<double> avg() const
        {
            return query().template avg<ColLiteral>();
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto try_select() const
        {
            return query().template try_select<ColLiteral>();
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto select() const
        {
            return query().template select<ColLiteral>();
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] std::expected<std::size_t, Core::DbError> try_count_distinct() const
        {
            return query().template try_count_distinct<ColLiteral>();
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] std::size_t count_distinct() const
        {
            return query().template count_distinct<ColLiteral>();
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto try_select_distinct() const
        {
            return query().template try_select_distinct<ColLiteral>();
        }

        template<rfl::internal::StringLiteral ColLiteral>
            requires DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiteral>
        [[nodiscard]] auto select_distinct() const
        {
            return query().template select_distinct<ColLiteral>();
        }

        template<rfl::internal::StringLiteral... ColLiterals>
            requires(sizeof...(ColLiterals) >= 2) && (DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiterals> && ...)
        [[nodiscard]] auto try_select_columns() const
        {
            return query().template try_select_columns<ColLiterals...>();
        }

        template<rfl::internal::StringLiteral... ColLiterals>
            requires(sizeof...(ColLiterals) >= 2) && (DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiterals> && ...)
        [[nodiscard]] auto select_columns() const
        {
            return query().template select_columns<ColLiterals...>();
        }

        template<rfl::internal::StringLiteral... ColLiterals>
            requires(sizeof...(ColLiterals) >= 2) && (DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiterals> && ...)
        [[nodiscard]] auto try_select_distinct_columns() const
        {
            return query().template try_select_distinct_columns<ColLiterals...>();
        }

        template<rfl::internal::StringLiteral... ColLiterals>
            requires(sizeof...(ColLiterals) >= 2) && (DAL::Mappers::ValidQueryColumn<Domain, DTO, ColLiterals> && ...)
        [[nodiscard]] auto select_distinct_columns() const
        {
            return query().template select_distinct_columns<ColLiterals...>();
        }

        // =====================================================================
        // Task 10.2: Identity Map & Snapshot ChangeTracker Helpers
        // =====================================================================

        // Value-owned tracking: stores a shared copy inside the ChangeTracker so caller stack
        // lifetimes can never dangle.
        [[nodiscard]] std::shared_ptr<Domain> track_copy(Domain entity) const
        {
            auto key = Schema::EntityMetadata<Domain, DTO>::extract_key(entity);
            auto owned = std::make_shared<Domain>(std::move(entity));
            auto baseline = std::make_shared<DTO>(DAL::Mappers::MapperTraits<Domain, DTO>::to_dto(*owned));
            upsert_snapshot_entry(upsert_tracked_fn_, format_key(key), false, owned, repo_, std::move(baseline));
            return owned;
        }

        // Loads an entity into the identity map and returns a shared_ptr whose mutations
        // are automatically detected and persisted on ctx.save_changes().
        [[nodiscard]] std::shared_ptr<Domain> find_tracked(const KeyType& id) const
        {
            const std::string key_repr = format_key(id);
            if (find_tracked_fn_)
            {
                if (auto existing = find_tracked_fn_(std::type_index(typeid(Domain)), key_repr))
                {
                    return std::static_pointer_cast<Domain>(existing);
                }
            }
            auto loaded = repo_->get_by_id(id);
            if (!loaded)
            {
                return nullptr;
            }
            auto shared = std::make_shared<Domain>(std::move(*loaded));
            auto baseline = std::make_shared<DTO>(DAL::Mappers::MapperTraits<Domain, DTO>::to_dto(*shared));
            upsert_snapshot_entry(upsert_tracked_fn_, key_repr, true, shared, repo_, std::move(baseline));
            return shared;
        }

        template<typename K>
            requires Core::SingleKeyType<KeyType> && std::constructible_from<typename KeyType::key_type, K> &&
            (!std::same_as<std::remove_cvref_t<K>, KeyType>)
        [[nodiscard]] std::shared_ptr<Domain> find_tracked(const K& raw_or_id) const
        {
            return find_tracked(KeyType{ static_cast<typename KeyType::key_type>(raw_or_id) });
        }

        template<typename... Ks>
            requires Core::CompositeKeyType<KeyType> && (sizeof...(Ks) == KeyType::arity) && std::constructible_from<KeyType, Ks...>
        [[nodiscard]] std::shared_ptr<Domain> find_tracked(Ks&&... parts) const
        {
            return find_tracked(KeyType{ std::forward<Ks>(parts)... });
        }

        // Returns a non-owning reference to the tracked entity in the change tracker if present (P2988).
        // Returns std::nullopt if the owning DbContext/tracker has expired so references can never dangle.
        [[nodiscard]] std::optional<Domain&> find_tracked_ref(const KeyType& id) const
        {
            if (!is_tracker_alive_fn_ || !is_tracker_alive_fn_())
            {
                return std::nullopt;
            }
            auto ptr = find_tracked(id);
            if (!ptr)
            {
                return std::nullopt;
            }
            if (!find_tracked_fn_ || !find_tracked_fn_(std::type_index(typeid(Domain)), format_key(id)))
            {
                return std::nullopt;
            }
            return *ptr;
        }

        template<typename K>
            requires Core::SingleKeyType<KeyType> && (!std::same_as<std::remove_cvref_t<K>, KeyType>) &&
            std::constructible_from<typename KeyType::key_type, K>
        [[nodiscard]] std::optional<Domain&> find_tracked_ref(const K& raw_or_id) const
        {
            return find_tracked_ref(KeyType{ static_cast<typename KeyType::key_type>(raw_or_id) });
        }

        template<typename... Ks>
            requires Core::CompositeKeyType<KeyType> && (sizeof...(Ks) == KeyType::arity) && std::constructible_from<KeyType, Ks...>
        [[nodiscard]] std::optional<Domain&> find_tracked_ref(Ks&&... parts) const
        {
            return find_tracked_ref(KeyType{ std::forward<Ks>(parts)... });
        }

        [[nodiscard]] std::vector<std::shared_ptr<Domain>> to_tracked_vector() const
        {
            auto items = repo_->get_all();
            std::vector<std::shared_ptr<Domain>> result;
            result.reserve(items.size());
            for (auto& item : items)
            {
                auto key = Schema::EntityMetadata<Domain, DTO>::extract_key(item);
                const std::string key_repr = format_key(key);
                if (find_tracked_fn_)
                {
                    if (auto existing = find_tracked_fn_(std::type_index(typeid(Domain)), key_repr))
                    {
                        result.push_back(std::static_pointer_cast<Domain>(existing));
                        continue;
                    }
                }
                auto shared = std::make_shared<Domain>(std::move(item));
                auto baseline = std::make_shared<DTO>(DAL::Mappers::MapperTraits<Domain, DTO>::to_dto(*shared));
                upsert_snapshot_entry(upsert_tracked_fn_, key_repr, true, shared, repo_, std::move(baseline));
                result.push_back(std::move(shared));
            }
            return result;
        }

        // Exposes underlying generic repository when direct repository polymorphism is needed
        [[nodiscard]] std::shared_ptr<RepoType> repository() const noexcept
        {
            return repo_;
        }

    private:
        [[nodiscard]] bool should_defer() const noexcept
        {
            return is_deferred_fn_ && is_deferred_fn_();
        }

        [[nodiscard]] bool is_tracking_enabled() const noexcept
        {
            return is_tracking_enabled_fn_ && is_tracking_enabled_fn_();
        }

        [[nodiscard]] static std::string format_key(const KeyType& key)
        {
            return Core::encode_key(key);
        }

        void untrack_key(const KeyType& key) const
        {
            if (untrack_fn_)
            {
                untrack_fn_(std::type_index(typeid(Domain)), format_key(key));
            }
        }

        using UpsertTrackedFn = std::function<void(
            std::type_index,
            std::string,
            bool,
            std::shared_ptr<void>,
            std::function<std::optional<std::function<std::expected<void, Core::DbError>()>>(const std::shared_ptr<void>&)>,
            std::function<std::shared_ptr<void>()>,
            std::function<void(const std::shared_ptr<void>&)>)>;

        void bind_tracking_callbacks(const std::weak_ptr<detail::ContextState>& weak_state)
        {
            is_tracker_alive_fn_ = [weak_state]() noexcept
            {
                return !weak_state.expired();
            };
            is_tracking_enabled_fn_ = [weak_state]() noexcept
            {
                auto st = weak_state.lock();
                return st && st->change_tracking_enabled_;
            };
            upsert_tracked_fn_ =
                [weak_state](
                    std::type_index type,
                    std::string key_repr,
                    bool refresh_baseline,
                    std::shared_ptr<void> owned,
                    std::function<std::optional<std::function<std::expected<void, Core::DbError>()>>(const std::shared_ptr<void>&)>
                        detect_fn,
                    std::function<std::shared_ptr<void>()> snap_fn,
                    std::function<void(const std::shared_ptr<void>&)> restore_fn)
            {
                if (auto st = weak_state.lock())
                {
                    st->upsert_tracked_entry(
                        type,
                        std::move(key_repr),
                        refresh_baseline,
                        std::move(owned),
                        std::move(detect_fn),
                        std::move(snap_fn),
                        std::move(restore_fn));
                }
            };
            untrack_fn_ = [weak_state](std::type_index type, const std::string& key_repr)
            {
                if (auto st = weak_state.lock())
                {
                    st->untrack_key(type, key_repr);
                }
            };
            untrack_type_fn_ = [weak_state](std::type_index type)
            {
                if (auto st = weak_state.lock())
                {
                    st->untrack_type(type);
                }
            };
            find_tracked_fn_ = [weak_state](std::type_index type, const std::string& key_repr) -> std::shared_ptr<void>
            {
                if (auto st = weak_state.lock())
                {
                    return st->find_tracked_instance(type, key_repr);
                }
                return nullptr;
            };
        }

        std::expected<void, Core::DbError> upsert_immediate(const Domain& item)
        {
            return upsert_immediate_with(repo_, conn_, upsert_tracked_fn_, is_tracking_enabled_fn_, item);
        }

        std::expected<void, Core::DbError> upsert_many_immediate(const std::vector<Domain>& items)
        {
            return upsert_many_immediate_with(repo_, conn_, upsert_tracked_fn_, is_tracking_enabled_fn_, items);
        }

        [[nodiscard]] static Core::DbError map_conn_error(const std::shared_ptr<ConnectionHandle>& conn, std::string_view msg)
        {
            if (conn)
            {
                if constexpr (requires(const ConnectionHandle& c, std::string_view m) { c.map_error(m); })
                {
                    return conn->map_error(msg);
                }
            }
            return Core::DbError::map_from_message(msg);
        }

        [[nodiscard]] std::expected<Domain, Core::DbError> add_returning_impl(const Domain& item, bool record_tracking)
        {
            detail::WriteBatchTxGuard<ConnectionHandle> tx_guard(conn_);
            if (auto begin_res = tx_guard.begin(); !begin_res)
            {
                return std::unexpected<Core::DbError>{ begin_res.error() };
            }

            auto ins_res = repo_->insert_one(item);
            if (!ins_res)
            {
                return std::unexpected<Core::DbError>{ ins_res.error() };
            }

            Domain result = item;
            if constexpr (Schema::SinglePrimaryKey<DTO> && Schema::has_auto_incr_primary_key<DTO>())
            {
                if (!conn_)
                {
                    return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is null") };
                }
                if constexpr (requires(const ConnectionHandle& c) { c.try_last_insert_rowid(); })
                {
                    auto rowid_res = conn_->try_last_insert_rowid();
                    if (!rowid_res)
                    {
                        return std::unexpected<Core::DbError>{ rowid_res.error() };
                    }
                    auto hydrated = DAL::Mappers::MapperTraits<Domain, DTO>::try_with_primary_key(item, *rowid_res);
                    if (!hydrated)
                    {
                        return std::unexpected<Core::DbError>{ hydrated.error() };
                    }
                    result = std::move(*hydrated);
                }
            }

            if (auto commit_res = tx_guard.commit(); !commit_res)
            {
                return std::unexpected<Core::DbError>{ commit_res.error() };
            }

            if (record_tracking && is_tracking_enabled())
            {
                record_snapshot(result);
            }
            return result;
        }

        [[nodiscard]] static bool has_unset_auto_incr_key(const Domain& item)
        {
            if constexpr (Schema::SinglePrimaryKey<DTO> && Schema::has_auto_incr_primary_key<DTO>())
            {
                const DTO dto = DAL::Mappers::MapperTraits<Domain, DTO>::to_dto(item);
                return dto_has_unset_auto_incr_key(dto);
            }
            else
            {
                return false;
            }
        }

        [[nodiscard]] static bool dto_has_unset_auto_incr_key(const DTO& dto)
        {
            if constexpr (Schema::SinglePrimaryKey<DTO> && Schema::has_auto_incr_primary_key<DTO>())
            {
                constexpr auto pk_m = Schema::primary_key_member<DTO>();
                const auto val = Schema::unwrap_field(dto.[:pk_m:]);
                using ValT = std::remove_cvref_t<decltype(val)>;
                if constexpr (std::is_integral_v<ValT> && !std::is_same_v<ValT, bool>)
                {
                    return val == 0;
                }
            }
            return false;
        }

        static std::expected<Domain, Core::DbError> upsert_single_returning_hydrated(
            const std::shared_ptr<RepoType>& repo, const std::shared_ptr<ConnectionHandle>& conn, const Domain& item)
        {
            const DTO dto = DAL::Mappers::MapperTraits<Domain, DTO>::to_dto(item);
            const bool needs_rowid = dto_has_unset_auto_incr_key(dto);

            if (conn)
            {
                if constexpr (requires(ConnectionHandle& c, const DTO& d) { c.upsert(d); })
                {
                    auto res = conn->upsert(dto);
                    if (!res)
                    {
                        return std::unexpected<Core::DbError>{ map_conn_error(conn, res.error().what()) };
                    }
                    if constexpr (
                        Schema::SinglePrimaryKey<DTO> && Schema::has_auto_incr_primary_key<DTO>() &&
                        requires(const ConnectionHandle& c) { c.try_last_insert_rowid(); })
                    {
                        if (needs_rowid)
                        {
                            auto rowid_res = conn->try_last_insert_rowid();
                            if (!rowid_res)
                            {
                                return std::unexpected<Core::DbError>{ rowid_res.error() };
                            }
                            return DAL::Mappers::MapperTraits<Domain, DTO>::try_with_primary_key(item, *rowid_res);
                        }
                    }
                    return item;
                }
            }

            const auto key = Schema::EntityMetadata<Domain, DTO>::extract_key(item);
            auto res = (!needs_rowid && repo->exists_by_id(key)) ? repo->update_one(item) : repo->insert_one(item);
            if (!res)
            {
                return std::unexpected<Core::DbError>{ res.error() };
            }
            if constexpr (Schema::SinglePrimaryKey<DTO> && Schema::has_auto_incr_primary_key<DTO>() && requires(const ConnectionHandle& c) {
                              c.try_last_insert_rowid();
                          })
            {
                if (needs_rowid && conn)
                {
                    auto rowid_res = conn->try_last_insert_rowid();
                    if (!rowid_res)
                    {
                        return std::unexpected<Core::DbError>{ rowid_res.error() };
                    }
                    return DAL::Mappers::MapperTraits<Domain, DTO>::try_with_primary_key(item, *rowid_res);
                }
            }
            return item;
        }

        static std::expected<void, Core::DbError> upsert_immediate_with(
            const std::shared_ptr<RepoType>& repo,
            const std::shared_ptr<ConnectionHandle>& conn,
            const UpsertTrackedFn& upsert_fn,
            const std::function<bool()>& tracking_fn,
            const Domain& item,
            bool record_tracking = true)
        {
            const bool tracking = record_tracking && tracking_fn && tracking_fn();
            detail::WriteBatchTxGuard<ConnectionHandle> tx_guard(conn);
            if (auto begin_res = tx_guard.begin(); !begin_res)
            {
                return begin_res;
            }

            auto hydrated = upsert_single_returning_hydrated(repo, conn, item);
            if (!hydrated)
            {
                return std::unexpected<Core::DbError>{ hydrated.error() };
            }

            if (auto commit_res = tx_guard.commit(); !commit_res)
            {
                return commit_res;
            }
            if (tracking)
            {
                record_snapshot_with(repo, upsert_fn, *hydrated);
            }
            return {};
        }

        static std::expected<void, Core::DbError> upsert_many_immediate_with(
            const std::shared_ptr<RepoType>& repo,
            const std::shared_ptr<ConnectionHandle>& conn,
            const UpsertTrackedFn& upsert_fn,
            const std::function<bool()>& tracking_fn,
            const std::vector<Domain>& items)
        {
            if (items.empty())
            {
                return {};
            }

            const bool tracking = tracking_fn && tracking_fn();
            detail::WriteBatchTxGuard<ConnectionHandle> tx_guard(conn);
            if (auto begin_res = tx_guard.begin(); !begin_res)
            {
                return begin_res;
            }

            bool needs_per_item_hydration = false;
            if (tracking && Schema::has_auto_incr_primary_key<DTO>())
            {
                for (const auto& item : items)
                {
                    if (has_unset_auto_incr_key(item))
                    {
                        needs_per_item_hydration = true;
                        break;
                    }
                }
            }

            std::vector<Domain> hydrated_to_track;
            bool executed_bulk = false;
            if (!needs_per_item_hydration && conn)
            {
                if constexpr (requires(ConnectionHandle& c, const std::vector<DTO>& dtos) { c.upsert_many(dtos); })
                {
                    std::vector<DTO> dtos;
                    dtos.reserve(items.size());
                    for (const auto& item : items)
                    {
                        dtos.push_back(DAL::Mappers::MapperTraits<Domain, DTO>::to_dto(item));
                    }
                    auto res = conn->upsert_many(dtos);
                    if (!res)
                    {
                        return std::unexpected<Core::DbError>{ map_conn_error(conn, res.error().what()) };
                    }
                    executed_bulk = true;
                }
            }

            if (!executed_bulk)
            {
                if (tracking)
                {
                    hydrated_to_track.reserve(items.size());
                }
                for (const auto& item : items)
                {
                    auto hydrated = upsert_single_returning_hydrated(repo, conn, item);
                    if (!hydrated)
                    {
                        return std::unexpected<Core::DbError>{ hydrated.error() };
                    }
                    if (tracking)
                    {
                        hydrated_to_track.push_back(std::move(*hydrated));
                    }
                }
            }

            if (auto commit_res = tx_guard.commit(); !commit_res)
            {
                return commit_res;
            }

            if (tracking)
            {
                const auto& to_track = executed_bulk ? items : hydrated_to_track;
                for (const auto& item : to_track)
                {
                    record_snapshot_with(repo, upsert_fn, item);
                }
            }
            return {};
        }

        [[nodiscard]] auto make_detect_fn(std::shared_ptr<DTO> baseline) const
        {
            return make_detect_fn_with(repo_, std::move(baseline));
        }

        [[nodiscard]] static auto make_detect_fn_with(std::shared_ptr<RepoType> repo, std::shared_ptr<DTO> baseline)
        {
            return [repo, baseline = std::move(baseline)](
                       const std::shared_ptr<void>& owned) -> std::optional<std::function<std::expected<void, Core::DbError>()>>
            {
                if (!owned)
                {
                    return std::nullopt;
                }
                const Domain* ptr = static_cast<const Domain*>(owned.get());

                DTO current_dto = DAL::Mappers::MapperTraits<Domain, DTO>::to_dto(*ptr);
                if (DAL::Schema::dto_equals(current_dto, *baseline))
                {
                    return std::nullopt;
                }

                Domain updated_domain = *ptr;
                return [repo, updated_domain = std::move(updated_domain), current_dto = std::move(current_dto), baseline]() mutable
                           -> std::expected<void, Core::DbError>
                {
                    auto res = repo->update_one(updated_domain);
                    if (res)
                    {
                        *baseline = std::move(current_dto);
                    }
                    return res;
                };
            };
        }

        static void upsert_snapshot_entry(
            const UpsertTrackedFn& upsert_fn,
            std::string key_repr,
            bool refresh_baseline,
            std::shared_ptr<Domain> owned,
            const std::shared_ptr<RepoType>& repo,
            std::shared_ptr<DTO> baseline)
        {
            if (!upsert_fn)
            {
                return;
            }
            auto snap_fn = [baseline]() -> std::shared_ptr<void>
            {
                return std::make_shared<DTO>(*baseline);
            };
            auto restore_fn = [baseline](const std::shared_ptr<void>& snap)
            {
                if (snap)
                {
                    *baseline = *static_cast<const DTO*>(snap.get());
                }
            };
            upsert_fn(
                std::type_index(typeid(Domain)),
                std::move(key_repr),
                refresh_baseline,
                std::move(owned),
                make_detect_fn_with(repo, std::move(baseline)),
                std::move(snap_fn),
                std::move(restore_fn));
        }

        void record_snapshot(const Domain& entity) const
        {
            record_snapshot_with(repo_, upsert_tracked_fn_, entity);
        }

        static void record_snapshot_with(
            const std::shared_ptr<RepoType>& repo, const UpsertTrackedFn& upsert_tracked_fn, const Domain& entity)
        {
            if (!upsert_tracked_fn)
            {
                return;
            }
            auto key = Schema::EntityMetadata<Domain, DTO>::extract_key(entity);
            auto baseline = std::make_shared<DTO>(DAL::Mappers::MapperTraits<Domain, DTO>::to_dto(entity));
            upsert_snapshot_entry(upsert_tracked_fn, format_key(key), true, nullptr, repo, std::move(baseline));
        }

        std::shared_ptr<RepoType> repo_;
        std::shared_ptr<ConnectionHandle> conn_;
        std::shared_ptr<detail::ContextState> local_state_{};
        std::function<bool()> is_deferred_fn_{};
        std::function<bool(std::move_only_function<std::expected<void, Core::DbError>()>)> enqueue_op_fn_{};
        std::function<bool()> is_tracker_alive_fn_{};
        std::function<bool()> is_tracking_enabled_fn_{};
        UpsertTrackedFn upsert_tracked_fn_{};
        std::function<void(std::type_index, const std::string&)> untrack_fn_{};
        std::function<void(std::type_index)> untrack_type_fn_{};
        std::function<std::shared_ptr<void>(std::type_index, const std::string&)> find_tracked_fn_{};
    };

} // namespace DAL
