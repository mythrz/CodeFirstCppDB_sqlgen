export module core:db_error;

import std;

export namespace Core {

    enum class DbErrorCode {
        Unknown,
        NotFound,
        DuplicateKey,
        ConnectionError,
        ConstraintViolation
    };

    struct DbError {
        DbErrorCode code;
        std::string message;

        [[nodiscard]] std::string to_string() const {
            return std::format("Error [{}]: {}", static_cast<int>(code), message);
        }
    };

} // namespace Core
