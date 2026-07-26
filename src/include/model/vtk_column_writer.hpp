#pragma once

#include "duckdb/common/types/vector.hpp"
#include "vtk/vtk_dataset.hpp"

class vtkAbstractArray;

namespace duckdb {

//! Converts `count` tuples of a VTK array, starting at `start`, into `out`.
//!
//! THE HIGHEST-RISK CODE IN THIS PROJECT. Two hard requirements:
//!
//!  1. Integer arrays are read through a TYPED accessor, never through
//!     `vtkDataArray::GetComponent`, which returns `double` and therefore
//!     silently corrupts values above 2^53. A bug here is invisible on small
//!     values and wrong on large ones — the worst possible failure mode. Both
//!     AOS and SOA array layouts are handled.
//!
//!  2. Multi-component arrays become LIST columns, and `ListVector::Reserve` is
//!     called BEFORE any pointer into the child vector is taken, because Reserve
//!     resizes the child and invalidates earlier pointers.
//!
//! Tuples beyond the array's own `num_tuples` are written as NULL rather than
//! erroring, per design §8: an array shorter than the point/cell count is a real
//! condition in the wild and must be visible, not fatal.
void VtkWriteArrayColumn(const VtkArrayInfo &info, vtkAbstractArray *array, int64_t start, idx_t count, Vector &out);

} // namespace duckdb
