module codefirst.core;

import std;

namespace Core
{
    bool DbError::is_stacktrace_enabled() noexcept
    {
#if defined(CODEFIRST_ENABLE_STACKTRACE) && !defined(NDEBUG)
        return true;
#else
        return false;
#endif
    }

    std::string DbError::capture_debug_trace()
    {
#if defined(CODEFIRST_ENABLE_STACKTRACE) && !defined(NDEBUG)
        const auto st = std::stacktrace::current(2, 8);
        return st.empty() ? std::string{} : std::to_string(st);
#else
        return {};
#endif
    }
} // namespace Core
