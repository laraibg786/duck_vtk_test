#include "catalog/vtk_catalog.hpp"

#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/storage/database_size.hpp"
#include "duckdb/storage/storage_extension.hpp"
#include "duckdb/storage/table_storage_info.hpp"
#include "duckdb/transaction/transaction.hpp"
#include "functions/vtk_table_functions.hpp"
#include "vtk/vtk_file_source.hpp"

namespace duckdb {

void VtkRefuseWrite(const char *operation) {
	throw NotImplementedException("duck_vtk: attached VTK databases are read-only (attempted: %s)", operation);
}

//===--------------------------------------------------------------------===//
// VtkTableEntry
//===--------------------------------------------------------------------===//

VtkTableEntry::VtkTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info,
                             std::shared_ptr<VtkDataset> dataset_p, std::shared_ptr<VtkSchemaSet> schemas_p,
                             VtkTableKind kind_p)
    : TableCatalogEntry(catalog, schema, info), dataset(std::move(dataset_p)), schemas(std::move(schemas_p)),
      kind(kind_p) {
}

unique_ptr<BaseStatistics> VtkTableEntry::GetStatistics(ClientContext &, column_t) {
	// nullptr is a valid "no statistics available". Phase 1 does not compute any.
	return nullptr;
}

TableFunction VtkTableEntry::GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) {
	// THE integration point. Returns the SAME TableFunction the standalone
	// vtk_points()/vtk_cells()/... functions use, with bind data pre-supplied so
	// the function's own bind callback is never invoked on this path.
	//
	// One scan implementation means `SELECT * FROM m.points` and
	// `SELECT * FROM vtk_points('f.vtu')` cannot diverge — which is exactly why
	// Phase 2 built the table functions before this layer existed.
	bind_data = VtkMakeBindData(dataset, schemas, kind);
	return VtkGetScanFunction(kind);
}

TableStorageInfo VtkTableEntry::GetStorageInfo(ClientContext &) {
	TableStorageInfo result;
	result.cardinality = static_cast<idx_t>(VtkTableRowCount(*dataset, *schemas, kind));
	return result;
}

//===--------------------------------------------------------------------===//
// VtkSchemaEntry
//===--------------------------------------------------------------------===//

VtkSchemaEntry::VtkSchemaEntry(Catalog &catalog, CreateSchemaInfo &info, std::shared_ptr<VtkDataset> dataset_p,
                               std::shared_ptr<VtkSchemaSet> schemas_p)
    : SchemaCatalogEntry(catalog, info), dataset(std::move(dataset_p)), schemas(std::move(schemas_p)) {
	for (auto kind : VtkAllTableKinds()) {
		auto &table_schema = schemas->Get(kind);

		// Column metadata is not virtual on TableCatalogEntry: it is held in a
		// ColumnList populated from the CreateTableInfo we pass to the constructor.
		// So the schema is declared here rather than by overriding GetColumn().
		CreateTableInfo table_info(catalog.GetName(), info.schema, VtkTableName(kind));
		for (auto &column : table_schema.columns) {
			table_info.columns.AddColumn(ColumnDefinition(column.name, column.type));
		}
		tables[VtkTableName(kind)] = make_uniq<VtkTableEntry>(catalog, *this, table_info, dataset, schemas, kind);
	}
}

void VtkSchemaEntry::Scan(ClientContext &, CatalogType type, const std::function<void(CatalogEntry &)> &callback) {
	Scan(type, callback);
}

void VtkSchemaEntry::Scan(CatalogType type, const std::function<void(CatalogEntry &)> &callback) {
	if (type != CatalogType::TABLE_ENTRY) {
		return; // we expose nothing but tables
	}
	// Iterate in the declared order rather than map order, so SHOW TABLES and
	// duckdb_tables() are deterministic.
	for (auto kind : VtkAllTableKinds()) {
		auto entry = tables.find(VtkTableName(kind));
		if (entry != tables.end()) {
			callback(*entry->second);
		}
	}
}

optional_ptr<CatalogEntry> VtkSchemaEntry::LookupEntry(CatalogTransaction, const EntryLookupInfo &lookup_info) {
	if (lookup_info.GetCatalogType() != CatalogType::TABLE_ENTRY) {
		return nullptr;
	}
	auto entry = tables.find(lookup_info.GetEntryName());
	if (entry == tables.end()) {
		// Returning nullptr rather than throwing lets DuckDB produce its own
		// "table does not exist" error, complete with did-you-mean suggestions.
		return nullptr;
	}
	return entry->second.get();
}

optional_ptr<CatalogEntry> VtkSchemaEntry::CreateIndex(CatalogTransaction, CreateIndexInfo &, TableCatalogEntry &) {
	VtkRefuseWrite("CREATE INDEX");
}
optional_ptr<CatalogEntry> VtkSchemaEntry::CreateFunction(CatalogTransaction, CreateFunctionInfo &) {
	VtkRefuseWrite("CREATE FUNCTION");
}
optional_ptr<CatalogEntry> VtkSchemaEntry::CreateTable(CatalogTransaction, BoundCreateTableInfo &) {
	VtkRefuseWrite("CREATE TABLE");
}
optional_ptr<CatalogEntry> VtkSchemaEntry::CreateView(CatalogTransaction, CreateViewInfo &) {
	VtkRefuseWrite("CREATE VIEW");
}
optional_ptr<CatalogEntry> VtkSchemaEntry::CreateSequence(CatalogTransaction, CreateSequenceInfo &) {
	VtkRefuseWrite("CREATE SEQUENCE");
}
optional_ptr<CatalogEntry> VtkSchemaEntry::CreateTableFunction(CatalogTransaction, CreateTableFunctionInfo &) {
	VtkRefuseWrite("CREATE TABLE FUNCTION");
}
optional_ptr<CatalogEntry> VtkSchemaEntry::CreateCopyFunction(CatalogTransaction, CreateCopyFunctionInfo &) {
	VtkRefuseWrite("CREATE COPY FUNCTION");
}
optional_ptr<CatalogEntry> VtkSchemaEntry::CreatePragmaFunction(CatalogTransaction, CreatePragmaFunctionInfo &) {
	VtkRefuseWrite("CREATE PRAGMA FUNCTION");
}
optional_ptr<CatalogEntry> VtkSchemaEntry::CreateCollation(CatalogTransaction, CreateCollationInfo &) {
	VtkRefuseWrite("CREATE COLLATION");
}
optional_ptr<CatalogEntry> VtkSchemaEntry::CreateType(CatalogTransaction, CreateTypeInfo &) {
	VtkRefuseWrite("CREATE TYPE");
}
void VtkSchemaEntry::DropEntry(ClientContext &, DropInfo &) {
	VtkRefuseWrite("DROP");
}
void VtkSchemaEntry::Alter(CatalogTransaction, AlterInfo &) {
	VtkRefuseWrite("ALTER");
}

//===--------------------------------------------------------------------===//
// VtkCatalog
//===--------------------------------------------------------------------===//

VtkCatalog::VtkCatalog(AttachedDatabase &db, std::string path_p, std::shared_ptr<VtkDataset> dataset_p)
    : Catalog(db), path(std::move(path_p)), dataset(std::move(dataset_p)) {
	// Built once here, then shared with every table entry and every scan. The
	// dataset is immutable, so this needs no locking.
	schemas = std::make_shared<VtkSchemaSet>(VtkBuildSchemas(*dataset));
}

void VtkCatalog::Initialize(bool) {
	CreateSchemaInfo info;
	info.schema = DEFAULT_SCHEMA;
	main_schema = make_uniq<VtkSchemaEntry>(*this, info, dataset, schemas);
}

string VtkCatalog::GetCatalogType() {
	return "vtk";
}

optional_ptr<SchemaCatalogEntry> VtkCatalog::LookupSchema(CatalogTransaction, const EntryLookupInfo &schema_lookup,
                                                          OnEntryNotFound if_not_found) {
	// Only LookupSchema is virtual; the many GetSchema overloads are non-virtual
	// conveniences that funnel through here, so overriding this is sufficient.
	auto &name = schema_lookup.GetEntryName();
	if (name.empty() || StringUtil::CIEquals(name, DEFAULT_SCHEMA)) {
		return main_schema.get();
	}
	if (if_not_found == OnEntryNotFound::THROW_EXCEPTION) {
		throw BinderException("duck_vtk: schema \"%s\" not found; attached VTK databases expose only \"main\"", name);
	}
	return nullptr;
}

void VtkCatalog::ScanSchemas(ClientContext &, std::function<void(SchemaCatalogEntry &)> callback) {
	callback(*main_schema);
}

optional_idx VtkCatalog::GetCatalogVersion(ClientContext &) {
	return optional_idx(1);
}

unique_ptr<LogicalOperator> VtkCatalog::BindCreateIndex(Binder &, CreateStatement &, TableCatalogEntry &,
                                                        unique_ptr<LogicalOperator>) {
	// See the header for why this override exists: DuckDB 1.4.5 otherwise reaches a
	// null optional_ptr dereference here instead of reporting a refusal.
	// Wording note: DuckDB 1.5's binder rejects CREATE INDEX before reaching this
	// override, with "can only create an index on a base table". Phrasing ours as
	// "... an index on a read-only VTK database" gives the two messages the common
	// substring "index on a", so one test assertion covers both version lines.
	throw BinderException("duck_vtk: cannot create an index on a read-only VTK database");
}

DatabaseSize VtkCatalog::GetDatabaseSize(ClientContext &) {
	DatabaseSize size;
	// Honest answer: the on-disk size of the file we read. The block-based fields
	// are meaningless for a file-backed read-only source and stay zero.
	size.bytes = static_cast<idx_t>(dataset->FileSizeBytes());
	return size;
}

bool VtkCatalog::InMemory() {
	return false;
}

string VtkCatalog::GetDBPath() {
	return path;
}

optional_ptr<CatalogEntry> VtkCatalog::CreateSchema(CatalogTransaction, CreateSchemaInfo &) {
	VtkRefuseWrite("CREATE SCHEMA");
}

void VtkCatalog::DropSchema(ClientContext &, DropInfo &) {
	VtkRefuseWrite("DROP SCHEMA");
}

// The Plan* methods are only reached if the binder let a write through. These use
// BinderException rather than NotImplementedException: for a read-only catalog
// they can never work, as opposed to "not built yet".
PhysicalOperator &VtkCatalog::PlanCreateTableAs(ClientContext &, PhysicalPlanGenerator &, LogicalCreateTable &,
                                                PhysicalOperator &) {
	throw BinderException("duck_vtk: cannot CREATE TABLE AS in a read-only VTK database");
}
PhysicalOperator &VtkCatalog::PlanInsert(ClientContext &, PhysicalPlanGenerator &, LogicalInsert &,
                                         optional_ptr<PhysicalOperator>) {
	throw BinderException("duck_vtk: cannot INSERT into a read-only VTK database");
}
PhysicalOperator &VtkCatalog::PlanDelete(ClientContext &, PhysicalPlanGenerator &, LogicalDelete &,
                                         PhysicalOperator &) {
	throw BinderException("duck_vtk: cannot DELETE from a read-only VTK database");
}
PhysicalOperator &VtkCatalog::PlanUpdate(ClientContext &, PhysicalPlanGenerator &, LogicalUpdate &,
                                         PhysicalOperator &) {
	throw BinderException("duck_vtk: cannot UPDATE a read-only VTK database");
}

//===--------------------------------------------------------------------===//
// VtkTransactionManager
//===--------------------------------------------------------------------===//

VtkTransactionManager::VtkTransactionManager(AttachedDatabase &db, VtkCatalog &catalog)
    : TransactionManager(db), vtk_catalog(catalog) {
}

Transaction &VtkTransactionManager::StartTransaction(ClientContext &context) {
	auto transaction = make_uniq<Transaction>(*this, context);
	auto &result = *transaction;
	lock_guard<mutex> guard(transaction_lock);
	transactions.push_back(std::move(transaction));
	return result;
}

ErrorData VtkTransactionManager::CommitTransaction(ClientContext &, Transaction &transaction) {
	lock_guard<mutex> guard(transaction_lock);
	for (idx_t i = 0; i < transactions.size(); i++) {
		if (transactions[i].get() == &transaction) {
			transactions.erase(transactions.begin() + static_cast<int64_t>(i));
			break;
		}
	}
	return ErrorData(); // default-constructed == success
}

void VtkTransactionManager::RollbackTransaction(Transaction &transaction) {
	// Nothing was ever modified, so dropping the object IS the whole rollback.
	lock_guard<mutex> guard(transaction_lock);
	for (idx_t i = 0; i < transactions.size(); i++) {
		if (transactions[i].get() == &transaction) {
			transactions.erase(transactions.begin() + static_cast<int64_t>(i));
			break;
		}
	}
}

void VtkTransactionManager::Checkpoint(ClientContext &, bool) {
	// Deliberately a no-op rather than an error: a global CHECKPOINT touches every
	// attached database, and failing it because a read-only VTK file is attached
	// would be hostile.
}

//===--------------------------------------------------------------------===//
// Storage extension
//===--------------------------------------------------------------------===//

namespace {

unique_ptr<Catalog> VtkAttach(optional_ptr<StorageExtensionInfo>, ClientContext &context, AttachedDatabase &db,
                              const string &, AttachInfo &info, AttachOptions &options) {
	// Refuse an explicit READ_WRITE rather than silently ignoring it: quietly
	// accepting a write-mode request that can never be honoured is worse than
	// failing (design §2).
	if (options.access_mode == AccessMode::READ_WRITE) {
		throw NotImplementedException(
		    "duck_vtk: VTK databases are read-only; remove READ_WRITE from the ATTACH options");
	}

	// Reject unknown options so that adding real ones later is not a silent
	// behaviour change. AttachOptions::options is a plain unordered_map with no
	// case normalisation applied, so compare case-insensitively ourselves.
	for (auto &option : options.options) {
		auto key = StringUtil::Lower(option.first);
		if (key != "type" && key != "read_only") {
			throw BinderException("duck_vtk: unknown ATTACH option \"%s\". Accepted: TYPE, READ_ONLY. "
			                      "(vector_layout, block and time_step are planned but not implemented.)",
			                      option.first);
		}
	}

	// Eager, complete read. Schema discovery for legacy formats requires it, and
	// failing here means the user sees the error at ATTACH rather than at first
	// SELECT. Throws IOException on a missing/corrupt file.
	// Same VFS routing as the table functions, so
	// ATTACH 'https://.../mesh.vtu' AS m (TYPE vtk) works with httpfs loaded.
	auto source = VtkMakeDuckDBFileSource(context);
	auto dataset = VtkGetCachedDataset(info.path, source.get());

	// Mark the attached database read-only so DuckDB AGREES with us.
	//
	// Mutating options.access_mode here would be too late: AttachedDatabase sets
	// its type from access_mode BEFORE calling attach(), so duckdb_databases()
	// reported readonly = false for a catalog that throws on every write path and
	// explicitly refuses READ_WRITE above. SetReadOnlyDatabase() is the supported
	// way to say it after the fact.
	db.SetReadOnlyDatabase();

	return make_uniq<VtkCatalog>(db, info.path, std::move(dataset));
}

unique_ptr<TransactionManager> VtkCreateTransactionManager(optional_ptr<StorageExtensionInfo>, AttachedDatabase &db,
                                                           Catalog &catalog) {
	return make_uniq<VtkTransactionManager>(db, catalog.Cast<VtkCatalog>());
}

class VtkStorageExtension : public StorageExtension {
public:
	VtkStorageExtension() {
		attach = VtkAttach;
		create_transaction_manager = VtkCreateTransactionManager;
	}
};

} // namespace

void VtkRegisterStorageExtension(DatabaseInstance &db) {
	auto &config = DBConfig::GetConfig(db);

	// THE ONLY API DIFFERENCE between DuckDB 1.4 (LTS) and 1.5 that affects this
	// extension, so it is the only place needing a version shim:
	//
	//   1.4.x: `DBConfig::storage_extensions` is a public
	//          case_insensitive_map_t<unique_ptr<StorageExtension>>.
	//   1.5.x: that member is gone; registration goes through the static
	//          `StorageExtension::Register(config, name, shared_ptr)`.
	//
	// DUCK_VTK_HAS_STORAGE_EXTENSION_REGISTER is set by cmake/DuckVTKFindVTK-adjacent
	// logic in CMakeLists.txt, which greps the actual duckdb header rather than
	// trusting a version string — so this adapts even to an unreleased revision.
#if defined(DUCK_VTK_HAS_STORAGE_EXTENSION_REGISTER) && DUCK_VTK_HAS_STORAGE_EXTENSION_REGISTER
	StorageExtension::Register(config, "vtk", make_shared_ptr<VtkStorageExtension>());
#else
	config.storage_extensions["vtk"] = make_uniq<VtkStorageExtension>();
#endif
}

} // namespace duckdb
