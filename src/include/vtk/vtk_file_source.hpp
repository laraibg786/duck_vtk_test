#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace duckdb {

class ClientContext;
class DatabaseInstance;

//! How VtkDataset obtains file bytes.
//!
//! Exists so the VTK layer can read remote objects without knowing anything about
//! DuckDB: the interface is plain C++, and the DuckDB-backed implementation lives
//! in its own translation unit. That keeps `vtk_dataset.cpp` free of filesystem
//! policy and makes the read path testable with a stub.
//!
//! Why this is needed at all: VTK's readers do their own file I/O with ifstream.
//! Handing them an "https://..." path fails with "no such file", because neither
//! VTK nor std::filesystem knows what a URL is. Routing through DuckDB's virtual
//! filesystem instead means every scheme DuckDB can open — https, s3, gcs, azure —
//! works the moment the user has the corresponding extension loaded. We do not
//! depend on httpfs; we simply stop bypassing the VFS.
class VtkFileSource {
public:
	virtual ~VtkFileSource() = default;

	//! True when the path is something VTK's own file I/O can open directly. For
	//! those we hand VTK the filename, which avoids holding the whole file in our
	//! memory *and* VTK's at the same time.
	virtual bool IsLocalPath(const std::string &path) = 0;

	virtual bool Exists(const std::string &path) = 0;

	//! Reads the entire object. Only called when IsLocalPath() is false, or when
	//! the memory path is being exercised deliberately (see VtkForceMemoryReads).
	virtual void ReadAll(const std::string &path, std::string &out) = 0;

	//! For diagnostics — a scheme name like "local" or "https".
	virtual std::string Describe(const std::string &path) = 0;
};

//! A source backed by DuckDB's virtual filesystem.
std::unique_ptr<VtkFileSource> VtkMakeDuckDBFileSource(ClientContext &context);
std::unique_ptr<VtkFileSource> VtkMakeDuckDBFileSource(DatabaseInstance &db);

//! A source that only understands local paths. Used when no ClientContext is
//! available, so behaviour degrades to exactly what it was before remote support.
std::unique_ptr<VtkFileSource> VtkMakeLocalFileSource();

//! Test hook: when DUCK_VTK_FORCE_MEMORY_READ=1, local files are also read through
//! ReadAll() and parsed from memory.
//!
//! This exists because the in-memory parse path is otherwise only reachable with a
//! remote filesystem extension loaded, which would leave it untested here. With the
//! flag set, the entire corpus can be run through it and compared against the
//! path-based results — see test/sql/remote.test and `make test-memory-reads`.
bool VtkForceMemoryReads();

} // namespace duckdb
