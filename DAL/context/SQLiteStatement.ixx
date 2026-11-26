module;
#include <meta>
#include <rfl.hpp>
#include <sqlgen.hpp>
#include <sqlite3.h>

export module codefirst.dal:sqlite_statement;

import std;
import codefirst.core;
import :schema_traits;

export namespace DAL
{
    using DynamicCell = std::variant<std::monostate, std::int64_t, double, std::string, std::vector<std::byte>>;

    struct DynamicRow
    {
        std::vector<DynamicCell> cells;
    };

    struct DynamicQueryResult
    {
        std::vector<std::string> columns;
        std::vector<DynamicRow> rows;
    };

    namespace detail
    {
        template<typename T>
        struct SingleValueRow
        {
            T val{};
        };

        template<typename F>
        inline constexpr bool is_sqlgen_unique_v = false;
        template<typename T>
        inline constexpr bool is_sqlgen_unique_v<sqlgen::Unique<T>> = true;

        template<typename T>
        inline constexpr bool is_strict_int_v = std::is_integral_v<std::remove_cvref_t<T>> &&
            !std::is_same_v<std::remove_cvref_t<T>, bool> && !std::is_same_v<std::remove_cvref_t<T>, char> &&
            !std::is_same_v<std::remove_cvref_t<T>, wchar_t> && !std::is_same_v<std::remove_cvref_t<T>, char8_t> &&
            !std::is_same_v<std::remove_cvref_t<T>, char16_t> && !std::is_same_v<std::remove_cvref_t<T>, char32_t>;

        [[nodiscard]] inline Core::DbError map_sqlite_db_error(sqlite3* db, std::string_view msg)
        {
            if (db != nullptr)
            {
                const int ext_code = sqlite3_extended_errcode(db);
                if (ext_code != SQLITE_OK && ext_code != SQLITE_ROW && ext_code != SQLITE_DONE)
                {
                    Core::DbError err = Core::DbError::unknown(std::string(msg));
                    if (ext_code == SQLITE_CONSTRAINT_UNIQUE || ext_code == SQLITE_CONSTRAINT_PRIMARYKEY)
                    {
                        err = Core::DbError::duplicate_key(std::string(msg));
                    }
                    else
                    {
                        const int primary_code = ext_code & 0xFF;
                        switch (primary_code)
                        {
                        case SQLITE_CONSTRAINT:
                            err = Core::DbError::constraint_violation(std::string(msg));
                            break;
                        case SQLITE_BUSY:
                        case SQLITE_LOCKED:
                        case SQLITE_CANTOPEN:
                        case SQLITE_IOERR:
                            err = Core::DbError::connection_error(std::string(msg));
                            break;
                        default:
                            err = Core::DbError::map_from_message(msg);
                            break;
                        }
                    }
                    err.native_code = ext_code;
                    return err;
                }
            }
            return Core::DbError::map_from_message(msg);
        }

        [[nodiscard]] constexpr bool is_valid_sql_identifier(std::string_view name) noexcept
        {
            if (name.empty() || name.size() > 128)
            {
                return false;
            }
            const unsigned char first = static_cast<unsigned char>(name.front());
            const bool first_ok = (first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z') || first == '_';
            if (!first_ok)
            {
                return false;
            }
            for (const char raw_ch : name)
            {
                const unsigned char ch = static_cast<unsigned char>(raw_ch);
                const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_';
                if (!ok)
                {
                    return false;
                }
            }
            return true;
        }

        [[nodiscard]] constexpr bool has_executable_sql_tail(std::string_view tail) noexcept
        {
            std::size_t i = 0;
            while (i < tail.size())
            {
                const char ch = tail[i];
                if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '\f' || ch == '\v')
                {
                    ++i;
                    continue;
                }
                if (ch == '-' && (i + 1) < tail.size() && tail[i + 1] == '-')
                {
                    i += 2;
                    while (i < tail.size() && tail[i] != '\n')
                    {
                        ++i;
                    }
                    continue;
                }
                if (ch == '/' && (i + 1) < tail.size() && tail[i + 1] == '*')
                {
                    i += 2;
                    bool closed = false;
                    while ((i + 1) < tail.size())
                    {
                        if (tail[i] == '*' && tail[i + 1] == '/')
                        {
                            i += 2;
                            closed = true;
                            break;
                        }
                        ++i;
                    }
                    if (!closed)
                    {
                        return true;
                    }
                    continue;
                }
                return true;
            }
            return false;
        }

        [[nodiscard]] inline std::expected<void, Core::DbError> validate_ddl_identifier(std::string_view name)
        {
            if (name.empty())
            {
                return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation("SQL identifier cannot be empty") };
            }
            if (name.find('\0') != std::string_view::npos)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation("SQL identifier contains embedded NUL") };
            }
            if (name.find('"') != std::string_view::npos)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation("SQL identifier cannot contain double quotes") };
            }
            return {};
        }

        template<typename DTO>
        [[nodiscard]] std::expected<void, Core::DbError> validate_dto_ddl_identifiers()
        {
            if (auto res = validate_ddl_identifier(std::string_view{ DTO::tablename }); !res)
            {
                return res;
            }
            std::expected<void, Core::DbError> status{};
            template for (constexpr auto m : DAL::Schema::members_v<DTO>)
            {
                if (status)
                {
                    status = validate_ddl_identifier(std::meta::identifier_of(m));
                }
                using RawFieldT = [:std::meta::type_of(m):];
                using FieldT = std::remove_cvref_t<RawFieldT>;
                if constexpr (DAL::Schema::SqlgenForeignKeyField<FieldT>)
                {
                    if (status)
                    {
                        status = validate_ddl_identifier(std::string_view{ FieldT::ForeignTableType::tablename });
                    }
                }
            }
            return status;
        }

        // Rewrites embedded NUL bytes inside single-quoted string literals emitted by sqlgen::sqlite::to_sql
        // into SQLite `'...' || char(0) || '...'` expressions while tracking double-quoted identifiers
        // and rejecting NUL bytes outside string literals.
        [[nodiscard]] inline std::expected<std::string, Core::DbError> encode_sqlgen_embedded_nuls(std::string_view sql)
        {
            if (sql.find('\0') == std::string_view::npos)
            {
                return std::string{ sql };
            }
            std::string out;
            out.reserve(sql.size() + 32);
            bool in_string = false;
            bool in_double_quote = false;
            for (std::size_t i = 0; i < sql.size(); ++i)
            {
                const char ch = sql[i];
                if (in_double_quote)
                {
                    if (ch == '\0')
                    {
                        return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation(
                            "SQL identifier contains embedded NUL") };
                    }
                    out.push_back(ch);
                    if (ch == '"')
                    {
                        if ((i + 1) < sql.size() && sql[i + 1] == '"')
                        {
                            out.push_back('"');
                            ++i;
                        }
                        else
                        {
                            in_double_quote = false;
                        }
                    }
                }
                else if (!in_string)
                {
                    if (ch == '\0')
                    {
                        return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation(
                            "SQL statement contains embedded NUL outside string literal") };
                    }
                    if (ch == '\'')
                    {
                        in_string = true;
                    }
                    else if (ch == '"')
                    {
                        in_double_quote = true;
                    }
                    out.push_back(ch);
                }
                else
                {
                    if (ch == '\0')
                    {
                        out += "' || char(0) || '";
                    }
                    else if (ch == '\'')
                    {
                        out.push_back('\'');
                        if ((i + 1) < sql.size() && sql[i + 1] == '\'')
                        {
                            out.push_back('\'');
                            ++i;
                        }
                        else
                        {
                            in_string = false;
                        }
                    }
                    else
                    {
                        out.push_back(ch);
                    }
                }
            }
            return out;
        }

        // Move-only RAII wrapper around a single prepared sqlite3_stmt.
        class SQLiteStatement
        {
        public:
            SQLiteStatement() = default;

            SQLiteStatement(sqlite3* db, sqlite3_stmt* stmt) noexcept
                : db_(db)
                , stmt_(stmt)
            {
            }

            SQLiteStatement(const SQLiteStatement&) = delete;
            SQLiteStatement& operator=(const SQLiteStatement&) = delete;

            SQLiteStatement(SQLiteStatement&& other) noexcept
                : db_(other.db_)
                , stmt_(other.stmt_)
            {
                other.db_ = nullptr;
                other.stmt_ = nullptr;
            }

            SQLiteStatement& operator=(SQLiteStatement&& other) noexcept
            {
                if (this != &other)
                {
                    finalize();
                    db_ = other.db_;
                    stmt_ = other.stmt_;
                    other.db_ = nullptr;
                    other.stmt_ = nullptr;
                }
                return *this;
            }

            ~SQLiteStatement() noexcept
            {
                finalize();
            }

            void finalize() noexcept
            {
                if (stmt_ != nullptr)
                {
                    sqlite3_finalize(stmt_);
                    stmt_ = nullptr;
                }
                db_ = nullptr;
            }

            [[nodiscard]] static std::expected<SQLiteStatement, Core::DbError> prepare_single(sqlite3* db, std::string_view sql)
            {
                if (db == nullptr)
                {
                    return std::unexpected<Core::DbError>{ Core::DbError::connection_error("Native SQLite handle is unavailable") };
                }
                if (sql.empty())
                {
                    return std::unexpected<Core::DbError>{ Core::DbError::unknown("SQL statement cannot be empty") };
                }
                if (sql.find('\0') != std::string_view::npos)
                {
                    return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation("SQL statement contains embedded NUL") };
                }
                if (sql.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
                {
                    return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation("SQL statement exceeds length limit") };
                }

                sqlite3_stmt* raw_stmt = nullptr;
                const char* pz_tail = nullptr;
                if (sqlite3_prepare_v2(db, sql.data(), static_cast<int>(sql.size()), &raw_stmt, &pz_tail) != SQLITE_OK)
                {
                    return std::unexpected<Core::DbError>{ map_sqlite_db_error(db, sqlite3_errmsg(db)) };
                }
                SQLiteStatement stmt(db, raw_stmt);
                if (raw_stmt == nullptr)
                {
                    return std::unexpected<Core::DbError>{ Core::DbError::unknown("SQL statement contains no executable command") };
                }

                const char* const sql_end = sql.data() + sql.size();
                const std::string_view tail = (pz_tail != nullptr && pz_tail <= sql_end)
                    ? std::string_view(pz_tail, static_cast<std::size_t>(sql_end - pz_tail))
                    : std::string_view{};
                if (has_executable_sql_tail(tail))
                {
                    return std::unexpected<Core::DbError>{ Core::DbError::unknown(
                        "Multi-statement SQL is not supported in prepared execution") };
                }
                return stmt;
            }

            [[nodiscard]] sqlite3* db() const noexcept
            {
                return db_;
            }

            [[nodiscard]] sqlite3_stmt* raw() const noexcept
            {
                return stmt_;
            }

            [[nodiscard]] int column_count() const noexcept
            {
                return stmt_ != nullptr ? sqlite3_column_count(stmt_) : 0;
            }

            [[nodiscard]] int step() const noexcept
            {
                return sqlite3_step(stmt_);
            }

            [[nodiscard]] std::expected<void, Core::DbError> reset_and_clear() const
            {
                if (sqlite3_reset(stmt_) != SQLITE_OK || sqlite3_clear_bindings(stmt_) != SQLITE_OK)
                {
                    return std::unexpected<Core::DbError>{ map_sqlite_db_error(db_, sqlite3_errmsg(db_)) };
                }
                return {};
            }

        private:
            sqlite3* db_{ nullptr };
            sqlite3_stmt* stmt_{ nullptr };
        };

        [[nodiscard]] inline std::expected<void, Core::DbError> bind_dynamic_params(
            sqlite3* db, sqlite3_stmt* stmt, std::span<const DynamicCell> params)
        {
            const int expected_params = sqlite3_bind_parameter_count(stmt);
            if (expected_params < 0 || params.size() != static_cast<std::size_t>(expected_params))
            {
                return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation(
                    std::format("Prepared statement expects {} parameters, but {} were provided", expected_params, params.size())) };
            }

            for (std::size_t i = 0; i < params.size(); ++i)
            {
                const int idx = static_cast<int>(i + 1);
                int rc = SQLITE_OK;
                std::expected<void, Core::DbError> size_err{};
                std::visit(
                    [&](const auto& val)
                    {
                        using T = std::decay_t<decltype(val)>;
                        if constexpr (std::is_same_v<T, std::monostate>)
                        {
                            rc = sqlite3_bind_null(stmt, idx);
                        }
                        else if constexpr (std::is_same_v<T, std::int64_t>)
                        {
                            rc = sqlite3_bind_int64(stmt, idx, val);
                        }
                        else if constexpr (std::is_same_v<T, double>)
                        {
                            rc = sqlite3_bind_double(stmt, idx, val);
                        }
                        else if constexpr (std::is_same_v<T, std::string>)
                        {
                            if (val.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
                            {
                                size_err = std::unexpected<Core::DbError>{ Core::DbError::constraint_violation(
                                    "Bound string parameter exceeds SQLite length limit") };
                                return;
                            }
                            rc =
                                sqlite3_bind_text(stmt, idx, val.empty() ? "" : val.data(), static_cast<int>(val.size()), SQLITE_TRANSIENT);
                        }
                        else if constexpr (std::is_same_v<T, std::vector<std::byte>>)
                        {
                            if (val.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
                            {
                                size_err = std::unexpected<Core::DbError>{ Core::DbError::constraint_violation(
                                    "Bound BLOB parameter exceeds SQLite length limit") };
                                return;
                            }
                            if (val.empty())
                            {
                                rc = sqlite3_bind_zeroblob(stmt, idx, 0);
                            }
                            else
                            {
                                rc = sqlite3_bind_blob(stmt, idx, val.data(), static_cast<int>(val.size()), SQLITE_TRANSIENT);
                            }
                        }
                    },
                    params[i]);
                if (!size_err)
                {
                    return size_err;
                }
                if (rc != SQLITE_OK)
                {
                    return std::unexpected<Core::DbError>{ map_sqlite_db_error(db, sqlite3_errmsg(db)) };
                }
            }
            return {};
        }

        template<typename FieldT>
        [[nodiscard]] std::expected<void, Core::DbError> bind_typed_field(
            sqlite3* db, sqlite3_stmt* stmt, int idx, const FieldT& field, bool treat_zero_auto_incr_as_null = false)
        {
            using CleanT = std::remove_cvref_t<FieldT>;
            if constexpr (Schema::SqlgenPrimaryKeyField<CleanT>)
            {
                if (treat_zero_auto_incr_as_null)
                {
                    if constexpr (CleanT::auto_incr)
                    {
                        using InnerT = std::remove_cvref_t<decltype(field.value())>;
                        if constexpr (std::is_integral_v<InnerT> && !std::is_same_v<InnerT, bool>)
                        {
                            if (field.value() == 0)
                            {
                                if (sqlite3_bind_null(stmt, idx) != SQLITE_OK)
                                {
                                    return std::unexpected<Core::DbError>{ map_sqlite_db_error(db, sqlite3_errmsg(db)) };
                                }
                                return {};
                            }
                        }
                    }
                }
                return bind_typed_field(db, stmt, idx, field.value(), false);
            }
            else if constexpr (Schema::SqlgenForeignKeyField<CleanT> || is_sqlgen_unique_v<CleanT>)
            {
                return bind_typed_field(db, stmt, idx, field.value(), false);
            }
            else if constexpr (Schema::detail::is_optional_v<CleanT>)
            {
                if (!field.has_value())
                {
                    if (sqlite3_bind_null(stmt, idx) != SQLITE_OK)
                    {
                        return std::unexpected<Core::DbError>{ map_sqlite_db_error(db, sqlite3_errmsg(db)) };
                    }
                    return {};
                }
                return bind_typed_field(db, stmt, idx, *field, false);
            }
            else if constexpr (Core::StrongIdType<CleanT> || Core::SingleKeyType<CleanT>)
            {
                return bind_typed_field(db, stmt, idx, field.get(), false);
            }
            else if constexpr (std::is_same_v<CleanT, bool>)
            {
                if (sqlite3_bind_int64(stmt, idx, field ? 1 : 0) != SQLITE_OK)
                {
                    return std::unexpected<Core::DbError>{ map_sqlite_db_error(db, sqlite3_errmsg(db)) };
                }
                return {};
            }
            else if constexpr (std::is_integral_v<CleanT>)
            {
                if constexpr (std::is_signed_v<CleanT> || sizeof(CleanT) < sizeof(std::int64_t))
                {
                    if (sqlite3_bind_int64(stmt, idx, static_cast<sqlite3_int64>(field)) != SQLITE_OK)
                    {
                        return std::unexpected<Core::DbError>{ map_sqlite_db_error(db, sqlite3_errmsg(db)) };
                    }
                    return {};
                }
                else
                {
                    // SQLite INTEGER is signed 64-bit; binding > INT64_MAX as text to an INTEGER-affinity
                    // column causes silent coercion to lossy 53-bit IEEE-754 REAL. Reject unrepresentable values.
                    if (field > static_cast<CleanT>(std::numeric_limits<std::int64_t>::max()))
                    {
                        return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation(
                            std::format("Unsigned 64-bit integer value {} exceeds SQLite signed 64-bit INTEGER range", field)) };
                    }
                    if (sqlite3_bind_int64(stmt, idx, static_cast<sqlite3_int64>(field)) != SQLITE_OK)
                    {
                        return std::unexpected<Core::DbError>{ map_sqlite_db_error(db, sqlite3_errmsg(db)) };
                    }
                    return {};
                }
            }
            else if constexpr (std::is_floating_point_v<CleanT>)
            {
                if (sqlite3_bind_double(stmt, idx, static_cast<double>(field)) != SQLITE_OK)
                {
                    return std::unexpected<Core::DbError>{ map_sqlite_db_error(db, sqlite3_errmsg(db)) };
                }
                return {};
            }
            else if constexpr (std::is_same_v<CleanT, std::string> || std::is_same_v<CleanT, std::string_view>)
            {
                if (field.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
                {
                    return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation(
                        "Bound string parameter exceeds SQLite length limit") };
                }
                if (sqlite3_bind_text(stmt, idx, field.empty() ? "" : field.data(), static_cast<int>(field.size()), SQLITE_TRANSIENT) !=
                    SQLITE_OK)
                {
                    return std::unexpected<Core::DbError>{ map_sqlite_db_error(db, sqlite3_errmsg(db)) };
                }
                return {};
            }
            else if constexpr (std::is_same_v<CleanT, const char*> || std::is_same_v<CleanT, char*>)
            {
                if (field == nullptr)
                {
                    if (sqlite3_bind_null(stmt, idx) != SQLITE_OK)
                    {
                        return std::unexpected<Core::DbError>{ map_sqlite_db_error(db, sqlite3_errmsg(db)) };
                    }
                    return {};
                }
                const std::string_view sv{ field };
                return bind_typed_field(db, stmt, idx, sv, false);
            }
            else if constexpr (std::is_same_v<CleanT, std::vector<std::byte>>)
            {
                if (field.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
                {
                    return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation(
                        "Bound BLOB parameter exceeds SQLite length limit") };
                }
                const int rc = field.empty() ? sqlite3_bind_zeroblob(stmt, idx, 0)
                                             : sqlite3_bind_blob(stmt, idx, field.data(), static_cast<int>(field.size()), SQLITE_TRANSIENT);
                if (rc != SQLITE_OK)
                {
                    return std::unexpected<Core::DbError>{ map_sqlite_db_error(db, sqlite3_errmsg(db)) };
                }
                return {};
            }
            else
            {
                const std::optional<std::string> encoded = sqlgen::parsing::Parser<CleanT>::write(field);
                if (!encoded.has_value())
                {
                    if (sqlite3_bind_null(stmt, idx) != SQLITE_OK)
                    {
                        return std::unexpected<Core::DbError>{ map_sqlite_db_error(db, sqlite3_errmsg(db)) };
                    }
                    return {};
                }
                if (encoded->size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
                {
                    return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation(
                        "Bound encoded parameter exceeds SQLite length limit") };
                }
                if (sqlite3_bind_text(
                        stmt, idx, encoded->empty() ? "" : encoded->data(), static_cast<int>(encoded->size()), SQLITE_TRANSIENT) !=
                    SQLITE_OK)
                {
                    return std::unexpected<Core::DbError>{ map_sqlite_db_error(db, sqlite3_errmsg(db)) };
                }
                return {};
            }
        }

        template<typename... Parts>
        [[nodiscard]] std::expected<void, Core::DbError> bind_variadic_fields(
            sqlite3* db, sqlite3_stmt* stmt, int start_idx, const Parts&... parts)
        {
            std::expected<void, Core::DbError> res{};
            int idx = start_idx;
            (
                [&]()
                {
                    if (res)
                    {
                        res = bind_typed_field(db, stmt, idx++, parts, false);
                    }
                }(),
                ...);
            return res;
        }

        [[nodiscard]] inline std::expected<std::string, Core::DbError> read_column_text_exact(sqlite3* db, sqlite3_stmt* stmt, int col_idx)
        {
            const auto* txt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, col_idx));
            const int len = sqlite3_column_bytes(stmt, col_idx);
            if (txt == nullptr)
            {
                if (sqlite3_errcode(db) == SQLITE_NOMEM)
                {
                    return std::unexpected<Core::DbError>{ map_sqlite_db_error(db, sqlite3_errmsg(db)) };
                }
                return std::string{};
            }
            return std::string(txt, static_cast<std::size_t>(len));
        }

        // Direct per-column strict decoder with storage-class validation, full-input parsing,
        // and exact std::in_range checks (eliminating std::stoll trailing-character acceptance and narrowing).
        template<typename FieldT>
        [[nodiscard]] std::expected<std::remove_cvref_t<FieldT>, Core::DbError> decode_column(sqlite3* db, sqlite3_stmt* stmt, int col_idx)
        {
            using CleanT = std::remove_cvref_t<FieldT>;
            const int col_type = sqlite3_column_type(stmt, col_idx);

            if constexpr (Schema::detail::is_optional_v<CleanT>)
            {
                if (col_type == SQLITE_NULL)
                {
                    return CleanT{ std::nullopt };
                }
                using InnerT = typename CleanT::value_type;
                auto inner = decode_column<InnerT>(db, stmt, col_idx);
                if (!inner)
                {
                    return std::unexpected<Core::DbError>{ inner.error() };
                }
                return CleanT{ std::move(*inner) };
            }
            else if constexpr (Schema::SqlgenPrimaryKeyField<CleanT> || Schema::SqlgenForeignKeyField<CleanT> || is_sqlgen_unique_v<CleanT>)
            {
                using InnerT = std::remove_cvref_t<typename CleanT::ReflectionType>;
                auto inner = decode_column<InnerT>(db, stmt, col_idx);
                if (!inner)
                {
                    return std::unexpected<Core::DbError>{ inner.error() };
                }
                return CleanT{ std::move(*inner) };
            }
            else if constexpr (Core::StrongIdType<CleanT>)
            {
                auto inner = decode_column<typename CleanT::value_type>(db, stmt, col_idx);
                if (!inner)
                {
                    return std::unexpected<Core::DbError>{ inner.error() };
                }
                return CleanT{ std::move(*inner) };
            }
            else if constexpr (Core::SingleKeyType<CleanT>)
            {
                auto inner = decode_column<typename CleanT::key_type>(db, stmt, col_idx);
                if (!inner)
                {
                    return std::unexpected<Core::DbError>{ inner.error() };
                }
                return CleanT{ std::move(*inner) };
            }
            else
            {
                if (col_type == SQLITE_NULL)
                {
                    return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation(
                        std::format("Unexpected NULL value when decoding non-optional column {}", col_idx)) };
                }

                if constexpr (std::is_same_v<CleanT, bool>)
                {
                    if (col_type == SQLITE_INTEGER)
                    {
                        return sqlite3_column_int64(stmt, col_idx) != 0;
                    }
                    if (col_type == SQLITE_TEXT)
                    {
                        auto text_res = read_column_text_exact(db, stmt, col_idx);
                        if (!text_res)
                        {
                            return std::unexpected<Core::DbError>{ text_res.error() };
                        }
                        const std::string_view sv = *text_res;
                        if (sv == "1" || sv == "t" || sv == "T" || sv == "true" || sv == "TRUE")
                        {
                            return true;
                        }
                        if (sv == "0" || sv == "f" || sv == "F" || sv == "false" || sv == "FALSE")
                        {
                            return false;
                        }
                        std::int64_t parsed = 0;
                        const auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), parsed);
                        if (ec == std::errc{} && ptr == sv.data() + sv.size())
                        {
                            return parsed != 0;
                        }
                        return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation(
                            std::format("Invalid boolean value '{}' in column {}", sv, col_idx)) };
                    }
                    return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation(
                        std::format("Incompatible SQLite storage class {} for boolean column {}", col_type, col_idx)) };
                }
                else if constexpr (is_strict_int_v<CleanT>)
                {
                    if (col_type == SQLITE_INTEGER)
                    {
                        const std::int64_t v = static_cast<std::int64_t>(sqlite3_column_int64(stmt, col_idx));
                        if (!std::in_range<CleanT>(v))
                        {
                            return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation(
                                std::format("Integer value {} in column {} is out of range for target type", v, col_idx)) };
                        }
                        return static_cast<CleanT>(v);
                    }
                    if (col_type == SQLITE_TEXT)
                    {
                        auto text_res = read_column_text_exact(db, stmt, col_idx);
                        if (!text_res)
                        {
                            return std::unexpected<Core::DbError>{ text_res.error() };
                        }
                        const std::string_view sv = *text_res;
                        if (sv.empty())
                        {
                            return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation(
                                std::format("Empty text cannot be decoded as integer in column {}", col_idx)) };
                        }
                        if constexpr (std::is_signed_v<CleanT>)
                        {
                            std::int64_t parsed = 0;
                            const auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), parsed);
                            if (ec != std::errc{} || ptr != sv.data() + sv.size() || !std::in_range<CleanT>(parsed))
                            {
                                return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation(
                                    std::format("Invalid or out-of-range integer '{}' in column {}", sv, col_idx)) };
                            }
                            return static_cast<CleanT>(parsed);
                        }
                        else
                        {
                            std::uint64_t parsed = 0;
                            const auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), parsed);
                            if (ec != std::errc{} || ptr != sv.data() + sv.size() || !std::in_range<CleanT>(parsed))
                            {
                                return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation(
                                    std::format("Invalid or out-of-range unsigned integer '{}' in column {}", sv, col_idx)) };
                            }
                            return static_cast<CleanT>(parsed);
                        }
                    }
                    return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation(
                        std::format("Incompatible SQLite storage class {} for integer column {}", col_type, col_idx)) };
                }
                else if constexpr (std::is_floating_point_v<CleanT>)
                {
                    if (col_type == SQLITE_FLOAT || col_type == SQLITE_INTEGER)
                    {
                        const double d = sqlite3_column_double(stmt, col_idx);
                        return static_cast<CleanT>(d);
                    }
                    if (col_type == SQLITE_TEXT)
                    {
                        auto text_res = read_column_text_exact(db, stmt, col_idx);
                        if (!text_res)
                        {
                            return std::unexpected<Core::DbError>{ text_res.error() };
                        }
                        const std::string_view sv = *text_res;
                        double parsed = 0.0;
                        const auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), parsed);
                        if (ec != std::errc{} || ptr != sv.data() + sv.size())
                        {
                            return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation(
                                std::format("Invalid floating-point value '{}' in column {}", sv, col_idx)) };
                        }
                        return static_cast<CleanT>(parsed);
                    }
                    return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation(
                        std::format("Incompatible SQLite storage class {} for floating-point column {}", col_type, col_idx)) };
                }
                else if constexpr (std::is_same_v<CleanT, std::string>)
                {
                    return read_column_text_exact(db, stmt, col_idx);
                }
                else if constexpr (std::is_same_v<CleanT, std::vector<std::byte>>)
                {
                    const auto* blob = static_cast<const std::byte*>(sqlite3_column_blob(stmt, col_idx));
                    const int len = sqlite3_column_bytes(stmt, col_idx);
                    if (blob != nullptr && len > 0)
                    {
                        return std::vector<std::byte>(blob, blob + len);
                    }
                    return std::vector<std::byte>{};
                }
                else if constexpr (std::is_enum_v<CleanT>)
                {
                    if (col_type == SQLITE_TEXT)
                    {
                        auto text_res = read_column_text_exact(db, stmt, col_idx);
                        if (!text_res)
                        {
                            return std::unexpected<Core::DbError>{ text_res.error() };
                        }
                        auto parsed = rfl::string_to_enum<CleanT>(*text_res);
                        if (!parsed)
                        {
                            return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation(parsed.error().what()) };
                        }
                        return *parsed;
                    }
                    if (col_type == SQLITE_INTEGER)
                    {
                        using UnderlyingT = std::underlying_type_t<CleanT>;
                        const std::int64_t v = static_cast<std::int64_t>(sqlite3_column_int64(stmt, col_idx));
                        if (!std::in_range<UnderlyingT>(v))
                        {
                            return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation(
                                std::format("Enum integer value {} in column {} is out of range", v, col_idx)) };
                        }
                        return static_cast<CleanT>(static_cast<UnderlyingT>(v));
                    }
                    return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation(
                        std::format("Incompatible SQLite storage class {} for enum column {}", col_type, col_idx)) };
                }
                else
                {
                    // Fallback for custom sqlgen wrapper/domain types (e.g. Timestamp, Date, Varchar, rfl::Validator).
                    auto text_res = read_column_text_exact(db, stmt, col_idx);
                    if (!text_res)
                    {
                        return std::unexpected<Core::DbError>{ text_res.error() };
                    }
                    auto parsed = sqlgen::parsing::Parser<CleanT>::read(std::optional<std::string>{ std::move(*text_res) });
                    if (!parsed)
                    {
                        return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation(parsed.error().what()) };
                    }
                    return std::move(*parsed);
                }
            }
        }

        template<typename RowT>
        [[nodiscard]] std::expected<RowT, Core::DbError> decode_row(sqlite3* db, sqlite3_stmt* stmt)
        {
            constexpr std::size_t expected_cols = DAL::Schema::members_v<RowT>.size();
            const int col_count = sqlite3_column_count(stmt);
            if (col_count != static_cast<int>(expected_cols))
            {
                return std::unexpected<Core::DbError>{ Core::DbError::unknown(
                    std::format("Expected {} columns in query result, got {}", expected_cols, col_count)) };
            }

            RowT row{};
            std::optional<Core::DbError> err;
            int col_idx = 0;
            template for (constexpr auto m : DAL::Schema::members_v<RowT>)
            {
                if (!err)
                {
                    using RawMemberT = [:std::meta::type_of(m):];
                    auto val = decode_column<RawMemberT>(db, stmt, col_idx);
                    if (!val)
                    {
                        err = std::move(val.error());
                    }
                    else
                    {
                        row.[:m:] = std::move(*val);
                    }
                }
                ++col_idx;
            }
            if (err)
            {
                return std::unexpected<Core::DbError>{ std::move(*err) };
            }
            return row;
        }

        template<typename RowTuple>
        [[nodiscard]] std::expected<RowTuple, Core::DbError> decode_tuple(sqlite3* db, sqlite3_stmt* stmt)
        {
            constexpr std::size_t tuple_arity = std::tuple_size_v<RowTuple>;
            const int col_count = sqlite3_column_count(stmt);
            if (col_count != static_cast<int>(tuple_arity))
            {
                return std::unexpected<Core::DbError>{ Core::DbError::unknown(
                    std::format("Expected {} columns in tuple projection, got {}", tuple_arity, col_count)) };
            }

            std::optional<Core::DbError> err;
            auto parsed = [&]<std::size_t... Is>(std::index_sequence<Is...>) -> std::optional<RowTuple>
            {
                auto decode_one = [&](auto idx_c)
                {
                    constexpr std::size_t I = decltype(idx_c)::value;
                    using ElemT = std::tuple_element_t<I, RowTuple>;
                    auto val = decode_column<ElemT>(db, stmt, static_cast<int>(I));
                    if (!val && !err)
                    {
                        err = std::move(val.error());
                    }
                    return val;
                };
                auto results = std::make_tuple(decode_one(std::integral_constant<std::size_t, Is>{})...);
                if (err)
                {
                    return std::nullopt;
                }
                return RowTuple{ std::move(*std::get<Is>(results))... };
            }(std::make_index_sequence<tuple_arity>{});

            if (err)
            {
                return std::unexpected<Core::DbError>{ std::move(*err) };
            }
            return std::move(*parsed);
        }
    } // namespace detail
} // namespace DAL
