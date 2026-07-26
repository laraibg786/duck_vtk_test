#pragma once

#include "vtk/vtk_file_source.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class vtkAbstractArray;
class vtkDataObject;
class vtkDataSet;

namespace duckdb {

enum class VtkAssociation : uint8_t { POINT = 0, CELL = 1, FIELD = 2 };

const char *VtkAssociationName(VtkAssociation association);

//! Everything we know about one VTK array, gathered once at read time.
//! Deliberately free of VTK types so the relational layers can consume it.
struct VtkArrayInfo {
	VtkAssociation association = VtkAssociation::POINT;
	int32_t array_index = 0;
	std::string name;        //! as stored in the file
	std::string column_name; //! after collision handling (design §6)
	int32_t vtk_type = 0;
	std::string vtk_type_name;
	int32_t num_components = 1;
	int64_t num_tuples = 0;
	std::vector<std::string> component_names;
	std::string active_as; //! SCALARS / VECTORS / ... comma-joined, empty if none
	bool is_string_array = false;
};

//! Uniform, read-only view over a VTK dataset.
//!
//! Hides the genuinely different APIs of vtkUnstructuredGrid, vtkPolyData,
//! vtkImageData, vtkRectilinearGrid and vtkStructuredGrid so that no table
//! function needs a five-way switch.
//!
//! IMMUTABLE after construction. That is what allows every scan to share one
//! instance without locking, and it is the basis for parallelising scans later.
//!
//! Two rules the implementation upholds and callers must not undermine:
//!   * `vtkDataSet::GetCell()` is NEVER called. It returns a shared per-object
//!     scratch cell, which is a data race under concurrent scans and can return
//!     wrong results even single-threaded when interleaved. Connectivity comes
//!     from `GetCellPoints` with a thread-local vtkIdList.
//!   * Reads happen inside a VtkErrorScope so that VTK's output-window-only
//!     failure signals are seen rather than lost.
class VtkDataset {
public:
	//! Reads `path` eagerly and completely. Throws IOException with an actionable
	//! message if the file is missing, not VTK, truncated, or an unsupported
	//! composite type. Never returns a partially-populated object.
	//!
	//! `source` decides how bytes are obtained. For a local path VTK is given the
	//! filename directly, so only VTK holds the data. For a remote object the bytes
	//! are fetched through the source and parsed from memory, which costs one extra
	//! copy for the duration of the parse. Pass nullptr for local-only behaviour.
	static std::shared_ptr<VtkDataset> Read(const std::string &path, VtkFileSource *source = nullptr);

	~VtkDataset();
	VtkDataset(const VtkDataset &) = delete;
	VtkDataset &operator=(const VtkDataset &) = delete;

	int64_t NumPoints() const {
		return num_points;
	}
	int64_t NumCells() const {
		return num_cells;
	}
	//! Total connectivity entries == sum(cell sizes) == cardinality of cell_points.
	int64_t NumCellPoints() const {
		return num_cell_points;
	}
	//! Largest cell size, so LIST children can be reserved exactly once.
	int32_t MaxCellSize() const {
		return max_cell_size;
	}

	//! Coordinates. Always double, synthesised for implicit-geometry datasets.
	void GetPoint(int64_t point_id, double out[3]) const;

	int32_t GetCellType(int64_t cell_id) const;
	//! Appends the cell's point ids to `out` in VTK's canonical vertex order.
	//! Order is semantically load-bearing (it defines face/normal orientation)
	//! and is never sorted. Returns the number appended.
	int32_t GetCellPoints(int64_t cell_id, std::vector<int64_t> &out) const;

	const std::vector<VtkArrayInfo> &Arrays() const {
		return arrays;
	}
	//! Opaque handle for the column writer. Null if the array vanished.
	vtkAbstractArray *Array(const VtkArrayInfo &info) const;

	const std::string &Path() const {
		return path;
	}
	const std::string &ReaderClass() const {
		return reader_class;
	}
	//! "local", "https", ... — surfaced in vtk_info so a user can see where a
	//! dataset actually came from.
	const std::string &SourceKind() const {
		return source_kind;
	}
	const std::string &DatasetClass() const {
		return dataset_class;
	}
	int64_t FileSizeBytes() const {
		return file_size_bytes;
	}
	//! False for an empty dataset, where VTK reports an inverted sentinel box
	//! rather than anything meaningful.
	bool HasBounds() const {
		return has_bounds;
	}
	const double *Bounds() const {
		return bounds;
	}

	//! Human-readable dump for vtk_debug_dump(), to isolate "is our mapping wrong
	//! or is VTK reporting something unexpected" in a single query.
	std::string DebugDump() const;

private:
	VtkDataset() = default;
	void Initialise();
	void CollectArrays();
	//! The held object. Non-null always; `dataset` is null for field-only files.
	vtkDataObject *DataObject() const;

	void *object_ref = nullptr; //! vtkSmartPointer<vtkDataObject> held opaquely
	//! Null for a legacy `DATASET FIELD` file, which has arrays but no geometry.
	//! Every geometry accessor must therefore tolerate a null dataset; they are
	//! only ever reached when NumPoints()/NumCells() are non-zero, which cannot
	//! happen in that case.
	vtkDataSet *dataset = nullptr;

	std::string path;
	std::string reader_class;
	std::string source_kind = "local";
	std::string dataset_class;
	int64_t file_size_bytes = 0;
	int64_t num_points = 0;
	int64_t num_cells = 0;
	int64_t num_cell_points = 0;
	int32_t max_cell_size = 0;
	bool has_bounds = false;
	double bounds[6] = {0, 0, 0, 0, 0, 0};
	std::vector<VtkArrayInfo> arrays;
};

//! Process-wide cache so that `vtk_points('f') JOIN vtk_cells('f')` reads the
//! file once. Entries are weak, so the dataset is released as soon as the last
//! scan and any attached catalog let go of it.
//!
//! The cache matters more for remote files than local ones: without it, a query
//! joining two tables of the same URL would download it twice.
std::shared_ptr<VtkDataset> VtkGetCachedDataset(const std::string &path, VtkFileSource *source = nullptr);

} // namespace duckdb
