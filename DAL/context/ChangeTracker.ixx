export module codefirst.dal:change_tracker;

import std;
import codefirst.core;
import :generic_repo;

export namespace DAL
{
    namespace detail
    {
        inline std::atomic<std::uint64_t> g_write_guard_sp_counter{ 0 };
        inline std::atomic<std::uint64_t> g_context_sp_counter{ 0 };

        // RAII transaction/savepoint guard for atomic single- and multi-row write operations.
        // Requires TransactionalConnection (and SavepointConnection when nested inside an active
        // transaction) and returns ConnectionError rather than silently succeeding when an adapter
        // lacks the required atomicity primitive.
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

            [[nodiscard]] std::expected<void, Core::DbError> begin(bool immediate = false)
            {
                if (!conn_)
                {
                    return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Database connection is null") };
                }
                if constexpr (!Repositories::TransactionalConnection<ConnectionHandle>)
                {
                    return std::unexpected<Core::DbError>{ Core::DbError::connection_error(
                        "Connection adapter does not support atomic transactions") };
                }
                else
                {
                    if (!conn_->in_transaction())
                    {
                        std::expected<void, Core::DbError> tx_res{};
                        if (immediate)
                        {
                            if constexpr (requires(ConnectionHandle& c) { c.begin_immediate_transaction(); })
                            {
                                tx_res = conn_->begin_immediate_transaction();
                            }
                            else
                            {
                                tx_res = conn_->begin_transaction();
                            }
                        }
                        else
                        {
                            tx_res = conn_->begin_transaction();
                        }
                        if (!tx_res)
                        {
                            return tx_res;
                        }
                        started_tx_ = true;
                        active_ = true;
                        return {};
                    }
                    if constexpr (Repositories::SavepointConnection<ConnectionHandle>)
                    {
                        sp_name_ = std::format("_codefirst_wg_{}", ++g_write_guard_sp_counter);
                        auto sp_res = conn_->savepoint(sp_name_);
                        if (!sp_res)
                        {
                            return sp_res;
                        }
                        started_sp_ = true;
                        active_ = true;
                        return {};
                    }
                    else
                    {
                        return std::unexpected<Core::DbError>{ Core::DbError::connection_error(
                            "Connection adapter does not support savepoints for nested atomic operations") };
                    }
                }
            }

            [[nodiscard]] std::expected<void, Core::DbError> commit()
            {
                if (!active_ || !conn_)
                {
                    return {};
                }
                if (started_sp_)
                {
                    if constexpr (Repositories::SavepointConnection<ConnectionHandle>)
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
                    if constexpr (Repositories::TransactionalConnection<ConnectionHandle>)
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
                    if constexpr (Repositories::SavepointConnection<ConnectionHandle>)
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
                    if constexpr (Repositories::TransactionalConnection<ConnectionHandle>)
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
                    tracked_entries_.insert_or_assign(k, entry);
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
                tracked_entries_.erase(map_key);
            }

            void untrack_type(std::type_index type) noexcept
            {
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
} // namespace DAL
