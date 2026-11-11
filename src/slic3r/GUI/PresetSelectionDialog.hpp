#ifndef slic3r_GUI_PresetSelectionDialog_hpp_
#define slic3r_GUI_PresetSelectionDialog_hpp_

// print_bot: native replacement for the webview setup wizard (GuideFrame).
// Chooses which system printer models/nozzles and which system filament
// families are visible, writing the same AppConfig "models" (vendor map) and
// "filaments" sections the wizard wrote.

#include "GUI_Utils.hpp"

#include <map>
#include <set>
#include <string>
#include <vector>

class wxCheckBox;
class wxCheckListBox;
class wxSimplebook;
class Button;
class wxTextCtrl;

namespace Slic3r { namespace GUI {

class PresetSelectionDialog : public DPIDialog
{
public:
    enum Page { pgPrinters = 0, pgFilaments = 1 };
    // Returned by ShowModal() when the user asked to create a custom filament
    // (selections are applied first).
    static constexpr int ID_CREATE_FILAMENT = wxID_HIGHEST + 4711;

    PresetSelectionDialog(wxWindow *parent, Page start_page);

    // Shows the dialog and applies the selection. Returns true if presets changed.
    static bool run(Page start_page);

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    struct VariantBox { std::string vendor, model, variant; wxCheckBox *box; };
    struct FilamentGroup {
        std::string              alias, vendor, type;
        std::vector<std::string> names; // system preset names sharing this alias
        bool                     checked{false};
    };

    void build_printers_page(wxWindow *page);
    void build_filaments_page(wxWindow *page);
    void refresh_filament_list();
    std::set<std::string> compatible_aliases(); // for the printers ticked right now
    bool apply();
    void select_page(int page);
    ::Button *make_button(wxWindow *parent, const wxString &label, bool primary, wxWindowID id = wxID_ANY);
    void apply_theme();

    // Theme colours (light or dark, picked once from GUI_App::dark_mode()).
    wxColour m_bg, m_fg, m_fg_muted, m_field_bg;

    wxSimplebook             *m_book{nullptr};
    std::vector<::Button *>   m_tabs;
    wxWindow                 *m_rule{nullptr};
    std::vector<VariantBox>   m_variants;
    std::vector<FilamentGroup> m_groups;
    std::vector<size_t>       m_shown;  // m_groups index per list row
    wxCheckListBox           *m_list{nullptr};
    wxTextCtrl               *m_search{nullptr};
    wxCheckBox               *m_only_compatible{nullptr};
    std::map<std::string, std::set<std::string>> m_compat_cache; // printer preset -> aliases
};

}} // namespace Slic3r::GUI

#endif
