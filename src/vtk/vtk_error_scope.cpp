#include "vtk/vtk_error_scope.hpp"

#include <vtkLogger.h>
#include <vtkObjectFactory.h>
#include <vtkOutputWindow.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace duckdb {

namespace {

//! Messages that mean "the data you just read is not trustworthy".
//!
//! Deliberately a curated list rather than "any warning is fatal": VTK warns
//! about plenty of benign things (deprecated fields, empty attribute arrays,
//! version notes) and escalating those would reject valid files. Each entry here
//! was chosen because it indicates the reader could not honour the file's own
//! declared sizes or type.
const char *const FATAL_SUBSTRINGS[] = {
    "Possible mismatch of datasize with declaration", // truncated file — Phase 0 §4
    "Error reading ascii data",
    "Error reading binary data",
    "Unrecognized file type",
    "Unable to read",
    "Premature EOF",
    "Cannot read",
};

//! A vtkOutputWindow that appends to a caller-owned vector instead of writing to
//! stderr. Registered as the global window for the lifetime of a VtkErrorScope.
class DuckVtkCaptureWindow : public vtkOutputWindow {
public:
	static DuckVtkCaptureWindow *New();
	vtkTypeMacro(DuckVtkCaptureWindow, vtkOutputWindow);

	std::vector<std::string> *sink = nullptr;

	void DisplayText(const char *text) override {
		Append(text);
	}
	void DisplayErrorText(const char *text) override {
		Append(text);
	}
	void DisplayWarningText(const char *text) override {
		Append(text);
	}
	void DisplayGenericWarningText(const char *text) override {
		Append(text);
	}
	void DisplayDebugText(const char *text) override {
		// Dropped entirely: debug chatter is high-volume and never diagnostic here.
	}

protected:
	DuckVtkCaptureWindow() = default;
	~DuckVtkCaptureWindow() override = default;

private:
	void Append(const char *text) {
		if (!sink || !text) {
			return;
		}
		// Cap the buffer. A reader stuck in a warning loop on a pathological file
		// would otherwise grow this without bound inside a database process.
		if (sink->size() >= 256) {
			return;
		}
		sink->emplace_back(text);
	}
};

vtkStandardNewMacro(DuckVtkCaptureWindow);

//! Strip `(0x55a79017f430)` style object addresses.
//!
//! VTK embeds the object pointer in its messages. That is non-deterministic
//! between runs, so leaving it in would make any test asserting an error message
//! flaky, and it tells a user nothing.
void StripAddresses(std::string &text) {
	size_t pos;
	while ((pos = text.find(" (0x")) != std::string::npos) {
		auto close = text.find(')', pos);
		if (close == std::string::npos) {
			break;
		}
		text.erase(pos, close - pos + 1);
	}
}

bool TraceEnabled() {
	const char *v = std::getenv("DUCK_VTK_TRACE");
	return v && *v && std::strcmp(v, "0") != 0;
}

} // namespace

void VtkSilenceVtkLogger() {
	// vtkOutputWindow::SetInstance + overriding Display*Text is NOT enough, and
	// neither is SetDisplayModeToNever(): VTK's error macros ALSO call vtkLogger
	// directly, which writes to stderr independently of the output window. Measured
	// on 9.6.2 — a successful legacy read still printed a red ERR| line from the
	// discarded XML probe until this was added.
	//
	// Set once at extension load and left off. A database extension has no business
	// writing to the host process's stderr; everything we actually need is captured
	// by VtkErrorScope and surfaced either in an exception or under DUCK_VTK_TRACE=1.
	// There is no GetStderrVerbosity() in the VTK API, so this cannot be saved and
	// restored — hence doing it once, deliberately, rather than per scope.
	vtkLogger::SetStderrVerbosity(vtkLogger::VERBOSITY_OFF);
}

VtkErrorScope::VtkErrorScope() {
	auto *capture = DuckVtkCaptureWindow::New();
	capture->sink = &messages;
	// Overriding the Display*Text methods is NOT sufficient on its own. VTK 9's
	// vtkOutputWindow ALSO forwards messages to stderr via vtkLogger, controlled
	// independently by DisplayMode; DEFAULT forwards whenever logging is enabled.
	// Measured: without this call the captured message is still printed, so a
	// successful legacy read prints a red ERR| line from the discarded XML probe.
	capture->SetDisplayModeToNever();

	auto *previous = vtkOutputWindow::GetInstance();
	if (previous) {
		previous->Register(nullptr); // keep it alive while we hold the pointer
	}
	previous_window = previous;
	capture_window = capture;

	vtkOutputWindow::SetInstance(capture);
}

VtkErrorScope::~VtkErrorScope() {
	// Restore first, so nothing can log into a window we are about to destroy.
	auto *previous = static_cast<vtkOutputWindow *>(previous_window);
	vtkOutputWindow::SetInstance(previous);

	if (auto *capture = static_cast<DuckVtkCaptureWindow *>(capture_window)) {
		if (TraceEnabled() && !messages.empty()) {
			for (auto &m : messages) {
				std::fputs(("[duck_vtk trace] " + m + "\n").c_str(), stderr);
			}
		}
		capture->sink = nullptr;
		capture->Delete();
	}
	if (previous) {
		previous->UnRegister(nullptr);
	}
}

void VtkErrorScope::Clear() {
	messages.clear();
}

bool VtkErrorScope::Empty() const {
	return messages.empty();
}

std::string VtkErrorScope::Fatal() const {
	for (auto &message : messages) {
		for (auto *needle : FATAL_SUBSTRINGS) {
			if (message.find(needle) != std::string::npos) {
				// Collapse to a single line: VTK's messages carry timestamps,
				// thread ids and ANSI colour codes that would look like noise in a
				// SQL error. Keep the informative tail.
				std::string cleaned = message;
				StripAddresses(cleaned);
				auto bar = cleaned.find("| ");
				if (bar != std::string::npos) {
					cleaned = cleaned.substr(bar + 2);
				}
				for (auto &ch : cleaned) {
					if (ch == '\n' || ch == '\r' || ch == '\t') {
						ch = ' ';
					}
				}
				// Collapse runs of spaces introduced by the above.
				cleaned.erase(std::unique(cleaned.begin(), cleaned.end(),
				                          [](char a, char b) { return a == ' ' && b == ' '; }),
				              cleaned.end());
				while (!cleaned.empty() && cleaned.back() == ' ') {
					cleaned.pop_back();
				}
				// Strip trailing ANSI reset if present.
				const std::string reset = "\033[0m";
				if (cleaned.size() >= reset.size() &&
				    cleaned.compare(cleaned.size() - reset.size(), reset.size(), reset) == 0) {
					cleaned.erase(cleaned.size() - reset.size());
				}
				return cleaned;
			}
		}
	}
	return std::string();
}

std::string VtkErrorScope::All() const {
	std::string result;
	for (auto &message : messages) {
		if (!result.empty()) {
			result += "\n";
		}
		result += message;
	}
	return result;
}

} // namespace duckdb
