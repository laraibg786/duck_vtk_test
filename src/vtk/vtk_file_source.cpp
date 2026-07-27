#include "vtk/vtk_file_source.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace duckdb {

bool VtkForceMemoryReads() {
	const char *v = std::getenv("DUCK_VTK_FORCE_MEMORY_READ");
	return v && *v && std::strcmp(v, "0") != 0;
}

namespace {

//! Refuse absurd sizes rather than attempting the allocation.
//!
//! A mistyped URL that returns an HTML page is harmless, but a bucket listing or a
//! multi-terabyte object would otherwise try to allocate itself into the database
//! process. 64 GiB is far above any real mesh and far below "kills the machine".
constexpr int64_t MAX_IN_MEMORY_BYTES = int64_t(64) * 1024 * 1024 * 1024;

//! Returns the URL scheme of a path ("s3", "ssh", "sftp", …), or "" if it has none.
//!
//! Deliberately NOT FileSystem::IsRemoteFile(). That matches the path against
//! EXTENSION_FILE_PREFIXES, a *hardcoded* table in DuckDB core
//! (`src/include/duckdb/main/extension_entries.hpp`) that lists only the schemes
//! owned by two core extensions: http/https/s3/s3a/s3n/gcs/gs/r2/hf (httpfs) and
//! azure/az/abfss (azure). Every *other* filesystem extension registers itself with
//! `FileSystem::RegisterSubSystem()` and is dispatched by `CanHandleFile()`, which
//! that table knows nothing about — including the community `sshfs` (`ssh://`),
//! `cloudfs` (`sftp://`, `gdfs://`, `spfs://`, …) and `duckdb_opendalfs`.
//!
//! Verified against DuckDB v1.5.5: `read_csv('ssh://host/x.csv')` falls through to
//! the LOCAL filesystem's glob and reports "No files found that match the pattern",
//! whereas `read_csv('s3://…')` raises MissingExtensionException. So
//! `IsRemoteFile("ssh://…")` is false even with sshfs installed and loaded.
//!
//! Using IsRemoteFile here therefore classified `ssh://host/mesh.vtu` as a LOCAL
//! path and handed the URL straight to VTK's `ifstream`, which cannot open a URL —
//! failing on a file DuckDB itself could read perfectly well. Scheme detection has
//! no list to fall out of date: every present and future filesystem extension is
//! routed through the VFS automatically.
//!
//! `file://` is included on purpose. DuckDB's LocalFileSystem strips that prefix in
//! ExpandPath(), but VTK does not, so it must take the read-through-VFS path too.
std::string UrlScheme(const std::string &path) {
	// RFC 3986: scheme = ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ), then "://".
	const auto sep = path.find("://");
	if (sep == std::string::npos || sep == 0) {
		return "";
	}
	// A one-character "scheme" is a Windows drive letter, never a real scheme.
	// Requiring two rules out `C://path` without special-casing the platform.
	if (sep < 2) {
		return "";
	}
	if (!std::isalpha(static_cast<unsigned char>(path[0]))) {
		return "";
	}
	for (size_t i = 1; i < sep; i++) {
		const unsigned char c = static_cast<unsigned char>(path[i]);
		if (!std::isalnum(c) && c != '+' && c != '-' && c != '.') {
			return "";
		}
	}
	return StringUtil::Lower(path.substr(0, sep));
}

class DuckDBFileSource : public VtkFileSource {
public:
	explicit DuckDBFileSource(FileSystem &fs) : fs(fs) {
	}

	bool IsLocalPath(const std::string &path) override {
		// "Local" means exactly one thing here: VTK's own file I/O can open it.
		// That is true when there is no URL scheme, and false otherwise —
		// regardless of which extension, if any, can handle that scheme.
		return UrlScheme(path).empty();
	}

	bool Exists(const std::string &path) override {
		try {
			return fs.FileExists(path);
		} catch (...) {
			// Some remote filesystems throw rather than returning false for an
			// unreachable host or a missing credential. Treat that as "not usable
			// here" and let the caller produce the actionable message.
			return false;
		}
	}

	void ReadAll(const std::string &path, std::string &out) override {
		auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ);
		if (!handle) {
			throw IOException("duck_vtk: could not open '%s'", path);
		}
		const int64_t size = static_cast<int64_t>(handle->GetFileSize());
		if (size < 0) {
			throw IOException("duck_vtk: '%s' reported a negative size", path);
		}
		if (size > MAX_IN_MEMORY_BYTES) {
			throw IOException("duck_vtk: '%s' is %lld bytes, above the %lld-byte limit for reading a "
			                  "remote VTK file into memory. Copy it to local storage first.",
			                  path, (long long)size, (long long)MAX_IN_MEMORY_BYTES);
		}
		out.resize(static_cast<size_t>(size));
		if (size == 0) {
			return;
		}

		// Loop: a single Read() is not guaranteed to return everything, and for HTTP
		// sources it frequently does not. Reading short and parsing the truncated
		// result is exactly the silent-corruption failure this project has already
		// been bitten by once, so the loop is not optional.
		idx_t offset = 0;
		while (offset < static_cast<idx_t>(size)) {
			const int64_t got = fs.Read(*handle, &out[offset], size - static_cast<int64_t>(offset));
			if (got <= 0) {
				throw IOException("duck_vtk: short read on '%s' — got %llu of %lld bytes", path,
				                  (unsigned long long)offset, (long long)size);
			}
			offset += static_cast<idx_t>(got);
		}
	}

	std::string Describe(const std::string &path) override {
		const auto scheme = UrlScheme(path);
		return scheme.empty() ? "local" : scheme;
	}

private:
	FileSystem &fs;
};

//! Fallback with no DuckDB filesystem behind it.
//!
//! Used when there is no ClientContext to hand us one. It deliberately reports
//! every path as local, so behaviour is byte-for-byte what it was before remote
//! support existed rather than silently degrading in some new way.
class LocalOnlyFileSource : public VtkFileSource {
public:
	bool IsLocalPath(const std::string &) override {
		return true;
	}

	bool Exists(const std::string &path) override {
		std::error_code ec;
		return std::filesystem::is_regular_file(std::filesystem::path(path), ec) && !ec;
	}

	void ReadAll(const std::string &path, std::string &out) override {
		// Only reachable via the DUCK_VTK_FORCE_MEMORY_READ test hook.
		std::error_code ec;
		const auto size = std::filesystem::file_size(std::filesystem::path(path), ec);
		if (ec) {
			throw IOException("duck_vtk: could not size '%s'", path);
		}
		out.resize(static_cast<size_t>(size));
		FILE *f = std::fopen(path.c_str(), "rb");
		if (!f) {
			throw IOException("duck_vtk: could not open '%s'", path);
		}
		const size_t got = size ? std::fread(&out[0], 1, static_cast<size_t>(size), f) : 0;
		std::fclose(f);
		if (got != static_cast<size_t>(size)) {
			throw IOException("duck_vtk: short read on '%s'", path);
		}
	}

	std::string Describe(const std::string &) override {
		return "local";
	}
};

} // namespace

std::unique_ptr<VtkFileSource> VtkMakeDuckDBFileSource(ClientContext &context) {
	return std::unique_ptr<VtkFileSource>(new DuckDBFileSource(FileSystem::GetFileSystem(context)));
}

std::unique_ptr<VtkFileSource> VtkMakeDuckDBFileSource(DatabaseInstance &db) {
	return std::unique_ptr<VtkFileSource>(new DuckDBFileSource(FileSystem::GetFileSystem(db)));
}

std::unique_ptr<VtkFileSource> VtkMakeLocalFileSource() {
	return std::unique_ptr<VtkFileSource>(new LocalOnlyFileSource());
}

} // namespace duckdb
