#pragma once

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/transaction/transaction_manager.hpp"
#include "model/vtk_table_schema.hpp"
#include "vtk/vtk_dataset.hpp"

namespace duckdb {

//! Single place for the read-only refusal, so every one of the ~30 write paths
//! produces an identical, actionable message (design §7).
[[noreturn]] void VtkRefuseWrite(const char *operation);

class VtkSchemaEntry;

//! One table of an attached VTK file.
class VtkTableEntry : public TableCatalogEntry {
public:
	VtkTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info,
	              std::shared_ptr<VtkDataset> dataset, std::shared_ptr<VtkSchemaSet> schemas, VtkTableKind kind);

	unique_ptr<BaseStatistics> GetStatistics(ClientContext &context, column_t column_id) override;
	TableFunction GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) override;
	TableStorageInfo GetStorageInfo(ClientContext &context) override;

private:
	std::shared_ptr<VtkDataset> dataset;
	std::shared_ptr<VtkSchemaSet> schemas;
	VtkTableKind kind;
};

//! The single schema (`main`) of an attached VTK file. Holds all six tables,
//! built eagerly because the file is already fully read, which makes DESCRIBE,
//! SHOW TABLES and autocomplete work with no lazy-loading machinery.
class VtkSchemaEntry : public SchemaCatalogEntry {
public:
	VtkSchemaEntry(Catalog &catalog, CreateSchemaInfo &info, std::shared_ptr<VtkDataset> dataset,
	               std::shared_ptr<VtkSchemaSet> schemas);

	void Scan(ClientContext &context, CatalogType type, const std::function<void(CatalogEntry &)> &callback) override;
	void Scan(CatalogType type, const std::function<void(CatalogEntry &)> &callback) override;

	optional_ptr<CatalogEntry> LookupEntry(CatalogTransaction transaction,
	                                       const EntryLookupInfo &lookup_info) override;

	// Every write path. Each throws; each is asserted by test/sql/read_only.test,
	// because an unimplemented pure virtual that crashes instead of throwing is
	// the most likely serious bug in this layer.
	optional_ptr<CatalogEntry> CreateIndex(CatalogTransaction transaction, CreateIndexInfo &info,
	                                       TableCatalogEntry &table) override;
	optional_ptr<CatalogEntry> CreateFunction(CatalogTransaction transaction, CreateFunctionInfo &info) override;
	optional_ptr<CatalogEntry> CreateTable(CatalogTransaction transaction, BoundCreateTableInfo &info) override;
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

private:
	std::shared_ptr<VtkDataset> dataset;
	std::shared_ptr<VtkSchemaSet> schemas;
	//! Keyed by lowercase table name: DuckDB resolves unquoted identifiers
	//! case-insensitively, so `m.POINTS` must find `points`.
	case_insensitive_map_t<unique_ptr<VtkTableEntry>> tables;
};

class VtkCatalog : public Catalog {
public:
	VtkCatalog(AttachedDatabase &db, std::string path, std::shared_ptr<VtkDataset> dataset);

	void Initialize(bool load_builtin) override;
	string GetCatalogType() override;

	optional_ptr<CatalogEntry> CreateSchema(CatalogTransaction transaction, CreateSchemaInfo &info) override;
	optional_ptr<SchemaCatalogEntry> LookupSchema(CatalogTransaction transaction,
	                                              const EntryLookupInfo &schema_lookup,
	                                              OnEntryNotFound if_not_found) override;
	void ScanSchemas(ClientContext &context, std::function<void(SchemaCatalogEntry &)> callback) override;
	void DropSchema(ClientContext &context, DropInfo &info) override;

	PhysicalOperator &PlanCreateTableAs(ClientContext &context, PhysicalPlanGenerator &planner,
	                                    LogicalCreateTable &op, PhysicalOperator &plan) override;
	PhysicalOperator &PlanInsert(ClientContext &context, PhysicalPlanGenerator &planner, LogicalInsert &op,
	                             optional_ptr<PhysicalOperator> plan) override;
	PhysicalOperator &PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner, LogicalDelete &op,
	                             PhysicalOperator &plan) override;
	PhysicalOperator &PlanUpdate(ClientContext &context, PhysicalPlanGenerator &planner, LogicalUpdate &op,
	                             PhysicalOperator &plan) override;

	//! Intercept index planning.
	//!
	//! Without this, DuckDB 1.4.5 crashes with
	//!   INTERNAL Error: Attempting to dereference an optional pointer that is not set
	//! on `CREATE INDEX ... ON <attached>.points`, because the 1.4 planner reaches
	//! into table storage that a non-DuckTable catalog entry does not have. DuckDB
	//! 1.5 happens to reject it earlier in the binder, which is why the v1.5.4-only
	//! test suite did not catch this. Design §7 requires a clean refusal on every
	//! write path, so intercept here rather than relying on the host version's
	//! binder to be defensive.
	unique_ptr<LogicalOperator> BindCreateIndex(Binder &binder, CreateStatement &stmt, TableCatalogEntry &table,
	                                            unique_ptr<LogicalOperator> plan) override;

	DatabaseSize GetDatabaseSize(ClientContext &context) override;
	bool InMemory() override;
	string GetDBPath() override;

	//! The catalog never changes after ATTACH, so a constant version lets DuckDB
	//! cache lookups instead of re-resolving them on every statement.
	optional_idx GetCatalogVersion(ClientContext &context) override;

private:
	std::string path;
	std::shared_ptr<VtkDataset> dataset;
	std::shared_ptr<VtkSchemaSet> schemas;
	unique_ptr<VtkSchemaEntry> main_schema;
};

//! Minimal read-only transaction manager.
//!
//! VtkDataset is immutable after attach, so there is genuinely nothing to
//! coordinate; transactions exist only because DuckDB requires one per attached
//! database. `Transaction` is concrete in v1.4/v1.5, so no subclass is needed.
class VtkTransactionManager : public TransactionManager {
public:
	VtkTransactionManager(AttachedDatabase &db, VtkCatalog &catalog);

	Transaction &StartTransaction(ClientContext &context) override;
	ErrorData CommitTransaction(ClientContext &context, Transaction &transaction) override;
	void RollbackTransaction(Transaction &transaction) override;
	void Checkpoint(ClientContext &context, bool force = false) override;

private:
	VtkCatalog &vtk_catalog;
	mutex transaction_lock;
	vector<unique_ptr<Transaction>> transactions;
};

//! Registers `ATTACH ... (TYPE vtk)`. Handles the one API difference between
//! DuckDB 1.4 (LTS) and 1.5 — see the implementation.
void VtkRegisterStorageExtension(DatabaseInstance &db);

} // namespace duckdb
