module;
#include <meta>
#include <rfl.hpp>
#include <sqlgen.hpp>
#include <sqlgen/transpilation/to_condition.hpp>
#include <sqlgen/transpilation/to_sql.hpp>
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
            for (const char raw_ch : name)
            {
                const unsigned char ch = static_cast<unsigned char>(raw_ch);
                if (ch == '\0')
                {
                    return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation("SQL identifier contains embedded NUL") };
                }
                if (ch == '"')
                {
                    return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation(
                        "SQL identifier cannot contain double quotes") };
                }
                if (ch < 0x20u || ch == 0x7Fu)
                {
                    return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation(
                        "SQL identifier cannot contain ASCII control characters") };
                }
            }
            return {};
        }

        template<typename FieldT>
        [[nodiscard]] inline std::expected<void, Core::DbError> validate_field_fk_identifiers()
        {
            using CleanT = std::remove_cvref_t<FieldT>;
            if constexpr (DAL::Schema::SqlgenForeignKeyField<CleanT>)
            {
                if (auto res = validate_ddl_identifier(std::string_view{ CleanT::ForeignTableType::tablename }); !res)
                {
                    return res;
                }
                return validate_ddl_identifier(DAL::Schema::detail::ForeignKeyColumnName<typename CleanT::ColumnType>::value);
            }
            else if constexpr (DAL::Schema::detail::is_optional_v<CleanT>)
            {
                return validate_field_fk_identifiers<typename CleanT::value_type>();
            }
            else if constexpr (is_sqlgen_unique_v<CleanT>)
            {
                return validate_field_fk_identifiers<typename CleanT::ReflectionType>();
            }
            else
            {
                return {};
            }
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
                if (status)
                {
                    status = validate_field_fk_identifiers<FieldT>();
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

        struct BoundSqlQuery
        {
            std::string sql;
            std::vector<DynamicCell> params;
        };

        struct EscapedLikeCond
        {
            std::string column_name;
            std::string escaped_pattern;
            bool negated{ false };
        };

        struct CollateNoCaseEqCond
        {
            std::string column_name;
            std::string value;
            bool negated{ false };
        };

        template<typename Inner>
        struct GroupCond
        {
            Inner inner;
        };

        template<typename Left, typename Right>
        struct AndCond
        {
            Left left;
            Right right;
        };

        template<typename Left, typename Right>
        struct OrCond
        {
            Left left;
            Right right;
        };

        template<typename T>
        inline constexpr bool is_custom_where_cond_v = false;
        template<>
        inline constexpr bool is_custom_where_cond_v<EscapedLikeCond> = true;
        template<>
        inline constexpr bool is_custom_where_cond_v<CollateNoCaseEqCond> = true;
        template<typename Inner>
        inline constexpr bool is_custom_where_cond_v<GroupCond<Inner>> = true;
        template<typename L, typename R>
        inline constexpr bool is_custom_where_cond_v<AndCond<L, R>> = true;
        template<typename L, typename R>
        inline constexpr bool is_custom_where_cond_v<OrCond<L, R>> = true;

        [[nodiscard]] constexpr std::string_view fk_action_to_sql(DAL::Schema::FkAction action) noexcept
        {
            switch (action)
            {
            case DAL::Schema::FkAction::NoAction:
                return "";
            case DAL::Schema::FkAction::Restrict:
                return "RESTRICT";
            case DAL::Schema::FkAction::SetNull:
                return "SET NULL";
            case DAL::Schema::FkAction::SetDefault:
                return "SET DEFAULT";
            case DAL::Schema::FkAction::Cascade:
                return "CASCADE";
            }
            return "";
        }

        template<typename RawFieldT>
        [[nodiscard]] consteval std::string_view sqlite_column_type_for()
        {
            using CleanT = std::remove_cvref_t<RawFieldT>;
            if constexpr (DAL::Schema::detail::is_optional_v<CleanT>)
            {
                return sqlite_column_type_for<typename CleanT::value_type>();
            }
            else if constexpr (
                DAL::Schema::SqlgenPrimaryKeyField<CleanT> || DAL::Schema::SqlgenForeignKeyField<CleanT> || is_sqlgen_unique_v<CleanT>)
            {
                return sqlite_column_type_for<typename CleanT::ReflectionType>();
            }
            else if constexpr (Core::StrongIdType<CleanT>)
            {
                return sqlite_column_type_for<typename CleanT::value_type>();
            }
            else if constexpr (std::is_same_v<CleanT, bool> || std::is_integral_v<CleanT>)
            {
                return "INTEGER";
            }
            else if constexpr (std::is_floating_point_v<CleanT>)
            {
                return "REAL";
            }
            else if constexpr (std::is_same_v<CleanT, std::vector<std::byte>> || DAL::Schema::detail::is_byte_array_v<CleanT>)
            {
                return "BLOB";
            }
            else if constexpr (requires { typename CleanT::ReflectionType; })
            {
                return sqlite_column_type_for<typename CleanT::ReflectionType>();
            }
            else
            {
                return "TEXT";
            }
        }

        template<typename DTO>
        [[nodiscard]] std::expected<std::vector<std::string>, Core::DbError> build_table_ddl_statements()
        {
            if (auto id_chk = validate_dto_ddl_identifiers<DTO>(); !id_chk)
            {
                return std::unexpected<Core::DbError>{ id_chk.error() };
            }

            constexpr bool is_composite_pk = DAL::Schema::CompositePrimaryKey<DTO>;
            constexpr bool has_unique_flags = requires { DTO::unique_field_flags; };
            constexpr bool has_pk_flags = requires { DTO::pk_field_flags; };
            constexpr bool has_ddl_meta = requires { DTO::field_ddl_meta; };

            const std::string_view table_name{ DTO::tablename };
            std::string create_sql = std::format("CREATE TABLE IF NOT EXISTS \"{}\" (", table_name);
            std::vector<std::string> post_stmts;

            std::size_t col_idx = 0;
            std::optional<Core::DbError> err;
            template for (constexpr auto m : DAL::Schema::members_v<DTO>)
            {
                if (!err)
                {
                    if (col_idx > 0)
                    {
                        create_sql += ", ";
                    }
                    const std::string_view col_name = std::meta::identifier_of(m);
                    using RawFieldT = [:std::meta::type_of(m):];
                    using FieldT = std::remove_cvref_t<RawFieldT>;
                    constexpr bool is_opt = DAL::Schema::detail::is_optional_v<FieldT>;
                    constexpr std::string_view col_type = sqlite_column_type_for<FieldT>();

                    bool is_pk = false;
                    bool is_auto_incr = false;
                    if constexpr (!is_composite_pk)
                    {
                        if constexpr (DAL::Schema::SqlgenPrimaryKeyField<FieldT>)
                        {
                            is_pk = true;
                            is_auto_incr = FieldT::auto_incr;
                        }
                        else if constexpr (has_pk_flags)
                        {
                            if (col_idx < DTO::pk_field_flags.size() && DTO::pk_field_flags[col_idx])
                            {
                                is_pk = true;
                            }
                        }
                    }

                    bool is_uniq = is_sqlgen_unique_v<FieldT>;
                    if constexpr (has_unique_flags)
                    {
                        if (col_idx < DTO::unique_field_flags.size() && DTO::unique_field_flags[col_idx])
                        {
                            is_uniq = true;
                        }
                    }

                    DAL::Schema::FieldDdlMeta meta{};
                    if constexpr (has_ddl_meta)
                    {
                        if (col_idx < DTO::field_ddl_meta.size())
                        {
                            meta = DTO::field_ddl_meta[col_idx];
                            if (meta.is_unique)
                            {
                                is_uniq = true;
                            }
                        }
                    }

                    create_sql += std::format("\"{}\" {}", col_name, col_type);
                    if (is_pk)
                    {
                        create_sql += " PRIMARY KEY";
                        if (is_auto_incr && col_type == "INTEGER")
                        {
                            create_sql += " AUTOINCREMENT";
                        }
                    }
                    if (!is_opt && !(is_pk && is_auto_incr))
                    {
                        create_sql += " NOT NULL";
                    }
                    if (is_uniq && !is_pk)
                    {
                        create_sql += " UNIQUE";
                    }
                    if (meta.has_default_sql)
                    {
                        create_sql += std::format(" DEFAULT ({})", std::string_view{ meta.default_expr });
                    }
                    if (meta.has_check_sql)
                    {
                        create_sql += std::format(" CHECK ({})", std::string_view{ meta.check_expr });
                    }

                    auto append_fk_actions = [&create_sql](DAL::Schema::FkAction on_del, DAL::Schema::FkAction on_upd)
                    {
                        if (const auto d = fk_action_to_sql(on_del); !d.empty())
                        {
                            create_sql += std::format(" ON DELETE {}", d);
                        }
                        if (const auto u = fk_action_to_sql(on_upd); !u.empty())
                        {
                            create_sql += std::format(" ON UPDATE {}", u);
                        }
                    };

                    if constexpr (DAL::Schema::AnyForeignKeyField<FieldT>)
                    {
                        using FkT = DAL::Schema::unwrap_fk_type_t<FieldT>;
                        const std::string_view ref_table{ FkT::ForeignTableType::tablename };
                        const std::string_view ref_col = DAL::Schema::detail::ForeignKeyColumnName<typename FkT::ColumnType>::value;
                        create_sql += std::format(" REFERENCES \"{}\"(\"{}\")", ref_table, ref_col);
                        append_fk_actions(meta.on_delete, meta.on_update);
                    }
                    else if (meta.has_self_fk)
                    {
                        const std::string_view ref_col{ meta.self_fk_col };
                        if (auto chk = validate_ddl_identifier(ref_col); !chk)
                        {
                            err = chk.error();
                        }
                        else
                        {
                            create_sql += std::format(" REFERENCES \"{}\"(\"{}\")", table_name, ref_col);
                            append_fk_actions(meta.on_delete, meta.on_update);
                        }
                    }

                    if (!err && meta.has_index)
                    {
                        const std::string idx_name =
                            meta.has_index_name ? std::string{ meta.index_name } : std::format("idx_{}_{}", table_name, col_name);
                        if (auto chk = validate_ddl_identifier(idx_name); !chk)
                        {
                            err = chk.error();
                        }
                        else
                        {
                            post_stmts.push_back(
                                std::format(
                                    "CREATE {}INDEX IF NOT EXISTS \"{}\" ON \"{}\" (\"{}\");",
                                    meta.index_unique ? "UNIQUE " : "",
                                    idx_name,
                                    table_name,
                                    col_name));
                        }
                    }
                }
                ++col_idx;
            }
            if (err)
            {
                return std::unexpected<Core::DbError>{ std::move(*err) };
            }

            if constexpr (is_composite_pk)
            {
                static constexpr auto key_members = DAL::Schema::key_members_of<DTO>();
                std::string cols;
                std::size_t k_idx = 0;
                template for (constexpr auto km : key_members)
                {
                    if (k_idx > 0)
                    {
                        cols += ", ";
                    }
                    cols += std::format("\"{}\"", std::meta::identifier_of(km));
                    ++k_idx;
                }
                create_sql += std::format(", PRIMARY KEY ({})", cols);
                post_stmts.push_back(
                    std::format("CREATE UNIQUE INDEX IF NOT EXISTS \"ux_{}_composite_pk\" ON \"{}\" ({});", table_name, table_name, cols));
            }
            create_sql += ");";

            if constexpr (requires { DTO::composite_indexes; })
            {
                for (const auto& ci : DTO::composite_indexes)
                {
                    const std::string_view idx_name{ ci.name };
                    if (auto chk = validate_ddl_identifier(idx_name); !chk)
                    {
                        return std::unexpected<Core::DbError>{ chk.error() };
                    }
                    std::string cols_sql;
                    std::string_view csv{ ci.columns_csv };
                    std::size_t pos = 0;
                    std::size_t col_count = 0;
                    while (pos <= csv.size())
                    {
                        const std::size_t comma = csv.find(',', pos);
                        std::string_view token = (comma == std::string_view::npos) ? csv.substr(pos) : csv.substr(pos, comma - pos);
                        while (!token.empty() && (token.front() == ' ' || token.front() == '\t'))
                        {
                            token.remove_prefix(1);
                        }
                        while (!token.empty() && (token.back() == ' ' || token.back() == '\t'))
                        {
                            token.remove_suffix(1);
                        }
                        if (auto chk = validate_ddl_identifier(token); !chk)
                        {
                            return std::unexpected<Core::DbError>{ chk.error() };
                        }
                        if (col_count > 0)
                        {
                            cols_sql += ", ";
                        }
                        cols_sql += std::format("\"{}\"", token);
                        ++col_count;
                        if (comma == std::string_view::npos)
                        {
                            break;
                        }
                        pos = comma + 1;
                    }
                    post_stmts.push_back(
                        std::format(
                            "CREATE {}INDEX IF NOT EXISTS \"{}\" ON \"{}\" ({});",
                            ci.unique ? "UNIQUE " : "",
                            idx_name,
                            table_name,
                            cols_sql));
                }
            }

            std::vector<std::string> stmts;
            stmts.reserve(1 + post_stmts.size());
            stmts.push_back(std::move(create_sql));
            for (auto& s : post_stmts)
            {
                stmts.push_back(std::move(s));
            }
            return stmts;
        }

        [[nodiscard]] inline std::string escape_like_literal(std::string_view input)
        {
            std::string out;
            out.reserve(input.size());
            for (const char ch : input)
            {
                if (ch == '\\' || ch == '%' || ch == '_')
                {
                    out.push_back('\\');
                }
                out.push_back(ch);
            }
            return out;
        }

        // Formats a string literal safely for DDL clauses (e.g. CREATE INDEX ... WHERE) where SQLite
        // forbids '?' bind parameters. Strings containing embedded NUL bytes use O(1)-depth
        // CAST(X'...' AS TEXT) hex encoding instead of nested '|| char(0) ||' concatenation chains.
        [[nodiscard]] inline std::string format_ddl_string_literal(std::string_view sv)
        {
            if (sv.find('\0') != std::string_view::npos)
            {
                static constexpr char kHex[] = "0123456789ABCDEF";
                std::string out;
                out.reserve(16 + sv.size() * 2);
                out += "CAST(X'";
                for (const char raw_ch : sv)
                {
                    const unsigned char b = static_cast<unsigned char>(raw_ch);
                    out.push_back(kHex[b >> 4]);
                    out.push_back(kHex[b & 0x0Fu]);
                }
                out += "' AS TEXT)";
                return out;
            }
            std::string out;
            out.reserve(sv.size() + 2);
            out.push_back('\'');
            for (const char ch : sv)
            {
                if (ch == '\'')
                {
                    out.push_back('\'');
                }
                out.push_back(ch);
            }
            out.push_back('\'');
            return out;
        }

        [[nodiscard]] inline std::string format_sqlite_duration(const sqlgen::dynamic::Duration& d)
        {
            const std::string prefix = (d.val >= 0) ? "+" : "-";
            const std::uint64_t val = (d.val < 0) ? (static_cast<std::uint64_t>(-(d.val + 1)) + 1ULL) : static_cast<std::uint64_t>(d.val);
            switch (d.unit)
            {
            case sqlgen::dynamic::TimeUnit::milliseconds:
                {
                    const auto h = val / 3600000ULL;
                    const auto m = (val / 60000ULL) % 60ULL;
                    const auto s = (val / 1000ULL) % 60ULL;
                    const auto ms = val % 1000ULL;
                    return std::format("{}{:02}:{:02}:{:02}.{:03}", prefix, h, m, s, ms);
                }
            case sqlgen::dynamic::TimeUnit::weeks:
                return std::format("{}{} days", prefix, val * 7ULL);
            default:
                return std::format("{}{} {}", prefix, val, rfl::enum_to_string(d.unit));
            }
        }

        [[nodiscard]] inline std::string format_sqlite_dynamic_type(const sqlgen::dynamic::Type& t)
        {
            return t.visit(
                [](const auto& v) -> std::string
                {
                    using T = std::remove_cvref_t<decltype(v)>;
                    if constexpr (
                        std::is_same_v<T, sqlgen::dynamic::types::Boolean> || std::is_same_v<T, sqlgen::dynamic::types::Int8> ||
                        std::is_same_v<T, sqlgen::dynamic::types::Int16> || std::is_same_v<T, sqlgen::dynamic::types::Int32> ||
                        std::is_same_v<T, sqlgen::dynamic::types::Int64> || std::is_same_v<T, sqlgen::dynamic::types::UInt8> ||
                        std::is_same_v<T, sqlgen::dynamic::types::UInt16> || std::is_same_v<T, sqlgen::dynamic::types::UInt32> ||
                        std::is_same_v<T, sqlgen::dynamic::types::UInt64>)
                    {
                        return "INTEGER";
                    }
                    else if constexpr (
                        std::is_same_v<T, sqlgen::dynamic::types::Float32> || std::is_same_v<T, sqlgen::dynamic::types::Float64>)
                    {
                        return "REAL";
                    }
                    else if constexpr (std::is_same_v<T, sqlgen::dynamic::types::JSON>)
                    {
                        return "JSONB";
                    }
                    else if constexpr (std::is_same_v<T, sqlgen::dynamic::types::Dynamic>)
                    {
                        return v.type_name;
                    }
                    else
                    {
                        return "TEXT";
                    }
                });
        }

        // Compiles sqlgen::dynamic AST nodes into SQLite SQL with true '?' bind parameters
        // (or hardened DDL-inline literals when inline_ddl_literals == true for CREATE INDEX ... WHERE).
        class SqlAstCompiler
        {
        public:
            explicit SqlAstCompiler(bool inline_ddl_literals = false) noexcept
                : inline_ddl_(inline_ddl_literals)
            {
            }

            [[nodiscard]] std::expected<std::string, Core::DbError> compile_value(const sqlgen::dynamic::Value& val)
            {
                return val.val.visit(
                    [this](const auto& v) -> std::expected<std::string, Core::DbError>
                    {
                        using T = std::remove_cvref_t<decltype(v)>;
                        if constexpr (std::is_same_v<T, sqlgen::dynamic::Null>)
                        {
                            return std::string{ "NULL" };
                        }
                        else if constexpr (std::is_same_v<T, sqlgen::dynamic::Boolean>)
                        {
                            if (inline_ddl_)
                            {
                                return std::string{ v.val ? "1" : "0" };
                            }
                            params_.emplace_back(std::int64_t{ v.val ? 1 : 0 });
                            return std::string{ "?" };
                        }
                        else if constexpr (std::is_same_v<T, sqlgen::dynamic::Integer>)
                        {
                            if (inline_ddl_)
                            {
                                return std::to_string(v.val);
                            }
                            params_.emplace_back(static_cast<std::int64_t>(v.val));
                            return std::string{ "?" };
                        }
                        else if constexpr (std::is_same_v<T, sqlgen::dynamic::Float>)
                        {
                            if (inline_ddl_)
                            {
                                if (std::isnan(v.val) || std::isinf(v.val))
                                {
                                    return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation(
                                        "Non-finite floating-point literal is not allowed in DDL index predicate") };
                                }
                                return std::format("{:.17g}", v.val);
                            }
                            params_.emplace_back(static_cast<double>(v.val));
                            return std::string{ "?" };
                        }
                        else if constexpr (std::is_same_v<T, sqlgen::dynamic::String>)
                        {
                            if (inline_ddl_)
                            {
                                return format_ddl_string_literal(v.val);
                            }
                            params_.emplace_back(v.val);
                            return std::string{ "?" };
                        }
                        else if constexpr (std::is_same_v<T, sqlgen::dynamic::Timestamp>)
                        {
                            if (inline_ddl_)
                            {
                                return std::to_string(v.seconds_since_unix);
                            }
                            params_.emplace_back(static_cast<std::int64_t>(v.seconds_since_unix));
                            return std::string{ "?" };
                        }
                        else if constexpr (std::is_same_v<T, sqlgen::dynamic::Duration>)
                        {
                            const std::string dur = format_sqlite_duration(v);
                            if (inline_ddl_)
                            {
                                return format_ddl_string_literal(dur);
                            }
                            params_.emplace_back(dur);
                            return std::string{ "?" };
                        }
                    });
            }

            [[nodiscard]] std::expected<std::string, Core::DbError> compile_column(const sqlgen::dynamic::Column& c)
            {
                if (auto chk = validate_ddl_identifier(c.name); !chk)
                {
                    return std::unexpected<Core::DbError>{ chk.error() };
                }
                return c.alias_or_table.visit(
                    [&](const auto& a) -> std::expected<std::string, Core::DbError>
                    {
                        using AliasType = std::remove_cvref_t<decltype(a)>;
                        if constexpr (std::is_same_v<AliasType, sqlgen::dynamic::alias_or_table::Alias>)
                        {
                            if (auto chk = validate_ddl_identifier(a.name); !chk)
                            {
                                return std::unexpected<Core::DbError>{ chk.error() };
                            }
                            return std::format("\"{}\".\"{}\"", a.name, c.name);
                        }
                        else if constexpr (std::is_same_v<AliasType, sqlgen::dynamic::alias_or_table::Table>)
                        {
                            if (auto chk = validate_ddl_identifier(a.name); !chk)
                            {
                                return std::unexpected<Core::DbError>{ chk.error() };
                            }
                            return std::format("\"{}\".\"{}\"", a.name, c.name);
                        }
                        else
                        {
                            return std::format("\"{}\"", c.name);
                        }
                    });
            }

            [[nodiscard]] std::expected<std::string, Core::DbError> compile_column_or_value(const sqlgen::dynamic::ColumnOrValue& cov)
            {
                return cov.visit(
                    [this](const auto& item) -> std::expected<std::string, Core::DbError>
                    {
                        using T = std::remove_cvref_t<decltype(item)>;
                        if constexpr (std::is_same_v<T, sqlgen::dynamic::Column>)
                        {
                            return compile_column(item);
                        }
                        else
                        {
                            return compile_value(item);
                        }
                    });
            }

            [[nodiscard]] std::expected<std::string, Core::DbError> compile_aggregation(const sqlgen::dynamic::Aggregation& agg)
            {
                return agg.val.visit(
                    [this](const auto& a) -> std::expected<std::string, Core::DbError>
                    {
                        using T = std::remove_cvref_t<decltype(a)>;
                        if constexpr (std::is_same_v<T, sqlgen::dynamic::Aggregation::Avg>)
                        {
                            auto op = compile_operation(*a.val);
                            if (!op)
                            {
                                return std::unexpected<Core::DbError>{ op.error() };
                            }
                            return std::format("AVG({})", *op);
                        }
                        else if constexpr (std::is_same_v<T, sqlgen::dynamic::Aggregation::Count>)
                        {
                            if (!a.val)
                            {
                                return std::string{ "COUNT(*)" };
                            }
                            auto col = compile_column(*a.val);
                            if (!col)
                            {
                                return std::unexpected<Core::DbError>{ col.error() };
                            }
                            return std::format("COUNT({}{})", a.distinct ? "DISTINCT " : "", *col);
                        }
                        else if constexpr (std::is_same_v<T, sqlgen::dynamic::Aggregation::Max>)
                        {
                            auto op = compile_operation(*a.val);
                            if (!op)
                            {
                                return std::unexpected<Core::DbError>{ op.error() };
                            }
                            return std::format("MAX({})", *op);
                        }
                        else if constexpr (std::is_same_v<T, sqlgen::dynamic::Aggregation::Min>)
                        {
                            auto op = compile_operation(*a.val);
                            if (!op)
                            {
                                return std::unexpected<Core::DbError>{ op.error() };
                            }
                            return std::format("MIN({})", *op);
                        }
                        else if constexpr (std::is_same_v<T, sqlgen::dynamic::Aggregation::Sum>)
                        {
                            auto op = compile_operation(*a.val);
                            if (!op)
                            {
                                return std::unexpected<Core::DbError>{ op.error() };
                            }
                            return std::format("SUM({})", *op);
                        }
                    });
            }

            [[nodiscard]] std::expected<std::string, Core::DbError> compile_operation(const sqlgen::dynamic::Operation& op)
            {
                auto unary =
                    [this](const auto& node, std::string_view prefix, std::string_view suffix) -> std::expected<std::string, Core::DbError>
                {
                    auto a = compile_operation(*node.op1);
                    if (!a)
                    {
                        return std::unexpected<Core::DbError>{ a.error() };
                    }
                    return std::format("{}{}{}", prefix, *a, suffix);
                };
                auto binary = [this](const auto& node, std::string_view prefix, std::string_view mid, std::string_view suffix)
                    -> std::expected<std::string, Core::DbError>
                {
                    auto a = compile_operation(*node.op1);
                    if (!a)
                    {
                        return std::unexpected<Core::DbError>{ a.error() };
                    }
                    auto b = compile_operation(*node.op2);
                    if (!b)
                    {
                        return std::unexpected<Core::DbError>{ b.error() };
                    }
                    return std::format("{}{}{}{}{}", prefix, *a, mid, *b, suffix);
                };

                return op.val.visit(
                    [this, &unary, &binary](const auto& s) -> std::expected<std::string, Core::DbError>
                    {
                        using T = std::remove_cvref_t<decltype(s)>;

                        if constexpr (std::is_same_v<T, sqlgen::dynamic::Operation::Abs>)
                        {
                            return unary(s, "abs(", ")");
                        }
                        else if constexpr (std::is_same_v<T, sqlgen::dynamic::Aggregation>)
                        {
                            return compile_aggregation(s);
                        }
                        else if constexpr (std::is_same_v<T, sqlgen::dynamic::Operation::Cast>)
                        {
                            auto a = compile_operation(*s.op1);
                            if (!a)
                            {
                                return std::unexpected<Core::DbError>{ a.error() };
                            }
                            return std::format("cast({} as {})", *a, format_sqlite_dynamic_type(s.target_type));
                        }
                        else if constexpr (std::is_same_v<T, sqlgen::dynamic::Operation::Ceil>)
                        {
                            return unary(s, "ceil(", ")");
                        }
                        else if constexpr (std::is_same_v<T, sqlgen::dynamic::Operation::Coalesce>)
                        {
                            std::string out = "coalesce(";
                            for (std::size_t i = 0; i < s.ops.size(); ++i)
                            {
                                if (i > 0)
                                {
                                    out += ", ";
                                }
                                auto part = compile_operation(*s.ops[i]);
                                if (!part)
                                {
                                    return std::unexpected<Core::DbError>{ part.error() };
                                }
                                out += *part;
                            }
                            out += ")";
                            return out;
                        }
                        else if constexpr (std::is_same_v<T, sqlgen::dynamic::Column>)
                        {
                            return compile_column(s);
                        }
                        else if constexpr (std::is_same_v<T, sqlgen::dynamic::Operation::Concat>)
                        {
                            std::string out = "(";
                            for (std::size_t i = 0; i < s.ops.size(); ++i)
                            {
                                if (i > 0)
                                {
                                    out += " || ";
                                }
                                auto part = compile_operation(*s.ops[i]);
                                if (!part)
                                {
                                    return std::unexpected<Core::DbError>{ part.error() };
                                }
                                out += *part;
                            }
                            out += ")";
                            return out;
                        }
                        else if constexpr (std::is_same_v<T, sqlgen::dynamic::Operation::Cos>)
                        {
                            return unary(s, "cos(", ")");
                        }
                        else if constexpr (std::is_same_v<T, sqlgen::dynamic::Operation::Day>)
                        {
                            return unary(s, "cast(strftime('%d', ", ") as INT)");
                        }
                        else if constexpr (std::is_same_v<T, sqlgen::dynamic::Operation::DaysBetween>)
                        {
                            auto b = compile_operation(*s.op2);
                            if (!b)
                            {
                                return std::unexpected<Core::DbError>{ b.error() };
                            }
                            auto a = compile_operation(*s.op1);
                            if (!a)
                            {
                                return std::unexpected<Core::DbError>{ a.error() };
                            }
                            return std::format("julianday({}) - julianday({})", *b, *a);
                        }
                        else if constexpr (std::is_same_v<T, sqlgen::dynamic::Operation::DatePlusDuration>)
                        {
                            auto dt = compile_operation(*s.date);
                            if (!dt)
                            {
                                return std::unexpected<Core::DbError>{ dt.error() };
                            }
                            std::string out = std::format("datetime({}", *dt);
                            for (const auto& d : s.durations)
                            {
                                auto dv = compile_value(sqlgen::dynamic::Value{ d });
                                if (!dv)
                                {
                                    return std::unexpected<Core::DbError>{ dv.error() };
                                }
                                out += ", ";
                                out += *dv;
                            }
                            out += ")";
                            return out;
                        }
                        else if constexpr (std::is_same_v<T, sqlgen::dynamic::Operation::Divides>)
                        {
                            return binary(s, "(", ") / (", ")");
                        }
                        else if constexpr (std::is_same_v<T, sqlgen::dynamic::Operation::Exp>)
                        {
                            return unary(s, "exp(", ")");
                        }
                        else if constexpr (std::is_same_v<T, sqlgen::dynamic::Operation::Floor>)
                        {
                            return unary(s, "floor(", ")");
                        }
                        else if constexpr (std::is_same_v<T, sqlgen::dynamic::Operation::Hour>)
                        {
                            return unary(s, "cast(strftime('%H', ", ") as INT)");
                        }
                        else if constexpr (std::is_same_v<T, sqlgen::dynamic::Operation::Length>)
                        {
                            return unary(s, "length(", ")");
                        }
                        else if constexpr (std::is_same_v<T, sqlgen::dynamic::Operation::Ln>)
                        {
                            return unary(s, "ln(", ")");
                        }
                        else if constexpr (std::is_same_v<T, sqlgen::dynamic::Operation::Log2>)
                        {
                            return unary(s, "log2(", ")");
                        }
                        else if constexpr (std::is_same_v<T, sqlgen::dynamic::Operation::Lower>)
                        {
                            return unary(s, "lower(", ")");
                        }
                        else if constexpr (std::is_same_v<T, sqlgen::dynamic::Operation::LTrim>)
                        {
                            return binary(s, "ltrim(", ", ", ")");
                        }
                        else if constexpr (std::is_same_v<T, sqlgen::dynamic::Operation::Minus>)
                        {
                            return binary(s, "(", ") - (", ")");
                        }
                        else if constexpr (std::is_same_v<T, sqlgen::dynamic::Operation::Minute>)
                        {
                            return unary(s, "cast(strftime('%M', ", ") as INT)");
                        }
                        else if constexpr (std::is_same_v<T, sqlgen::dynamic::Operation::Mod>)
                        {
                            return binary(s, "mod(", ", ", ")");
                        }
                        else if constexpr (std::is_same_v<T, sqlgen::dynamic::Operation::Month>)
                        {
                            return unary(s, "cast(strftime('%m', ", ") as INT)");
                        }
                        else if constexpr (std::is_same_v<T, sqlgen::dynamic::Operation::Multiplies>)
                        {
                            return binary(s, "(", ") * (", ")");
                        }
                        else if constexpr (std::is_same_v<T, sqlgen::dynamic::Operation::Plus>)
                        {
                            return binary(s, "(", ") + (", ")");
                        }
                        else if constexpr (std::is_same_v<T, sqlgen::dynamic::Operation::Replace>)
                        {
                            auto a = compile_operation(*s.op1);
                            if (!a)
                            {
                                return std::unexpected<Core::DbError>{ a.error() };
                            }
                            auto b = compile_operation(*s.op2);
                            if (!b)
                            {
                                return std::unexpected<Core::DbError>{ b.error() };
                            }
                            auto c = compile_operation(*s.op3);
                            if (!c)
                            {
                                return std::unexpected<Core::DbError>{ c.error() };
                            }
                            return std::format("replace({}, {}, {})", *a, *b, *c);
                        }
                        else if constexpr (std::is_same_v<T, sqlgen::dynamic::Operation::Round>)
                        {
                            return binary(s, "round(", ", ", ")");
                        }
                        else if constexpr (std::is_same_v<T, sqlgen::dynamic::Operation::RTrim>)
                        {
                            return binary(s, "rtrim(", ", ", ")");
                        }
                        else if constexpr (std::is_same_v<T, sqlgen::dynamic::Operation::Second>)
                        {
                            return unary(s, "cast(strftime('%S', ", ") as INT)");
                        }
                        else if constexpr (std::is_same_v<T, sqlgen::dynamic::Operation::Sin>)
                        {
                            return unary(s, "sin(", ")");
                        }
                        else if constexpr (std::is_same_v<T, sqlgen::dynamic::Operation::Sqrt>)
                        {
                            return unary(s, "sqrt(", ")");
                        }
                        else if constexpr (std::is_same_v<T, sqlgen::dynamic::Operation::Tan>)
                        {
                            return unary(s, "tan(", ")");
                        }
                        else if constexpr (std::is_same_v<T, sqlgen::dynamic::Operation::Trim>)
                        {
                            return binary(s, "trim(", ", ", ")");
                        }
                        else if constexpr (std::is_same_v<T, sqlgen::dynamic::Operation::Unixepoch>)
                        {
                            return unary(s, "unixepoch(", ", 'subsec')");
                        }
                        else if constexpr (std::is_same_v<T, sqlgen::dynamic::Operation::Upper>)
                        {
                            return unary(s, "upper(", ")");
                        }
                        else if constexpr (std::is_same_v<T, sqlgen::dynamic::Value>)
                        {
                            return compile_value(s);
                        }
                        else if constexpr (std::is_same_v<T, sqlgen::dynamic::Operation::Weekday>)
                        {
                            return unary(s, "cast(strftime('%w', ", ") as INT)");
                        }
                        else if constexpr (std::is_same_v<T, sqlgen::dynamic::Operation::Year>)
                        {
                            return unary(s, "cast(strftime('%Y', ", ") as INT)");
                        }
                    });
            }

            [[nodiscard]] std::expected<std::string, Core::DbError> compile_condition(const sqlgen::dynamic::Condition& cond)
            {
                auto cmp = [this](const auto& node, std::string_view op_sym) -> std::expected<std::string, Core::DbError>
                {
                    auto lhs = compile_operation(node.op1);
                    if (!lhs)
                    {
                        return std::unexpected<Core::DbError>{ lhs.error() };
                    }
                    auto rhs = compile_operation(node.op2);
                    if (!rhs)
                    {
                        return std::unexpected<Core::DbError>{ rhs.error() };
                    }
                    return std::format("{} {} {}", *lhs, op_sym, *rhs);
                };

                return cond.val.visit(
                    [this, &cmp](const auto& c) -> std::expected<std::string, Core::DbError>
                    {
                        using C = std::remove_cvref_t<decltype(c)>;

                        if constexpr (std::is_same_v<C, sqlgen::dynamic::Condition::And>)
                        {
                            auto a = compile_condition(*c.cond1);
                            if (!a)
                            {
                                return std::unexpected<Core::DbError>{ a.error() };
                            }
                            auto b = compile_condition(*c.cond2);
                            if (!b)
                            {
                                return std::unexpected<Core::DbError>{ b.error() };
                            }
                            return std::format("({}) AND ({})", *a, *b);
                        }
                        else if constexpr (std::is_same_v<C, sqlgen::dynamic::Condition::Or>)
                        {
                            auto a = compile_condition(*c.cond1);
                            if (!a)
                            {
                                return std::unexpected<Core::DbError>{ a.error() };
                            }
                            auto b = compile_condition(*c.cond2);
                            if (!b)
                            {
                                return std::unexpected<Core::DbError>{ b.error() };
                            }
                            return std::format("({}) OR ({})", *a, *b);
                        }
                        else if constexpr (std::is_same_v<C, sqlgen::dynamic::Condition::Not>)
                        {
                            auto inner = compile_condition(*c.cond);
                            if (!inner)
                            {
                                return std::unexpected<Core::DbError>{ inner.error() };
                            }
                            return std::format("NOT ({})", *inner);
                        }
                        else if constexpr (std::is_same_v<C, sqlgen::dynamic::Condition::BooleanColumnOrValue>)
                        {
                            return compile_column_or_value(c.col_or_val);
                        }
                        else if constexpr (std::is_same_v<C, sqlgen::dynamic::Condition::Equal>)
                        {
                            return cmp(c, "=");
                        }
                        else if constexpr (std::is_same_v<C, sqlgen::dynamic::Condition::NotEqual>)
                        {
                            return cmp(c, "!=");
                        }
                        else if constexpr (std::is_same_v<C, sqlgen::dynamic::Condition::GreaterEqual>)
                        {
                            return cmp(c, ">=");
                        }
                        else if constexpr (std::is_same_v<C, sqlgen::dynamic::Condition::GreaterThan>)
                        {
                            return cmp(c, ">");
                        }
                        else if constexpr (std::is_same_v<C, sqlgen::dynamic::Condition::LesserEqual>)
                        {
                            return cmp(c, "<=");
                        }
                        else if constexpr (std::is_same_v<C, sqlgen::dynamic::Condition::LesserThan>)
                        {
                            return cmp(c, "<");
                        }
                        else if constexpr (std::is_same_v<C, sqlgen::dynamic::Condition::IsNull>)
                        {
                            auto a = compile_operation(c.op);
                            if (!a)
                            {
                                return std::unexpected<Core::DbError>{ a.error() };
                            }
                            return std::format("{} IS NULL", *a);
                        }
                        else if constexpr (std::is_same_v<C, sqlgen::dynamic::Condition::IsNotNull>)
                        {
                            auto a = compile_operation(c.op);
                            if (!a)
                            {
                                return std::unexpected<Core::DbError>{ a.error() };
                            }
                            return std::format("{} IS NOT NULL", *a);
                        }
                        else if constexpr (
                            std::is_same_v<C, sqlgen::dynamic::Condition::Like> || std::is_same_v<C, sqlgen::dynamic::Condition::NotLike>)
                        {
                            auto a = compile_operation(c.op);
                            if (!a)
                            {
                                return std::unexpected<Core::DbError>{ a.error() };
                            }
                            auto pat = compile_value(c.pattern);
                            if (!pat)
                            {
                                return std::unexpected<Core::DbError>{ pat.error() };
                            }
                            constexpr std::string_view kw = std::is_same_v<C, sqlgen::dynamic::Condition::Like> ? "LIKE" : "NOT LIKE";
                            return std::format("{} {} {}", *a, kw, *pat);
                        }
                        else if constexpr (
                            std::is_same_v<C, sqlgen::dynamic::Condition::In> || std::is_same_v<C, sqlgen::dynamic::Condition::NotIn>)
                        {
                            auto a = compile_operation(c.op);
                            if (!a)
                            {
                                return std::unexpected<Core::DbError>{ a.error() };
                            }
                            constexpr std::string_view kw = std::is_same_v<C, sqlgen::dynamic::Condition::In> ? "IN" : "NOT IN";
                            std::string out = std::format("{} {} (", *a, kw);
                            for (std::size_t i = 0; i < c.patterns.size(); ++i)
                            {
                                if (i > 0)
                                {
                                    out += ", ";
                                }
                                auto pv = compile_value(c.patterns[i]);
                                if (!pv)
                                {
                                    return std::unexpected<Core::DbError>{ pv.error() };
                                }
                                out += *pv;
                            }
                            out += ")";
                            return out;
                        }
                    });
            }

            template<typename DTO, typename CondT>
            [[nodiscard]] std::expected<std::string, Core::DbError> compile_where_expr(const CondT& cond)
            {
                using CleanC = std::remove_cvref_t<CondT>;
                if constexpr (std::is_same_v<CleanC, EscapedLikeCond>)
                {
                    if (auto chk = validate_ddl_identifier(cond.column_name); !chk)
                    {
                        return std::unexpected<Core::DbError>{ chk.error() };
                    }
                    std::string pat_sql;
                    if (inline_ddl_)
                    {
                        pat_sql = format_ddl_string_literal(cond.escaped_pattern);
                    }
                    else
                    {
                        params_.emplace_back(cond.escaped_pattern);
                        pat_sql = "?";
                    }
                    return std::format("\"{}\" {} {} ESCAPE '\\'", cond.column_name, cond.negated ? "NOT LIKE" : "LIKE", pat_sql);
                }
                else if constexpr (std::is_same_v<CleanC, CollateNoCaseEqCond>)
                {
                    if (auto chk = validate_ddl_identifier(cond.column_name); !chk)
                    {
                        return std::unexpected<Core::DbError>{ chk.error() };
                    }
                    std::string val_sql;
                    if (inline_ddl_)
                    {
                        val_sql = format_ddl_string_literal(cond.value);
                    }
                    else
                    {
                        params_.emplace_back(cond.value);
                        val_sql = "?";
                    }
                    return std::format("\"{}\" {} {} COLLATE NOCASE", cond.column_name, cond.negated ? "!=" : "=", val_sql);
                }
                else if constexpr (requires { cond.inner; })
                {
                    auto sub = compile_where_expr<DTO>(cond.inner);
                    if (!sub)
                    {
                        return std::unexpected<Core::DbError>{ sub.error() };
                    }
                    return std::format("({})", *sub);
                }
                else if constexpr (requires {
                                       cond.left;
                                       cond.right;
                                   })
                {
                    auto lhs = compile_where_expr<DTO>(cond.left);
                    if (!lhs)
                    {
                        return std::unexpected<Core::DbError>{ lhs.error() };
                    }
                    auto rhs = compile_where_expr<DTO>(cond.right);
                    if (!rhs)
                    {
                        return std::unexpected<Core::DbError>{ rhs.error() };
                    }
                    if constexpr (std::is_same_v<CleanC, AndCond<decltype(cond.left), decltype(cond.right)>>)
                    {
                        return std::format("({}) AND ({})", *lhs, *rhs);
                    }
                    else
                    {
                        return std::format("({}) OR ({})", *lhs, *rhs);
                    }
                }
                else
                {
                    const sqlgen::dynamic::Condition dyn_cond = sqlgen::transpilation::ToCondition<DTO, CleanC>{}(cond);
                    return compile_condition(dyn_cond);
                }
            }

            [[nodiscard]] std::expected<std::string, Core::DbError> compile_table(const sqlgen::dynamic::Table& t)
            {
                if (auto chk = validate_ddl_identifier(t.name); !chk)
                {
                    return std::unexpected<Core::DbError>{ chk.error() };
                }
                if (t.schema)
                {
                    if (auto chk = validate_ddl_identifier(*t.schema); !chk)
                    {
                        return std::unexpected<Core::DbError>{ chk.error() };
                    }
                    return std::format("\"{}\".\"{}\"", *t.schema, t.name);
                }
                return std::format("\"{}\"", t.name);
            }

            [[nodiscard]] std::expected<std::string, Core::DbError> compile_table_or_query(
                const sqlgen::dynamic::SelectFrom::TableOrQueryType& toq)
            {
                return toq.visit(
                    [this](const auto& item) -> std::expected<std::string, Core::DbError>
                    {
                        using T = std::remove_cvref_t<decltype(item)>;
                        if constexpr (std::is_same_v<T, sqlgen::dynamic::Table>)
                        {
                            return compile_table(item);
                        }
                        else if constexpr (std::is_same_v<T, sqlgen::Ref<sqlgen::dynamic::SelectFrom::Union>>)
                        {
                            auto u = compile_union(*item);
                            if (!u)
                            {
                                return std::unexpected<Core::DbError>{ u.error() };
                            }
                            return std::format("({})", *u);
                        }
                        else
                        {
                            auto s = compile_select_from(*item, false, {});
                            if (!s)
                            {
                                return std::unexpected<Core::DbError>{ s.error() };
                            }
                            return std::format("({})", *s);
                        }
                    });
            }

            [[nodiscard]] std::expected<std::string, Core::DbError> compile_union(const sqlgen::dynamic::SelectFrom::Union& u)
            {
                std::string cols;
                for (std::size_t i = 0; i < u.columns.size(); ++i)
                {
                    if (auto chk = validate_ddl_identifier(u.columns[i]); !chk)
                    {
                        return std::unexpected<Core::DbError>{ chk.error() };
                    }
                    if (i > 0)
                    {
                        cols += ", ";
                    }
                    cols += std::format("\"{}\"", u.columns[i]);
                }
                std::string out;
                const std::string_view sep = u.all ? " UNION ALL " : " UNION ";
                for (std::size_t i = 0; i < u.selects->size(); ++i)
                {
                    if (i > 0)
                    {
                        out += sep;
                    }
                    auto sel = compile_select_from((*u.selects)[i], false, {});
                    if (!sel)
                    {
                        return std::unexpected<Core::DbError>{ sel.error() };
                    }
                    out += std::format("SELECT {} FROM ({})", cols, *sel);
                }
                return out;
            }

            [[nodiscard]] std::expected<std::string, Core::DbError> compile_select_from(
                const sqlgen::dynamic::SelectFrom& stmt,
                bool select_rowid_only = false,
                std::function<std::expected<std::string, Core::DbError>()> injected_where = {})
            {
                std::string out = "SELECT ";
                if (select_rowid_only)
                {
                    out += "rowid";
                }
                else
                {
                    for (std::size_t i = 0; i < stmt.fields.size(); ++i)
                    {
                        if (i > 0)
                        {
                            out += ", ";
                        }
                        auto f = compile_operation(stmt.fields[i].val);
                        if (!f)
                        {
                            return std::unexpected<Core::DbError>{ f.error() };
                        }
                        out += *f;
                        if (stmt.fields[i].as)
                        {
                            if (auto chk = validate_ddl_identifier(*stmt.fields[i].as); !chk)
                            {
                                return std::unexpected<Core::DbError>{ chk.error() };
                            }
                            out += std::format(" AS \"{}\"", *stmt.fields[i].as);
                        }
                    }
                }

                auto from = compile_table_or_query(stmt.table_or_query);
                if (!from)
                {
                    return std::unexpected<Core::DbError>{ from.error() };
                }
                out += " FROM ";
                out += *from;

                if (stmt.alias)
                {
                    if (auto chk = validate_ddl_identifier(*stmt.alias); !chk)
                    {
                        return std::unexpected<Core::DbError>{ chk.error() };
                    }
                    out += std::format(" \"{}\"", *stmt.alias);
                }

                if (stmt.joins)
                {
                    for (const auto& j : *stmt.joins)
                    {
                        std::string how = rfl::enum_to_string(j.how);
                        for (char& ch : how)
                        {
                            ch = (ch == '_') ? ' ' : static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                        }
                        auto jt = compile_table_or_query(j.table_or_query);
                        if (!jt)
                        {
                            return std::unexpected<Core::DbError>{ jt.error() };
                        }
                        if (auto chk = validate_ddl_identifier(j.alias); !chk)
                        {
                            return std::unexpected<Core::DbError>{ chk.error() };
                        }
                        out += std::format(" {} {} \"{}\" ON ", how, *jt, j.alias);
                        if (j.on)
                        {
                            auto on_sql = compile_condition(*j.on);
                            if (!on_sql)
                            {
                                return std::unexpected<Core::DbError>{ on_sql.error() };
                            }
                            out += *on_sql;
                        }
                        else
                        {
                            out += "1 = 1";
                        }
                    }
                }

                if (injected_where)
                {
                    auto w = injected_where();
                    if (!w)
                    {
                        return std::unexpected<Core::DbError>{ w.error() };
                    }
                    if (!w->empty())
                    {
                        out += " WHERE ";
                        out += *w;
                    }
                }
                else if (stmt.where)
                {
                    auto w = compile_condition(*stmt.where);
                    if (!w)
                    {
                        return std::unexpected<Core::DbError>{ w.error() };
                    }
                    out += " WHERE ";
                    out += *w;
                }

                if (stmt.group_by)
                {
                    out += " GROUP BY ";
                    for (std::size_t i = 0; i < stmt.group_by->columns.size(); ++i)
                    {
                        if (i > 0)
                        {
                            out += ", ";
                        }
                        auto gc = compile_column(stmt.group_by->columns[i]);
                        if (!gc)
                        {
                            return std::unexpected<Core::DbError>{ gc.error() };
                        }
                        out += *gc;
                    }
                }

                if (stmt.order_by)
                {
                    out += " ORDER BY ";
                    for (std::size_t i = 0; i < stmt.order_by->columns.size(); ++i)
                    {
                        if (i > 0)
                        {
                            out += ", ";
                        }
                        auto oc = compile_column(stmt.order_by->columns[i].column);
                        if (!oc)
                        {
                            return std::unexpected<Core::DbError>{ oc.error() };
                        }
                        out += *oc;
                        if (stmt.order_by->columns[i].desc)
                        {
                            out += " DESC";
                        }
                    }
                }

                if (stmt.limit)
                {
                    if (stmt.limit->val > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()))
                    {
                        return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation(
                            "Query limit exceeds SQLite signed 64-bit INTEGER range") };
                    }
                    out += std::format(" LIMIT {}", stmt.limit->val);
                }

                if (stmt.offset)
                {
                    if (stmt.offset->val > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()))
                    {
                        return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation(
                            "Query offset exceeds SQLite signed 64-bit INTEGER range") };
                    }
                    out += std::format(" OFFSET {}", stmt.offset->val);
                }

                return out;
            }

            [[nodiscard]] std::expected<std::string, Core::DbError> compile_delete_from(const sqlgen::dynamic::DeleteFrom& stmt)
            {
                auto tbl = compile_table(stmt.table);
                if (!tbl)
                {
                    return std::unexpected<Core::DbError>{ tbl.error() };
                }
                std::string out = std::format("DELETE FROM {}", *tbl);
                if (stmt.where)
                {
                    auto w = compile_condition(*stmt.where);
                    if (!w)
                    {
                        return std::unexpected<Core::DbError>{ w.error() };
                    }
                    out += " WHERE ";
                    out += *w;
                }
                return out;
            }

            [[nodiscard]] std::expected<std::string, Core::DbError> compile_update(const sqlgen::dynamic::Update& stmt)
            {
                auto tbl = compile_table(stmt.table);
                if (!tbl)
                {
                    return std::unexpected<Core::DbError>{ tbl.error() };
                }
                std::string out = std::format("UPDATE {} SET ", *tbl);
                for (std::size_t i = 0; i < stmt.sets.size(); ++i)
                {
                    if (i > 0)
                    {
                        out += ", ";
                    }
                    if (auto chk = validate_ddl_identifier(stmt.sets[i].col.name); !chk)
                    {
                        return std::unexpected<Core::DbError>{ chk.error() };
                    }
                    auto val = compile_column_or_value(stmt.sets[i].to);
                    if (!val)
                    {
                        return std::unexpected<Core::DbError>{ val.error() };
                    }
                    out += std::format("\"{}\" = {}", stmt.sets[i].col.name, *val);
                }
                if (stmt.where)
                {
                    auto w = compile_condition(*stmt.where);
                    if (!w)
                    {
                        return std::unexpected<Core::DbError>{ w.error() };
                    }
                    out += " WHERE ";
                    out += *w;
                }
                return out;
            }

            [[nodiscard]] std::expected<BoundSqlQuery, Core::DbError> compile_statement(const sqlgen::dynamic::Statement& stmt)
            {
                auto sql_res = stmt.visit(
                    [this](const auto& s) -> std::expected<std::string, Core::DbError>
                    {
                        using S = std::remove_cvref_t<decltype(s)>;
                        if constexpr (std::is_same_v<S, sqlgen::dynamic::SelectFrom>)
                        {
                            return compile_select_from(s, false, {});
                        }
                        else if constexpr (std::is_same_v<S, sqlgen::dynamic::SelectFrom::Union>)
                        {
                            return compile_union(s);
                        }
                        else if constexpr (std::is_same_v<S, sqlgen::dynamic::DeleteFrom>)
                        {
                            return compile_delete_from(s);
                        }
                        else if constexpr (std::is_same_v<S, sqlgen::dynamic::Update>)
                        {
                            return compile_update(s);
                        }
                        else
                        {
                            return std::unexpected<Core::DbError>{ Core::DbError::unknown(
                                "Unsupported dynamic statement type for parameterized SQL compilation") };
                        }
                    });
                if (!sql_res)
                {
                    return std::unexpected<Core::DbError>{ sql_res.error() };
                }
                *sql_res += ";";
                return BoundSqlQuery{ std::move(*sql_res), std::move(params_) };
            }

            [[nodiscard]] std::vector<DynamicCell> take_params() noexcept
            {
                return std::move(params_);
            }

        private:
            bool inline_ddl_{ false };
            std::vector<DynamicCell> params_{};
        };

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

        [[nodiscard]] inline std::expected<void, Core::DbError> check_variable_limit(sqlite3* db, std::size_t param_count)
        {
            if (db != nullptr && param_count > 0)
            {
                const int max_vars = sqlite3_limit(db, SQLITE_LIMIT_VARIABLE_NUMBER, -1);
                if (max_vars > 0 && param_count > static_cast<std::size_t>(max_vars))
                {
                    return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation(
                        std::format("Query parameter count ({}) exceeds SQLite variable limit ({})", param_count, max_vars)) };
                }
            }
            return {};
        }

        [[nodiscard]] inline std::expected<void, Core::DbError> bind_dynamic_params(
            sqlite3* db, sqlite3_stmt* stmt, std::span<const DynamicCell> params)
        {
            if (auto lim = check_variable_limit(db, params.size()); !lim)
            {
                return lim;
            }
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
                            std::format("Unsigned 64-bit integer parameter at index {} exceeds SQLite signed 64-bit INTEGER range", idx)) };
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
            else if constexpr (Schema::detail::is_byte_array_v<CleanT>)
            {
                const int rc = field.empty() ? sqlite3_bind_zeroblob(stmt, idx, 0)
                                             : sqlite3_bind_blob(stmt, idx, field.data(), static_cast<int>(field.size()), SQLITE_TRANSIENT);
                if (rc != SQLITE_OK)
                {
                    return std::unexpected<Core::DbError>{ map_sqlite_db_error(db, sqlite3_errmsg(db)) };
                }
                return {};
            }
            else if constexpr (Schema::detail::is_sys_time_v<CleanT>)
            {
                const std::string iso = Schema::detail::format_sys_time_iso8601(field);
                return bind_typed_field(db, stmt, idx, iso, false);
            }
            else if constexpr (Schema::detail::is_ymd_v<CleanT>)
            {
                if (!field.ok())
                {
                    return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation("Calendar date is invalid") };
                }
                const std::string iso = Schema::detail::format_ymd_iso8601(field);
                return bind_typed_field(db, stmt, idx, iso, false);
            }
            else if constexpr (Schema::detail::is_fs_path_v<CleanT>)
            {
                const std::string utf8 = field.string();
                return bind_typed_field(db, stmt, idx, utf8, false);
            }
            else if constexpr (std::is_enum_v<CleanT>)
            {
                return bind_typed_field(db, stmt, idx, std::to_underlying(field), false);
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

        template<typename Val>
        [[nodiscard]] std::expected<DynamicCell, Core::DbError> to_dynamic_cell(const Val& val)
        {
            using CleanT = std::remove_cvref_t<Val>;
            if constexpr (std::is_same_v<CleanT, DynamicCell>)
            {
                return val;
            }
            else if constexpr (std::is_same_v<CleanT, std::monostate> || std::is_same_v<CleanT, std::nullptr_t>)
            {
                return DynamicCell{ std::monostate{} };
            }
            else if constexpr (Schema::detail::is_optional_v<CleanT>)
            {
                if (!val.has_value())
                {
                    return DynamicCell{ std::monostate{} };
                }
                return to_dynamic_cell(*val);
            }
            else if constexpr (Core::StrongIdType<CleanT> || Core::SingleKeyType<CleanT>)
            {
                return to_dynamic_cell(val.get());
            }
            else if constexpr (std::is_same_v<CleanT, bool>)
            {
                return DynamicCell{ static_cast<std::int64_t>(val ? 1 : 0) };
            }
            else if constexpr (is_strict_int_v<CleanT>)
            {
                if constexpr (std::is_unsigned_v<CleanT> && sizeof(CleanT) >= sizeof(std::int64_t))
                {
                    if (val > static_cast<CleanT>(std::numeric_limits<std::int64_t>::max()))
                    {
                        return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation(
                            "Unsigned 64-bit parameter exceeds SQLite signed 64-bit INTEGER range") };
                    }
                }
                return DynamicCell{ static_cast<std::int64_t>(val) };
            }
            else if constexpr (std::is_floating_point_v<CleanT>)
            {
                return DynamicCell{ static_cast<double>(val) };
            }
            else if constexpr (std::is_same_v<CleanT, std::string>)
            {
                return DynamicCell{ val };
            }
            else if constexpr (std::is_same_v<CleanT, std::string_view>)
            {
                return DynamicCell{ std::string{ val } };
            }
            else if constexpr (std::is_same_v<CleanT, const char*> || std::is_same_v<CleanT, char*>)
            {
                if (val == nullptr)
                {
                    return DynamicCell{ std::monostate{} };
                }
                return DynamicCell{ std::string{ val } };
            }
            else if constexpr (std::is_convertible_v<CleanT, std::string_view>)
            {
                return DynamicCell{ std::string{ std::string_view{ val } } };
            }
            else if constexpr (std::is_same_v<CleanT, std::vector<std::byte>>)
            {
                return DynamicCell{ val };
            }
            else if constexpr (Schema::detail::is_byte_array_v<CleanT>)
            {
                return DynamicCell{ std::vector<std::byte>(val.begin(), val.end()) };
            }
            else if constexpr (Schema::detail::is_sys_time_v<CleanT>)
            {
                return DynamicCell{ Schema::detail::format_sys_time_iso8601(val) };
            }
            else if constexpr (Schema::detail::is_ymd_v<CleanT>)
            {
                if (!val.ok())
                {
                    return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation("Calendar date is invalid") };
                }
                return DynamicCell{ Schema::detail::format_ymd_iso8601(val) };
            }
            else if constexpr (Schema::detail::is_fs_path_v<CleanT>)
            {
                return DynamicCell{ val.string() };
            }
            else if constexpr (std::is_enum_v<CleanT>)
            {
                return to_dynamic_cell(std::to_underlying(val));
            }
            else
            {
                static_assert(std::is_same_v<CleanT, DynamicCell>, "Unsupported parameter type for DynamicCell conversion");
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
            if (len <= 0)
            {
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
                            std::format("Invalid boolean text ({} bytes) in column {}", sv.size(), col_idx)) };
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
                                std::format("Integer value in column {} is out of range for target type", col_idx)) };
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
                                    std::format("Invalid or out-of-range integer text ({} bytes) in column {}", sv.size(), col_idx)) };
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
                                    std::format(
                                        "Invalid or out-of-range unsigned integer text ({} bytes) in column {}", sv.size(), col_idx)) };
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
                                std::format("Invalid floating-point text ({} bytes) in column {}", sv.size(), col_idx)) };
                        }
                        return static_cast<CleanT>(parsed);
                    }
                    return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation(
                        std::format("Incompatible SQLite storage class {} for floating-point column {}", col_type, col_idx)) };
                }
                else if constexpr (std::is_same_v<CleanT, std::string>)
                {
                    if (col_type == SQLITE_BLOB)
                    {
                        return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation(
                            std::format("Incompatible SQLite BLOB storage class for string column {}", col_idx)) };
                    }
                    return read_column_text_exact(db, stmt, col_idx);
                }
                else if constexpr (std::is_same_v<CleanT, std::vector<std::byte>>)
                {
                    if (col_type != SQLITE_BLOB)
                    {
                        return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation(
                            std::format("Incompatible SQLite storage class {} for BLOB column {}", col_type, col_idx)) };
                    }
                    const auto* blob = static_cast<const std::byte*>(sqlite3_column_blob(stmt, col_idx));
                    const int len = sqlite3_column_bytes(stmt, col_idx);
                    if (blob == nullptr)
                    {
                        if (sqlite3_errcode(db) == SQLITE_NOMEM)
                        {
                            return std::unexpected<Core::DbError>{ map_sqlite_db_error(db, sqlite3_errmsg(db)) };
                        }
                        return std::vector<std::byte>{};
                    }
                    if (len <= 0)
                    {
                        return std::vector<std::byte>{};
                    }
                    return std::vector<std::byte>(blob, blob + len);
                }
                else if constexpr (Schema::detail::is_byte_array_v<CleanT>)
                {
                    constexpr std::size_t N = Schema::detail::byte_array_extent_v<CleanT>;
                    if (col_type != SQLITE_BLOB)
                    {
                        return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation(
                            std::format("Incompatible SQLite storage class {} for fixed BLOB column {}", col_type, col_idx)) };
                    }
                    const auto* blob = static_cast<const std::byte*>(sqlite3_column_blob(stmt, col_idx));
                    const int len = sqlite3_column_bytes(stmt, col_idx);
                    if (blob == nullptr && sqlite3_errcode(db) == SQLITE_NOMEM)
                    {
                        return std::unexpected<Core::DbError>{ map_sqlite_db_error(db, sqlite3_errmsg(db)) };
                    }
                    if (len < 0 || static_cast<std::size_t>(len) != N)
                    {
                        return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation(
                            std::format("Expected {}-byte BLOB in column {}, got {} bytes", N, col_idx, len)) };
                    }
                    CleanT out{};
                    if constexpr (N > 0)
                    {
                        if (blob != nullptr)
                        {
                            std::memcpy(out.data(), blob, N);
                        }
                    }
                    return out;
                }
                else if constexpr (Schema::detail::is_sys_time_v<CleanT>)
                {
                    if (col_type != SQLITE_TEXT)
                    {
                        return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation(
                            std::format("Incompatible SQLite storage class {} for timestamp column {}", col_type, col_idx)) };
                    }
                    auto text_res = read_column_text_exact(db, stmt, col_idx);
                    if (!text_res)
                    {
                        return std::unexpected<Core::DbError>{ text_res.error() };
                    }
                    return Schema::detail::parse_sys_time_iso8601<CleanT>(*text_res);
                }
                else if constexpr (Schema::detail::is_ymd_v<CleanT>)
                {
                    if (col_type != SQLITE_TEXT)
                    {
                        return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation(
                            std::format("Incompatible SQLite storage class {} for date column {}", col_type, col_idx)) };
                    }
                    auto text_res = read_column_text_exact(db, stmt, col_idx);
                    if (!text_res)
                    {
                        return std::unexpected<Core::DbError>{ text_res.error() };
                    }
                    return Schema::detail::parse_ymd_iso8601(*text_res);
                }
                else if constexpr (Schema::detail::is_fs_path_v<CleanT>)
                {
                    if (col_type == SQLITE_BLOB)
                    {
                        return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation(
                            std::format("Incompatible SQLite BLOB storage class for path column {}", col_idx)) };
                    }
                    auto text_res = read_column_text_exact(db, stmt, col_idx);
                    if (!text_res)
                    {
                        return std::unexpected<Core::DbError>{ text_res.error() };
                    }
                    return std::filesystem::path{ std::move(*text_res) };
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
                            return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation(
                                std::format("Invalid enum text ({} bytes) in column {}", text_res->size(), col_idx)) };
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
                                std::format("Enum integer value in column {} is out of range", col_idx)) };
                        }
                        return static_cast<CleanT>(static_cast<UnderlyingT>(v));
                    }
                    return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation(
                        std::format("Incompatible SQLite storage class {} for enum column {}", col_type, col_idx)) };
                }
                else
                {
                    // Fallback for custom sqlgen wrapper/domain types (e.g. Timestamp, Date, Varchar, rfl::Validator).
                    if (col_type == SQLITE_BLOB)
                    {
                        return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation(
                            std::format("Incompatible SQLite BLOB storage class for column {}", col_idx)) };
                    }
                    auto text_res = read_column_text_exact(db, stmt, col_idx);
                    if (!text_res)
                    {
                        return std::unexpected<Core::DbError>{ text_res.error() };
                    }
                    const std::size_t raw_size = text_res->size();
                    auto parsed = sqlgen::parsing::Parser<CleanT>::read(std::optional<std::string>{ std::move(*text_res) });
                    if (!parsed)
                    {
                        return std::unexpected<Core::DbError>{ Core::DbError::constraint_violation(
                            std::format("Failed to decode column {} ({} bytes) into target type", col_idx, raw_size)) };
                    }
                    return std::move(*parsed);
                }
            }
        }

        template<typename RowT>
        [[nodiscard]] std::expected<RowT, Core::DbError> decode_row_at(sqlite3* db, sqlite3_stmt* stmt, int start_col)
        {
            constexpr std::size_t expected_cols = DAL::Schema::members_v<RowT>.size();
            const int col_count = sqlite3_column_count(stmt);
            if (start_col < 0 || (start_col + static_cast<int>(expected_cols)) > col_count)
            {
                return std::unexpected<Core::DbError>{ Core::DbError::unknown(
                    std::format("Expected at least {} columns starting at {}, got {}", expected_cols, start_col, col_count)) };
            }

            RowT row{};
            std::optional<Core::DbError> err;
            int col_idx = start_col;
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
            return decode_row_at<RowT>(db, stmt, 0);
        }

        [[nodiscard]] inline bool column_name_matches_member(std::string_view sql_col, std::string_view member_name) noexcept
        {
            auto norm = [](std::string_view s) -> std::string
            {
                if (s.size() > 2 && (s.starts_with("m_") || s.starts_with("M_")))
                {
                    s.remove_prefix(2);
                }
                if (s.size() > 1 && s.ends_with('_'))
                {
                    s.remove_suffix(1);
                }
                std::string out;
                out.reserve(s.size());
                for (const char ch : s)
                {
                    if (ch == '_')
                    {
                        continue;
                    }
                    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
                }
                return out;
            };
            if (sql_col == member_name)
            {
                return true;
            }
            return norm(sql_col) == norm(member_name);
        }

        template<typename RowT>
        [[nodiscard]] std::expected<RowT, Core::DbError> decode_row_by_name(sqlite3* db, sqlite3_stmt* stmt)
        {
            static constexpr auto& members = DAL::Schema::members_v<RowT>;
            constexpr std::size_t member_count = members.size();
            const int col_count = sqlite3_column_count(stmt);
            std::vector<std::string_view> col_names;
            col_names.reserve(static_cast<std::size_t>(col_count));
            for (int c = 0; c < col_count; ++c)
            {
                const char* raw = sqlite3_column_name(stmt, c);
                col_names.emplace_back(raw != nullptr ? raw : "");
            }

            // Build column-index map for each reflected member (exact -> normalized match -> positional fallback).
            std::array<int, member_count> mapped_indices{};
            mapped_indices.fill(-1);
            bool all_named_matched = true;
            std::size_t m_idx = 0;
            template for (constexpr auto m : members)
            {
                const std::string_view m_name = std::meta::identifier_of(m);
                int hit = -1;
                for (int c = 0; c < col_count; ++c)
                {
                    if (col_names[static_cast<std::size_t>(c)] == m_name)
                    {
                        hit = c;
                        break;
                    }
                }
                if (hit < 0)
                {
                    for (int c = 0; c < col_count; ++c)
                    {
                        if (column_name_matches_member(col_names[static_cast<std::size_t>(c)], m_name))
                        {
                            hit = c;
                            break;
                        }
                    }
                }
                if (hit < 0)
                {
                    all_named_matched = false;
                }
                mapped_indices[m_idx++] = hit;
            }

            if (!all_named_matched)
            {
                if (col_count == static_cast<int>(member_count))
                {
                    return decode_row_at<RowT>(db, stmt, 0);
                }
                return std::unexpected<Core::DbError>{ Core::DbError::unknown(
                    std::format("Could not map {} result columns to target struct ({} fields)", col_count, member_count)) };
            }

            RowT row{};
            std::optional<Core::DbError> err;
            std::size_t idx = 0;
            template for (constexpr auto m : members)
            {
                if (!err)
                {
                    using RawMemberT = [:std::meta::type_of(m):];
                    auto val = decode_column<RawMemberT>(db, stmt, mapped_indices[idx]);
                    if (!val)
                    {
                        err = std::move(val.error());
                    }
                    else
                    {
                        row.[:m:] = std::move(*val);
                    }
                }
                ++idx;
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
