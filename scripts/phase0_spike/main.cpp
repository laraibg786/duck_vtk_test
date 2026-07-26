// Phase 0 ABI spike for duck_vtk. See CMakeLists.txt for rationale.
//
// This program deliberately exercises the specific things that would break on a
// libstdc++ ABI mismatch or a missing vtk_module_autoinit:
//
//   1. std::string crossing the VTK shared-library boundary in BOTH directions
//      (we pass a std::string-derived filename in, and read array names out).
//      This is the ABI failure mode that link-checking alone does not catch.
//   2. Object-factory instantiation via vtkSmartPointer<...>::New(), which is
//      what silently returns null when autoinit is missing.
//   3. Reading typed array data, including the int64 path, so we learn early
//      whether the typed accessors behave as the research doc describes.
//   4. vtkIdType width, so the type-mapping layer can be written against fact
//      rather than assumption.
//
// It prints a machine-checkable summary that scripts/phase0.sh diffs against
// the ground truth from the corpus catalogue.

#include <vtkCellData.h>
#include <vtkCellTypes.h>
#include <vtkDataArray.h>
#include <vtkDataSet.h>
#include <vtkErrorCode.h>
#include <vtkGenericDataObjectReader.h>
#include <vtkNew.h>
#include <vtkPointData.h>
#include <vtkSmartPointer.h>
#include <vtkStringArray.h>
#include <vtkType.h>
#include <vtkVersion.h>
#include <vtkXMLGenericDataObjectReader.h>

#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>

namespace {

// Print metadata for one attribute container. Reading array *names* out of VTK
// returns const char* backed by VTK-side storage; assigning it into a
// std::string here is the boundary crossing we want to prove is safe.
void DumpFieldData(const char *label, vtkFieldData *fd) {
  if (!fd) {
    std::printf("%s.arrays=0\n", label);
    return;
  }
  const int n = fd->GetNumberOfArrays();
  std::printf("%s.arrays=%d\n", label, n);
  for (int i = 0; i < n; ++i) {
    // GetAbstractArray, not GetArray: GetArray() returns nullptr for
    // vtkStringArray, which would silently hide string columns.
    vtkAbstractArray *aa = fd->GetAbstractArray(i);
    if (!aa) {
      std::printf("%s.array[%d] <null>\n", label, i);
      continue;
    }
    const char *raw_name = aa->GetName();
    std::string name = raw_name ? raw_name : "";  // std::string across the boundary
    std::printf("%s.array[%d] name=%s type=%d type_name=%s ncomp=%d ntuples=%lld\n",
                label, i, name.c_str(), aa->GetDataType(),
                aa->GetDataTypeAsString() ? aa->GetDataTypeAsString() : "?",
                aa->GetNumberOfComponents(),
                static_cast<long long>(aa->GetNumberOfTuples()));

    // Print the first tuple's first component so a wrong-endianness or
    // wrong-offset read is visible immediately rather than only in aggregate.
    if (auto *da = vtkDataArray::SafeDownCast(aa)) {
      if (da->GetNumberOfTuples() > 0) {
        std::printf("%s.array[%d] first=%.17g\n", label, i, da->GetComponent(0, 0));
      }
    } else if (auto *sa = vtkStringArray::SafeDownCast(aa)) {
      if (sa->GetNumberOfValues() > 0) {
        // vtkStdString -> std::string: another deliberate boundary crossing.
        std::string v = sa->GetValue(0);
        std::printf("%s.array[%d] first_str=%s\n", label, i, v.c_str());
      }
    }
  }
}

vtkSmartPointer<vtkDataObject> ReadAny(const std::string &path, std::string &reader_used) {
  // Try the XML family first, then fall back to legacy.
  // Content-based, not extension-based, per the design doc.
  //
  // IMPORTANT, measured on VTK 9.6.2 (see docs/research/03 §2.3 correction):
  // vtkXMLGenericDataObjectReader::CanReadFile() returns 0 even for a perfectly
  // valid .vtu — it is not overridden on the generic reader, so it must NOT be
  // used for dispatch. `ReadOutputType()` is the working sniffing API: it returns
  // a VTK_* data-object type id (e.g. 4 == VTK_UNSTRUCTURED_GRID) or -1 if the
  // file is not readable XML. CanReadFile IS reliable on the *concrete* readers
  // (vtkXMLUnstructuredGridReader etc.), just not on the generic one.
  {
    vtkNew<vtkXMLGenericDataObjectReader> xml;
    bool parallel = false;
    const int output_type = xml->ReadOutputType(path.c_str(), parallel);
    if (output_type >= 0) {
      xml->SetFileName(path.c_str());
      xml->Update();
      reader_used = "vtkXMLGenericDataObjectReader";
      std::printf("xml_output_type=%d xml_parallel=%d\n", output_type, parallel ? 1 : 0);
      return vtkSmartPointer<vtkDataObject>(xml->GetOutput());
    }
  }
  {
    vtkNew<vtkGenericDataObjectReader> legacy;
    legacy->SetFileName(path.c_str());
    if (legacy->OpenVTKFile() && legacy->ReadHeader()) {
      legacy->CloseVTKFile();
      legacy->Update();
      reader_used = "vtkGenericDataObjectReader";
      // MUST check the error code. A truncated legacy file does NOT make Update()
      // fail: VTK reads "POINTS 27 float", allocates 27 points, hits EOF, and
      // hands back an object that *looks* valid (num_points == 27) with unfilled
      // coordinates. Reporting that as data would violate design §8, which
      // forbids confusing a failed read with a valid or empty mesh.
      const unsigned long err = legacy->GetErrorCode();
      std::printf("legacy_error_code=%lu legacy_error_string=%s\n", err,
                  vtkErrorCode::GetStringFromErrorCode(err));
      if (err != vtkErrorCode::NoError) {
        reader_used = "vtkGenericDataObjectReader(FAILED)";
        return nullptr;
      }
      return vtkSmartPointer<vtkDataObject>(legacy->GetOutput());
    }
  }
  reader_used = "<none>";
  return nullptr;
}

}  // namespace

int main(int argc, char **argv) {
  // Establish build facts first: these are printed even if no file is given,
  // because the type-mapping layer depends on them.
  std::printf("vtk_version=%s\n", vtkVersion::GetVTKVersion());
  std::printf("vtk_sizeof_id_type=%d\n", static_cast<int>(sizeof(vtkIdType)));
  std::printf("vtk_use_64bit_ids=%d\n", VTK_SIZEOF_ID_TYPE == 8 ? 1 : 0);
  std::printf("cxx_lib_check=%s\n", std::string("std_string_ok").c_str());

  if (argc < 2) {
    std::printf("status=no_file_given\n");
    return 0;
  }

  const std::string path = argv[1];
  std::string reader_used;
  vtkSmartPointer<vtkDataObject> obj = ReadAny(path, reader_used);

  std::printf("path=%s\n", path.c_str());
  std::printf("reader=%s\n", reader_used.c_str());

  if (!obj) {
    // Distinguishing "unreadable" from "empty" matters: the design forbids
    // treating a failed read as an empty mesh.
    std::printf("status=unreadable\n");
    return 2;
  }

  std::printf("dataset_class=%s\n", obj->GetClassName());

  vtkDataSet *ds = vtkDataSet::SafeDownCast(obj);
  if (!ds) {
    // e.g. a composite/multiblock object — out of scope for Phase 1 but must be
    // reported clearly rather than crashing.
    std::printf("status=not_a_vtkDataSet\n");
    return 3;
  }

  std::printf("num_points=%lld\n", static_cast<long long>(ds->GetNumberOfPoints()));
  std::printf("num_cells=%lld\n", static_cast<long long>(ds->GetNumberOfCells()));

  double b[6] = {0, 0, 0, 0, 0, 0};
  if (ds->GetNumberOfPoints() > 0) {
    ds->GetBounds(b);
    std::printf("bounds=%.17g %.17g %.17g %.17g %.17g %.17g\n", b[0], b[1], b[2], b[3], b[4], b[5]);
  } else {
    std::printf("bounds=<empty>\n");
  }

  // First point, to catch x/y/z transposition and float/double confusion.
  if (ds->GetNumberOfPoints() > 0) {
    double p[3];
    ds->GetPoint(0, p);
    std::printf("point[0]=%.17g %.17g %.17g\n", p[0], p[1], p[2]);
  }

  // Cell-type histogram via the cheap path. Note: deliberately NOT using
  // ds->GetCell(i), which returns a shared per-object scratch object and is a
  // data-race landmine once scans go parallel.
  {
    // GetDistinctCellTypes, not GetCellTypes: the latter is deprecated in VTK 9.6
    // ("Use GetDistinctCellTypes(vtkCellTypes* types) instead", vtkDataSet.h:183)
    // and building against it emits -Wdeprecated-declarations.
    vtkNew<vtkCellTypes> types;
    ds->GetDistinctCellTypes(types);
    std::printf("distinct_cell_types=%d\n", static_cast<int>(types->GetNumberOfTypes()));
    for (int i = 0; i < types->GetNumberOfTypes(); ++i) {
      std::printf("cell_type[%d]=%d\n", i, static_cast<int>(types->GetCellType(i)));
    }
  }

  DumpFieldData("point_data", ds->GetPointData());
  DumpFieldData("cell_data", ds->GetCellData());
  DumpFieldData("field_data", ds->GetFieldData());

  std::printf("status=ok\n");
  return 0;
}
