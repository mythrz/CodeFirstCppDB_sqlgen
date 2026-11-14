module;
#include <sqlgen.hpp>
#include <sqlgen/sqlite.hpp>

export module dal:sqlite_connection;

import std;
import core;

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
        [[nodiscard]] std::vector<DTO> fetch_all() const
        {
            auto res = sqlgen::read<std::vector<DTO>>(conn_);
            if (res)
            {
                return res.value();
            }
            return {};
        }

        template<typename DTO>
        [[nodiscard]] std::optional<DTO> get_by_id(std::int32_t id) const
        {
            auto all = fetch_all<DTO>();
            for (const auto& item : all)
            {
                if constexpr (requires { item.id; })
                {
                    if (item.id.value() == id)
                    {
                        return item;
                    }
                }
            }
            return std::nullopt;
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
            std::vector<DTO> vec{ item };
            return sqlgen::write(conn_, vec);
        }

        template<typename DTO>
        auto delete_by_id(std::int32_t id)
        {
            std::string sql = std::format("DELETE FROM {} WHERE id = {};", DTO::tablename, id);
            return conn_->execute(sql);
        }

        [[nodiscard]] auto delete_by_composite_id(std::int32_t personId, std::int32_t somethingId)
        {
            std::string sql = std::format(
                "DELETE FROM Person_Something WHERE person_id = {} AND something_id = {};",
                personId, somethingId);
            return conn_->execute(sql);
        }

        template<typename DTO>
        [[nodiscard]] bool exists(std::int32_t id) const
        {
            return get_by_id<DTO>(id).has_value();
        }

        template<typename DTO>
        [[nodiscard]] std::size_t count() const
        {
            return fetch_all<DTO>().size();
        }

    private:
        sqlgen::Ref<sqlgen::sqlite::Connection> conn_;
    };

} // namespace DAL
