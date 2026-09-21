// In-app log.
//
// The binaries are GUI-subsystem, so double-clicking one does not open a console
// window. Output goes to a ring buffer the UI can show instead — and still to
// stdout when the app was started from a terminal, so scripted runs and
// --headless keep working unchanged.

#pragma once

#include <string>
#include <vector>

namespace raidcast {

// Attaches to the parent console if there is one, so printf-style output is
// visible when launched from a terminal but no window appears otherwise. Safe to
// call once at startup; returns true if a console was attached.
bool AttachParentConsole();

void Log(const char* fmt, ...);
void LogError(const char* fmt, ...);

// Logs, then shows a message box. For failures that happen before there is a
// window to put a message in: without this, a GUI-subsystem app that exits
// during startup simply appears to do nothing.
void FatalError(const char* fmt, ...);

// Newest lines last. Copied under lock; callers may be on any thread.
std::vector<std::string> LogLines();

// Whole log as text, for the copy-to-clipboard button.
std::string LogText();

// Collapsible log view. `open` persists the header's state across frames.
void DrawLogPanel(const char* label, bool* open, float height);

}  // namespace raidcast
