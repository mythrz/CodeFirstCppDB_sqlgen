export module codefirst.dal;

export import :schema_traits;
export import :entity_metadata;
export import :mappers;
export import :generic_repo;
export import :sqlite_statement;
export import :sqlite_connection;
export import :change_tracker;
export import :db_query;
export import :db_set;
export import :db_context;
export import :connection_pool;

static_assert(DAL::Repositories::SavepointConnection<DAL::SQLiteConnection>, "SQLiteConnection must satisfy SavepointConnection");
