export module codefirst.dal:connection_pool;

import std;
import codefirst.core;
import :sqlite_connection;
import :db_context;

export namespace DAL
{
    struct SQLitePoolOptions
    {
        std::size_t min_size = 1;
        std::size_t max_size = 8;
        std::chrono::milliseconds acquire_timeout{ 5000 };
        SQLiteConnectionOptions connection_options{
            .busy_timeout = std::chrono::milliseconds{ 5000 },
            .wal_mode = true,
        };
    };

    class SQLiteConnectionPool;

    class PooledConnection
    {
    public:
        PooledConnection() = default;
        PooledConnection(std::shared_ptr<SQLiteConnectionPool> pool, std::shared_ptr<SQLiteConnection> conn) noexcept
            : pool_(std::move(pool))
            , conn_(std::move(conn))
        {
        }

        PooledConnection(const PooledConnection&) = delete;
        PooledConnection& operator=(const PooledConnection&) = delete;

        PooledConnection(PooledConnection&& other) noexcept
            : pool_(std::move(other.pool_))
            , conn_(std::move(other.conn_))
        {
        }

        PooledConnection& operator=(PooledConnection&& other) noexcept
        {
            if (this != &other)
            {
                reset();
                pool_ = std::move(other.pool_);
                conn_ = std::move(other.conn_);
            }
            return *this;
        }

        ~PooledConnection() noexcept
        {
            reset();
        }

        void reset() noexcept;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return conn_ != nullptr;
        }

        [[nodiscard]] SQLiteConnection* get() const noexcept
        {
            return conn_.get();
        }

        [[nodiscard]] SQLiteConnection* operator->() const noexcept
        {
            return conn_.get();
        }

        [[nodiscard]] SQLiteConnection& operator*() const noexcept
        {
            return *conn_;
        }

        [[nodiscard]] std::shared_ptr<SQLiteConnection> connection() const noexcept
        {
            return conn_;
        }

    private:
        std::shared_ptr<SQLiteConnectionPool> pool_;
        std::shared_ptr<SQLiteConnection> conn_;
    };

    class SQLiteConnectionPool : public std::enable_shared_from_this<SQLiteConnectionPool>
    {
        struct PrivateTag
        {
        };

    public:
        SQLiteConnectionPool(PrivateTag, std::string path, SQLitePoolOptions options)
            : path_(std::move(path))
            , options_(std::move(options))
        {
            if (path_.empty() || path_ == ":memory:" || options_.connection_options.read_only)
            {
                options_.connection_options.wal_mode = false;
            }
        }

        SQLiteConnectionPool(const SQLiteConnectionPool&) = delete;
        SQLiteConnectionPool& operator=(const SQLiteConnectionPool&) = delete;

        ~SQLiteConnectionPool() noexcept
        {
            close();
        }

        [[nodiscard]] static std::expected<std::shared_ptr<SQLiteConnectionPool>, Core::DbError> create(
            std::string path, SQLitePoolOptions options = {})
        {
            if (options.max_size == 0)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation("SQLitePoolOptions::max_size must be >= 1") };
            }
            if (options.min_size > options.max_size)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation(
                    "SQLitePoolOptions::min_size cannot exceed max_size") };
            }

            auto pool = std::make_shared<SQLiteConnectionPool>(PrivateTag{}, std::move(path), std::move(options));
            for (std::size_t i = 0; i < pool->options_.min_size; ++i)
            {
                auto conn_res = pool->open_connection();
                if (!conn_res)
                {
                    pool->close();
                    return std::unexpected<Core::DbError>{ conn_res.error() };
                }
                pool->idle_.push_back(std::move(*conn_res));
                ++pool->total_created_;
            }
            return pool;
        }

        template<std::same_as<std::filesystem::path> PathT>
        [[nodiscard]] static std::expected<std::shared_ptr<SQLiteConnectionPool>, Core::DbError> create(
            const PathT& path, SQLitePoolOptions options = {})
        {
            return create(path.string(), std::move(options));
        }

        [[nodiscard]] std::expected<PooledConnection, Core::DbError> acquire()
        {
            const auto deadline = std::chrono::steady_clock::now() + options_.acquire_timeout;

            for (;;)
            {
                bool should_create = false;
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    auto ready = [this]()
                    {
                        return closed_ || !idle_.empty() || total_created_ < options_.max_size;
                    };

                    if (!ready())
                    {
                        if (!cv_.wait_until(lock, deadline, ready))
                        {
                            return std::unexpected<Core::DbError>{ Core::DbError::connection_error(
                                "Timed out waiting for an available connection from SQLiteConnectionPool") };
                        }
                    }

                    if (closed_)
                    {
                        return std::unexpected<Core::DbError>{ Core::DbError::connection_error("SQLiteConnectionPool is closed") };
                    }

                    if (!idle_.empty())
                    {
                        auto conn = std::move(idle_.back());
                        idle_.pop_back();
                        if (conn && conn->is_open())
                        {
                            return PooledConnection{ shared_from_this(), std::move(conn) };
                        }
                        if (total_created_ > 0)
                        {
                            --total_created_;
                        }
                        continue;
                    }

                    if (total_created_ < options_.max_size)
                    {
                        ++total_created_;
                        should_create = true;
                    }
                }

                if (should_create)
                {
                    auto conn_res = open_connection();
                    if (!conn_res)
                    {
                        {
                            std::lock_guard<std::mutex> lock(mutex_);
                            if (total_created_ > 0)
                            {
                                --total_created_;
                            }
                        }
                        cv_.notify_one();
                        return std::unexpected<Core::DbError>{ conn_res.error() };
                    }

                    bool is_closed_now = false;
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        is_closed_now = closed_;
                        if (is_closed_now && total_created_ > 0)
                        {
                            --total_created_;
                        }
                    }
                    if (is_closed_now)
                    {
                        cv_.notify_all();
                        return std::unexpected<Core::DbError>{ Core::DbError::connection_error("SQLiteConnectionPool is closed") };
                    }

                    return PooledConnection{ shared_from_this(), std::move(*conn_res) };
                }
            }
        }

        template<typename ContextT, typename Fn>
        [[nodiscard]] auto with_context(Fn&& fn) ->
            typename detail::TransactionReturn<std::remove_cvref_t<std::invoke_result_t<Fn, ContextT&>>>::expected_type
        {
            using FnResult = std::invoke_result_t<Fn, ContextT&>;
            using Traits = detail::TransactionReturn<std::remove_cvref_t<FnResult>>;

            auto lease_res = acquire();
            if (!lease_res)
            {
                return std::unexpected<Core::DbError>{ lease_res.error() };
            }

            PooledConnection lease = std::move(*lease_res);
            ContextT ctx(lease.connection());

            if constexpr (std::is_void_v<FnResult>)
            {
                std::invoke(std::forward<Fn>(fn), ctx);
                return {};
            }
            else if constexpr (Traits::is_expected_result)
            {
                return std::invoke(std::forward<Fn>(fn), ctx);
            }
            else
            {
                return std::invoke(std::forward<Fn>(fn), ctx);
            }
        }

        void close() noexcept
        {
            std::vector<std::shared_ptr<SQLiteConnection>> to_close;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (closed_)
                {
                    return;
                }
                closed_ = true;
                to_close.swap(idle_);
                if (total_created_ >= to_close.size())
                {
                    total_created_ -= to_close.size();
                }
                else
                {
                    total_created_ = 0;
                }
            }
            cv_.notify_all();
            to_close.clear();
        }

        [[nodiscard]] std::size_t size() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return total_created_;
        }

        [[nodiscard]] std::size_t available() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return idle_.size();
        }

        [[nodiscard]] bool is_closed() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return closed_;
        }

    private:
        friend class PooledConnection;

        [[nodiscard]] std::expected<std::shared_ptr<SQLiteConnection>, Core::DbError> open_connection() const
        {
            return SQLiteConnection::connect(path_, options_.connection_options);
        }

        void release(std::shared_ptr<SQLiteConnection> conn) noexcept
        {
            if (!conn)
            {
                return;
            }
            try
            {
                if (conn->is_open() && conn->in_transaction())
                {
                    (void)conn->rollback();
                }
            } catch (...)
            {
            }

            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!closed_ && conn->is_open())
                {
                    idle_.push_back(std::move(conn));
                }
                else if (total_created_ > 0)
                {
                    --total_created_;
                }
            }
            cv_.notify_one();
        }

        std::string path_;
        SQLitePoolOptions options_;
        mutable std::mutex mutex_;
        std::condition_variable cv_;
        std::vector<std::shared_ptr<SQLiteConnection>> idle_;
        std::size_t total_created_{ 0 };
        bool closed_{ false };
    };

    inline void PooledConnection::reset() noexcept
    {
        if (pool_ && conn_)
        {
            auto pool = std::move(pool_);
            auto conn = std::move(conn_);
            pool->release(std::move(conn));
        }
        else
        {
            pool_.reset();
            conn_.reset();
        }
    }

} // namespace DAL
