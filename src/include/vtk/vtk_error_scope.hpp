#pragma once

#include <string>
#include <vector>

namespace duckdb {

//! RAII capture of VTK's output window.
//!
//! This class is CORRECTNESS-CRITICAL, not a tidiness measure. Phase 0 measured
//! two behaviours that make it load-bearing (docs/PHASE0_RESULTS.md §4-§5):
//!
//!  1. A truncated legacy file is silently accepted. `GetErrorCode()` returns
//!     `Success`, the *declared* point count is reported, and coordinates are
//!     uninitialised memory. The ONLY signal is a WARN on the output window:
//!     "Error reading ascii data. Possible mismatch of datasize with declaration."
//!     Without capturing that text there is no way to distinguish a corrupt file
//!     from a valid one, which design §8 forbids.
//!
//!  2. Reader auto-detection probes XML first, and that probe writes a red
//!     "ERR| ... could not load <file>" line for EVERY legacy .vtk file —
//!     including ones that then read perfectly. Left alone, every successful
//!     legacy read would print an alarming error into the user's DuckDB session.
//!
//! Messages are therefore captured, never passed through. Call `Fatal()` after a
//! read to learn whether anything captured indicates real corruption.
class VtkErrorScope {
public:
	VtkErrorScope();
	~VtkErrorScope();

	VtkErrorScope(const VtkErrorScope &) = delete;
	VtkErrorScope &operator=(const VtkErrorScope &) = delete;

	//! Drop everything captured so far. Used to discard reader-probe noise before
	//! the real read starts, so probe failures are never mistaken for read errors.
	void Clear();

	//! Non-empty if a captured message matches the escalation list, i.e. VTK told
	//! us (only via the output window) that the data is not trustworthy. The
	//! returned string is suitable for embedding in an IOException.
	std::string Fatal() const;

	//! Everything captured, newline-joined. For diagnostics and DUCK_VTK_TRACE=1.
	std::string All() const;

	bool Empty() const;

private:
	std::vector<std::string> messages;
	void *previous_window = nullptr; // vtkOutputWindow*, held without leaking the header
	void *capture_window = nullptr;  // our subclass
};

//! Silences VTK's direct-to-stderr logging path. Call once at extension load.
//!
//! Neither overriding vtkOutputWindow's Display*Text methods nor
//! SetDisplayModeToNever() is sufficient: VTK's error macros also call vtkLogger
//! directly, which writes to stderr independently of the output window.
void VtkSilenceVtkLogger();

} // namespace duckdb
