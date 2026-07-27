# Implementing an `ATTACH`-able data source in a DuckDB C++ extension (DuckDB v1.5.4)

> **VERSION SCOPE — read before trusting a line number.**
>
> This document was researched against DuckDB **v1.5.4** (`08e34c447b`), which was
> the pin at the time. The project now targets **v1.5.5** on the default line and
> **v1.4.5** on the LTS line; `git ls-tree HEAD duckdb` is the live pin.
>
> It is kept at v1.5.4 deliberately rather than rewritten. Every fact here was read
> from real headers at that tag, and the API facts have held: the `ExtensionLoader` /
> `DUCKDB_CPP_EXTENSION_ENTRY` entrypoint, the removal of `ExtensionUtil`, and the
> `LookupSchema`/`LookupEntry` pure virtuals are all unchanged in v1.5.5. Rewriting
> the citations to a new tag would cost the one thing that makes this useful — that
> every claim was verified against a specific, checkable revision.
>
> What that means in practice: trust the **API shapes**, re-check **`file:line`
> citations** against the tag you are actually building. The one API that genuinely
> differs between the 1.4 and 1.5 lines is storage-extension registration, and
> `CMakeLists.txt` handles it by probing the header rather than by version string —
> `make check-api-compat` proves both branches compile.


**Target goal**

```sql
LOAD vtk;
ATTACH 'mesh.vtu' AS m (TYPE vtk);
SELECT * FROM m.points;
SELECT * FROM m.cells;
```

## Provenance of every citation in this document

| Source | Resolved revision | How it was obtained |
| --- | --- | --- |
| **duckdb** (authoritative) | tag `v1.5.4` = commit **`08e34c447bae34eaee3723cac61f2878b6bdf787`** | individual files fetched from `https://raw.githubusercontent.com/duckdb/duckdb/v1.5.4/<path>` |
| duckdb-sqlite (example) | `4f8506b0a0300fc158c88f718b62765b9306dfaa` (2026-07-12) | `git clone --depth 1` |
| duckdb-delta (example) | `e74cf88ebf64a6638de443025510d8269c31616a` (2026-07-13) | `git clone --depth 1` |
| duckdb-postgres (example) | `4daa46502e330591bea85a0ccf0e5745edbbd35f` (2026-07-25) | `git clone --depth 1` |

Tag verification: `git ls-remote --tags` shows `v1.5.0 … v1.5.5`; `v1.5.4` exists and resolves to `08e34c44…`. All three example extensions track duckdb **main**, which is *ahead* of v1.5.4. **Where they disagree with the v1.5.4 headers, the headers win**; every such disagreement is recorded in [§9 Drift](#9-drift-example-extensions-vs-v154) and [§11 Uncertainties](#11-uncertainties--must-verify-by-compiling).

`file:line` citations of the form `src/...:N` are line numbers **in the v1.5.4 checkout**. Citations prefixed `duckdb-sqlite/`, `duckdb-delta/`, `duckdb-postgres/` are from those repos at the commits above.

---

## Table of contents

1. [The `ATTACH` code path](#1-the-attach-code-path)
2. [The exact classes to subclass](#2-the-exact-classes-to-subclass)
   - [2.1 `StorageExtension` + callback typedefs](#21-storageextension--callback-typedefs)
   - [2.2 `Catalog` — 13 pure virtuals](#22-catalog--13-pure-virtuals)
   - [2.3 `SchemaCatalogEntry` — 15 pure virtuals](#23-schemacatalogentry--15-pure-virtuals)
   - [2.4 `TableCatalogEntry` — 3 pure virtuals](#24-tablecatalogentry--3-pure-virtuals)
   - [2.5 `TransactionManager` — 4 pure virtuals](#25-transactionmanager--4-pure-virtuals)
   - [2.6 `Transaction`](#26-transaction)
3. [The minimal read-only skeleton (complete code)](#3-the-minimal-read-only-skeleton)
4. [How the table scan works](#4-how-the-table-scan-works)
5. [Schema discovery & lifetime](#5-schema-discovery--lifetime)
6. [`information_schema` / `duckdb_tables()` integration](#6-information_schema--duckdb_tables-integration)
7. [Registration](#7-registration)
8. [Fallback plan: plain table functions](#8-fallback-plan-plain-table-functions)
9. [Drift: example extensions vs v1.5.4](#9-drift-example-extensions-vs-v154)
10. [Recommended build-out order](#10-recommended-build-out-order)
11. [Uncertainties / must verify by compiling](#11-uncertainties--must-verify-by-compiling)

---

## 1. The `ATTACH` code path

Brief trace, bottom of the funnel first (that is where our code lives).

### 1.1 Physical operator

`src/execution/operator/schema/physical_attach.cpp:16-38` — verbatim:

```cpp
SourceResultType PhysicalAttach::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                 OperatorSourceInput &input) const {
	// parse the options
	auto &config = DBConfig::GetConfig(context.client);
	// construct the options
	AttachOptions options(info->options, config.options.access_mode);

	// get the name and path of the database
	auto &name = info->name;
	auto &path = info->path;
	if (options.db_type.empty()) {
		DBPathAndType::ExtractExtensionPrefix(path, options.db_type);
	}
	if (name.empty()) {
		auto &fs = FileSystem::GetFileSystem(context.client);
		name = AttachedDatabase::ExtractDatabaseName(path, fs);
	}

	// check ATTACH IF NOT EXISTS
	auto &db_manager = DatabaseManager::Get(context.client);
	db_manager.AttachDatabase(context.client, *info, options);
	return SourceResultType::FINISHED;
}
```

Notes:
- `AttachOptions options(info->options, ...)` — `info->options` is the **already-bound** `unordered_map<string, Value>` from the parser. The `AttachOptions` constructor consumes the well-known keys and leaves the rest in `options.options` (see §1.3).
- If `TYPE` is absent, `DBPathAndType::ExtractExtensionPrefix` lets `ATTACH 'vtk:mesh.vtu'` work as a synonym for `(TYPE vtk)` (`src/main/database_path_and_type.cpp:10-20`).

### 1.2 Type → `StorageExtension` resolution

`DatabaseManager::AttachDatabase` first ensures the extension is loaded (`src/main/database_manager.cpp:374-397`, verbatim):

```cpp
	// Try to extract the database type from the path.
	if (options.db_type.empty()) {
		auto &fs = FileSystem::GetFileSystem(context);
		DBPathAndType::CheckMagicBytes(context, fs, info.path, options.db_type);
	}

	if (options.db_type.empty()) {
		return;
	}

	auto extension_name = ExtensionHelper::ApplyExtensionAlias(options.db_type);
	if (StorageExtension::Find(config, extension_name)) {
		// If the database type is already registered, we don't need to load it again.
		return;
	}

	// If we are loading a database type from an extension, then we need to check if that extension is loaded.
	if (!Catalog::TryAutoLoad(context, options.db_type)) {
		ExtensionHelper::LoadExternalExtension(context, options.db_type);
	}
```

It then calls `db.CreateAttachedDatabase(...)` — `src/main/database.cpp:173-200`, verbatim:

```cpp
shared_ptr<AttachedDatabase> DatabaseInstance::CreateAttachedDatabase(ClientContext &context, AttachInfo &info,
                                                                      AttachOptions &options) {
	shared_ptr<AttachedDatabase> attached_database;
	auto &catalog = Catalog::GetSystemCatalog(*this);

	if (!options.db_type.empty()) {
		// Find the storage extension for this database file.
		auto extension_name = ExtensionHelper::ApplyExtensionAlias(options.db_type);
		auto storage_extension = StorageExtension::Find(config, extension_name);
		if (!storage_extension) {
			throw BinderException("Unrecognized storage type \"%s\"", options.db_type);
		}

		if (storage_extension->attach != nullptr && storage_extension->create_transaction_manager != nullptr) {
			// Use the storage extension to create the initial database.
			attached_database = make_shared_ptr<AttachedDatabase>(*this, catalog, *storage_extension, context,
			                                                      info.name, info, options);
			return attached_database;
		}
		...
```

**Case-insensitivity of `TYPE`.** Three separate mechanisms make `(TYPE VTK)`, `(TYPE vtk)`, `(TYPE 'Vtk')` all work:

1. `AttachOptions` lower-cases the value: `db_type = StringUtil::Lower(StringValue::Get(...))` (`src/main/attached_database.cpp:75`).
2. `ExtensionHelper::ApplyExtensionAlias` lower-cases again and returns `lname` (`src/main/extension/extension_alias.cpp:29-37`).
3. The registry map is `case_insensitive_map_t<shared_ptr<StorageExtension>>` (`src/main/extension_callback_manager.cpp:21`).

`ApplyExtensionAlias` also applies a *hard-coded* alias table (`src/main/extension/extension_alias.cpp:5-15`) — `sqlite → sqlite_scanner`, `postgres → postgres_scanner`, etc. **There is no `vtk` alias**, so we must register under exactly the name the user types: register under `"vtk"` and `ATTACH … (TYPE vtk)` matches directly. (This is why duckdb-sqlite registers under `"sqlite_scanner"`, not `"sqlite"` — do **not** copy that detail.)

### 1.3 `AttachedDatabase` calls our two callbacks

`src/main/attached_database.cpp:150-180`, verbatim (the heart of the mechanism):

```cpp
AttachedDatabase::AttachedDatabase(DatabaseInstance &db, Catalog &catalog_p, StorageExtension &storage_extension_p,
                                   ClientContext &context, string name_p, AttachInfo &info, AttachOptions &options)
    : CatalogEntry(CatalogType::DATABASE_ENTRY, catalog_p, std::move(name_p)), db(db), parent_catalog(&catalog_p),
      storage_extension(&storage_extension_p), close_lock(make_shared_ptr<mutex>()) {
	if (options.access_mode == AccessMode::READ_ONLY) {
		type = AttachedDatabaseType::READ_ONLY_DATABASE;
	} else {
		type = AttachedDatabaseType::READ_WRITE_DATABASE;
	}
	recovery_mode = options.recovery_mode;
	visibility = options.visibility;
	vacuum_rebuild_threshold = options.vacuum_rebuild_indexes_threshold;

	optional_ptr<StorageExtensionInfo> storage_info = storage_extension->storage_info.get();
	catalog = storage_extension->attach(storage_info, context, *this, name, info, options);
	stored_database_path = std::move(options.stored_database_path);
	if (!catalog) {
		throw InternalException("AttachedDatabase - attach function did not return a catalog");
	}
	if (catalog->IsDuckCatalog()) {
		// The attached database uses the DuckCatalog.
		storage = make_uniq<SingleFileStorageManager>(*this, info.path, options);
	}
	transaction_manager = storage_extension->create_transaction_manager(storage_info, *this, *catalog);
	if (!transaction_manager) {
		throw InternalException(
		    "AttachedDatabase - create_transaction_manager function did not return a transaction manager");
	}
	attach_options = options.options;
	internal = true;
}
```

Because our catalog returns `IsDuckCatalog() == false` (the base default, `src/include/duckdb/catalog/catalog.hpp:112-114`), **no `StorageManager` is created** — nothing tries to read a duckdb block header from `mesh.vtu`. That is exactly what we want.

Immediately afterwards `DatabaseManager::AttachDatabase` calls (`src/main/database_manager.cpp:205-216`):

```cpp
	auto attached_db = db.CreateAttachedDatabase(context, info, options);

	//! Initialize the database.
	if (options.is_main_database) {
		attached_db->SetInitialDatabase();
		attached_db->Initialize(context);
	} else {
		attached_db->Initialize(context);
		if (!options.default_table.name.empty()) {
			attached_db->GetCatalog().SetDefaultTable(options.default_table.schema, options.default_table.name);
		}
		attached_db->FinalizeLoad(context);
```

and `AttachedDatabase::Initialize` forwards the `ClientContext` into the catalog (`src/main/attached_database.cpp:245-254`):

```cpp
void AttachedDatabase::Initialize(optional_ptr<ClientContext> context) {
	if (IsSystem()) {
		catalog->Initialize(context, true);
	} else {
		catalog->Initialize(context, false);
	}
	if (storage) {
		storage->Initialize(context);
	}
}
```

So there are **two** context-bearing hooks after construction: `Catalog::Initialize(optional_ptr<ClientContext>, bool)` and `Catalog::FinalizeLoad(optional_ptr<ClientContext>)`, both non-pure (`catalog.hpp:117-118`, defaults at `src/catalog/catalog.cpp:1286-1291`).

### 1.4 How ATTACH options reach us

`AttachInfo` — `src/include/duckdb/parser/parsed_data/attach_info.hpp:19-45`, verbatim:

```cpp
struct AttachInfo : public ParseInfo {
public:
	static constexpr const ParseInfoType TYPE = ParseInfoType::ATTACH_INFO;

public:
	AttachInfo() : ParseInfo(TYPE) {
	}

	//! The alias of the attached database
	string name;
	//! The path to the attached database
	string path;
	//! Set of (key, value) options
	case_insensitive_map_t<unique_ptr<ParsedExpression>> parsed_options;
	//! Set of bound (key, value) options
	unordered_map<string, Value> options;
	//! What to do on create conflict
	OnCreateConflict on_conflict = OnCreateConflict::ERROR_ON_CONFLICT;
	...
```

`AttachOptions` — `src/include/duckdb/main/attached_database.hpp:56-82`, verbatim:

```cpp
//! AttachOptions holds information about a database we plan to attach. These options are generalized, i.e.,
//! they have to apply to any database file type (duckdb, sqlite, etc.).
struct AttachOptions {
	//! Constructor for databases we attach outside of the ATTACH DATABASE statement.
	explicit AttachOptions(const DBConfigOptions &options);
	//! Constructor for databases we attach when using ATTACH DATABASE.
	AttachOptions(const unordered_map<string, Value> &options, const AccessMode default_access_mode);

	//! Defaults to the access mode configured in the DBConfig, unless specified otherwise.
	AccessMode access_mode;
	//! The recovery type of the database.
	RecoveryMode recovery_mode = RecoveryMode::DEFAULT;
	//! The file format type. The default type is a duckdb database file, but other file formats are possible.
	string db_type;
	//! Set of remaining (key, value) options
	unordered_map<string, Value> options;
	//! (optionally) a catalog can be provided with a default table
	QualifiedName default_table;
	//! Whether this is the main database.
	bool is_main_database = false;
	//! The visibility of the attached database
	AttachVisibility visibility = AttachVisibility::SHOWN;
	//! The stored database path (in the path manager)
	unique_ptr<StoredDatabasePath> stored_database_path;
	//! Per-database override of vacuum_rebuild_indexes. If not set, the global setting value is used.
	optional_idx vacuum_rebuild_indexes_threshold;
};
```

The constructor at `src/main/attached_database.cpp:39-105` consumes and removes these keys:

| Key(s) | Lands in |
| --- | --- |
| `readonly`, `read_only` | `access_mode = AccessMode::READ_ONLY` |
| `readwrite`, `read_write` | `access_mode` |
| `recovery_mode` | `recovery_mode` |
| `type` | `db_type` (lower-cased) |
| `default_table` | `default_table` (via `QualifiedName::Parse`) |
| `hidden` | `visibility` |
| `vacuum_rebuild_indexes` | `vacuum_rebuild_indexes_threshold` |
| **anything else** | `options.emplace(entry.first, entry.second)` → `AttachOptions::options` |

So for `ATTACH 'mesh.vtu' AS m (TYPE vtk, READ_ONLY, custom_option 'x')`:
- `options.db_type == "vtk"`
- `options.access_mode == AccessMode::READ_ONLY`
- `options.options == { "custom_option": Value("x") }`
- `info.path == "mesh.vtu"`, `info.name == "m"`
- `info.options` still contains **all** of them (it is the untouched source map) — this is why duckdb-delta iterates `info.options` while duckdb-sqlite iterates `attach_options.options`. **Prefer `AttachOptions::options`**: iterating `info.options` means you must skip `type`/`read_only`/etc. yourself or you will throw on them.

`AttachInfo::options` keys are lower-cased by the binder in practice (the well-known comparisons above are exact-match against lower-case), but the safe idiom used by all three extensions is `StringUtil::CIEquals(entry.first, "my_option")`.

Also available later: `AttachedDatabase::GetAttachOptions()` returns `const unordered_map<string, Value> &` (`attached_database.hpp:139-141`) — the leftover options are retained on the attached database (`attach_options = options.options;`).

---

## 2. The exact classes to subclass

### 2.1 `StorageExtension` + callback typedefs

`src/include/duckdb/storage/storage_extension.hpp` **in full** (it is only 57 lines and every line matters):

```cpp
//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/storage_extension.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/storage/storage_manager.hpp"

namespace duckdb {
class AttachedDatabase;
struct AttachInfo;
class Catalog;
class TransactionManager;

//! The StorageExtensionInfo holds static information relevant to the storage extension
struct StorageExtensionInfo {
	virtual ~StorageExtensionInfo() {
	}
};

typedef unique_ptr<Catalog> (*attach_function_t)(optional_ptr<StorageExtensionInfo> storage_info,
                                                 ClientContext &context, AttachedDatabase &db, const string &name,
                                                 AttachInfo &info, AttachOptions &options);
typedef unique_ptr<TransactionManager> (*create_transaction_manager_t)(optional_ptr<StorageExtensionInfo> storage_info,
                                                                       AttachedDatabase &db, Catalog &catalog);

class StorageExtension {
public:
	attach_function_t attach;
	create_transaction_manager_t create_transaction_manager;

	//! Additional info passed to the various storage functions
	shared_ptr<StorageExtensionInfo> storage_info;

	virtual ~StorageExtension() {
	}

	virtual void OnCheckpointStart(AttachedDatabase &db, CheckpointOptions checkpoint_options) {
	}

	virtual void OnCheckpointEnd(AttachedDatabase &db, CheckpointOptions checkpoint_options) {
	}

	static optional_ptr<StorageExtension> Find(const DBConfig &config, const string &extension_name);
	static void Register(DBConfig &config, const string &extension_name, shared_ptr<StorageExtension> extension);
};

struct OpenFileStorageExtension {
	static shared_ptr<StorageExtension> Create();
};

} // namespace duckdb
```

Key facts:
- `attach` and `create_transaction_manager` are **raw C function pointers**, not virtuals. You subclass `StorageExtension` only to assign them in the constructor.
- Neither is initialised by the base class — assign both, or `DatabaseInstance::CreateAttachedDatabase` silently falls through to the *duckdb-file* path (`src/main/database.cpp:186`).
- `storage_info` is a `shared_ptr<StorageExtensionInfo>` (was `unique_ptr` in older versions). Optional; leave null.
- `OnCheckpointStart` / `OnCheckpointEnd` are optional no-op virtuals.
- **Registration is via the static `Register`, not a config map.** See §7.

There is **no `DBConfig::storage_extensions` member in v1.5.4.** Grepping `src/include/duckdb/main/config.hpp` for `extension` yields only `load_extensions`, `extension_directories`, `ResetOption(const ExtensionOption&)`, `RegisterArrowExtension`, `arrow_extensions` — plus the forward declaration `class StorageExtension;` at `config.hpp:52`. The registry lives behind `ExtensionCallbackManager`:

`src/main/extension_callback_manager.cpp:21` (inside `struct ExtensionCallbackRegistry`):
```cpp
	case_insensitive_map_t<shared_ptr<StorageExtension>> storage_extensions;
```

`src/main/extension_callback_manager.cpp:155-166`, verbatim:
```cpp
optional_ptr<StorageExtension> StorageExtension::Find(const DBConfig &config, const string &extension_name) {
	return config.GetCallbackManager().FindStorageExtension(extension_name);
}

void ExtensionCallback::Register(DBConfig &config, shared_ptr<ExtensionCallback> extension) {
	config.GetCallbackManager().Register(std::move(extension));
}

void StorageExtension::Register(DBConfig &config, const string &extension_name,
                                shared_ptr<StorageExtension> extension) {
	config.GetCallbackManager().Register(extension_name, std::move(extension));
}
```

`ExtensionCallbackManager::Register` / `FindStorageExtension` are declared at `src/include/duckdb/main/extension_callback_manager.hpp:44` and `:52`:
```cpp
	void Register(const string &name, shared_ptr<StorageExtension> extension);
	...
	optional_ptr<StorageExtension> FindStorageExtension(const string &name) const;
```

> **Any tutorial or older extension doing `config.storage_extensions["x"] = make_uniq<...>()` will not compile against v1.5.4.** duckdb-sqlite/delta at main already use the new form, so they are correct here.

### 2.2 `Catalog` — 13 pure virtuals

From `src/include/duckdb/catalog/catalog.hpp`. Verbatim, with line numbers:

```cpp
// catalog.hpp:116
	virtual void Initialize(bool load_builtin) = 0;

// catalog.hpp:134
	DUCKDB_API virtual string GetCatalogType() = 0;

// catalog.hpp:139-140
	DUCKDB_API virtual optional_ptr<CatalogEntry> CreateSchema(CatalogTransaction transaction,
	                                                           CreateSchemaInfo &info) = 0;

// catalog.hpp:221-223
	DUCKDB_API virtual optional_ptr<SchemaCatalogEntry> LookupSchema(CatalogTransaction transaction,
	                                                                 const EntryLookupInfo &schema_lookup,
	                                                                 OnEntryNotFound if_not_found) = 0;

// catalog.hpp:253
	DUCKDB_API virtual void ScanSchemas(ClientContext &context, std::function<void(SchemaCatalogEntry &)> callback) = 0;

// catalog.hpp:308-309
	virtual PhysicalOperator &PlanCreateTableAs(ClientContext &context, PhysicalPlanGenerator &planner,
	                                            LogicalCreateTable &op, PhysicalOperator &plan) = 0;

// catalog.hpp:310-311
	virtual PhysicalOperator &PlanInsert(ClientContext &context, PhysicalPlanGenerator &planner, LogicalInsert &op,
	                                     optional_ptr<PhysicalOperator> plan) = 0;

// catalog.hpp:312-313
	virtual PhysicalOperator &PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner, LogicalDelete &op,
	                                     PhysicalOperator &plan) = 0;

// catalog.hpp:315-316
	virtual PhysicalOperator &PlanUpdate(ClientContext &context, PhysicalPlanGenerator &planner, LogicalUpdate &op,
	                                     PhysicalOperator &plan) = 0;

// catalog.hpp:327
	virtual DatabaseSize GetDatabaseSize(ClientContext &context) = 0;

// catalog.hpp:330-331
	virtual bool InMemory() = 0;
	virtual string GetDBPath() = 0;

// catalog.hpp:452  --  NOTE: declared in a `private:` section (catalog.hpp:422)
	virtual void DropSchema(ClientContext &context, DropInfo &info) = 0;
```

That is **13**. `DropSchema` being a *private* pure virtual is legal and intentional — you override it in your derived class (SQLite and Delta both declare their override under `private:`; access control on a virtual is per-declaration and does not affect overriding).

**`LookupSchema`, not `GetSchema`, is the pure virtual.** All the `GetSchema(...)` overloads (`catalog.hpp:226-251`) are concrete wrappers that funnel into `LookupSchema`. There is **no** `OptionalSchemaEntry` type in v1.5.4 — the return type is plain `optional_ptr<SchemaCatalogEntry>`. (Searched `catalog.hpp`, `schema_catalog_entry.hpp`, `entry_lookup_info.hpp`: the identifier does not appear.)

#### Notable *non*-pure virtuals worth overriding

```cpp
// catalog.hpp:112-114 -- leave as false: this is what suppresses SingleFileStorageManager creation
	virtual bool IsDuckCatalog() {
		return false;
	}

// catalog.hpp:117-118 -- context-bearing Initialize; default forwards to Initialize(bool)
	virtual void Initialize(optional_ptr<ClientContext> context, bool load_builtin);
	virtual void FinalizeLoad(optional_ptr<ClientContext> context);

// catalog.hpp:127-129 -- statement-cache invalidation key
	DUCKDB_API virtual optional_idx GetCatalogVersion(ClientContext &context) {
		return {}; // don't return anything by default
	}

// catalog.hpp:314 / :317 / :318-319 -- 3-arg convenience overloads (have base impls)
	virtual PhysicalOperator &PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner, LogicalDelete &op);
	virtual PhysicalOperator &PlanUpdate(ClientContext &context, PhysicalPlanGenerator &planner, LogicalUpdate &op);
	virtual PhysicalOperator &PlanMergeInto(ClientContext &context, PhysicalPlanGenerator &planner,
	                                        LogicalMergeInto &op, PhysicalOperator &plan);

// catalog.hpp:320-325
	virtual unique_ptr<LogicalOperator> BindCreateIndex(Binder &binder, CreateStatement &stmt, TableCatalogEntry &table,
	                                                    unique_ptr<LogicalOperator> plan);
	virtual unique_ptr<LogicalOperator> BindAlterAddIndex(Binder &binder, TableCatalogEntry &table_entry,
	                                                      unique_ptr<LogicalOperator> plan,
	                                                      unique_ptr<CreateIndexInfo> create_info,
	                                                      unique_ptr<AlterTableInfo> alter_info);

// catalog.hpp:328
	virtual vector<MetadataBlockInfo> GetMetadataInfo(ClientContext &context);

// catalog.hpp:332-341
	virtual bool SupportsTimeTravel() const {
		return false;
	}
	virtual bool IsEncrypted() const {
		return string().empty();  // (see header for exact body)
	}
	virtual string GetEncryptionCipher() const {
		return string();
	}
	virtual ErrorData SupportsCreateTable(BoundCreateTableInfo &info);

// catalog.hpp:344-346
	DUCKDB_API virtual CatalogLookupBehavior CatalogTypeLookupRule(CatalogType type) const {
		return CatalogLookupBehavior::STANDARD;
	}

// catalog.hpp:349
	virtual string GetDefaultSchema() const;      // base returns DEFAULT_SCHEMA ("main")

// catalog.hpp:359
	virtual optional_ptr<DependencyManager> GetDependencyManager();   // base returns nullptr

// catalog.hpp:362
	virtual bool HasConflictingAttachOptions(const string &path, const AttachOptions &options);

// catalog.hpp:396 / :407
	virtual void Verify();
	DUCKDB_API virtual void OnDetach(ClientContext &context);

// catalog.hpp:424-425 -- private; the base impl is the generic schema→entry funnel; do NOT override
	virtual CatalogEntryLookup TryLookupEntryInternal(CatalogTransaction transaction, const string &schema,
	                                                  const EntryLookupInfo &lookup_info);
```

Base implementations of interest (`src/catalog/catalog.cpp`):
- `Catalog::Initialize(optional_ptr<ClientContext> context, bool load_builtin)` → `Initialize(load_builtin);` (`:1286-1288`)
- `Catalog::FinalizeLoad` → `{}` (`:1290-1291`)
- `Catalog::GetDefaultSchema()` → `return DEFAULT_SCHEMA;` (`:1253-1255`)
- `Catalog::SetDefaultTable(const string &schema, const string &name)` → sets `default_table`/`default_table_schema` (`:1262-1265`). **Plain `string` args in v1.5.4.**
- `Catalog::HasConflictingAttachOptions` compares `GetDBPath() != path` and the alias-normalised `GetCatalogType()` (`:1296-1302`) — so `GetCatalogType()` **must** return `"vtk"` for repeated-`ATTACH` conflict detection to behave.
- `Catalog::TryLookupEntryInternal` rejects `AT` clauses unless `SupportsTimeTravel()` (`:789-795`), then does `LookupSchema(...)` → `schema->LookupEntry(...)`.

Supporting types:

`src/include/duckdb/catalog/catalog_transaction.hpp:20-37`, verbatim:
```cpp
struct CatalogTransaction {
	CatalogTransaction(Catalog &catalog, ClientContext &context);
	CatalogTransaction(DatabaseInstance &db, transaction_t transaction_id_p, transaction_t start_time_p);

	optional_ptr<DatabaseInstance> db;
	optional_ptr<ClientContext> context;
	optional_ptr<Transaction> transaction;
	transaction_t transaction_id;
	transaction_t start_time;

	bool HasContext() const {
		return context;
	}
	ClientContext &GetContext();

	static CatalogTransaction GetSystemCatalogTransaction(ClientContext &context);
	static CatalogTransaction GetSystemTransaction(DatabaseInstance &db);
};
```

`src/include/duckdb/catalog/entry_lookup_info.hpp:18-51`, verbatim:
```cpp
struct EntryLookupInfo {
public:
	EntryLookupInfo(CatalogType catalog_type, const string &name,
	                QueryErrorContext error_context = QueryErrorContext());
	EntryLookupInfo(CatalogType catalog_type, const string &name, optional_ptr<BoundAtClause> at_clause,
	                QueryErrorContext error_context);
	EntryLookupInfo(const EntryLookupInfo &parent, const string &name);
	EntryLookupInfo(const EntryLookupInfo &parent, optional_ptr<BoundAtClause> at_clause);

public:
	CatalogType GetCatalogType() const;
	const string &GetEntryName() const;
	const QueryErrorContext &GetErrorContext() const;
	const optional_ptr<BoundAtClause> GetAtClause() const;

	static EntryLookupInfo SchemaLookup(const EntryLookupInfo &parent, const string &schema_name);

private:
	CatalogType catalog_type;
	const string &name;                        // <-- REFERENCE, not a copy
	optional_ptr<BoundAtClause> at_clause;
	QueryErrorContext error_context;
};

//! Return value of Catalog::LookupEntry
struct CatalogEntryLookup {
	optional_ptr<SchemaCatalogEntry> schema;
	optional_ptr<CatalogEntry> entry;
	ErrorData error;

	DUCKDB_API bool Found() const {
		return entry;
	}
};
```

⚠️ **`EntryLookupInfo::name` is a `const string &` member.** Never construct one from a temporary (`EntryLookupInfo(type, string("points"))` or `EntryLookupInfo(type, GetName() + "_x")`) — you get a dangling reference. Bind the string to a named local with a lifetime that outlives the lookup.

`OnEntryNotFound` (`src/include/duckdb/common/enums/on_entry_not_found.hpp`):
```cpp
enum class OnEntryNotFound : uint8_t { THROW_EXCEPTION = 0, RETURN_NULL = 1 };
```

`DatabaseSize` (`src/include/duckdb/storage/database_size.hpp:16-23`):
```cpp
struct DatabaseSize {
	idx_t total_blocks = 0;
	idx_t block_size = 0;
	idx_t free_blocks = 0;
	idx_t used_blocks = 0;
	idx_t bytes = 0;
	idx_t wal_size = 0;
};
```

### 2.3 `SchemaCatalogEntry` — 15 pure virtuals

`src/include/duckdb/catalog/catalog_entry/schema_catalog_entry.hpp:42-111` **in full** (verbatim):

```cpp
//! A schema in the catalog
class SchemaCatalogEntry : public InCatalogEntry {
public:
	static constexpr const CatalogType Type = CatalogType::SCHEMA_ENTRY;
	static constexpr const char *Name = "schema";

public:
	SchemaCatalogEntry(Catalog &catalog, CreateSchemaInfo &info);

public:
	unique_ptr<CreateInfo> GetInfo() const override;

	//! Scan the specified catalog set, invoking the callback method for every entry
	virtual void Scan(ClientContext &context, CatalogType type,
	                  const std::function<void(CatalogEntry &)> &callback) = 0;
	//! Scan the specified catalog set, invoking the callback method for every committed entry
	virtual void Scan(CatalogType type, const std::function<void(CatalogEntry &)> &callback) = 0;

	string ToSQL() const override;

	//! Creates an index with the given name in the schema
	virtual optional_ptr<CatalogEntry> CreateIndex(CatalogTransaction transaction, CreateIndexInfo &info,
	                                               TableCatalogEntry &table) = 0;
	optional_ptr<CatalogEntry> CreateIndex(ClientContext &context, CreateIndexInfo &info, TableCatalogEntry &table);
	//! Create a scalar or aggregate function within the given schema
	virtual optional_ptr<CatalogEntry> CreateFunction(CatalogTransaction transaction, CreateFunctionInfo &info) = 0;
	//! Creates a table with the given name in the schema
	virtual optional_ptr<CatalogEntry> CreateTable(CatalogTransaction transaction, BoundCreateTableInfo &info) = 0;
	//! Creates a view with the given name in the schema
	virtual optional_ptr<CatalogEntry> CreateView(CatalogTransaction transaction, CreateViewInfo &info) = 0;
	//! Creates a sequence with the given name in the schema
	virtual optional_ptr<CatalogEntry> CreateSequence(CatalogTransaction transaction, CreateSequenceInfo &info) = 0;
	//! Create a table function within the given schema
	virtual optional_ptr<CatalogEntry> CreateTableFunction(CatalogTransaction transaction,
	                                                       CreateTableFunctionInfo &info) = 0;
	//! Create a copy function within the given schema
	virtual optional_ptr<CatalogEntry> CreateCopyFunction(CatalogTransaction transaction,
	                                                      CreateCopyFunctionInfo &info) = 0;
	//! Create a pragma function within the given schema
	virtual optional_ptr<CatalogEntry> CreatePragmaFunction(CatalogTransaction transaction,
	                                                        CreatePragmaFunctionInfo &info) = 0;
	//! Create a collation within the given schema
	virtual optional_ptr<CatalogEntry> CreateCollation(CatalogTransaction transaction, CreateCollationInfo &info) = 0;
	//! Create a coordiante system within the given schema
	virtual optional_ptr<CatalogEntry> CreateCoordinateSystem(CatalogTransaction transaction,
	                                                          CreateCoordinateSystemInfo &info) {
		throw NotImplementedException("Coordinate systems are not supported in schema '%s'", name);
	}

	//! Create a enum within the given schema
	virtual optional_ptr<CatalogEntry> CreateType(CatalogTransaction transaction, CreateTypeInfo &info) = 0;

	//! Lookup an entry in the schema
	DUCKDB_API virtual optional_ptr<CatalogEntry> LookupEntry(CatalogTransaction transaction,
	                                                          const EntryLookupInfo &lookup_info) = 0;
	DUCKDB_API virtual CatalogSet::EntryLookup LookupEntryDetailed(CatalogTransaction transaction,
	                                                               const EntryLookupInfo &lookup_info);
	DUCKDB_API virtual SimilarCatalogEntry GetSimilarEntry(CatalogTransaction transaction,
	                                                       const EntryLookupInfo &lookup_info);

	DUCKDB_API optional_ptr<CatalogEntry> GetEntry(CatalogTransaction transaction, CatalogType type,
	                                               const string &name);

	//! Drops an entry from the schema
	virtual void DropEntry(ClientContext &context, DropInfo &info) = 0;

	//! Alters a catalog entry
	virtual void Alter(CatalogTransaction transaction, AlterInfo &info) = 0;

	CatalogTransaction GetCatalogTransaction(ClientContext &context);
};
```

Pure virtual roll-call (**15**):
`Scan(ClientContext&,…)`, `Scan(CatalogType,…)`, `CreateIndex`, `CreateFunction`, `CreateTable`, `CreateView`, `CreateSequence`, `CreateTableFunction`, `CreateCopyFunction`, `CreatePragmaFunction`, `CreateCollation`, `CreateType`, `LookupEntry`, `DropEntry`, `Alter`.

**`CreateCoordinateSystem` is NOT pure** — it is new in the v1.5 era and has a throwing default body (`:85-88`). Do not declare it; duckdb-sqlite/delta don't either, and that is compatible.

Concrete base helpers you get for free (`src/catalog/catalog_entry/schema_catalog_entry.cpp`):
- `GetEntry(CatalogTransaction, CatalogType, const string &name)` → builds an `EntryLookupInfo` and calls `LookupEntry` (`:42-46`)
- `LookupEntryDetailed` → wraps `LookupEntry` (`:50-60`, with the comment *"This should not be used, it's only implemented to not put the burden of implementing it on every derived class"*)
- `GetSimilarEntry` → drives `Scan(context, …)` to produce did-you-mean suggestions (`:29-40`) — so implementing `Scan` also gives you good error messages
- `GetInfo()` / `ToSQL()` (`:62-73`)
- `SchemaCatalogEntry(Catalog &catalog, CreateSchemaInfo &info) : InCatalogEntry(CatalogType::SCHEMA_ENTRY, catalog, info.schema)` (`:13-18`) — the schema name comes from `info.schema`, which a default-constructed `CreateSchemaInfo` sets to `DEFAULT_SCHEMA` = `"main"` (`src/include/duckdb/parser/parsed_data/create_info.hpp:26`, `src/include/duckdb/common/constants.hpp:31`).

### 2.4 `TableCatalogEntry` — 3 pure virtuals

From `src/include/duckdb/catalog/catalog_entry/table_catalog_entry.hpp`:

```cpp
// table_catalog_entry.hpp:89
	//! Get statistics of a column (physical or virtual) within the table
	virtual unique_ptr<BaseStatistics> GetStatistics(ClientContext &context, column_t column_id) = 0;

// table_catalog_entry.hpp:100-101
	//! Returns the scan function that can be used to scan the given table
	virtual TableFunction GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) = 0;

// table_catalog_entry.hpp:118
	//! Returns the storage info of this table
	virtual TableStorageInfo GetStorageInfo(ClientContext &context) = 0;
```

That's it — **3**. Everything else is concrete or has a default:

```cpp
// table_catalog_entry.hpp:62 -- the constructor you must chain
	DUCKDB_API TableCatalogEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info);

// table_catalog_entry.hpp:102-103 -- AT-clause-aware overload; base forwards to the 2-arg form
	virtual TableFunction GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data,
	                                      const EntryLookupInfo &lookup_info);

// table_catalog_entry.hpp:67-86 -- concrete column accessors provided by the base
	DUCKDB_API bool HasGeneratedColumns() const;
	DUCKDB_API bool ColumnExists(const string &name) const;
	DUCKDB_API const ColumnDefinition &GetColumn(const string &name) const;
	DUCKDB_API const ColumnDefinition &GetColumn(LogicalIndex idx) const;
	DUCKDB_API vector<LogicalType> GetTypes() const;
	DUCKDB_API const ColumnList &GetColumns() const;
	virtual DataTable &GetStorage();
	DUCKDB_API const vector<unique_ptr<Constraint>> &GetConstraints() const;
	DUCKDB_API string ToSQL() const override;

// table_catalog_entry.hpp:91, :105-107, :115, :120-121, :129, :131 -- optional virtuals
	virtual unique_ptr<BlockingSample> GetSample();
	virtual bool IsDuckTable() const {
		return false;
	}
	virtual vector<ColumnSegmentInfo> GetColumnSegmentInfo(const QueryContext &context);
	virtual void BindUpdateConstraints(Binder &binder, LogicalGet &get, LogicalProjection &proj, LogicalUpdate &update,
	                                   ClientContext &context);
	virtual virtual_column_map_t GetVirtualColumns() const;
	virtual vector<column_t> GetRowIdColumns() const;

// table_catalog_entry.hpp:133-137 -- protected state the base owns
protected:
	//! A list of columns that are part of this table
	ColumnList columns;
	//! A list of constraints that are part of this table
	vector<unique_ptr<Constraint>> constraints;
```

Note there is **no `ColumnCount()`** on `TableCatalogEntry` in v1.5.4. Use `GetColumns().LogicalColumnCount()` (`src/include/duckdb/parser/column_list.hpp:42`) or iterate `columns.Logical()` (`:56`).

The constructor takes the columns *by move* out of the `CreateTableInfo` (`src/catalog/catalog_entry/table_catalog_entry.cpp:26-33`):
```cpp
TableCatalogEntry::TableCatalogEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info)
    : StandardEntry(CatalogType::TABLE_ENTRY, schema, catalog, info.table), columns(std::move(info.columns)),
      constraints(std::move(info.constraints)) {
	this->temporary = info.temporary;
	this->dependencies = info.dependencies;
	this->comment = info.comment;
	this->tags = info.tags;
}
```
— so the table *name* comes from `CreateTableInfo::table` (a plain `string`, `src/include/duckdb/parser/parsed_data/create_table_info.hpp:25`).

Defaults worth knowing (`src/catalog/catalog_entry/table_catalog_entry.cpp`):
- `GetScanFunction(ctx, bind_data, lookup_info)` → `return GetScanFunction(context, bind_data);` (`:240-243`)
- `GetVirtualColumns()` → inserts `COLUMN_IDENTIFIER_ROW_ID → TableColumn("rowid", LogicalType::ROW_TYPE)` (`:374-378`)
- `GetRowIdColumns()` → `{COLUMN_IDENTIFIER_ROW_ID}` (`:380-384`)
- `GetStorage()` → `throw InternalException("Calling GetStorage on a TableCatalogEntry that is not a DuckTableEntry")` (`:258-260`)
- `GetSample()` → `nullptr` (`:70-72`)
- `GetColumnSegmentInfo(const QueryContext &)` → `{}` (`:297-299`)

⚠️ **Override `GetVirtualColumns()` to return an empty map** unless your scan actually produces a `rowid`. The default advertises a virtual `rowid` column that your table function will be asked to materialise (`column_id == COLUMN_IDENTIFIER_ROW_ID`, i.e. `(column_t)-1`). sqlite_scanner handles it by translating to SQLite `ROWID` (`duckdb-sqlite/src/sqlite_scanner.cpp:70`). For a VTK file the cheap correct choice is *either* return `{}` from both `GetVirtualColumns()` and `GetRowIdColumns()`, *or* synthesise a row number in the scan. See §11.

`TableStorageInfo` — `src/include/duckdb/storage/table_storage_info.hpp:38-44`, verbatim:
```cpp
//! Table storage information
class TableStorageInfo {
public:
	//! The (estimated) cardinality of the table
	optional_idx cardinality;
	//! Info of the indexes of a table
	vector<IndexInfo> index_info;
};
```

### 2.5 `TransactionManager` — 4 pure virtuals

`src/include/duckdb/transaction/transaction_manager.hpp:29-68` **in full** (verbatim):

```cpp
//! The Transaction Manager is responsible for creating and managing
//! transactions
class TransactionManager {
public:
	explicit TransactionManager(AttachedDatabase &db);
	virtual ~TransactionManager();

	//! Start a new transaction
	virtual Transaction &StartTransaction(ClientContext &context) = 0;
	//! Commit the given transaction. Returns a non-empty error message on failure.
	virtual ErrorData CommitTransaction(ClientContext &context, Transaction &transaction) = 0;
	//! Rollback the given transaction
	virtual void RollbackTransaction(Transaction &transaction) = 0;

	virtual void Checkpoint(ClientContext &context, bool force = false) = 0;

	static TransactionManager &Get(AttachedDatabase &db);

	virtual bool IsDuckTransactionManager() {
		return false;
	}

	AttachedDatabase &GetDB() {
		return db;
	}

protected:
	//! The attached database
	AttachedDatabase &db;

public:
	template <class TARGET>
	TARGET &Cast() {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<TARGET &>(*this);
	}
	template <class TARGET>
	const TARGET &Cast() const {
		D_ASSERT(dynamic_cast<const TARGET *>(this));
		return reinterpret_cast<const TARGET &>(*this);
	}
};
```

**The minimal read-only implementation.** `StartTransaction` must return a `Transaction &` that outlives the query, so the manager must own the objects. Both worked examples use the identical shape — duckdb-delta/src/storage/delta_transaction_manager.cpp:11-18 and duckdb-sqlite/src/storage/sqlite_transaction_manager.cpp:10-30:

```cpp
Transaction &SQLiteTransactionManager::StartTransaction(ClientContext &context) {
	auto transaction = make_uniq<SQLiteTransaction>(sqlite_catalog, *this, context);
	transaction->Start();
	auto &result = *transaction;
	lock_guard<mutex> l(transaction_lock);
	transactions[result] = std::move(transaction);
	return result;
}

ErrorData SQLiteTransactionManager::CommitTransaction(ClientContext &context, Transaction &transaction) {
	auto &sqlite_transaction = transaction.Cast<SQLiteTransaction>();
	sqlite_transaction.Commit();
	ExtractAndCloseAfterUnlock(transaction);
	return ErrorData();
}

void SQLiteTransactionManager::RollbackTransaction(Transaction &transaction) {
	auto &sqlite_transaction = transaction.Cast<SQLiteTransaction>();
	sqlite_transaction.Rollback();
	ExtractAndCloseAfterUnlock(transaction);
}
```

with the owning container `reference_map_t<Transaction, unique_ptr<SQLiteTransaction>> transactions;` guarded by a `mutex` (`duckdb-sqlite/src/include/storage/sqlite_transaction_manager.hpp:32-33`). For read-only, `Commit`/`Rollback` reduce to "erase from the map"; `Checkpoint` is a no-op.

A subtle-but-important lesson from duckdb-sqlite (`sqlite_transaction_manager.cpp:32-47`): **destroy the transaction object outside the manager's lock**, because the destructor may re-enter code that takes the same lock. The pattern is to `std::move` the `unique_ptr` out under the lock and let it destruct after release. Our VTK transaction destructor is trivial, but the pattern costs nothing to copy.

### 2.6 `Transaction`

`src/include/duckdb/transaction/transaction.hpp:41-83` (verbatim, trimmed to the public surface):

```cpp
class Transaction {
public:
	DUCKDB_API Transaction(TransactionManager &manager, ClientContext &context);
	DUCKDB_API virtual ~Transaction();

	TransactionManager &manager;
	weak_ptr<ClientContext> context;
	//! The current active query for the transaction. Set to MAXIMUM_QUERY_ID if
	//! no query is active.
	atomic<transaction_t> active_query;

public:
	DUCKDB_API static Transaction &Get(ClientContext &context, AttachedDatabase &db);
	DUCKDB_API static Transaction &Get(ClientContext &context, Catalog &catalog);
	//! Returns the transaction for the given context if it has already been started
	DUCKDB_API static optional_ptr<Transaction> TryGet(ClientContext &context, AttachedDatabase &db);

	//! Whether or not the transaction has made any modifications to the database so far
	DUCKDB_API bool IsReadOnly();
	//! Promotes the transaction to a read-write transaction
	DUCKDB_API virtual void SetReadWrite();
	//! Sets the database modifications that are planned to be performed in this transaction
	DUCKDB_API virtual void SetModifications(DatabaseModificationType type);

	virtual bool IsDuckTransaction() const {
		return false;
	}
	...
```

**Do you need a `Transaction` subclass?** Yes, minimally — you need *something* to hand back from `StartTransaction`, and you want a `static Get(ClientContext&, Catalog&)` helper. It has no pure virtuals, so a subclass with just a constructor is legal. Note `context` is a `weak_ptr<ClientContext>` — lock it before use (`duckdb-sqlite/src/storage/sqlite_transaction.cpp:107-110` shows the check-and-throw idiom).

Whether the transaction or the schema entry owns the parsed VTK state is a design decision — see §5.

---

## 3. The minimal read-only skeleton

Design decisions baked in, and why:

| Decision | Rationale |
| --- | --- |
| Parse VTK metadata **eagerly in `attach`** | `DESCRIBE`, `SHOW TABLES`, autocomplete, and `duckdb_tables()` all work without a query having run; attach-time errors surface on `ATTACH` rather than on first `SELECT`. See §5. |
| Table entries owned by the **`VtkSchemaEntry`**, created once, immutable | File is read-only and its schema cannot change under us. Avoids the whole `PostgresCatalogSet` staleness machinery. |
| The open reader is a `shared_ptr<VtkFileHandle>` on the catalog | Outlives every transaction and every parallel scan thread; `shared_ptr` into `bind_data` keeps it alive even across a `DETACH` race. |
| One `TableFunction` (`vtk_catalog_scan`) with `bind == nullptr` | The scan function is only ever reached via `GetScanFunction`, which pre-supplies `bind_data`; a `bind` callback would never be invoked. See §4. |
| `Transaction` subclass is a near-empty shell | Nothing is transactional. |
| Every write path `throw`s | `BinderException` for user-visible "this catalog doesn't do that"; `NotImplementedException` for internal plumbing. |

> Namespace `duck_vtk` is used for the VTK-side helpers so nothing collides with `duckdb::`. Everything DuckDB-derived lives in `namespace duckdb` as the framework requires (`Cast<>` and `reference_map_t` machinery assume it).

### 3.1 `src/include/vtk_file_handle.hpp` — the format-side seam

This is deliberately abstract: it is the *only* file the implementation agent has to fill in with real VTK code, and the catalog plumbing below never mentions VTK types.

```cpp
//===----------------------------------------------------------------------===//
// vtk_file_handle.hpp
//
// The seam between DuckDB plumbing and the VTK reader. Nothing below this
// line knows about vtkUnstructuredGrid; nothing above it does either.
//===----------------------------------------------------------------------===//
#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/case_insensitive_map.hpp"

namespace duckdb {

//! One logical relation exposed by a VTK file (e.g. "points", "cells").
struct VtkTableSchema {
	string name;
	vector<string> column_names;
	vector<LogicalType> column_types;
	//! Exact row count, known after the header/metadata is read.
	idx_t cardinality = 0;
};

//! Owns the open VTK dataset for the lifetime of the ATTACH.
//! MUST be safe to call ReadRange() on from several threads concurrently
//! (either because VTK access is genuinely thread-safe for reads, or because
//! this class serialises with `read_lock`). See Uncertainties.
class VtkFileHandle {
public:
	//! Opens `path` and derives the full table list. Throws IOException / InvalidInputException on failure.
	static shared_ptr<VtkFileHandle> Open(const string &path);

	~VtkFileHandle();

	const string &GetPath() const {
		return path;
	}
	//! Ordered table list, as discovered at open time.
	const vector<VtkTableSchema> &GetTables() const {
		return tables;
	}
	//! nullptr if no such table.
	const VtkTableSchema *FindTable(const string &table_name) const;

	//! Materialise rows [row_offset, row_offset + row_count) of `table_name`
	//! into `output`, writing only the columns named by `column_ids` (in that
	//! exact order, matching output.data[0..n)). Returns rows actually written.
	//! `column_ids` never contains COLUMN_IDENTIFIER_ROW_ID because
	//! VtkTableEntry::GetVirtualColumns() returns {}.
	idx_t ReadRange(const string &table_name, const vector<column_t> &column_ids, idx_t row_offset, idx_t row_count,
	                DataChunk &output);

private:
	explicit VtkFileHandle(string path);

	string path;
	vector<VtkTableSchema> tables;
	case_insensitive_map_t<idx_t> table_index;
	//! Guards non-thread-safe reader access, if needed.
	mutex read_lock;
};

} // namespace duckdb
```

### 3.2 `src/include/storage/vtk_catalog.hpp`

```cpp
#pragma once

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/enums/access_mode.hpp"
#include "vtk_file_handle.hpp"

namespace duckdb {
class VtkSchemaEntry;

class VtkCatalog : public Catalog {
public:
	VtkCatalog(AttachedDatabase &db_p, string path_p, AccessMode access_mode_p, shared_ptr<VtkFileHandle> handle_p);
	~VtkCatalog() override;

public:
	//===------------------------------------------------------------------===//
	// Catalog pure virtuals (13)
	//===------------------------------------------------------------------===//
	void Initialize(bool load_builtin) override;

	string GetCatalogType() override {
		// MUST match the registered storage-extension name: Catalog::HasConflictingAttachOptions
		// compares this against the requested TYPE. src/catalog/catalog.cpp:1296-1302
		return "vtk";
	}

	optional_ptr<CatalogEntry> CreateSchema(CatalogTransaction transaction, CreateSchemaInfo &info) override;

	optional_ptr<SchemaCatalogEntry> LookupSchema(CatalogTransaction transaction, const EntryLookupInfo &schema_lookup,
	                                              OnEntryNotFound if_not_found) override;

	void ScanSchemas(ClientContext &context, std::function<void(SchemaCatalogEntry &)> callback) override;

	PhysicalOperator &PlanCreateTableAs(ClientContext &context, PhysicalPlanGenerator &planner, LogicalCreateTable &op,
	                                    PhysicalOperator &plan) override;
	PhysicalOperator &PlanInsert(ClientContext &context, PhysicalPlanGenerator &planner, LogicalInsert &op,
	                             optional_ptr<PhysicalOperator> plan) override;
	PhysicalOperator &PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner, LogicalDelete &op,
	                             PhysicalOperator &plan) override;
	PhysicalOperator &PlanUpdate(ClientContext &context, PhysicalPlanGenerator &planner, LogicalUpdate &op,
	                             PhysicalOperator &plan) override;

	DatabaseSize GetDatabaseSize(ClientContext &context) override;

	bool InMemory() override;
	string GetDBPath() override;

	//===------------------------------------------------------------------===//
	// Optional overrides
	//===------------------------------------------------------------------===//
	//! Read-only, immutable file: refuse index creation with a clear message
	//! rather than letting the base class try to build an ART over us.
	unique_ptr<LogicalOperator> BindCreateIndex(Binder &binder, CreateStatement &stmt, TableCatalogEntry &table,
	                                            unique_ptr<LogicalOperator> plan) override;

	//! Catalog contents never change for an attached file -> a constant version
	//! lets DuckDB cache prepared statements against us forever.
	optional_idx GetCatalogVersion(ClientContext &context) override {
		return 0;
	}

public:
	VtkSchemaEntry &GetMainSchema() {
		return *main_schema;
	}
	shared_ptr<VtkFileHandle> GetHandle() const {
		return handle;
	}
	AccessMode GetAccessMode() const {
		return access_mode;
	}

private:
	//! NOTE: `DropSchema` is a *private* pure virtual in catalog.hpp:452.
	void DropSchema(ClientContext &context, DropInfo &info) override;

private:
	string path;
	AccessMode access_mode;
	shared_ptr<VtkFileHandle> handle;
	unique_ptr<VtkSchemaEntry> main_schema;
};

} // namespace duckdb
```

### 3.3 `src/storage/vtk_catalog.cpp`

```cpp
#include "storage/vtk_catalog.hpp"
#include "storage/vtk_schema_entry.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/storage/database_size.hpp"

namespace duckdb {

VtkCatalog::VtkCatalog(AttachedDatabase &db_p, string path_p, AccessMode access_mode_p,
                       shared_ptr<VtkFileHandle> handle_p)
    : Catalog(db_p), path(std::move(path_p)), access_mode(access_mode_p), handle(std::move(handle_p)) {
	D_ASSERT(handle);
}

VtkCatalog::~VtkCatalog() = default;

void VtkCatalog::Initialize(bool load_builtin) {
	// Called from AttachedDatabase::Initialize (src/main/attached_database.cpp:245-254),
	// i.e. AFTER our attach callback returned. The file is already open by then, so
	// building the schema entry here cannot fail.
	CreateSchemaInfo info;   // info.schema defaults to DEFAULT_SCHEMA == "main"
	main_schema = make_uniq<VtkSchemaEntry>(*this, info);
	main_schema->LoadTables();
}

optional_ptr<CatalogEntry> VtkCatalog::CreateSchema(CatalogTransaction transaction, CreateSchemaInfo &info) {
	if (info.schema == DEFAULT_SCHEMA) {
		// Tolerate a no-op CREATE SCHEMA main (COPY DATABASE emits this).
		return main_schema.get();
	}
	throw BinderException("VTK catalogs do not support creating new schemas");
}

void VtkCatalog::DropSchema(ClientContext &context, DropInfo &info) {
	throw BinderException("VTK catalogs do not support dropping schemas");
}

void VtkCatalog::ScanSchemas(ClientContext &context, std::function<void(SchemaCatalogEntry &)> callback) {
	callback(*main_schema);
}

optional_ptr<SchemaCatalogEntry> VtkCatalog::LookupSchema(CatalogTransaction transaction,
                                                          const EntryLookupInfo &schema_lookup,
                                                          OnEntryNotFound if_not_found) {
	auto &schema_name = schema_lookup.GetEntryName();
	if (schema_name == DEFAULT_SCHEMA || schema_name == INVALID_SCHEMA) {
		return main_schema.get();
	}
	if (if_not_found == OnEntryNotFound::RETURN_NULL) {
		return nullptr;
	}
	throw BinderException("VTK catalogs only have a single schema - \"%s\"", string(DEFAULT_SCHEMA));
}

bool VtkCatalog::InMemory() {
	return false;
}

string VtkCatalog::GetDBPath() {
	return path;
}

DatabaseSize VtkCatalog::GetDatabaseSize(ClientContext &context) {
	// Reported by PRAGMA database_size. Blocks are meaningless for a VTK file;
	// report bytes only and leave the rest zero.
	DatabaseSize size;
	size.bytes = 0; // TODO: stat(path) if a real number is wanted
	size.wal_size = idx_t(-1);
	return size;
}

//===--------------------------------------------------------------------===//
// Write paths: all refused
//===--------------------------------------------------------------------===//
PhysicalOperator &VtkCatalog::PlanCreateTableAs(ClientContext &context, PhysicalPlanGenerator &planner,
                                                LogicalCreateTable &op, PhysicalOperator &plan) {
	throw BinderException("VTK catalogs are read-only: CREATE TABLE AS is not supported");
}

PhysicalOperator &VtkCatalog::PlanInsert(ClientContext &context, PhysicalPlanGenerator &planner, LogicalInsert &op,
                                         optional_ptr<PhysicalOperator> plan) {
	throw BinderException("VTK catalogs are read-only: INSERT is not supported");
}

PhysicalOperator &VtkCatalog::PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner, LogicalDelete &op,
                                         PhysicalOperator &plan) {
	throw BinderException("VTK catalogs are read-only: DELETE is not supported");
}

PhysicalOperator &VtkCatalog::PlanUpdate(ClientContext &context, PhysicalPlanGenerator &planner, LogicalUpdate &op,
                                         PhysicalOperator &plan) {
	throw BinderException("VTK catalogs are read-only: UPDATE is not supported");
}

unique_ptr<LogicalOperator> VtkCatalog::BindCreateIndex(Binder &binder, CreateStatement &stmt,
                                                        TableCatalogEntry &table, unique_ptr<LogicalOperator> plan) {
	throw BinderException("VTK catalogs do not support creating indexes");
}

} // namespace duckdb
```

### 3.4 `src/include/storage/vtk_schema_entry.hpp`

```cpp
#pragma once

#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/common/case_insensitive_map.hpp"

namespace duckdb {
class VtkCatalog;
class VtkTableEntry;

class VtkSchemaEntry : public SchemaCatalogEntry {
public:
	VtkSchemaEntry(Catalog &catalog, CreateSchemaInfo &info);
	~VtkSchemaEntry() override;

public:
	//! Build every VtkTableEntry from the already-open file handle. Called once,
	//! from VtkCatalog::Initialize. Not thread-safe; runs before the catalog is visible.
	void LoadTables();

	//===------------------------------------------------------------------===//
	// SchemaCatalogEntry pure virtuals (15)
	//===------------------------------------------------------------------===//
	void Scan(ClientContext &context, CatalogType type, const std::function<void(CatalogEntry &)> &callback) override;
	void Scan(CatalogType type, const std::function<void(CatalogEntry &)> &callback) override;

	optional_ptr<CatalogEntry> LookupEntry(CatalogTransaction transaction, const EntryLookupInfo &lookup_info) override;

	optional_ptr<CatalogEntry> CreateTable(CatalogTransaction transaction, BoundCreateTableInfo &info) override;
	optional_ptr<CatalogEntry> CreateFunction(CatalogTransaction transaction, CreateFunctionInfo &info) override;
	optional_ptr<CatalogEntry> CreateIndex(CatalogTransaction transaction, CreateIndexInfo &info,
	                                       TableCatalogEntry &table) override;
	optional_ptr<CatalogEntry> CreateView(CatalogTransaction transaction, CreateViewInfo &info) override;
	optional_ptr<CatalogEntry> CreateSequence(CatalogTransaction transaction, CreateSequenceInfo &info) override;
	optional_ptr<CatalogEntry> CreateTableFunction(CatalogTransaction transaction,
	                                               CreateTableFunctionInfo &info) override;
	optional_ptr<CatalogEntry> CreateCopyFunction(CatalogTransaction transaction,
	                                              CreateCopyFunctionInfo &info) override;
	optional_ptr<CatalogEntry> CreatePragmaFunction(CatalogTransaction transaction,
	                                                CreatePragmaFunctionInfo &info) override;
	optional_ptr<CatalogEntry> CreateCollation(CatalogTransaction transaction, CreateCollationInfo &info) override;
	optional_ptr<CatalogEntry> CreateType(CatalogTransaction transaction, CreateTypeInfo &info) override;

	void DropEntry(ClientContext &context, DropInfo &info) override;
	void Alter(CatalogTransaction transaction, AlterInfo &info) override;
	// NOTE: CreateCoordinateSystem is NOT pure (schema_catalog_entry.hpp:85-88) - inherit the throwing default.

private:
	//! Insertion-ordered, so SHOW TABLES / Scan() are deterministic.
	vector<unique_ptr<VtkTableEntry>> tables;
	case_insensitive_map_t<idx_t> table_index;
};

} // namespace duckdb
```

### 3.5 `src/storage/vtk_schema_entry.cpp`

```cpp
#include "storage/vtk_schema_entry.hpp"

#include "storage/vtk_catalog.hpp"
#include "storage/vtk_table_entry.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/parser/column_definition.hpp"
#include "duckdb/parser/parsed_data/alter_info.hpp"
#include "duckdb/parser/parsed_data/create_index_info.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "duckdb/planner/parsed_data/bound_create_table_info.hpp"

namespace duckdb {

VtkSchemaEntry::VtkSchemaEntry(Catalog &catalog, CreateSchemaInfo &info) : SchemaCatalogEntry(catalog, info) {
}

VtkSchemaEntry::~VtkSchemaEntry() = default;

void VtkSchemaEntry::LoadTables() {
	auto &vtk_catalog = catalog.Cast<VtkCatalog>();
	auto handle = vtk_catalog.GetHandle();

	for (auto &table_schema : handle->GetTables()) {
		CreateTableInfo info;
		info.catalog = catalog.GetName();
		info.schema = name;         // "main"
		info.table = table_schema.name;
		D_ASSERT(table_schema.column_names.size() == table_schema.column_types.size());
		for (idx_t i = 0; i < table_schema.column_names.size(); i++) {
			info.columns.AddColumn(ColumnDefinition(table_schema.column_names[i], table_schema.column_types[i]));
		}
		// NOTE: TableCatalogEntry's ctor MOVES info.columns out, so `info` is spent after this line.
		auto entry = make_uniq<VtkTableEntry>(vtk_catalog, *this, info, table_schema.cardinality);
		entry->internal = false;    // show up in duckdb_tables() / SHOW TABLES
		table_index[table_schema.name] = tables.size();
		tables.push_back(std::move(entry));
	}
}

//===--------------------------------------------------------------------===//
// Lookup + Scan
//===--------------------------------------------------------------------===//
optional_ptr<CatalogEntry> VtkSchemaEntry::LookupEntry(CatalogTransaction transaction,
                                                       const EntryLookupInfo &lookup_info) {
	if (lookup_info.GetCatalogType() != CatalogType::TABLE_ENTRY) {
		// We expose nothing else: no views, indexes, sequences, types, functions.
		return nullptr;
	}
	auto entry = table_index.find(lookup_info.GetEntryName());
	if (entry == table_index.end()) {
		return nullptr;
	}
	return tables[entry->second].get();
}

void VtkSchemaEntry::Scan(ClientContext &context, CatalogType type,
                          const std::function<void(CatalogEntry &)> &callback) {
	if (type != CatalogType::TABLE_ENTRY) {
		return;
	}
	for (auto &table : tables) {
		callback(*table);
	}
}

void VtkSchemaEntry::Scan(CatalogType type, const std::function<void(CatalogEntry &)> &callback) {
	// The context-free overload. Our entries are immutable and context-independent,
	// so unlike SQLite (which throws here) we can serve it identically.
	if (type != CatalogType::TABLE_ENTRY) {
		return;
	}
	for (auto &table : tables) {
		callback(*table);
	}
}

//===--------------------------------------------------------------------===//
// Write paths: all refused
//===--------------------------------------------------------------------===//
optional_ptr<CatalogEntry> VtkSchemaEntry::CreateTable(CatalogTransaction, BoundCreateTableInfo &) {
	throw BinderException("VTK catalogs are read-only: CREATE TABLE is not supported");
}
optional_ptr<CatalogEntry> VtkSchemaEntry::CreateFunction(CatalogTransaction, CreateFunctionInfo &) {
	throw BinderException("VTK catalogs do not support creating functions");
}
optional_ptr<CatalogEntry> VtkSchemaEntry::CreateIndex(CatalogTransaction, CreateIndexInfo &, TableCatalogEntry &) {
	throw BinderException("VTK catalogs do not support creating indexes");
}
optional_ptr<CatalogEntry> VtkSchemaEntry::CreateView(CatalogTransaction, CreateViewInfo &) {
	throw BinderException("VTK catalogs do not support creating views");
}
optional_ptr<CatalogEntry> VtkSchemaEntry::CreateSequence(CatalogTransaction, CreateSequenceInfo &) {
	throw BinderException("VTK catalogs do not support creating sequences");
}
optional_ptr<CatalogEntry> VtkSchemaEntry::CreateTableFunction(CatalogTransaction, CreateTableFunctionInfo &) {
	throw BinderException("VTK catalogs do not support creating table functions");
}
optional_ptr<CatalogEntry> VtkSchemaEntry::CreateCopyFunction(CatalogTransaction, CreateCopyFunctionInfo &) {
	throw BinderException("VTK catalogs do not support creating copy functions");
}
optional_ptr<CatalogEntry> VtkSchemaEntry::CreatePragmaFunction(CatalogTransaction, CreatePragmaFunctionInfo &) {
	throw BinderException("VTK catalogs do not support creating pragma functions");
}
optional_ptr<CatalogEntry> VtkSchemaEntry::CreateCollation(CatalogTransaction, CreateCollationInfo &) {
	throw BinderException("VTK catalogs do not support creating collations");
}
optional_ptr<CatalogEntry> VtkSchemaEntry::CreateType(CatalogTransaction, CreateTypeInfo &) {
	throw BinderException("VTK catalogs do not support creating types");
}

void VtkSchemaEntry::DropEntry(ClientContext &context, DropInfo &info) {
	if (info.if_not_found == OnEntryNotFound::RETURN_NULL) {
		return;
	}
	throw BinderException("VTK catalogs are read-only: DROP is not supported");
}

void VtkSchemaEntry::Alter(CatalogTransaction, AlterInfo &) {
	throw BinderException("VTK catalogs are read-only: ALTER is not supported");
}

} // namespace duckdb
```

### 3.6 `src/include/storage/vtk_table_entry.hpp`

```cpp
#pragma once

#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"

namespace duckdb {

class VtkTableEntry : public TableCatalogEntry {
public:
	VtkTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info, idx_t cardinality);

public:
	//===------------------------------------------------------------------===//
	// TableCatalogEntry pure virtuals (3)
	//===------------------------------------------------------------------===//
	unique_ptr<BaseStatistics> GetStatistics(ClientContext &context, column_t column_id) override;
	TableFunction GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) override;
	TableStorageInfo GetStorageInfo(ClientContext &context) override;

	//===------------------------------------------------------------------===//
	// Optional overrides
	//===------------------------------------------------------------------===//
	//! Suppress the inherited virtual `rowid` column (base default is
	//! table_catalog_entry.cpp:374-378) - our scanner cannot produce one.
	virtual_column_map_t GetVirtualColumns() const override {
		return virtual_column_map_t();
	}
	vector<column_t> GetRowIdColumns() const override {
		return vector<column_t>();
	}
	void BindUpdateConstraints(Binder &binder, LogicalGet &get, LogicalProjection &proj, LogicalUpdate &update,
	                           ClientContext &context) override {
		throw BinderException("VTK tables are read-only");
	}

	idx_t GetCardinality() const {
		return cardinality;
	}

private:
	idx_t cardinality;
};

} // namespace duckdb
```

### 3.7 `src/storage/vtk_table_entry.cpp`

```cpp
#include "storage/vtk_table_entry.hpp"

#include "storage/vtk_catalog.hpp"
#include "vtk_scan.hpp"

#include "duckdb/storage/statistics/base_statistics.hpp"
#include "duckdb/storage/table_storage_info.hpp"

namespace duckdb {

VtkTableEntry::VtkTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info, idx_t cardinality_p)
    : TableCatalogEntry(catalog, schema, info), cardinality(cardinality_p) {
	this->internal = false;
}

unique_ptr<BaseStatistics> VtkTableEntry::GetStatistics(ClientContext &context, column_t column_id) {
	// No per-column statistics. (SQLite and Delta both return nullptr here:
	// duckdb-sqlite/src/storage/sqlite_table_entry.cpp:15-17)
	return nullptr;
}

TableStorageInfo VtkTableEntry::GetStorageInfo(ClientContext &context) {
	TableStorageInfo result;
	result.cardinality = cardinality;   // exact, from the VTK header
	return result;
}

TableFunction VtkTableEntry::GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) {
	auto &vtk_catalog = catalog.Cast<VtkCatalog>();

	auto result = make_uniq<VtkScanBindData>();
	result->handle = vtk_catalog.GetHandle();   // shared_ptr: keeps the file alive for the whole query
	result->table_name = name;                  // v1.5.4: CatalogEntry::name is a plain `string`
	result->cardinality = cardinality;
	for (auto &col : columns.Logical()) {
		result->names.push_back(col.Name());     // v1.5.4: ColumnDefinition::Name() -> const string &
		result->types.push_back(col.Type());
	}

	bind_data = std::move(result);
	// Returned BY VALUE: the LogicalGet stores a copy of the TableFunction.
	return VtkScanFunction();
}

} // namespace duckdb
```

### 3.8 `src/include/vtk_scan.hpp`

```cpp
#pragma once

#include "duckdb/function/table_function.hpp"
#include "vtk_file_handle.hpp"

namespace duckdb {

struct VtkScanBindData : public TableFunctionData {
	//! Keeps the VTK reader alive independently of the catalog.
	shared_ptr<VtkFileHandle> handle;
	string table_name;
	vector<string> names;
	vector<LogicalType> types;
	idx_t cardinality = 0;

	//! Rows handed to one worker at a time. Tune against VTK read granularity.
	static constexpr idx_t ROWS_PER_MORSEL = 122880; // 60 * STANDARD_VECTOR_SIZE
};

class VtkScanFunction : public TableFunction {
public:
	VtkScanFunction();
};

} // namespace duckdb
```

`TableFunctionData` (`src/include/duckdb/function/function.hpp:83-91`) already implements `Copy()`/`Equals()`, so `VtkScanBindData` needs no overrides:
```cpp
struct TableFunctionData : public FunctionData {
	// used to pass on projections to table functions that support them. NB, can contain COLUMN_IDENTIFIER_ROW_ID
	vector<idx_t> column_ids;

	DUCKDB_API ~TableFunctionData() override;

	DUCKDB_API unique_ptr<FunctionData> Copy() const override;
	DUCKDB_API bool Equals(const FunctionData &other) const override;
};
```

### 3.9 `src/vtk_scan.cpp`

```cpp
#include "vtk_scan.hpp"

#include "duckdb/common/mutex.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/storage/statistics/node_statistics.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// States
//===--------------------------------------------------------------------===//
struct VtkScanGlobalState : public GlobalTableFunctionState {
	VtkScanGlobalState(idx_t total_rows_p, idx_t max_threads_p)
	    : total_rows(total_rows_p), max_threads(max_threads_p) {
	}

	mutex lock;
	idx_t next_row = 0;
	idx_t total_rows;
	idx_t max_threads;

	idx_t MaxThreads() const override {
		return max_threads;
	}

	//! Hand out the next morsel. Returns false when the table is exhausted.
	bool NextMorsel(idx_t morsel_size, idx_t &out_offset, idx_t &out_count) {
		lock_guard<mutex> guard(lock);
		if (next_row >= total_rows) {
			return false;
		}
		out_offset = next_row;
		out_count = MinValue<idx_t>(morsel_size, total_rows - next_row);
		next_row += out_count;
		return true;
	}
};

struct VtkScanLocalState : public LocalTableFunctionState {
	//! Physical columns to materialise, in output order. Set from
	//! TableFunctionInitInput::column_ids because projection_pushdown = true.
	vector<column_t> column_ids;
	idx_t row_offset = 0;
	idx_t rows_remaining = 0;
};

//===--------------------------------------------------------------------===//
// Init
//===--------------------------------------------------------------------===//
static idx_t VtkMaxThreads(const VtkScanBindData &bind_data) {
	if (bind_data.cardinality == 0) {
		return 1;
	}
	auto morsels = (bind_data.cardinality + VtkScanBindData::ROWS_PER_MORSEL - 1) / VtkScanBindData::ROWS_PER_MORSEL;
	return MaxValue<idx_t>(1, morsels);
}

static unique_ptr<GlobalTableFunctionState> VtkScanInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<VtkScanBindData>();
	return make_uniq<VtkScanGlobalState>(bind_data.cardinality, VtkMaxThreads(bind_data));
}

static unique_ptr<LocalTableFunctionState> VtkScanInitLocal(ExecutionContext &context, TableFunctionInitInput &input,
                                                            GlobalTableFunctionState *global_state) {
	auto result = make_uniq<VtkScanLocalState>();
	// Projection pushdown: only these columns are requested, in exactly this order.
	result->column_ids = input.column_ids;
	return std::move(result);
}

//===--------------------------------------------------------------------===//
// Scan
//===--------------------------------------------------------------------===//
static void VtkScan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &bind_data = data.bind_data->Cast<VtkScanBindData>();
	auto &gstate = data.global_state->Cast<VtkScanGlobalState>();
	auto &lstate = data.local_state->Cast<VtkScanLocalState>();

	while (true) {
		if (lstate.rows_remaining == 0) {
			idx_t offset;
			idx_t count;
			if (!gstate.NextMorsel(VtkScanBindData::ROWS_PER_MORSEL, offset, count)) {
				output.SetCardinality(0);   // zero rows == end of stream
				return;
			}
			lstate.row_offset = offset;
			lstate.rows_remaining = count;
		}

		auto to_read = MinValue<idx_t>(lstate.rows_remaining, STANDARD_VECTOR_SIZE);
		auto produced = bind_data.handle->ReadRange(bind_data.table_name, lstate.column_ids, lstate.row_offset, to_read,
		                                           output);
		lstate.row_offset += produced;
		lstate.rows_remaining -= produced;
		output.SetCardinality(produced);
		if (produced > 0) {
			return;
		}
		// produced == 0 with a non-empty morsel would be a reader bug; guard against a spin.
		if (to_read > 0 && produced == 0) {
			throw InternalException("VTK reader returned 0 rows for a non-empty range");
		}
	}
}

//===--------------------------------------------------------------------===//
// Metadata callbacks
//===--------------------------------------------------------------------===//
static unique_ptr<NodeStatistics> VtkScanCardinality(ClientContext &context, const FunctionData *bind_data_p) {
	auto &bind_data = bind_data_p->Cast<VtkScanBindData>();
	// Exact count: pass it as both estimate and max so the optimizer trusts it.
	return make_uniq<NodeStatistics>(bind_data.cardinality, bind_data.cardinality);
}

static double VtkScanProgress(ClientContext &context, const FunctionData *bind_data_p,
                              const GlobalTableFunctionState *global_state) {
	auto &gstate = global_state->Cast<VtkScanGlobalState>();
	if (gstate.total_rows == 0) {
		return 100.0;
	}
	return 100.0 * double(gstate.next_row) / double(gstate.total_rows);
}

static InsertionOrderPreservingMap<string> VtkScanToString(TableFunctionToStringInput &input) {
	InsertionOrderPreservingMap<string> result;
	auto &bind_data = input.bind_data->Cast<VtkScanBindData>();
	result["Table"] = bind_data.table_name;
	result["File"] = bind_data.handle->GetPath();
	return result;
}

//===--------------------------------------------------------------------===//
// Function definition
//===--------------------------------------------------------------------===//
VtkScanFunction::VtkScanFunction()
    // bind == nullptr: this function is reachable only through
    // VtkTableEntry::GetScanFunction, which pre-supplies bind_data. Note the
    // std::nullptr_t overload of the TableFunction ctor (table_function.hpp:374-380)
    // is NOT what we want - we do have a `function`, we lack a `bind`.
    : TableFunction("vtk_catalog_scan", {}, VtkScan, nullptr, VtkScanInitGlobal, VtkScanInitLocal) {
	projection_pushdown = true;
	filter_pushdown = false;      // we do no filtering ourselves; DuckDB adds a FILTER above us
	filter_prune = false;
	cardinality = VtkScanCardinality;
	table_scan_progress = VtkScanProgress;
	to_string = VtkScanToString;
}

} // namespace duckdb
```

### 3.10 `src/include/storage/vtk_transaction.hpp` + `.cpp`

```cpp
#pragma once

#include "duckdb/transaction/transaction.hpp"

namespace duckdb {
class VtkCatalog;

//! A VTK "transaction" carries no state: the file is read-only and immutable
//! for the lifetime of the ATTACH, so there is nothing to snapshot, begin,
//! commit or roll back. It exists purely because TransactionManager::
//! StartTransaction must return a Transaction &.
class VtkTransaction : public Transaction {
public:
	VtkTransaction(VtkCatalog &vtk_catalog, TransactionManager &manager, ClientContext &context);
	~VtkTransaction() override;

	static VtkTransaction &Get(ClientContext &context, Catalog &catalog);

	VtkCatalog &GetCatalog() {
		return vtk_catalog;
	}

private:
	VtkCatalog &vtk_catalog;
};

} // namespace duckdb
```

```cpp
#include "storage/vtk_transaction.hpp"
#include "storage/vtk_catalog.hpp"

namespace duckdb {

VtkTransaction::VtkTransaction(VtkCatalog &vtk_catalog_p, TransactionManager &manager, ClientContext &context)
    : Transaction(manager, context), vtk_catalog(vtk_catalog_p) {
}

VtkTransaction::~VtkTransaction() = default;

VtkTransaction &VtkTransaction::Get(ClientContext &context, Catalog &catalog) {
	return Transaction::Get(context, catalog).Cast<VtkTransaction>();
}

} // namespace duckdb
```

### 3.11 `src/include/storage/vtk_transaction_manager.hpp` + `.cpp`

```cpp
#pragma once

#include "duckdb/common/mutex.hpp"
#include "duckdb/common/reference_map.hpp"
#include "duckdb/transaction/transaction_manager.hpp"
#include "storage/vtk_transaction.hpp"

namespace duckdb {
class VtkCatalog;

class VtkTransactionManager : public TransactionManager {
public:
	VtkTransactionManager(AttachedDatabase &db_p, VtkCatalog &vtk_catalog);

	Transaction &StartTransaction(ClientContext &context) override;
	ErrorData CommitTransaction(ClientContext &context, Transaction &transaction) override;
	void RollbackTransaction(Transaction &transaction) override;
	void Checkpoint(ClientContext &context, bool force = false) override;

private:
	void Release(Transaction &transaction);

	VtkCatalog &vtk_catalog;
	mutex transaction_lock;
	reference_map_t<Transaction, unique_ptr<VtkTransaction>> transactions;
};

} // namespace duckdb
```

```cpp
#include "storage/vtk_transaction_manager.hpp"
#include "storage/vtk_catalog.hpp"
#include "duckdb/main/attached_database.hpp"

namespace duckdb {

VtkTransactionManager::VtkTransactionManager(AttachedDatabase &db_p, VtkCatalog &vtk_catalog_p)
    : TransactionManager(db_p), vtk_catalog(vtk_catalog_p) {
}

Transaction &VtkTransactionManager::StartTransaction(ClientContext &context) {
	auto transaction = make_uniq<VtkTransaction>(vtk_catalog, *this, context);
	auto &result = *transaction;
	lock_guard<mutex> l(transaction_lock);
	transactions[result] = std::move(transaction);
	return result;
}

ErrorData VtkTransactionManager::CommitTransaction(ClientContext &context, Transaction &transaction) {
	Release(transaction);
	return ErrorData();   // default-constructed == success
}

void VtkTransactionManager::RollbackTransaction(Transaction &transaction) {
	Release(transaction);
}

void VtkTransactionManager::Release(Transaction &transaction) {
	// Move the owning unique_ptr out under the lock and let it destruct after
	// release - the pattern duckdb-sqlite uses to avoid lock-order inversion in
	// the transaction destructor (sqlite_transaction_manager.cpp:32-47).
	unique_ptr<VtkTransaction> to_close;
	{
		lock_guard<mutex> l(transaction_lock);
		auto entry = transactions.find(transaction);
		if (entry == transactions.end()) {
			return;
		}
		to_close = std::move(entry->second);
		transactions.erase(entry);
	}
}

void VtkTransactionManager::Checkpoint(ClientContext &context, bool force) {
	// Nothing to flush. Must not throw: CHECKPOINT and shutdown both reach here.
	// AttachedDatabase::~AttachedDatabase calls Close(TRY_CHECKPOINT).
}

} // namespace duckdb
```

### 3.12 `src/include/vtk_storage.hpp` + `src/vtk_storage.cpp` — the `StorageExtension`

```cpp
#pragma once

#include "duckdb/storage/storage_extension.hpp"

namespace duckdb {

class VtkStorageExtension : public StorageExtension {
public:
	VtkStorageExtension();
};

} // namespace duckdb
```

```cpp
#include "vtk_storage.hpp"

#include "storage/vtk_catalog.hpp"
#include "storage/vtk_transaction_manager.hpp"
#include "vtk_file_handle.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "duckdb/transaction/transaction_manager.hpp"

namespace duckdb {

static unique_ptr<Catalog> VtkAttach(optional_ptr<StorageExtensionInfo> storage_info, ClientContext &context,
                                     AttachedDatabase &db, const string &name, AttachInfo &info,
                                     AttachOptions &options) {
	// 1. Reject write access up front rather than failing on the first INSERT.
	if (options.access_mode == AccessMode::READ_WRITE) {
		// AccessMode::AUTOMATIC and READ_ONLY are both fine; only an *explicit*
		// READ_WRITE is a user error we should name.
		// NOTE: DBConfig's default access_mode is AUTOMATIC, so a bare
		// `ATTACH 'x.vtu' (TYPE vtk)` lands here as AUTOMATIC, not READ_WRITE.
	}

	// 2. Consume our own ATTACH options. Well-known keys (TYPE, READ_ONLY, ...)
	//    have already been stripped by the AttachOptions ctor
	//    (src/main/attached_database.cpp:39-105), so anything left is ours.
	for (auto &entry : options.options) {
		if (StringUtil::CIEquals(entry.first, "some_vtk_option")) {
			// ... entry.second.GetValue<bool>() / .ToString() / DefaultCastAs(...)
			continue;
		}
		throw BinderException("Unrecognized option for VTK ATTACH: \"%s\"", entry.first);
	}

	// 3. Eagerly open the file and derive the table list. Throwing here surfaces
	//    the error on ATTACH, which is what users expect.
	auto handle = VtkFileHandle::Open(info.path);

	auto result = make_uniq<VtkCatalog>(db, info.path, options.access_mode, std::move(handle));

	// Optional: make `SELECT * FROM m;` mean `SELECT * FROM m.main.points;`
	// (Catalog::SetDefaultTable takes plain strings in v1.5.4 - catalog.cpp:1262-1265)
	// result->SetDefaultTable(DEFAULT_SCHEMA, "points");

	return std::move(result);
}

static unique_ptr<TransactionManager> VtkCreateTransactionManager(optional_ptr<StorageExtensionInfo> storage_info,
                                                                  AttachedDatabase &db, Catalog &catalog) {
	auto &vtk_catalog = catalog.Cast<VtkCatalog>();
	return make_uniq<VtkTransactionManager>(db, vtk_catalog);
}

VtkStorageExtension::VtkStorageExtension() {
	attach = VtkAttach;
	create_transaction_manager = VtkCreateTransactionManager;
	// storage_info stays null; OnCheckpointStart/End inherit the no-op defaults.
}

} // namespace duckdb
```

### 3.13 `src/vtk_extension.cpp` — `Load()`

```cpp
#define DUCKDB_EXTENSION_MAIN

#include "vtk_extension.hpp"
#include "vtk_storage.hpp"

#include "duckdb.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/storage/storage_extension.hpp"

namespace duckdb {

static void LoadInternal(ExtensionLoader &loader) {
	auto &db = loader.GetDatabaseInstance();
	auto &config = DBConfig::GetConfig(db);

	// Register under exactly "vtk": there is no entry for it in the hard-coded
	// alias table (src/main/extension/extension_alias.cpp:5-15), so ATTACH ...
	// (TYPE vtk) looks up the literal lower-cased name.
	StorageExtension::Register(config, "vtk", make_shared_ptr<VtkStorageExtension>());

	// (phase 1 / fallback) plain table functions:
	// loader.RegisterFunction(VtkPointsFunction());
	// loader.RegisterFunction(VtkCellsFunction());
}

void VtkExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string VtkExtension::Name() {
	return "vtk";
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(vtk, loader) {
	duckdb::LoadInternal(loader);
}

} // extern "C"
```

---

## 4. How the table scan works

### 4.1 The call site: how `bind_data` is pre-supplied

The binder calls `GetScanFunction` and *never* calls the returned function's `bind`. `src/planner/binder/tableref/bind_basetableref.cpp:236-267`, verbatim:

```cpp
		auto &properties = GetStatementProperties();
		properties.RegisterDBRead(table.ParentCatalog(), context);

		unique_ptr<FunctionData> bind_data;
		auto scan_function = table.GetScanFunction(context, bind_data, table_lookup);
		if (bind_data && !bind_data->SupportStatementCache()) {
			SetAlwaysRequireRebind();
		}
		// TODO: bundle the type and name vector in a struct (e.g PackedColumnMetadata)
		vector<LogicalType> table_types;
		vector<string> table_names;
		vector<TableColumnType> table_categories;

		vector<LogicalType> return_types;
		vector<string> return_names;
		for (auto &col : table.GetColumns().Logical()) {
			table_types.push_back(col.Type());
			table_names.push_back(col.Name());
			return_types.push_back(col.Type());
			return_names.push_back(col.Name());
		}
		table_names = BindContext::AliasColumnNames(ref.table_name, table_names, ref.column_name_alias);

		virtual_column_map_t virtual_columns;
		if (scan_function.get_virtual_columns) {
			virtual_columns = scan_function.get_virtual_columns(context, bind_data.get());
		} else {
			virtual_columns = table.GetVirtualColumns();
		}
		auto logical_get =
		    make_uniq<LogicalGet>(table_index, scan_function, std::move(bind_data), std::move(return_types),
		                          std::move(return_names), std::move(virtual_columns));
```

Consequences, all load-bearing:

1. **Return types and names come from `TableCatalogEntry::GetColumns()`, not from a bind callback.** Whatever `LoadTables()` put in the `ColumnList` *is* the SQL schema. `GetScanFunction`'s job is only to build a `FunctionData` consistent with it.
2. `scan_function` is copied **by value** into the `LogicalGet` — returning a temporary `VtkScanFunction()` is correct and idiomatic.
3. `table.GetScanFunction(context, bind_data, table_lookup)` — the **3-arg** overload is called. The base forwards to the 2-arg one (`src/catalog/catalog_entry/table_catalog_entry.cpp:240-243`), so implementing only the 2-arg pure virtual is sufficient. Override the 3-arg form only if you need `EntryLookupInfo::GetAtClause()` (time travel), as delta does (`duckdb-delta/src/storage/delta_table_entry.cpp:111-118`).
4. `bind_data->SupportStatementCache()` (`src/include/duckdb/function/function.hpp:64`) — return `false` from your `FunctionData` if the plan must be rebuilt every execution. For an immutable file, the default `true` plus a constant `GetCatalogVersion()` gives free prepared-statement caching.
5. `get_virtual_columns` on the `TableFunction` **overrides** `TableCatalogEntry::GetVirtualColumns()` when set. Our skeleton leaves it null and overrides the entry instead.

### 4.2 The sqlite_scanner equivalent, verbatim

`duckdb-sqlite/src/storage/sqlite_table_entry.cpp:23-67`:

```cpp
TableFunction SQLiteTableEntry::GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) {
	auto result = make_uniq<SqliteBindData>();
	for (auto &col : columns.Logical()) {
		result->names.emplace_back(col.GetName().GetIdentifierName());
		result->types.push_back(col.GetType());
	}
	auto &sqlite_catalog = catalog.Cast<SQLiteCatalog>();
	result->file_name = sqlite_catalog.path;
	result->table_name = name.GetIdentifierName();
	result->all_varchar = all_varchar;

	auto &transaction = Transaction::Get(context, catalog).Cast<SQLiteTransaction>();
	auto &db = transaction.GetDB();

	if (!db.GetRowIdInfo(name.GetIdentifierName(), result->row_id_info)) {
		result->rows_per_group = optional_idx();
	}

	int64_t threads = 1;
	Value threads_val;
	if (context.TryGetCurrentSetting("threads", threads_val)) {
		threads = BigIntValue::Get(threads_val);
	}

	bool disable_multithreaded_scans = false;
	Value disable_multithreaded_scans_val;
	if (context.TryGetCurrentSetting("sqlite_disable_multithreaded_scans", disable_multithreaded_scans_val)) {
		disable_multithreaded_scans = BooleanValue::Get(disable_multithreaded_scans_val);
	}

	bool use_global_db =
	    !transaction.IsReadOnly() || sqlite_catalog.InMemory() || threads <= 1 || disable_multithreaded_scans;

	if (use_global_db) {
		// for in-memory databases or if we have transaction-local changes we can
		// only do a single-threaded scan set up the transaction's connection object
		// as the global db
		result->global_db = &db;
		result->rows_per_group = optional_idx();
	}
	result->table = this;

	bind_data = std::move(result);
	return static_cast<TableFunction>(SqliteScanFunction());
}
```

> ⚠️ `col.GetName().GetIdentifierName()` and `name.GetIdentifierName()` are **main-only**. In v1.5.4 `CatalogEntry::name` is `string` (`src/include/duckdb/catalog/catalog_entry.hpp:47-48`) and `ColumnDefinition::GetName()` returns `string` (`src/include/duckdb/parser/column_definition.hpp:84`). Use `col.Name()` / `name`.

`duckdb-sqlite/src/sqlite_scanner.cpp:381-387` (the function definition):

```cpp
SqliteScanFunction::SqliteScanFunction()
    : TableFunction("sqlite_scan", {LogicalType::VARCHAR, LogicalType::VARCHAR}, SqliteScan, SqliteBind,
                    SqliteInitGlobalState, SqliteInitLocalState) {
	cardinality = SqliteCardinality;
	to_string = SqliteToString;
	get_bind_info = SqliteBindInfo;
	projection_pushdown = true;
}
```

Note SQLite's function *does* have a `bind` and takes `{VARCHAR, VARCHAR}` arguments — because the same function is also exposed to users as `sqlite_scan('file.db', 'tbl')`. We don't need that for the catalog path; taking `{}` arguments and `bind = nullptr` is fine because the function is never resolved by name from SQL.

### 4.3 Projection pushdown

`TableFunctionInitInput` — `src/include/duckdb/function/table_function.hpp:117-160`, verbatim:

```cpp
struct TableFunctionInitInput {
	TableFunctionInitInput(optional_ptr<const FunctionData> bind_data_p, vector<column_t> column_ids_p,
	                       const vector<idx_t> &projection_ids_p, optional_ptr<TableFilterSet> filters_p,
	                       optional_ptr<SampleOptions> sample_options_p = nullptr,
	                       optional_ptr<const PhysicalOperator> op_p = nullptr)
	    : bind_data(bind_data_p), column_ids(std::move(column_ids_p)), projection_ids(projection_ids_p),
	      filters(filters_p), sample_options(sample_options_p), op(op_p) {
		for (auto &col_id : column_ids) {
			column_indexes.emplace_back(col_id);
		}
	}
	...
	optional_ptr<const FunctionData> bind_data;
	vector<column_t> column_ids;
	vector<ColumnIndex> column_indexes;
	const vector<idx_t> projection_ids;
	optional_ptr<TableFilterSet> filters;
	optional_ptr<SampleOptions> sample_options;
	optional_ptr<const PhysicalOperator> op;

	bool CanRemoveFilterColumns() const {
		if (projection_ids.empty()) {
			// No filter columns to remove.
			return false;
		}
		if (projection_ids.size() == column_ids.size()) {
			// Filter column is used in remainder of plan, so we cannot remove it.
			return false;
		}
		// Fewer columns need to be projected out than that we scan.
		return true;
	}
};
```

The contract, driven by `TableFunction::projection_pushdown` (`table_function.hpp:486-488`):

```cpp
	//! Whether or not the table function supports projection pushdown. If not supported a projection will be added
	//! that filters out unused columns.
	bool projection_pushdown;
```

- **`projection_pushdown = true`**: `input.column_ids` lists the columns actually needed, in the order DuckDB expects them in `output.data[]`. `output.ColumnCount() == column_ids.size()`. You must write `output.data[i]` from source column `column_ids[i]`. `column_ids` may contain `COLUMN_IDENTIFIER_ROW_ID` (`(column_t)-1`) if you advertise virtual columns.
- **`projection_pushdown = false`**: `column_ids` is the identity `0..n-1` and DuckDB inserts a `PROJECTION` above you. Simpler but you read every column. Recommended for a first cut *only if* VTK column extraction is trivially cheap; for a mesh with dozens of point-data arrays, projection pushdown is the single biggest win, so the skeleton turns it on.
- `column_indexes` (`vector<ColumnIndex>`) is the newer, nested-aware form. Use `column_ids` unless you support struct-field pruning.

sqlite_scanner stores `input.column_ids` in the *local* state and builds the SQL from it (`duckdb-sqlite/src/sqlite_scanner.cpp:206-217` and `:89-96`) — the same shape our skeleton uses.

### 4.4 Filter pushdown

```cpp
// table_function.hpp:489-494
	//! Whether or not the table function supports filter pushdown. If not supported a filter will be added
	//! that applies the table filter directly.
	bool filter_pushdown;
	//! Whether or not the table function can immediately prune out filter columns that are unused in the remainder of
	//! the query plan, e.g., "SELECT i FROM tbl WHERE j = 42;" - j does not need to leave the table function at all
	bool filter_prune;
```

With `filter_pushdown = true`, `TableFunctionInitInput::filters` is a `TableFilterSet` you must apply yourself (and DuckDB removes the `FILTER` operator above you). With `filter_prune = true` you may additionally drop filter-only columns from the output — coordinate with `CanRemoveFilterColumns()` / `projection_ids`.

Related, all optional (`table_function.hpp:448-452`, `:467-470`):
```cpp
	table_function_pushdown_complex_filter_t pushdown_complex_filter;
	table_function_pushdown_expression_t pushdown_expression;
	table_function_supports_pushdown_type_t supports_pushdown_type;
	table_function_supports_pushdown_extract_t supports_pushdown_extract;
```

**Recommendation for VTK: leave `filter_pushdown = false` initially.** Notably duckdb-sqlite does *not* set it either — the SQLite scanner does projection pushdown only. A VTK reader has no index structures, so predicate evaluation inside the scan buys nothing over DuckDB's vectorised filter.

### 4.5 Cardinality estimation

```cpp
// table_function.hpp:441-443
	//! (Optional) cardinality function
	//! Returns the expected cardinality of this scan
	table_function_cardinality_t cardinality;
```
```cpp
// table_function.hpp:322-323
typedef unique_ptr<NodeStatistics> (*table_function_cardinality_t)(ClientContext &context,
                                                                   const FunctionData *bind_data);
```

sqlite_scanner (`duckdb-sqlite/src/sqlite_scanner.cpp:142-150`) returns `make_uniq<NodeStatistics>(row_count)` — an estimate. Because a VTK file's row counts are **exact**, pass both arguments (`NodeStatistics(estimated, max)`) so the join-order optimizer treats them as hard bounds. Also fill `TableStorageInfo::cardinality` from `GetStorageInfo` — that is the path `duckdb_tables().estimated_size` reads.

### 4.6 Parallelism

Two knobs:

```cpp
// table_function.hpp:59-69
struct GlobalTableFunctionState {
public:
	// value returned from MaxThreads when as many threads as possible should be used
	constexpr static const int64_t MAX_THREADS = 999999999;

public:
	DUCKDB_API virtual ~GlobalTableFunctionState();

	virtual idx_t MaxThreads() const {
		return 1;
	}
	...
```
```cpp
// table_function.hpp:422-425
	//! (Optional) local init function
	//! Initialize the local operator state of the function.
	//! The local operator state is used to keep track of the progress in the table function and is thread-local.
	table_function_init_local_t init_local;
```
```cpp
// table_function.hpp:294-298
typedef unique_ptr<GlobalTableFunctionState> (*table_function_init_global_t)(ClientContext &context,
                                                                             TableFunctionInitInput &input);
typedef unique_ptr<LocalTableFunctionState> (*table_function_init_local_t)(ExecutionContext &context,
                                                                           TableFunctionInitInput &input,
                                                                           GlobalTableFunctionState *global_state);
```

The morsel pattern: `MaxThreads()` returns the number of independent work units; `init_local` is called once per worker; each `function(...)` invocation pulls a morsel from the global state under a lock and fills up to `STANDARD_VECTOR_SIZE` rows. sqlite_scanner does exactly this with `ROWID BETWEEN ? AND ?` range predicates and `rows_per_group = 122880` (`duckdb-sqlite/src/include/sqlite_scanner.hpp:30`).

For VTK: rows are `[0, cardinality)` with random access by index, so range morsels are natural — **provided** `VtkFileHandle::ReadRange` is safe to call concurrently. See §11.

Also note `TableFunction::global_initialization` (`table_function.hpp:505-508`):
```cpp
	//! When to call init_global
	//! By default init_global is called when the pipeline is ready for execution
	//! If this is set to `INITIALIZE_ON_SCHEDULE` the table function is initialized when the query is scheduled
	TableFunctionInitialization global_initialization = TableFunctionInitialization::INITIALIZE_ON_EXECUTE;
```
Leave the default.

The scan signals end-of-stream by producing a chunk of size 0 (`typedef void (*table_function_t)(ClientContext &context, TableFunctionInput &data, DataChunk &output);` — `table_function.hpp:303`; see the `state.done` / `SetCardinality(0)` handling in `duckdb-sqlite/src/sqlite_scanner.cpp:243-260`).

---

## 5. Schema discovery & lifetime

### 5.1 Eager (attach-time) vs lazy (lookup-time)

**Recommendation: eager, in the `attach` callback.** Reasons:

1. `attach` receives `ClientContext &context` directly (`storage_extension.hpp:25-27`), so a `FileSystem`/`FileOpener` is available.
2. Errors thrown there surface on `ATTACH`, where the user can act on them, rather than on a later `SELECT`.
3. A fixed, immutable schema means there is no correctness argument for laziness.
4. `SHOW TABLES`, `DESCRIBE`, `duckdb_tables()`, and shell autocomplete all go through `ScanSchemas`/`Scan` and need entries to exist without a scan having run (§6).
5. `Catalog::Initialize(bool)` — which is where SQLite and Delta construct their `main_schema` — has **no** `ClientContext`. The context-bearing `Initialize(optional_ptr<ClientContext>, bool)` override does (`catalog.hpp:117`, `src/main/attached_database.cpp:245-254`), but by then `attach` has already returned, so the cleanest split is: **open the file in `attach`**, **build the catalog entries in `Initialize`** (which is exactly what the skeleton in §3 does — `VtkFileHandle::Open` in `VtkAttach`, `main_schema->LoadTables()` in `VtkCatalog::Initialize`).

The cost of eager discovery is an unconditional file open on `ATTACH`. If VTK header parsing turns out to be expensive for huge files (e.g. a legacy ASCII `.vtk` where counting cells requires a full pass), split it: read *names + column types + cardinality* eagerly, and defer the bulk arrays to first scan.

### 5.2 How the examples cache catalog entries

Three distinct strategies, in increasing complexity:

**(a) Delta — cached in the `SchemaCatalogEntry`, mutex-guarded** (`duckdb-delta/src/include/storage/delta_schema_entry.hpp:49-53`):
```cpp
private:
	//! Delta tables may be cached in the SchemaEntry. Since the TableEntry holds the snapshot, this allows sharing a
	//! snapshot between different scans.
	unique_ptr<DeltaTableEntry> cached_table;
	mutex lock;
```
with a lazy `LookupEntry` that builds on demand (`duckdb-delta/src/storage/delta_schema_entry.cpp:175-227`) and a `Scan` that just drives `LookupEntry` (`:155-165`). **This is the closest analogue to VTK and the model the §3 skeleton follows** — except we populate up front and drop the mutex entirely, because the entries are immutable after `Initialize`.

**(b) SQLite — cached per-transaction** (`duckdb-sqlite/src/storage/sqlite_transaction.cpp:19-54, 183-249`). `SQLiteCatalogMap` is a `case_insensitive_map_t<unique_ptr<CatalogEntry>>` behind a `mutex`, owned by the `SQLiteTransaction`. `GetCatalogEntry(name)` is memoised; `ClearTableEntry(name)` invalidates after DDL. Transaction-scoped caching is what gives SQLite read-your-own-DDL semantics — **irrelevant for a read-only immutable file** and strictly more machinery.

**(c) Postgres — a full `PostgresCatalogSet` with staleness detection** (`duckdb-postgres/src/include/storage/postgres_catalog_set.hpp:25-73`): `LoadEntries` / `TryLoadEntries` / `GetStalenessQuery` / `RefreshStalenessSignature` / `ReloadEntry` / `ClearEntries`, plus an explicit `pg_clear_cache()` table function and a settings hook (`duckdb-postgres/src/storage/postgres_clear_cache.cpp:24-51`):
```cpp
void PostgresClearCacheFunction::ClearPostgresCaches(ClientContext &context) {
	auto databases = DatabaseManager::Get(context).GetDatabases(context);
	for (auto &db_ref : databases) {
		auto &db = *db_ref;
		auto &catalog = db.GetCatalog();
		if (catalog.GetCatalogType() != "postgres") {
			continue;
		}
		catalog.Cast<PostgresCatalog>().ClearCache();
	}
}
```
**Do not build this.** A `vtk_clear_cache()` only becomes interesting if someone can overwrite `mesh.vtu` under a live `ATTACH`. If that matters later, the escape hatch is the same one Postgres uses: a table function that calls a `ClearCache()` on every `VtkCatalog`, plus a non-constant `GetCatalogVersion()`.

### 5.3 `catalog_version` and statement caching

```cpp
// catalog.hpp:123-129
	//! Returns a version number that uniquely characterizes the current catalog snapshot.
	//! If there are transaction-local changes, the version returned is >= TRANSACTION_START, o.w. it is a simple number
	//! starting at 0 that is incremented at each commit that has had catalog changes.
	//! If the catalog does not support versioning, no index is returned.
	DUCKDB_API virtual optional_idx GetCatalogVersion(ClientContext &context) {
		return {}; // don't return anything by default
	}
```

Returning a **constant** `0` (as the skeleton does) tells DuckDB the catalog never changes, so prepared statements over `m.points` are never invalidated. Delta returns the snapshot version so that time travel and pinned snapshots invalidate correctly (`duckdb-delta/src/storage/delta_catalog.cpp:76-92`). If you later add a `vtk_clear_cache()`, bump a counter here instead of returning a constant.

### 5.4 Who owns the open file handle, and for how long

The skeleton's ownership chain:

```
AttachedDatabase  (shared_ptr, owned by DatabaseManager)
  └─ unique_ptr<Catalog>  ==  VtkCatalog                        [attached_database.hpp:154]
       ├─ shared_ptr<VtkFileHandle> handle                       ← the open reader
       └─ unique_ptr<VtkSchemaEntry> main_schema
            └─ vector<unique_ptr<VtkTableEntry>> tables
                                                                (each scan)
VtkScanBindData (unique_ptr, owned by LogicalGet → PhysicalTableScan)
  └─ shared_ptr<VtkFileHandle> handle                            ← same object, +1 refcount
```

`shared_ptr` (not a raw reference into the catalog) is the important choice: a `DETACH` concurrent with an in-flight query would otherwise free the reader under the scan threads. With `shared_ptr` the handle simply outlives the catalog until the last chunk is produced. `AttachedDatabase::~AttachedDatabase` calls `Close(DatabaseCloseAction::TRY_CHECKPOINT)` (`src/main/attached_database.cpp:182-187`), which reaches our no-op `Checkpoint`; the handle's destructor then runs when the last `bind_data` also dies.

### 5.5 Threading / `ClientContext` considerations

- `LookupEntry`, `Scan`, `LookupSchema`, `ScanSchemas` may be called **concurrently from different connections** on the same catalog object. In the skeleton they are read-only over containers frozen during `Initialize`, hence lock-free and safe. If you ever mutate `tables` after `Initialize`, you need Delta's `mutex`.
- `Transaction::context` is a `weak_ptr<ClientContext>` (`transaction.hpp:47`). Lock before use and throw if expired — the idiom is at `duckdb-sqlite/src/storage/sqlite_transaction.cpp:107-110`.
- The table function's `function(...)` callback runs on **worker threads**, not the client thread. Anything reachable from `bind_data` and the global state must be thread-safe (or serialised). `init_local` gets an `ExecutionContext &`; `context.client` gives the `ClientContext` (used at `duckdb-sqlite/src/sqlite_scanner.cpp:210`).
- `SchemaCatalogEntry::Scan(CatalogType, callback)` — the **context-free** overload — is called from paths that have no `ClientContext`. SQLite throws `InternalException("Scan")` there (`duckdb-sqlite/src/storage/sqlite_schema_entry.cpp:277-279`) and Delta throws `NotImplementedException` (`duckdb-delta/src/storage/delta_schema_entry.cpp:167-169`), because their entries are context-dependent. **Ours are not**, so we implement it properly — strictly better, and it removes a class of "works interactively, crashes in a background task" bug.

---

## 6. `information_schema` / `duckdb_tables()` integration

Short answer: **it comes for free from `ScanSchemas` + `SchemaCatalogEntry::Scan`.**

`duckdb_tables()` (`src/function/table/system/duckdb_tables.cpp:81-83`):
```cpp
	auto schemas = Catalog::GetAllSchemas(context);
	for (auto &schema : schemas) {
		schema.get().Scan(context, CatalogType::TABLE_ENTRY,
```
`duckdb_columns()` does the same (`src/function/table/system/duckdb_columns.cpp:89-91`). `Catalog::GetAllSchemas(ClientContext&)` (`catalog.hpp:392`) iterates every attached database and calls each catalog's `ScanSchemas`.

So once `VtkCatalog::ScanSchemas` yields `main_schema` and `VtkSchemaEntry::Scan(context, TABLE_ENTRY, cb)` yields the table entries, you automatically get:

| Feature | Works? | Path |
| --- | --- | --- |
| `SHOW TABLES` / `SHOW ALL TABLES` | ✅ | `duckdb_tables()` view |
| `duckdb_tables()`, `duckdb_columns()` | ✅ | `Scan(TABLE_ENTRY)` |
| `information_schema.tables` / `.columns` | ✅ | built as SQL views over `duckdb_tables`/`duckdb_columns` (`src/catalog/default/default_views.cpp`) |
| `DESCRIBE m.points` | ✅ | binds the table entry, reads `GetColumns()` |
| `SELECT * FROM m.points` | ✅ | `LookupSchema` → `LookupEntry` → `GetScanFunction` |
| shell tab-completion | ✅ | driven by `duckdb_tables()`/`duckdb_columns()` |
| `duckdb_tables().estimated_size` | ✅ | from `TableStorageInfo::cardinality` |
| `SELECT * FROM m` (no table name) | only if you call `SetDefaultTable` | `Catalog::TryLookupDefaultTable` (`catalog.hpp:438-440`) |
| `duckdb_indexes()`, `duckdb_constraints()` | trivially empty | our `Scan` returns nothing for those `CatalogType`s |

Two gotchas:

1. **`CatalogEntry::internal` must be `false`.** `duckdb_tables.cpp:111` filters on entry type, and the standard system views filter `internal` entries out. The skeleton sets `entry->internal = false;` explicitly (Delta does the same at `duckdb-delta/src/storage/delta_table_entry.cpp:19`). Note that `AttachedDatabase` itself sets `internal = true` on the *database* entry — that is normal and unrelated.
2. **Implement the context-free `Scan` overload** if you want these to work from every code path. See §5.5.

---

## 7. Registration

### 7.1 The v1.5.4 entrypoint

`src/include/duckdb/main/extension/extension_loader.hpp:115-123`, verbatim:
```cpp
//! Helper macro to define the entrypoint for a C++ extension
//! Usage:
//!
//!		DUCKDB_CPP_EXTENSION_ENTRY(my_extension, loader) {
//!			loader.RegisterFunction(...);
//!		}
//!
#define DUCKDB_CPP_EXTENSION_ENTRY(EXTENSION_NAME, LOADER_NAME)                                                        \
	DUCKDB_EXTENSION_API void EXTENSION_NAME##_duckdb_cpp_init(duckdb::ExtensionLoader &LOADER_NAME)
```

`ExtensionLoader` has **no** storage-extension registration method — I read the header in full (114 lines): the surface is `GetDatabaseInstance()` (`:37`), `SetDescription`, eleven `RegisterFunction` overloads, `RegisterCollation`, `RegisterCoordinateSystem`, `GetFunction`/`GetTableFunction`/`TryGet*`, `AddFunctionOverload`, `RegisterType`, `RegisterSecretType`, `RegisterCastFunction`. So the route is:

```cpp
auto &db = loader.GetDatabaseInstance();                 // extension_loader.hpp:37
auto &config = DBConfig::GetConfig(db);                  // config.hpp:222
StorageExtension::Register(config, "vtk", make_shared_ptr<VtkStorageExtension>());   // storage_extension.hpp:49
```

`DBConfig::GetConfig` overloads (`src/include/duckdb/main/config.hpp:221-225`), verbatim:
```cpp
	DUCKDB_API static DBConfig &GetConfig(ClientContext &context);
	DUCKDB_API static DBConfig &GetConfig(DatabaseInstance &db);

	DUCKDB_API static const DBConfig &GetConfig(const ClientContext &context);
	DUCKDB_API static const DBConfig &GetConfig(const DatabaseInstance &db);
```

### 7.2 The real code from duckdb-sqlite, verbatim

`duckdb-sqlite/src/sqlite_extension.cpp:34-64`:

```cpp
static void LoadInternal(ExtensionLoader &loader) {
	SqliteScanFunction sqlite_fun;
	loader.RegisterFunction(sqlite_fun);

	SqliteAttachFunction attach_func;
	loader.RegisterFunction(attach_func);

	SQLiteQueryFunction query_func;
	loader.RegisterFunction(query_func);

	auto &db = loader.GetDatabaseInstance();
	auto &config = DBConfig::GetConfig(db);
	config.AddExtensionOption("sqlite_all_varchar", "Load all SQLite columns as VARCHAR columns", LogicalType::BOOLEAN);

	config.AddExtensionOption("sqlite_debug_show_queries", "DEBUG SETTING: print all queries sent to SQLite to stdout",
	                          LogicalType::BOOLEAN, Value::BOOLEAN(false), SetSqliteDebugQueryPrint);

	config.AddExtensionOption("sqlite_disable_multithreaded_scans", "Make all scans over the SQLite DB to be performed using a single worker thread",
	                          LogicalType::BOOLEAN, Value::BOOLEAN(false));

	StorageExtension::Register(config, "sqlite_scanner", make_shared_ptr<SQLiteStorageExtension>());
	ExtensionCallback::Register(config, make_shared_ptr<SQLiteVFSCleanupCallback>());
}

void SqliteScannerExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

DUCKDB_CPP_EXTENSION_ENTRY(sqlite_scanner, loader) {
	LoadInternal(loader);
}
```

and duckdb-delta (`duckdb-delta/src/delta_extension.cpp`, `LoadInternal`):
```cpp
	// Register the "single table" delta catalog (to ATTACH a single delta table)
	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	StorageExtension::Register(config, "delta", make_shared_ptr<DeltaStorageExtension>());
```

Both use `StorageExtension::Register(config, name, make_shared_ptr<...>())` — matching v1.5.4 exactly. **The adapted VTK version is in §3.13.**

### 7.3 Name choice and case sensitivity

- **Register under `"vtk"`.** SQLite registers under `"sqlite_scanner"` because `ExtensionHelper::ApplyExtensionAlias` maps `sqlite → sqlite_scanner` (`src/main/extension/extension_alias.cpp:12`). There is no `vtk` entry in that table, so `ApplyExtensionAlias("vtk")` returns `"vtk"` unchanged — register under the literal name the user types.
- **`(TYPE VTK)` is case-insensitive**, three times over: `db_type` is lower-cased in the `AttachOptions` ctor (`src/main/attached_database.cpp:75`), `ApplyExtensionAlias` lower-cases again (`extension_alias.cpp:30, 36`), and the registry is a `case_insensitive_map_t` (`extension_callback_manager.cpp:21`). `TYPE vtk`, `TYPE VTK`, `TYPE 'Vtk'` all resolve.
- **Autoload** works if the extension binary is named `vtk`: `DatabaseManager` calls `Catalog::TryAutoLoad(context, options.db_type)` when no storage extension for the type is registered yet (`src/main/database_manager.cpp:391-396`), so `ATTACH 'm.vtu' (TYPE vtk)` can work without an explicit `LOAD vtk`.
- **`ATTACH 'vtk:mesh.vtu'`** also works via `DBPathAndType::ExtractExtensionPrefix` (`physical_attach.cpp:26-28`), *provided* `ExtensionHelper::ExtractExtensionPrefixFromPath` recognises `vtk` as a candidate prefix — **unverified**, see §11.
- Extension name/description metadata: `loader.SetDescription(...)` (`extension_loader.hpp:41`) is optional but nice.

---

## 8. Fallback plan: plain table functions

If the `StorageExtension` route proves too costly, the phase-1 de-risking path is two ordinary table functions and (optionally) a replacement scan. Nothing here needs the catalog layer.

```sql
SELECT * FROM vtk_points('mesh.vtu');
SELECT * FROM vtk_cells('mesh.vtu');
SELECT * FROM 'mesh.vtu';           -- only with the replacement scan
```

**APIs needed** — strictly a subset of what §3/§4 already uses:

1. `TableFunction` with a real `bind` (`table_function.hpp:288-289`):
   ```cpp
   typedef unique_ptr<FunctionData> (*table_function_bind_t)(ClientContext &context, TableFunctionBindInput &input,
                                                             vector<LogicalType> &return_types, vector<string> &names);
   ```
   The bind opens the file, pushes column types into `return_types` and names into `names`, and returns your `FunctionData`. **This replaces `TableCatalogEntry::GetColumns()` as the source of the SQL schema** — the whole `Catalog`/`SchemaCatalogEntry`/`TableCatalogEntry` tier disappears.
2. The same `init_global` / `init_local` / `function` triple and the same `projection_pushdown` / `cardinality` flags as §3.9. `VtkScanBindData`, `VtkScanGlobalState`, `VtkScanLocalState`, `VtkScan`, `VtkFileHandle` are all reusable **verbatim**.
3. Registration: `loader.RegisterFunction(TableFunction(...))` (`extension_loader.hpp:55-57`). No `DBConfig` access needed.
4. Named parameters if wanted: `TableFunction::named_parameters` (inherited from `SimpleNamedParameterFunction`), populated in `bind` from `input.named_parameters`.
5. Optional replacement scan so a bare filename works (`src/include/duckdb/function/replacement_scan.hpp:50-58`):
   ```cpp
   typedef unique_ptr<TableRef> (*replacement_scan_t)(ClientContext &context, ReplacementScanInput &input,
                                                      optional_ptr<ReplacementScanData> data);
   struct ReplacementScan {
       explicit ReplacementScan(replacement_scan_t function, unique_ptr<ReplacementScanData> data_p = nullptr)
           : function(function), data(std::move(data_p)) {}
       static bool CanReplace(const string &table_name, const vector<string> &extensions);
       ...
   ```
   registered by appending to the still-public `config.replacement_scans` vector (`src/include/duckdb/main/config.hpp:190`):
   ```cpp
   vector<ReplacementScan> replacement_scans;
   ```
   Your callback checks `ReplacementScan::CanReplace(input.table_name, {"vtu", "vtp", "vtk"})` and returns a `TableFunctionRef` for `vtk_points(<path>)`.

**Trade-offs**

| | Table functions | StorageExtension |
| --- | --- | --- |
| Code volume | ~1 header + 1 cpp | ~7 headers + 7 cpps (§3) |
| `SELECT * FROM m.points` | ✗ (`vtk_points('f.vtu')`) | ✓ |
| `SHOW TABLES` / `DESCRIBE` / `information_schema` | ✗ | ✓ (§6) |
| File opened per query | yes, in `bind` | once, at `ATTACH` |
| Multiple relations from one file | separate functions | naturally, one catalog |
| Discoverability of what's in a file | needs a `vtk_metadata()` function | `SHOW TABLES` |

**The scan machinery is 100 % shared.** Building the table functions first and the catalog second is not wasted work — the catalog tier is a thin wrapper that pre-supplies `bind_data` instead of running `bind`. This is the recommended sequencing (§10).

---

## 9. Drift: example extensions vs v1.5.4

All three example repos track duckdb **main**, which has landed a strong `Identifier` type for catalog/column names. **v1.5.4 still uses plain `string` everywhere.** Copy-pasting from the examples will not compile. The full list I observed:

| main (examples) | v1.5.4 (authoritative) | Evidence |
| --- | --- | --- |
| `CatalogEntry::name` is an `Identifier`; `name.GetIdentifierName()` | `string name;` — use `name` directly | `src/include/duckdb/catalog/catalog_entry.hpp:47-48` |
| `col.GetName().GetIdentifierName()`, `col.GetType()` | `col.Name()` → `const string &`, `col.Type()` → `const LogicalType &` (also `GetName()`/`GetType()` returning by value) | `src/include/duckdb/parser/column_definition.hpp:38,43,84,86`; binder uses `col.Type()`/`col.Name()` at `src/planner/binder/tableref/bind_basetableref.cpp:252-255` |
| `ColumnDefinition(Identifier(n), type)` | `ColumnDefinition(string name, LogicalType type)` | `column_definition.hpp:27` |
| `Identifier::DefaultSchema()` | `DEFAULT_SCHEMA` macro (`"main"`) | `src/include/duckdb/common/constants.hpp:31` |
| `info.SchemaName()` on `CreateSchemaInfo` | `info.schema` (a `string` on `CreateInfo`) | `src/include/duckdb/parser/parsed_data/create_info.hpp:37-38`; `create_schema_info.hpp` has no such method |
| `info.GetTableName()` / `info.SetTableName(Identifier)` | `info.table` (a `string`) | `src/include/duckdb/parser/parsed_data/create_table_info.hpp:25` |
| `info.GetQualifiedName().Name()` on `DropInfo` | `info.name`, `info.schema` (plain `string`s) | `src/include/duckdb/parser/parsed_data/drop_info.hpp:32,34` |
| `catalog.GetName().GetIdentifierName()` | `Catalog::GetName()` → `const string &` | `catalog.hpp:132`; impl `src/catalog/catalog.cpp:66-68` |
| `SetDefaultTable(Identifier::DefaultSchema(), Identifier(n))` (delta) | `SetDefaultTable(const string &schema, const string &name)` | `catalog.hpp:354`; impl `catalog.cpp:1262-1265` |
| `table.GetColumn(Identifier(colname))` | `GetColumn(const string &name)` | `table_catalog_entry.hpp:73` |
| `vector<Identifier> names` + `StringsToIdentifiers(...)` (delta) | `vector<string> names` | — |
| `info.SetName(Identifier(name))` on `DropInfo` | `info.name = name;` | `drop_info.hpp:34` |

Things that are **not** drift (examples and v1.5.4 agree):

- `StorageExtension::Register(DBConfig&, const string&, shared_ptr<StorageExtension>)` — matches (`storage_extension.hpp:49`; sqlite `sqlite_extension.cpp:54`; delta `delta_extension.cpp`).
- `attach_function_t` / `create_transaction_manager_t` signatures — match (`storage_extension.hpp:25-29`; sqlite `sqlite_storage.cpp:16-37`; delta `delta_extension.cpp:23-24, 89-90`).
- `Catalog::LookupSchema(CatalogTransaction, const EntryLookupInfo &, OnEntryNotFound)` — match.
- `SchemaCatalogEntry::LookupEntry(CatalogTransaction, const EntryLookupInfo &)` — match.
- `TableCatalogEntry::GetScanFunction(ClientContext&, unique_ptr<FunctionData>&)` and the 3-arg `EntryLookupInfo` overload — match.
- `TransactionManager`'s four pure virtuals — match.
- `DUCKDB_CPP_EXTENSION_ENTRY(name, loader)` — match.
- `TableFunctionInitInput::column_ids`, `projection_pushdown`, `GlobalTableFunctionState::MaxThreads()` — match.

Also worth flagging as *not present in older tutorials*:
- `DBConfig::storage_extensions` no longer exists (moved into `ExtensionCallbackManager`). §2.1.
- `SchemaCatalogEntry::CreateCoordinateSystem` is new (non-pure, throwing default). `schema_catalog_entry.hpp:85-88`.
- `Catalog::SupportsCreateTable`, `HasConflictingAttachOptions`, `IsEncrypted`, `GetEncryptionCipher`, `SupportsTimeTravel`, `CatalogTypeLookupRule`, `GetDependencyManager`, `OnDetach`, `BindAlterAddIndex`, `PlanMergeInto` are all newer non-pure virtuals.
- Legacy `<name>_init(DatabaseInstance &)` / `<name>_version()` entrypoints are gone.

---

## 10. Recommended build-out order

1. **`VtkFileHandle`** (§3.1) standalone, with a `main()` harness. No DuckDB involved. This is where all the actual risk lives.
2. **Table functions** `vtk_points(path)` / `vtk_cells(path)` (§8). Proves the extension builds and links against v1.5.4, exercises `VtkScan*` end-to-end, and delivers user value immediately.
3. **`VtkTransaction` + `VtkTransactionManager` + a stub `VtkCatalog`** whose `Initialize` builds an empty `VtkSchemaEntry`. Target: `ATTACH 'm.vtu' AS m (TYPE vtk);` succeeds and `SHOW TABLES` returns zero rows without crashing. Every pure virtual is now satisfied — this is the compile-gate milestone that flushes out signature drift.
4. **`VtkSchemaEntry::LoadTables` + `VtkTableEntry`** reusing the step-2 scan function via `GetScanFunction`. Target: `SELECT * FROM m.points`, `DESCRIBE m.points`, `SHOW TABLES`.
5. **Projection pushdown, exact cardinality, morsel parallelism.** Measure before and after.
6. Optional polish: `SetDefaultTable`, `vtk:` path prefix, `vtk_clear_cache()`, filter pushdown.

---

## 11. Uncertainties / must verify by compiling

Ordered by how likely each is to bite.

### Signature drift I could not fully rule out

1. **`ExtensionCallbackManager::Get` / `DBConfig::GetCallbackManager` are declared non-const and const**, but I did **not** verify `DBConfig::GetCallbackManager()` is *public* — I only saw it at `config.hpp:317-318` inside the file without capturing the surrounding access specifier. If it turns out to be private, `StorageExtension::Register(config, ...)` still works (it is a `static` member of `StorageExtension` and DuckDB compiles it internally), so this is a non-issue for our code. **Confirm only if you try to touch the callback manager directly.**
2. **`Catalog::IsEncrypted()`'s exact default body** — I paraphrased it in §2.2 (`catalog.hpp:335-337`). It is a non-pure virtual we don't override; verify only if you need it.
3. **`Catalog::PlanDelete` / `PlanUpdate` 3-arg overloads and `PlanMergeInto`** are declared non-pure at `catalog.hpp:314, 317, 318-319`, but I could not locate their definitions in the files I fetched (`src/catalog/catalog.cpp` does not contain them; `src/catalog/catalog_plan.cpp` 404s). The header is authoritative for pure-vs-not, so **do not** declare them; but if the linker complains about missing vtable entries, they may be defined in a physical-plan translation unit I did not fetch. **Low risk, easy to diagnose.**
4. **`ExtensionHelper::ExtractExtensionPrefixFromPath`** — I fetched `src/main/extension/extension_helper.cpp` but the grep for that symbol returned nothing, so it lives elsewhere (probably `src/main/extension_helper.cpp` or `src/main/extension/extension_load.cpp`). I therefore **cannot confirm** whether `ATTACH 'vtk:mesh.vtu'` resolves `vtk` as a prefix, or whether it requires the prefix to be a *known* extension name / installed binary. Verify empirically; it is a nice-to-have, not a requirement.
5. **`Catalog::AutoloadExtensionByConfigName(ClientContext &, const String &)`** at `catalog.hpp:401` uses a capital-`String` type (note `#include "duckdb/common/types/string.hpp"` at `catalog.hpp:24`). This suggests a `String` class exists in v1.5.4 alongside `string`. **I did not read `duckdb/common/types/string.hpp`.** It does not appear in any signature we implement, but if a compile error mentions `duckdb::String`, that's the cause.
6. **`InsertionOrderPreservingMap<string>` return type of `to_string`** (`table_function.hpp:334`) — I use it in `VtkScanToString` but did not verify the include path or that it is header-visible from `table_function.hpp`. If it fails, drop `to_string` (it's purely cosmetic in EXPLAIN output) or find the right include.
7. **`MinValue` / `MaxValue` / `STANDARD_VECTOR_SIZE` / `make_shared_ptr` / `StringUtil::CIEquals` / `reference_map_t`** — all used freely in the skeleton because the example extensions use them, but I did not verify their exact include paths for v1.5.4. Expect a round of missing-include fixes.
8. **`ColumnList::AddColumn` takes `ColumnDefinition` by value** (`column_list.hpp:24`) — confirmed. But I did **not** verify whether `AddColumn` assigns oids/logical indexes automatically or whether `ColumnDefinition` needs an explicit `SetOid`. `duckdb-delta` does `table_info.columns.AddColumn(ColumnDefinition(name, type))` with nothing else, which strongly suggests it's automatic. **Verify by checking that `DESCRIBE m.points` shows the right column order.**

### Semantic questions the headers cannot answer

9. **Is VTK safe for concurrent reads from multiple threads on one reader object?** This determines whether `MaxThreads()` may exceed 1. VTK's `vtkDataArray::GetTuple` on an already-loaded in-memory dataset is generally safe, but `vtkXMLReader` re-reads (`RequestData`, piece-wise reads) are **not**. Safe default: fully materialise into memory during `Open()`, then `MaxThreads() > 1` is sound. Otherwise serialise on `VtkFileHandle::read_lock` and return `1`. **Decide this before writing the parallel path.**
10. **Does `DataChunk` come in pre-sized?** `duckdb-sqlite`'s scan calls `FlatVector::SetSize(output.data[col_idx], count)` explicitly for every column in addition to `output.SetCardinality(...)` (`duckdb-sqlite/src/sqlite_scanner.cpp:192-196, 200-201`). That per-vector `SetSize` looks like a newer main-only requirement; the skeleton only calls `SetCardinality`. **Verify against v1.5.4 whether `FlatVector::SetSize` exists / is required** — if scans return garbage or assert, this is why.
11. **`GetVirtualColumns()` returning an empty map** — I recommend this to avoid having to synthesise `rowid`, but I did not find code proving DuckDB tolerates a table with *no* rowid virtual column. `LogicalGet` handles virtual columns generically and the base default is the only thing that adds `rowid`, so it should be fine; **but if `SELECT * FROM m.points` fails with a rowid-related error, put the default back and make `ReadRange` emit `row_offset + i` for `column_id == COLUMN_IDENTIFIER_ROW_ID`.**
12. **`CreateTableInfo::catalog` / `::schema` assignment in `LoadTables`** — I set them for tidiness, but `TableCatalogEntry`'s constructor takes the catalog and schema as explicit arguments and only reads `info.table` for the name (`table_catalog_entry.cpp:26-33`). Harmless either way; unverified whether they're read anywhere downstream (e.g. `GetInfo()` / `ToSQL()`).
13. **Whether `DropEntry` should silently succeed when `if_not_found == RETURN_NULL`.** The skeleton returns quietly, mirroring SQLite's behaviour (`duckdb-sqlite/src/storage/sqlite_schema_entry.cpp:281-301`). Untested against `DROP TABLE IF EXISTS m.points`.
14. **`AccessMode::AUTOMATIC` vs explicit `READ_WRITE` on ATTACH.** `AttachOptions` defaults `access_mode` to `config.options.access_mode`, which is `AUTOMATIC` for a normal session — so the skeleton's "reject READ_WRITE" branch is deliberately a no-op comment. Whether you should instead *force* `options.access_mode = AccessMode::READ_ONLY` in `attach` (so `AttachedDatabase` reports `IsReadOnly()` and DuckDB's own guards fire before ours) is a **design decision I recommend but did not verify the consequences of**. Note `AttachedDatabase`'s constructor reads `options.access_mode` *before* calling `attach` (`src/main/attached_database.cpp:154-158`), so mutating it inside `attach` is **too late** to affect `AttachedDatabaseType`. To get read-only semantics you would have to require the user to write `(TYPE vtk, READ_ONLY)`, or rely purely on your own `BinderException`s. **This is worth resolving early.**

### Not investigated at all

15. Serialization of `bind_data` (`table_function_serialize_t` / `deserialize`, `table_function.hpp:338-340`, `verify_serialization = true` at `:484`). Needed for `EXPLAIN` plan round-tripping in some test harnesses and for distributed execution. sqlite_scanner does not implement it; presumably neither must we, but our `bind_data` holds a `shared_ptr<VtkFileHandle>` which is fundamentally unserializable. **If tests fail with a serialization verification error, set `verify_serialization = false`.**
16. `Catalog::OnDetach(ClientContext &)` (`catalog.hpp:407`) — the right hook to release the file handle deterministically on `DETACH` rather than at destruction. Not used in the skeleton.
17. `StorageExtension::OnCheckpointStart` / `OnCheckpointEnd` (`storage_extension.hpp:42-46`) — inherited no-ops; not exercised.
18. `CheckpointOptions` — appears in the `OnCheckpoint*` signatures and comes from `storage_manager.hpp`; contents not read.
19. `AttachInfo::parsed_options` (unbound `ParsedExpression`s, `attach_info.hpp:32`) vs `options` (bound `Value`s, `:34`). We only ever need the bound form; when and by whom `parsed_options` is bound was not traced.
20. The parser → `AttachStatement` → `LogicalCreateDatabase` → `PhysicalAttach` half of §1. I traced from `PhysicalAttach` downward (which is all our code interacts with); `src/execution/operator/schema/physical_attach.hpp` and `src/include/duckdb/planner/operator/logical_create_database.hpp` both 404'd at those paths, so the planner-side file locations are unconfirmed.
21. `duckdb-postgres` was checked out at a main-tracking commit (2026-07-25) and used only for the caching-strategy comparison in §5.2. **None of its signatures were adopted.**

---

*Every code block marked "verbatim" was copied from a file on disk in the checkout named at the top of this document. Blocks in §3 are newly written for this project and have never been compiled — treat them as a specification with the correct v1.5.4 signatures, not as tested code.*
