#ifndef slic3r_GUI_HomePanel_hpp_
#define slic3r_GUI_HomePanel_hpp_

// print_bot: native Home tab (replaces the old webview home page).
// Shows New / Open project buttons and a thumbnail grid of recent projects.

#include <wx/panel.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/wrapsizer.h>
#include <string>
#include <vector>

class Button;
class Label;

namespace Slic3r { namespace GUI {

class HomePanel : public wxPanel
{
public:
    explicit HomePanel(wxWindow *parent);
    ~HomePanel() override = default;

    // Rebuild the recent-project grid. Kept under the old webview name so existing
    // call sites keep working; the argument (thumbnail count) is ignored.
    void SendRecentList(int images = 0);
    void update_mode() {}
    void msw_rescale();
    void sys_color_changed();

private:
    void build_header();
    void rebuild();
    void apply_colours();
    wxWindow *make_tile(wxWindow *parent, size_t index, const wxString &path, const wxString &name, const wxString &time, bool missing, const std::string &png);
    void show_tile_menu(size_t index, const wxString &path);

    wxBoxSizer       *m_main_sizer{nullptr};
    wxScrolledWindow *m_scroll{nullptr};
    wxWrapSizer      *m_grid{nullptr};
    Label            *m_title{nullptr};
    Label            *m_empty{nullptr};
    Button           *m_btn_new{nullptr};
    Button           *m_btn_open{nullptr};
    Button           *m_btn_clear{nullptr};
    bool              m_rebuild_pending{false};
};

}} // namespace Slic3r::GUI

#endif
