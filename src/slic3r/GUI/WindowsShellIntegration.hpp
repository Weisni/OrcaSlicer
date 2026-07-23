#pragma once

#include <string>

namespace Slic3r::GUI::WindowsShellIntegration {

// Must run before the first top-level window is created.
void initialize_process_app_id();

// Creates/repairs only shortcuts owned by the current installation kind.
// A build repairs shortcuts targeting CMake build trees, never installed ones.
void repair_shortcuts();

// Adds a document to the Windows Recent/Frequent Jump List for this app identity.
void add_recent_document(const std::wstring &filename);

} // namespace Slic3r::GUI::WindowsShellIntegration
