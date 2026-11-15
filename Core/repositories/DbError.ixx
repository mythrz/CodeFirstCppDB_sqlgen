export module core:db_error;

import std;

export namespace Core
{
    enum class DbErrorCode
    {
        Unknown,
        NotFound,
        DuplicateKey,
        ConnectionError,
        ConstraintViolation
    };

    struct DbError
    {
        DbErrorCode code{ DbErrorCode::Unknown };
        std::string message;
        int native_code{ 0 };
        std::string trace{};

        [[nodiscard]] static std::string capture_debug_trace()
        {
#ifndef NDEBUG
            const auto st = std::stacktrace::current(2, 8);
            return st.empty() ? std::string{} : std::to_string(st);
#else
            return {};
#endif
        }

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
                return DbError{ DbErrorCode::DuplicateKey, std::string(msg) };
            }
            if (lower_msg.find("foreign key") != std::string::npos || lower_msg.find("constraint") != std::string::npos)
            {
                return DbError{ DbErrorCode::ConstraintViolation, std::string(msg) };
            }
            if (lower_msg.find("not found") != std::string::npos)
            {
                return DbError{ DbErrorCode::NotFound, std::string(msg) };
            }
            if (lower_msg.find("connection") != std::string::npos || lower_msg.find("busy") != std::string::npos ||
                lower_msg.find("locked") != std::string::npos || lower_msg.find("closed") != std::string::npos)
            {
                return DbError{ DbErrorCode::ConnectionError, std::string(msg) };
            }
            return DbError{ DbErrorCode::Unknown, std::string(msg) };
        }
    };

} // namespace Core
