export module codefirst.samples:ids;

import codefirst.core;

export namespace Core
{
    using PersonId = Id<struct PersonTag>;
    using SomethingId = Id<struct SomethingTag>;
} // namespace Core
