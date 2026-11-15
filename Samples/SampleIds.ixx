export module samples:ids;

import core;

export namespace Core
{
    using PersonId = Id<struct PersonTag>;
    using SomethingId = Id<struct SomethingTag>;
} // namespace Core
