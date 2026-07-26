#include "vtk/vtk_dataset.hpp"

#include "vtk/vtk_error_scope.hpp"
#include "vtk/vtk_file_source.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"

#include <vtkCharArray.h>
#include <vtkCellData.h>
#include <vtkCellTypes.h>
#include <vtkDataArray.h>
#include <vtkDataObject.h>
#include <vtkDataObjectReader.h>
#include <vtkDataSet.h>
#include <vtkDataSetAttributes.h>
#include <vtkErrorCode.h>
#include <vtkFieldData.h>
#include <vtkGenericDataObjectReader.h>
#include <vtkIdList.h>
#include <vtkNew.h>
#include <vtkPointData.h>
#include <vtkPointSet.h>
#include <vtkPoints.h>
#include <vtkSmartPointer.h>
#include <vtkStringArray.h>
#include <vtkType.h>
#include <vtkXMLGenericDataObjectReader.h>
#include <vtkXMLImageDataReader.h>
#include <vtkXMLPolyDataReader.h>
#include <vtkXMLRectilinearGridReader.h>
#include <vtkXMLStructuredGridReader.h>
#include <vtkXMLUnstructuredGridReader.h>

#include <cmath>
#include <filesystem>
#include <map>
#include <mutex>

namespace duckdb {

const char *VtkAssociationName(VtkAssociation association) {
	switch (association) {
	case VtkAssociation::POINT:
		return "POINT";
	case VtkAssociation::CELL:
		return "CELL";
	default:
		return "FIELD";
	}
}

namespace {

// std::filesystem rather than POSIX stat(): stat()/S_ISREG do not exist under
// MSVC, and this was the only thing in the extension blocking a Windows build.
// The error_code overloads are used so a permission problem or a race yields a
// clean false/0 instead of throwing from inside a read path.
int64_t FileSize(const std::string &path) {
	std::error_code ec;
	const auto size = std::filesystem::file_size(std::filesystem::path(path), ec);
	if (ec) {
		return 0;
	}
	return static_cast<int64_t>(size);
}

bool FileExists(const std::string &path) {
	std::error_code ec;
	// is_regular_file, not exists(): a directory or a device node handed to a VTK
	// reader produces far more confusing failures than "no such file".
	return std::filesystem::is_regular_file(std::filesystem::path(path), ec) && !ec;
}

//! One vtkIdList per thread, reused across rows.
//!
//! Avoids both a per-row allocation and the shared-scratch hazard of
//! vtkDataSet::GetCell(). thread_local keeps it correct once scans go parallel.
vtkIdList &CellScratch() {
	static thread_local vtkSmartPointer<vtkIdList> scratch = vtkSmartPointer<vtkIdList>::New();
	return *scratch;
}

//! Reader dispatch, on CONTENT rather than file extension.
//!
//! Phase 0 measured that vtkXMLGenericDataObjectReader::CanReadFile() returns 0
//! for every file, including a valid .vtu — it is not overridden on the generic
//! reader. ReadOutputType() is the working sniffing API. See
//! docs/PHASE0_RESULTS.md §3 and the correction box in research doc 03 §2.
vtkSmartPointer<vtkDataObject> ReadDataObject(const std::string &path, VtkErrorScope &scope,
                                              std::string &reader_class, bool &was_parallel) {
	was_parallel = false;

	{
		vtkNew<vtkXMLGenericDataObjectReader> xml;
		bool parallel = false;
		const int output_type = xml->ReadOutputType(path.c_str(), parallel);
		// Probe noise: ReadOutputType writes an ERR line for every legacy file.
		// Discard it, or a perfectly good legacy read reports a scary error.
		if (output_type < 0) {
			scope.Clear();
		} else {
			was_parallel = parallel;
			xml->SetFileName(path.c_str());
			xml->Update();
			reader_class = "vtkXMLGenericDataObjectReader";
			auto *out = xml->GetOutput();
			if (out) {
				return vtkSmartPointer<vtkDataObject>(out);
			}
			return nullptr;
		}
	}

	vtkNew<vtkGenericDataObjectReader> legacy;
	legacy->SetFileName(path.c_str());

	// MANDATORY, and a silent data-loss bug if omitted.
	//
	// Every vtkDataReader::ReadAll* flag defaults to OFF, which means the legacy
	// reader loads only the FIRST (active) array of each attribute kind. A .vtk
	// file with two SCALARS sections would expose one of them and drop the other
	// with no warning at all — the file would look like it only ever had one array.
	// `ReadAllFields` additionally controls whether legacy `FIELD` data is read at
	// all, which is what makes field-only files (e.g. financial.vtk) work.
	legacy->ReadAllScalarsOn();
	legacy->ReadAllVectorsOn();
	legacy->ReadAllNormalsOn();
	legacy->ReadAllTensorsOn();
	legacy->ReadAllColorScalarsOn();
	legacy->ReadAllTCoordsOn();
	legacy->ReadAllFieldsOn();

	if (!legacy->OpenVTKFile() || !legacy->ReadHeader()) {
		reader_class = "<none>";
		return nullptr;
	}
	legacy->CloseVTKFile();
	scope.Clear(); // header probe succeeded; start clean for the real read
	legacy->Update();
	reader_class = "vtkGenericDataObjectReader";

	// GetErrorCode is checked, but note it is NOT sufficient on its own: a
	// truncated file leaves it at NoError. The VtkErrorScope check in Read() is
	// what actually catches that case.
	if (legacy->GetErrorCode() != vtkErrorCode::NoError) {
		return nullptr;
	}
	auto *out = legacy->GetOutput();

	// Legacy `DATASET FIELD` files need a second attempt.
	//
	// vtkGenericDataObjectReader does not handle the FIELD dataset type, but it
	// does not fail cleanly either: it returns a NON-null, bare vtkDataObject with
	// no geometry and no field arrays. So the trigger for the fallback is
	// "not a vtkDataSet and carries no arrays", not "null output".
	// vtkDataObjectReader is the dedicated reader for these files.
	const bool empty_data_object =
	    out && !vtkDataSet::SafeDownCast(out) &&
	    (!out->GetFieldData() || out->GetFieldData()->GetNumberOfArrays() == 0);

	if (!out || empty_data_object) {
		scope.Clear();
		vtkNew<vtkDataObjectReader> field_reader;
		field_reader->SetFileName(path.c_str());
		field_reader->ReadAllFieldsOn();
		field_reader->Update();
		auto *field_out = field_reader->GetOutput();
		if (field_out && field_out->GetFieldData() && field_out->GetFieldData()->GetNumberOfArrays() > 0) {
			reader_class = "vtkDataObjectReader";
			return vtkSmartPointer<vtkDataObject>(field_out);
		}
		scope.Clear(); // the fallback's own failure is not the user-facing reason
	}

	return out ? vtkSmartPointer<vtkDataObject>(out) : nullptr;
}

//! Sniffs the dataset type out of the first bytes of an XML VTK file.
//!
//! Needed because the filename-based sniffing API (ReadOutputType) cannot be used
//! when the bytes came from a URL — it opens the path itself. Rather than hoping
//! vtkXMLGenericDataObjectReader behaves correctly in ReadFromInputString mode,
//! parse the `type="..."` attribute from the header and instantiate the concrete
//! reader. That is deterministic and relies on documented format structure.
std::string SniffXmlDatasetType(const std::string &bytes) {
	// The attribute lives in the <VTKFile ...> tag, always near the start.
	const size_t window = bytes.size() < 4096 ? bytes.size() : 4096;
	const std::string head = bytes.substr(0, window);
	const auto tag = head.find("<VTKFile");
	if (tag == std::string::npos) {
		return "";
	}
	const auto key = head.find("type=", tag);
	if (key == std::string::npos) {
		return "";
	}
	auto open = head.find_first_of("\"'", key);
	if (open == std::string::npos) {
		return "";
	}
	const char quote = head[open];
	const auto close = head.find(quote, open + 1);
	if (close == std::string::npos) {
		return "";
	}
	return head.substr(open + 1, close - open - 1);
}

//! Reads a VTK dataset from an in-memory buffer.
//!
//! `bytes` MUST outlive the returned object's construction: the readers are given a
//! vtkCharArray wrapping our buffer with save=1, which avoids a second full copy but
//! means VTK does not own it (vtkDataReader.h is explicit that deleting it early
//! causes "bad things ... during a pipeline update").
vtkSmartPointer<vtkDataObject> ReadDataObjectFromMemory(const std::string &bytes, VtkErrorScope &scope,
                                                        std::string &reader_class, bool &was_parallel) {
	was_parallel = false;
	if (bytes.empty()) {
		reader_class = "<none>";
		return nullptr;
	}

	// Wrap without copying. const_cast is safe: the readers only read.
	vtkNew<vtkCharArray> buffer;
	buffer->SetNumberOfComponents(1);
	buffer->SetArray(const_cast<char *>(bytes.data()), static_cast<vtkIdType>(bytes.size()), /*save=*/1);

	const std::string xml_type = SniffXmlDatasetType(bytes);
	if (!xml_type.empty()) {
		// Parallel/partitioned variants are named P<Type> and are unsupported; flag
		// rather than silently reading one piece.
		if (xml_type.rfind('P', 0) == 0 && xml_type.size() > 1 && std::isupper(xml_type[1])) {
			was_parallel = true;
		}
		vtkSmartPointer<vtkXMLReader> reader;
		if (xml_type == "UnstructuredGrid") {
			reader = vtkSmartPointer<vtkXMLUnstructuredGridReader>::New();
		} else if (xml_type == "PolyData") {
			reader = vtkSmartPointer<vtkXMLPolyDataReader>::New();
		} else if (xml_type == "ImageData") {
			reader = vtkSmartPointer<vtkXMLImageDataReader>::New();
		} else if (xml_type == "RectilinearGrid") {
			reader = vtkSmartPointer<vtkXMLRectilinearGridReader>::New();
		} else if (xml_type == "StructuredGrid") {
			reader = vtkSmartPointer<vtkXMLStructuredGridReader>::New();
		}
		if (!reader) {
			// A recognised XML wrapper of a type we do not support (multiblock,
			// hypertreegrid, a parallel variant, ...). Name it in the error.
			reader_class = "<unsupported:" + xml_type + ">";
			return nullptr;
		}
		reader->SetReadFromInputString(1);
		reader->SetInputArray(buffer);
		reader->Update();
		reader_class = std::string(reader->GetClassName()) + " (in-memory)";
		auto *out = reader->GetOutputAsDataSet();
		return out ? vtkSmartPointer<vtkDataObject>(out) : nullptr;
	}

    // Legacy: same ReadAll* requirement as the file path, or arrays are dropped.
	vtkNew<vtkGenericDataObjectReader> legacy;
	legacy->SetReadFromInputString(1);
	legacy->SetInputArray(buffer);
	legacy->ReadAllScalarsOn();
	legacy->ReadAllVectorsOn();
	legacy->ReadAllNormalsOn();
	legacy->ReadAllTensorsOn();
	legacy->ReadAllColorScalarsOn();
	legacy->ReadAllTCoordsOn();
	legacy->ReadAllFieldsOn();
	legacy->Update();
	reader_class = "vtkGenericDataObjectReader (in-memory)";
	if (legacy->GetErrorCode() != vtkErrorCode::NoError) {
		return nullptr;
	}
	auto *out = legacy->GetOutput();
	if (out && (vtkDataSet::SafeDownCast(out) ||
	            (out->GetFieldData() && out->GetFieldData()->GetNumberOfArrays() > 0))) {
		return vtkSmartPointer<vtkDataObject>(out);
	}

	// Field-only legacy files need the dedicated reader, exactly as on the file path.
	scope.Clear();
	vtkNew<vtkDataObjectReader> field_reader;
	field_reader->SetReadFromInputString(1);
	field_reader->SetInputArray(buffer);
	field_reader->ReadAllFieldsOn();
	field_reader->Update();
	auto *field_out = field_reader->GetOutput();
	if (field_out && field_out->GetFieldData() && field_out->GetFieldData()->GetNumberOfArrays() > 0) {
		reader_class = "vtkDataObjectReader (in-memory)";
		return vtkSmartPointer<vtkDataObject>(field_out);
	}
	return nullptr;
}

void AppendActiveRole(std::string &target, const char *role) {
	if (!target.empty()) {
		target += ",";
	}
	target += role;
}

//! Which "active attribute" roles an array currently holds.
std::string ActiveRoles(vtkDataSetAttributes *attributes, vtkAbstractArray *array) {
	std::string roles;
	if (!attributes || !array) {
		return roles;
	}
	// Compare by pointer identity: names can repeat across associations.
	if (attributes->GetScalars() == array) {
		AppendActiveRole(roles, "SCALARS");
	}
	if (attributes->GetVectors() == array) {
		AppendActiveRole(roles, "VECTORS");
	}
	if (attributes->GetNormals() == array) {
		AppendActiveRole(roles, "NORMALS");
	}
	if (attributes->GetTCoords() == array) {
		AppendActiveRole(roles, "TCOORDS");
	}
	if (attributes->GetTensors() == array) {
		AppendActiveRole(roles, "TENSORS");
	}
	if (attributes->GetGlobalIds() == array) {
		AppendActiveRole(roles, "GLOBALIDS");
	}
	if (attributes->GetPedigreeIds() == array) {
		AppendActiveRole(roles, "PEDIGREEIDS");
	}
	return roles;
}

void CollectFrom(vtkFieldData *field_data, VtkAssociation association, std::vector<VtkArrayInfo> &out) {
	if (!field_data) {
		return;
	}
	auto *attributes = vtkDataSetAttributes::SafeDownCast(field_data);
	const int n = field_data->GetNumberOfArrays();
	for (int i = 0; i < n; i++) {
		// GetAbstractArray, not GetArray: GetArray() returns nullptr for
		// vtkStringArray, which would silently drop string columns entirely.
		vtkAbstractArray *array = field_data->GetAbstractArray(i);
		if (!array) {
			continue;
		}
		VtkArrayInfo info;
		info.association = association;
		info.array_index = i;
		const char *raw_name = array->GetName();
		info.name = raw_name ? raw_name : "";
		info.vtk_type = array->GetDataType();
		const char *type_name = array->GetDataTypeAsString();
		info.vtk_type_name = type_name ? type_name : "unknown";
		info.num_components = array->GetNumberOfComponents();
		info.num_tuples = static_cast<int64_t>(array->GetNumberOfTuples());
		info.is_string_array = vtkStringArray::SafeDownCast(array) != nullptr;
		for (int c = 0; c < info.num_components; c++) {
			const char *cn = array->GetComponentName(c);
			info.component_names.emplace_back(cn ? cn : "");
		}
		bool any_component_named = false;
		for (auto &cn : info.component_names) {
			if (!cn.empty()) {
				any_component_named = true;
				break;
			}
		}
		if (!any_component_named) {
			info.component_names.clear();
		}
		info.active_as = ActiveRoles(attributes, array);
		out.push_back(std::move(info));
	}
}

} // namespace

VtkDataset::~VtkDataset() {
	if (object_ref) {
		auto *held = static_cast<vtkSmartPointer<vtkDataObject> *>(object_ref);
		delete held;
		object_ref = nullptr;
	}
}

std::shared_ptr<VtkDataset> VtkDataset::Read(const std::string &path, VtkFileSource *source) {
	// A local-only source keeps behaviour identical to before remote support when no
	// ClientContext was available to give us the VFS.
	std::unique_ptr<VtkFileSource> fallback;
	if (!source) {
		fallback = VtkMakeLocalFileSource();
		source = fallback.get();
	}

	const bool local = source->IsLocalPath(path);
	// The memory path is normally only reachable for remote objects, which would
	// leave it untested without a filesystem extension loaded. The env hook lets the
	// whole corpus be run through it and compared against the file path.
	const bool via_memory = !local || VtkForceMemoryReads();

	if (!source->Exists(path)) {
		throw IOException("duck_vtk: cannot read '%s': no such file%s", path,
		                  local ? "" : " (or it is unreachable — is the relevant filesystem "
		                               "extension loaded, e.g. INSTALL httpfs; LOAD httpfs;)");
	}

	// The scope must be live before any probing, because probe failures write to
	// the output window and must not surface to the user.
	VtkErrorScope scope;

	std::string reader_class;
	bool was_parallel = false;
	vtkSmartPointer<vtkDataObject> object;
	// Declared here, not inside the branch: the readers wrap this buffer without
	// copying it, so it must outlive the parse.
	std::string bytes;

	if (via_memory) {
		source->ReadAll(path, bytes);
		object = ReadDataObjectFromMemory(bytes, scope, reader_class, was_parallel);
	} else {
		object = ReadDataObject(path, scope, reader_class, was_parallel);
	}

	if (!object) {
		auto detail = scope.Fatal();
		if (detail.empty()) {
			// Name the unsupported XML type when the sniffer identified one, rather
			// than the useless "no reader recognised it".
			if (reader_class.rfind("<unsupported:", 0) == 0) {
				detail = "unsupported XML dataset type " +
				         reader_class.substr(13, reader_class.size() - 14) +
				         "; supported: UnstructuredGrid, PolyData, ImageData, RectilinearGrid, StructuredGrid";
			} else {
				detail = "no VTK reader recognised the file contents";
			}
		}
		throw IOException("duck_vtk: cannot read '%s': %s", path, detail);
	}

	// The output-window check, NOT GetErrorCode(), is what catches a truncated
	// file. See docs/PHASE0_RESULTS.md §4: a truncated read leaves GetErrorCode()
	// at Success while returning uninitialised coordinates.
	auto fatal = scope.Fatal();
	if (!fatal.empty()) {
		throw IOException("duck_vtk: '%s' appears corrupt or truncated: %s", path, fatal);
	}

	if (was_parallel) {
		throw NotImplementedException(
		    "duck_vtk: '%s' is a parallel/partitioned VTK dataset; only serial datasets are supported "
		    "so far. Attach the individual piece files instead.",
		    path);
	}

	auto *as_dataset = vtkDataSet::SafeDownCast(object);
	if (!as_dataset) {
		// Legacy `DATASET FIELD` files (and anything else that is a bare
		// vtkDataObject) carry no geometry at all — just a bag of named arrays. That
		// is a legitimate VTK format, so accept it with zero points and zero cells
		// and expose the arrays through field_data. Refusing it would reject a real
		// file for no good reason.
		auto *field_data = object->GetFieldData();
		const bool field_only = field_data && field_data->GetNumberOfArrays() > 0;
		if (!field_only) {
			// Composite types (vtkMultiBlockDataSet, vtkPartitionedDataSet, ...) are
			// a later phase. Fail clearly rather than reading only block 0.
			throw NotImplementedException(
			    "duck_vtk: '%s' contains a %s, which is not a simple dataset. Supported so far: "
			    "UnstructuredGrid, PolyData, ImageData, RectilinearGrid, StructuredGrid, and "
			    "field-only (legacy DATASET FIELD) files.",
			    path, object->GetClassName());
		}
	}

	auto result = std::shared_ptr<VtkDataset>(new VtkDataset());
	result->object_ref = new vtkSmartPointer<vtkDataObject>(object);
	result->dataset = as_dataset;
	result->path = path;
	result->reader_class = reader_class;
	result->dataset_class = object->GetClassName();
	result->file_size_bytes = FileSize(path);
	result->Initialise();
	result->CollectArrays();
	return result;
}

void VtkDataset::Initialise() {
	if (!dataset) {
		// Field-only dataset: no geometry, no cells, no bounds. Everything below is
		// geometry-dependent, so there is nothing more to do.
		return;
	}
	num_points = static_cast<int64_t>(dataset->GetNumberOfPoints());
	num_cells = static_cast<int64_t>(dataset->GetNumberOfCells());

	if (num_points > 0) {
		double b[6];
		dataset->GetBounds(b);
		// Guard against VTK's uninitialised/inverted sentinel box, and against a
		// truncated read that slipped past the error scope. A non-finite or
		// inverted box means the coordinates cannot be trusted.
		bool finite = true;
		for (int i = 0; i < 6; i++) {
			if (!std::isfinite(b[i])) {
				finite = false;
				break;
			}
		}
		if (finite && b[0] <= b[1] && b[2] <= b[3] && b[4] <= b[5]) {
			has_bounds = true;
			for (int i = 0; i < 6; i++) {
				bounds[i] = b[i];
			}
		}
	}

	// Precompute connectivity totals once. cell_points needs the exact cardinality
	// for its planner estimate, and the LIST writer needs the maximum cell size to
	// reserve child capacity in a single call.
	std::vector<int64_t> scratch;
	for (int64_t c = 0; c < num_cells; c++) {
		scratch.clear();
		const int32_t n = GetCellPoints(c, scratch);
		num_cell_points += n;
		if (n > max_cell_size) {
			max_cell_size = n;
		}
	}
}

void VtkDataset::CollectArrays() {
	if (dataset) {
		CollectFrom(dataset->GetPointData(), VtkAssociation::POINT, arrays);
		CollectFrom(dataset->GetCellData(), VtkAssociation::CELL, arrays);
	}
	CollectFrom(DataObject()->GetFieldData(), VtkAssociation::FIELD, arrays);
}

vtkDataObject *VtkDataset::DataObject() const {
	return static_cast<vtkSmartPointer<vtkDataObject> *>(object_ref)->Get();
}

void VtkDataset::GetPoint(int64_t point_id, double out[3]) const {
	// GetPoint is implemented by every dataset type, including the implicit ones
	// (ImageData synthesises from origin+spacing, RectilinearGrid from its per-axis
	// coordinate arrays). Using it uniformly avoids a five-way switch and
	// guarantees point_id ordering matches VTK's own.
	dataset->GetPoint(static_cast<vtkIdType>(point_id), out);
}

int32_t VtkDataset::GetCellType(int64_t cell_id) const {
	return static_cast<int32_t>(dataset->GetCellType(static_cast<vtkIdType>(cell_id)));
}

int32_t VtkDataset::GetCellPoints(int64_t cell_id, std::vector<int64_t> &out) const {
	// Never GetCell(): that returns a shared per-object scratch cell.
	auto &ids = CellScratch();
	dataset->GetCellPoints(static_cast<vtkIdType>(cell_id), &ids);
	const vtkIdType n = ids.GetNumberOfIds();
	for (vtkIdType i = 0; i < n; i++) {
		out.push_back(static_cast<int64_t>(ids.GetId(i)));
	}
	return static_cast<int32_t>(n);
}

vtkAbstractArray *VtkDataset::Array(const VtkArrayInfo &info) const {
	vtkFieldData *field_data = nullptr;
	switch (info.association) {
	case VtkAssociation::POINT:
		field_data = dataset ? dataset->GetPointData() : nullptr;
		break;
	case VtkAssociation::CELL:
		field_data = dataset ? dataset->GetCellData() : nullptr;
		break;
	case VtkAssociation::FIELD:
		field_data = DataObject()->GetFieldData();
		break;
	}
	if (!field_data) {
		return nullptr;
	}
	return field_data->GetAbstractArray(info.array_index);
}

std::string VtkDataset::DebugDump() const {
	std::string out;
	out += StringUtil::Format("path            = %s\n", path);
	out += StringUtil::Format("file_size_bytes = %lld\n", (long long)file_size_bytes);
	out += StringUtil::Format("reader_class    = %s\n", reader_class);
	out += StringUtil::Format("source_kind     = %s\n", source_kind);
	out += StringUtil::Format("dataset_class   = %s\n", dataset_class);
	out += StringUtil::Format("num_points      = %lld\n", (long long)num_points);
	out += StringUtil::Format("num_cells       = %lld\n", (long long)num_cells);
	out += StringUtil::Format("num_cell_points = %lld\n", (long long)num_cell_points);
	out += StringUtil::Format("max_cell_size   = %d\n", max_cell_size);
	if (has_bounds) {
		out += StringUtil::Format("bounds          = %.17g %.17g %.17g %.17g %.17g %.17g\n", bounds[0], bounds[1],
		                          bounds[2], bounds[3], bounds[4], bounds[5]);
	} else {
		out += "bounds          = <none>\n";
	}
	for (auto &a : arrays) {
		out += StringUtil::Format("array %-5s [%d] name='%s' column='%s' vtk_type=%d (%s) ncomp=%d ntuples=%lld%s%s\n",
		                          VtkAssociationName(a.association), a.array_index, a.name, a.column_name, a.vtk_type,
		                          a.vtk_type_name, a.num_components, (long long)a.num_tuples,
		                          a.is_string_array ? " string" : "",
		                          a.active_as.empty() ? "" : (" active=" + a.active_as).c_str());
	}
	return out;
}

//===--------------------------------------------------------------------===//
// Dataset cache
//===--------------------------------------------------------------------===//

std::shared_ptr<VtkDataset> VtkGetCachedDataset(const std::string &path, VtkFileSource *source) {
	static std::mutex cache_lock;
	static std::map<std::string, std::weak_ptr<VtkDataset>> cache;

	// Read outside the lock is not safe here, so keep it simple: the whole
	// operation is guarded. Reads dominate the cost anyway, and holding the lock
	// across the read also prevents two threads reading the same large file twice.
	std::lock_guard<std::mutex> guard(cache_lock);
	auto entry = cache.find(path);
	if (entry != cache.end()) {
		if (auto existing = entry->second.lock()) {
			return existing;
		}
		cache.erase(entry);
	}
	auto fresh = VtkDataset::Read(path, source);
	cache[path] = fresh;

	// Opportunistically drop expired entries so the map does not grow without
	// bound in a long-lived session that touches many files.
	for (auto it = cache.begin(); it != cache.end();) {
		it = it->second.expired() ? cache.erase(it) : std::next(it);
	}
	return fresh;
}

} // namespace duckdb
