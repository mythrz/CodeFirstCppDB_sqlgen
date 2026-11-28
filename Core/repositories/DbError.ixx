export module codefirst.core:db_error;

import std;

export namespace Core
{
    enum class DbErrorCode
    {
        Unknown,
        NotFound,
        DuplicateKey,
        ConnectionError,
        ConstraintViolation,
        ConcurrencyConflict
    };

    struct DbError
    {
        DbErrorCode code{ DbErrorCode::Unknown };
        std::string message;
        int native_code{ 0 };
        std::string trace{};

        [[nodiscard]] static std::string capture_debug_trace();
        [[nodiscard]] static bool is_stacktrace_enabled() noexcept;

        [[nodiscard]] static DbError create(DbErrorCode err_code, std::string msg, int sql_native_code = 0)
        {
            return DbError{ err_code, std::move(msg), sql_native_code, capture_debug_trace() };
        }

        [[nodiscard]] std::string_view code_name() const noexcept
        {
            switch (code)
            {
            case DbErrorCode::Unknown:
                return "Unknown";
            case DbErrorCode::NotFound:
                return "NotFound";
            case DbErrorCode::DuplicateKey:
                return "DuplicateKey";
            case DbErrorCode::ConnectionError:
                return "ConnectionError";
            case DbErrorCode::ConstraintViolation:
                return "ConstraintViolation";
            case DbErrorCode::ConcurrencyConflict:
                return "ConcurrencyConflict";
            }
            return "Unknown";
        }

        [[nodiscard]] std::string to_string() const
        {
            std::string base;
            if (native_code != 0)
            {
                base = std::format("Error [{}: {}, native={}]: {}", static_cast<int>(code), code_name(), native_code, message);
            }
            else
            {
                base = std::format("Error [{}: {}]: {}", static_cast<int>(code), code_name(), message);
            }
            if (!trace.empty())
            {
                base += "\nStacktrace:\n";
                base += trace;
            }
            return base;
        }

        // Factory methods
        [[nodiscard]] static DbError not_found(std::string msg = "Record not found")
        {
            return create(DbErrorCode::NotFound, std::move(msg));
        }

        [[nodiscard]] static DbError duplicate_key(std::string msg = "Duplicate key violation")
        {
            return create(DbErrorCode::DuplicateKey, std::move(msg));
        }

        [[nodiscard]] static DbError constraint_violation(std::string msg = "Constraint violation")
        {
            return create(DbErrorCode::ConstraintViolation, std::move(msg));
        }

        [[nodiscard]] static DbError connection_error(std::string msg = "Database connection error")
        {
            return create(DbErrorCode::ConnectionError, std::move(msg));
        }

        [[nodiscard]] static DbError concurrency_conflict(std::string msg = "Optimistic concurrency conflict")
        {
            return create(DbErrorCode::ConcurrencyConflict, std::move(msg));
        }

        [[nodiscard]] static DbError unknown(std::string msg = "Unknown database error")
        {
            return create(DbErrorCode::Unknown, std::move(msg));
        }

        // Diagnostic parser mapping database/driver messages to structured codes
        [[nodiscard]] static DbError map_from_message(std::string_view msg)
        {
            std::string lower_msg(msg);
            std::transform(
                lower_msg.begin(),
                lower_msg.end(),
                lower_msg.begin(),
                [](unsigned char c)
                {
                    return std::tolower(c);
                });

            if (lower_msg.find("unique") != std::string::npos || lower_msg.find("duplicate") != std::string::npos)
            {
                return create(DbErrorCode::DuplicateKey, std::string(msg));
            }
            if (lower_msg.find("foreign key") != std::string::npos || lower_msg.find("constraint") != std::string::npos)
            {
                return create(DbErrorCode::ConstraintViolation, std::string(msg));
            }
            if (lower_msg.find("not found") != std::string::npos)
            {
                return create(DbErrorCode::NotFound, std::string(msg));
            }
            if (lower_msg.find("connection") != std::string::npos || lower_msg.find("busy") != std::string::npos ||
                lower_msg.find("locked") != std::string::npos || lower_msg.find("closed") != std::string::npos)
            {
                return create(DbErrorCode::ConnectionError, std::string(msg));
            }
            return create(DbErrorCode::Unknown, std::string(msg));
        }
    };

} // namespace Core

template<>
struct std::formatter<Core::DbErrorCode> : std::formatter<std::string_view>
{
    template<typename FormatContext>
    auto format(Core::DbErrorCode code, FormatContext& ctx) const
    {
        std::string_view name = "Unknown";
        switch (code)
        {
        case Core::DbErrorCode::Unknown:
            name = "Unknown";
            break;
        case Core::DbErrorCode::NotFound:
            name = "NotFound";
            break;
        case Core::DbErrorCode::DuplicateKey:
            name = "DuplicateKey";
            break;
        case Core::DbErrorCode::ConnectionError:
            name = "ConnectionError";
            break;
        case Core::DbErrorCode::ConstraintViolation:
            name = "ConstraintViolation";
            break;
        case Core::DbErrorCode::ConcurrencyConflict:
            name = "ConcurrencyConflict";
            break;
        }
        return std::formatter<std::string_view>::format(name, ctx);
    }
};

template<>
struct std::formatter<Core::DbError> : std::formatter<std::string>
{
    template<typename FormatContext>
    auto format(const Core::DbError& err, FormatContext& ctx) const
    {
        return std::formatter<std::string>::format(err.to_string(), ctx);
    }
};
