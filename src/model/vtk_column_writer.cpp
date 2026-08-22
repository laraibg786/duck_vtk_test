#include "model/vtk_column_writer.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/vector.hpp"
#include "model/vtk_types.hpp"

#include <vtkAOSDataArrayTemplate.h>
#include <vtkAbstractArray.h>
#include <vtkBitArray.h>
#include <vtkDataArray.h>
#include <vtkSOADataArrayTemplate.h>
#include <vtkStringArray.h>
#include <vtkType.h>

namespace duckdb {

namespace {

//! Typed component reader that survives both array layouts.
//!
//! vtkAOSDataArrayTemplate and vtkSOADataArrayTemplate both expose
//! GetTypedComponent, which returns the array's OWN value type rather than
//! double. The downcasts happen once, outside the row loop.
//!
//! The GetComponent fallback exists only for exotic subclasses (vtkMappedDataArray
//! and friends). For 64-bit integer types that fallback would be lossy, so it is
//! refused rather than allowed to silently corrupt data.
template <class VTK_T>
class TypedReader {
public:
	TypedReader(vtkDataArray *array, int32_t vtk_type) : plain(array) {
		aos = vtkAOSDataArrayTemplate<VTK_T>::FastDownCast(array);
		soa = vtkSOADataArrayTemplate<VTK_T>::FastDownCast(array);
		if (!aos && !soa && VtkTypeNeedsExactIntegerPath(vtk_type)) {
			throw NotImplementedException(
			    "duck_vtk: array '%s' uses an unsupported VTK array layout (%s) for a 64-bit integer type. "
			    "Reading it through the generic double-based accessor would silently lose precision above 2^53, "
			    "so it is refused rather than returning wrong values.",
			    array->GetName() ? array->GetName() : "<unnamed>", array->GetClassName());
		}
	}

	inline VTK_T Get(vtkIdType tuple, int component) const {
		if (aos) {
			return aos->GetTypedComponent(tuple, component);
		}
		if (soa) {
			return soa->GetTypedComponent(tuple, component);
		}
		return static_cast<VTK_T>(plain->GetComponent(tuple, component));
	}

private:
	vtkAOSDataArrayTemplate<VTK_T> *aos = nullptr;
	vtkSOADataArrayTemplate<VTK_T> *soa = nullptr;
	vtkDataArray *plain = nullptr;
};

//! Scalar (single-component) numeric column.
template <class VTK_T, class DUCK_T>
void WriteScalarNumeric(vtkDataArray *array, int32_t vtk_type, int64_t start, idx_t count, int64_t num_tuples,
                        Vector &out) {
	TypedReader<VTK_T> reader(array, vtk_type);
	auto data = FlatVector::GetData<DUCK_T>(out);
	auto &validity = FlatVector::Validity(out);
	for (idx_t i = 0; i < count; i++) {
		const int64_t tuple = start + static_cast<int64_t>(i);
		if (tuple >= num_tuples) {
			validity.SetInvalid(i); // short array — visible, not fatal (design §8)
			continue;
		}
		data[i] = static_cast<DUCK_T>(reader.Get(static_cast<vtkIdType>(tuple), 0));
	}
}

//! Multi-component numeric column, as LIST(element).
template <class VTK_T, class DUCK_T>
void WriteListNumeric(vtkDataArray *array, int32_t vtk_type, int64_t start, idx_t count, int64_t num_tuples,
                      int32_t ncomp, Vector &out) {
	TypedReader<VTK_T> reader(array, vtk_type);

	// Reserve FIRST. ListVector::Reserve resizes the child vector, which
	// invalidates any pointer taken into it beforehand — a use-after-free that
	// typically survives a release build and only asserts in debug.
	const idx_t total = count * static_cast<idx_t>(ncomp);
	ListVector::Reserve(out, total);

	auto list_entries = FlatVector::GetData<list_entry_t>(out);
	auto &validity = FlatVector::Validity(out);
	auto &child = ListVector::GetEntry(out);
	auto child_data = FlatVector::GetData<DUCK_T>(child); // only valid after Reserve
	auto &child_validity = FlatVector::Validity(child);

	idx_t offset = 0;
	for (idx_t i = 0; i < count; i++) {
		const int64_t tuple = start + static_cast<int64_t>(i);
		if (tuple >= num_tuples) {
			validity.SetInvalid(i);
			list_entries[i].offset = offset;
			list_entries[i].length = 0;
			continue;
		}
		list_entries[i].offset = offset;
		list_entries[i].length = static_cast<uint64_t>(ncomp);
		for (int32_t c = 0; c < ncomp; c++) {
			child_data[offset + static_cast<idx_t>(c)] =
			    static_cast<DUCK_T>(reader.Get(static_cast<vtkIdType>(tuple), c));
		}
		offset += static_cast<idx_t>(ncomp);
	}
	child_validity.SetAllValid(offset);
	ListVector::SetListSize(out, offset);
}

//! VTK_BIT is stored packed in vtkBitArray; values are 0/1 so the generic
//! double-returning accessor is lossless here.
void WriteBit(vtkDataArray *array, int64_t start, idx_t count, int64_t num_tuples, int32_t ncomp, Vector &out) {
	if (ncomp <= 1) {
		auto data = FlatVector::GetData<bool>(out);
		auto &validity = FlatVector::Validity(out);
		for (idx_t i = 0; i < count; i++) {
			const int64_t tuple = start + static_cast<int64_t>(i);
			if (tuple >= num_tuples) {
				validity.SetInvalid(i);
				continue;
			}
			data[i] = array->GetComponent(static_cast<vtkIdType>(tuple), 0) != 0.0;
		}
		return;
	}
	const idx_t total = count * static_cast<idx_t>(ncomp);
	ListVector::Reserve(out, total);
	auto list_entries = FlatVector::GetData<list_entry_t>(out);
	auto &validity = FlatVector::Validity(out);
	auto &child = ListVector::GetEntry(out);
	auto child_data = FlatVector::GetData<bool>(child);
	idx_t offset = 0;
	for (idx_t i = 0; i < count; i++) {
		const int64_t tuple = start + static_cast<int64_t>(i);
		if (tuple >= num_tuples) {
			validity.SetInvalid(i);
			list_entries[i] = {offset, 0};
			continue;
		}
		list_entries[i] = {offset, static_cast<uint64_t>(ncomp)};
		for (int32_t c = 0; c < ncomp; c++) {
			child_data[offset + static_cast<idx_t>(c)] = array->GetComponent(static_cast<vtkIdType>(tuple), c) != 0.0;
		}
		offset += static_cast<idx_t>(ncomp);
	}
	ListVector::SetListSize(out, offset);
}

//! Multi-component handling matters here, not just for the numeric writers.
//! VtkArrayLogicalType promotes ANY element type to LIST(element) when ncomp > 1,
//! strings included — so a two-component string array binds as VARCHAR[]. This
//! function previously ignored ncomp and wrote string_t straight into what was a
//! LIST vector, which tripped
//!     INTERNAL Error: Expected vector of type VARCHAR, but found vector of type LIST
//! and, in a build with DUCKDB_DEBUG_NO_SAFETY, silently type-punned string_t over
//! list_entry_t instead. Reproduced with a legacy FIELD array declared
//! `labels 2 2 string`.
void WriteStringArray(vtkStringArray *array, int64_t start, idx_t count, int32_t ncomp, Vector &out) {
	const int64_t num_values = static_cast<int64_t>(array->GetNumberOfValues());

	if (ncomp <= 1) {
		auto data = FlatVector::GetData<string_t>(out);
		auto &validity = FlatVector::Validity(out);
		for (idx_t i = 0; i < count; i++) {
			const int64_t index = start + static_cast<int64_t>(i);
			if (index >= num_values) {
				validity.SetInvalid(i);
				continue;
			}
			const auto &value = array->GetValue(static_cast<vtkIdType>(index));
			data[i] = StringVector::AddString(out, value.data(), value.size());
		}
		return;
	}

	// vtkStringArray indexes by VALUE, so tuple t component c is value t*ncomp + c.
	ListVector::Reserve(out, count * static_cast<idx_t>(ncomp));
	auto list_entries = FlatVector::GetData<list_entry_t>(out);
	auto &validity = FlatVector::Validity(out);
	auto &child = ListVector::GetEntry(out);
	auto child_data = FlatVector::GetData<string_t>(child);
	auto &child_validity = FlatVector::Validity(child);
	idx_t offset = 0;
	for (idx_t i = 0; i < count; i++) {
		const int64_t base = (start + static_cast<int64_t>(i)) * static_cast<int64_t>(ncomp);
		if (base + static_cast<int64_t>(ncomp) > num_values) {
			validity.SetInvalid(i);
			list_entries[i] = {offset, 0};
			continue;
		}
		list_entries[i] = {offset, static_cast<uint64_t>(ncomp)};
		for (int32_t c = 0; c < ncomp; c++) {
			const auto &value = array->GetValue(static_cast<vtkIdType>(base + c));
			// AddString must target the CHILD vector: that is where the string data
			// has to be owned for a LIST column.
			child_data[offset + static_cast<idx_t>(c)] = StringVector::AddString(child, value.data(), value.size());
		}
		offset += static_cast<idx_t>(ncomp);
	}
	child_validity.SetAllValid(offset);
	ListVector::SetListSize(out, offset);
}

//! Last resort for array classes we cannot type (e.g. vtkVariantArray): render as
//! text so the data is at least visible rather than silently dropped.
//! ncomp-aware for the same reason as WriteStringArray above — the VARCHAR
//! fallback is promoted to VARCHAR[] whenever ncomp > 1.
void WriteAsText(vtkAbstractArray *array, int64_t start, idx_t count, int32_t ncomp, Vector &out) {
	const int64_t num_tuples = static_cast<int64_t>(array->GetNumberOfTuples());

	if (ncomp <= 1) {
		auto data = FlatVector::GetData<string_t>(out);
		auto &validity = FlatVector::Validity(out);
		for (idx_t i = 0; i < count; i++) {
			const int64_t tuple = start + static_cast<int64_t>(i);
			if (tuple >= num_tuples) {
				validity.SetInvalid(i);
				continue;
			}
			vtkVariant v = array->GetVariantValue(static_cast<vtkIdType>(tuple));
			auto text = v.ToString();
			data[i] = StringVector::AddString(out, text.data(), text.size());
		}
		return;
	}

	// GetVariantValue indexes by VALUE, like vtkStringArray::GetValue.
	ListVector::Reserve(out, count * static_cast<idx_t>(ncomp));
	auto list_entries = FlatVector::GetData<list_entry_t>(out);
	auto &validity = FlatVector::Validity(out);
	auto &child = ListVector::GetEntry(out);
	auto child_data = FlatVector::GetData<string_t>(child);
	auto &child_validity = FlatVector::Validity(child);
	idx_t offset = 0;
	for (idx_t i = 0; i < count; i++) {
		const int64_t tuple = start + static_cast<int64_t>(i);
		if (tuple >= num_tuples) {
			validity.SetInvalid(i);
			list_entries[i] = {offset, 0};
			continue;
		}
		const int64_t base = tuple * static_cast<int64_t>(ncomp);
		list_entries[i] = {offset, static_cast<uint64_t>(ncomp)};
		for (int32_t c = 0; c < ncomp; c++) {
			vtkVariant v = array->GetVariantValue(static_cast<vtkIdType>(base + c));
			auto text = v.ToString();
			child_data[offset + static_cast<idx_t>(c)] = StringVector::AddString(child, text.data(), text.size());
		}
		offset += static_cast<idx_t>(ncomp);
	}
	child_validity.SetAllValid(offset);
	ListVector::SetListSize(out, offset);
}

} // namespace

void VtkWriteArrayColumn(const VtkArrayInfo &info, vtkAbstractArray *array, int64_t start, idx_t count, Vector &out) {
	out.SetVectorType(VectorType::FLAT_VECTOR);

	if (!array) {
		// The array disappeared between schema discovery and the scan. Report NULL
		// rather than crashing; vtk_arrays still shows what was expected.
		//
		// For a LIST column, setting validity alone leaves list_entry_t
		// uninitialised. Every other NULL path in this file writes {offset, 0}; a
		// consumer that reads entries without consulting validity would otherwise
		// see garbage offsets.
		const bool is_list = out.GetType().id() == LogicalTypeId::LIST;
		if (is_list) {
			ListVector::Reserve(out, 0);
			auto list_entries = FlatVector::GetData<list_entry_t>(out);
			for (idx_t i = 0; i < count; i++) {
				list_entries[i] = {0, 0};
			}
			ListVector::SetListSize(out, 0);
		}
		for (idx_t i = 0; i < count; i++) {
			FlatVector::SetNull(out, i, true);
		}
		return;
	}

	if (info.is_string_array) {
		if (auto *strings = vtkStringArray::SafeDownCast(array)) {
			WriteStringArray(strings, start, count, info.num_components, out);
			return;
		}
	}

	auto *numeric = vtkDataArray::SafeDownCast(array);
	if (!numeric) {
		WriteAsText(array, start, count, info.num_components, out);
		return;
	}

	const int64_t num_tuples = info.num_tuples;
	const int32_t ncomp = info.num_components;
	const bool is_list = ncomp > 1;

// One dispatch table for both the scalar and list cases, so a type can never be
// handled correctly in one and wrongly in the other.
#define DUCK_VTK_DISPATCH(VTK_ENUM, VTK_T, DUCK_T)                                                                     \
	case VTK_ENUM:                                                                                                     \
		if (is_list) {                                                                                                 \
			WriteListNumeric<VTK_T, DUCK_T>(numeric, info.vtk_type, start, count, num_tuples, ncomp, out);             \
		} else {                                                                                                       \
			WriteScalarNumeric<VTK_T, DUCK_T>(numeric, info.vtk_type, start, count, num_tuples, out);                  \
		}                                                                                                              \
		return;

	switch (info.vtk_type) {
	case VTK_BIT:
		WriteBit(numeric, start, count, num_tuples, ncomp, out);
		return;
		DUCK_VTK_DISPATCH(VTK_CHAR, char, int8_t)
		DUCK_VTK_DISPATCH(VTK_SIGNED_CHAR, signed char, int8_t)
		DUCK_VTK_DISPATCH(VTK_UNSIGNED_CHAR, unsigned char, uint8_t)
		DUCK_VTK_DISPATCH(VTK_SHORT, short, int16_t)
		DUCK_VTK_DISPATCH(VTK_UNSIGNED_SHORT, unsigned short, uint16_t)
		DUCK_VTK_DISPATCH(VTK_INT, int, int32_t)
		DUCK_VTK_DISPATCH(VTK_UNSIGNED_INT, unsigned int, uint32_t)
		DUCK_VTK_DISPATCH(VTK_LONG, long, int64_t)
		DUCK_VTK_DISPATCH(VTK_UNSIGNED_LONG, unsigned long, uint64_t)
		DUCK_VTK_DISPATCH(VTK_LONG_LONG, long long, int64_t)
		DUCK_VTK_DISPATCH(VTK_UNSIGNED_LONG_LONG, unsigned long long, uint64_t)
		DUCK_VTK_DISPATCH(VTK_FLOAT, float, float)
		DUCK_VTK_DISPATCH(VTK_DOUBLE, double, double)
		// VTK_ID_TYPE aliases int or long long depending on VTK_USE_64BIT_IDS, and
		// duplicating an existing case label would not compile, so it is handled
		// with the vtkIdType typedef itself.
	default:
		break;
	}

	if (info.vtk_type == VTK_ID_TYPE) {
		if (is_list) {
			WriteListNumeric<vtkIdType, int64_t>(numeric, info.vtk_type, start, count, num_tuples, ncomp, out);
		} else {
			WriteScalarNumeric<vtkIdType, int64_t>(numeric, info.vtk_type, start, count, num_tuples, out);
		}
		return;
	}
#undef DUCK_VTK_DISPATCH

	// Unknown numeric VTK type: fall back to text, list-aware like the branch above.
	WriteAsText(array, start, count, ncomp, out);
}

} // namespace duckdb
