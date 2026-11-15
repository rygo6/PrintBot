// print_bot: native human approval for prints requested over MCP.
//
// The dialog is the only source of print approval for MCP: an agent cannot list, inspect,
// screenshot or click it (it is hidden from every ui_* tool, and every MCP call is refused while
// it is open), and only a real pointer click or key press on its Approve button counts.
#ifndef slic3r_GUI_PrintBotApproval_hpp_
#define slic3r_GUI_PrintBotApproval_hpp_

#include <string>

#include <wx/string.h>

class wxWindow;

namespace Slic3r { namespace GUI { namespace pbmcp {

// True while the approval dialog is shown (any thread).
bool approval_dialog_open();
// True for the approval dialog or a window inside it.
bool is_approval_window(wxWindow *w);

// Main thread only. Shows the modal approval dialog listing `summary` and returns true only when
// a human approved it on Approve with real input. Cancel, closing the window, Escape or the
// timeout return false.
bool run_print_approval_dialog(wxWindow *parent, const wxString &summary, const std::string &fingerprint, int timeout_s);

}}} // namespace Slic3r::GUI::pbmcp

#endif
