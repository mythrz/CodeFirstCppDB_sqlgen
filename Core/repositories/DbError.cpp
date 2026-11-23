module core;

import std;

namespace Core
{
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
