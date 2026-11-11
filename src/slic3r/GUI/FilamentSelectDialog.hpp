#ifndef slic3r_FilamentSelectDialog_hpp_
#define slic3r_FilamentSelectDialog_hpp_

#include "GUI_Utils.hpp"
#include "DeviceManager.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/StaticBox.hpp"

#include <map>
#include <set>
#include <unordered_map>
#include <vector>
#include <wx/arrstr.h>
#include <wx/panel.h>
#include <wx/scrolwin.h>
#include <wx/statbmp.h>
#include <wx/textctrl.h>

namespace Slic3r { namespace GUI {

class FilamentSelectDialog : public DPIDialog
{
public:
    struct SelectionResult {
        wxString    preset_alias;   // selected system preset

        bool is_valid() const { return !preset_alias.IsEmpty(); }
    };

    explicit FilamentSelectDialog(wxWindow* parent);
    ~FilamentSelectDialog() {}

    void Popup(const wxArrayString&                          filament_items,
               const std::unordered_map<wxString, wxString>& vendors,
               const std::unordered_map<wxString, wxString>& types,
               const wxString&                               current_alias = wxString(),
               std::set<std::string>                         printer_names = {});

    const SelectionResult& get_result() const { return m_result; }

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    void      create();
    wxWindow* build_default_page(wxWindow* parent);

    void      fill_default_tab();
    void      select_brand(const wxString& brand);
    void      on_confirm();

    wxArrayString                             m_filament_items;
    std::unordered_map<wxString, wxString>    m_vendors;
    std::unordered_map<wxString, wxString>    m_types;
    std::set<std::string>                     m_printer_names;
    std::map<wxString, std::vector<wxString>> m_brand_to_aliases;
    std::vector<wxString>                     m_ordered_brands;
    wxString                                  m_checked_alias;
    wxString                                  m_selected_brand;

    wxScrolledWindow* m_brand_list{nullptr};
    wxBoxSizer*       m_brand_list_sizer{nullptr};
    wxScrolledWindow* m_type_list{nullptr};
    wxBoxSizer*       m_type_list_sizer{nullptr};

    // confirmed selection result (valid after EndModal(wxID_OK))
    SelectionResult   m_result;

    StateColor        m_btn_bg_green;
    StateColor        m_btn_bg_gray;   // reuses AMSMaterialsSetting gray button style
};

}} // namespace Slic3r::GUI

#endif // slic3r_FilamentSelectDialog_hpp_
