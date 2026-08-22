#include "vtk/vtk_file_source.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"

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

class DuckDBFileSource : public VtkFileSource {
public:
	explicit DuckDBFileSource(FileSystem &fs) : fs(fs) {
	}

	bool IsLocalPath(const std::string &path) override {
		// FileSystem::IsRemoteFile understands every scheme DuckDB knows about, so
		// this stays correct as the user loads httpfs, azure, and so on.
		return !FileSystem::IsRemoteFile(path);
	}

	bool Exists(const std::string &path) override {
		try {
			return fs.FileExists(path);
		} catch (const PermissionException &) {
			// Deliberately NOT swallowed. DuckDB's sandbox (enable_external_access,
			// allowed_directories) signals refusal by throwing, and the catch-all this
			// replaced turned "you are not permitted to read this" into
			// "duck_vtk: cannot read '...': no such file" — sending the user off to
			// hunt for a typo. DuckDB core reports
			//   Permission Error: Cannot access file "..." - file system operations
			//   are disabled by configuration
			// and we should say the same thing rather than contradict it.
			throw;
		} catch (const IOException &) {
			// An unreachable host or a genuinely missing object. The caller turns this
			// into a message that names the filesystem extensions to load.
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
		string extension;
		if (FileSystem::IsRemoteFile(path, extension)) {
			return extension.empty() ? "remote" : extension;
		}
		return "local";
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
