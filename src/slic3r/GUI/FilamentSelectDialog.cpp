#include "FilamentSelectDialog.hpp"
#include "AMSMaterialsSetting.hpp"  // AMS_MATERIALS_SETTING_GREY* colour constants
#include "FilamentBitmapUtils.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "wxExtensions.hpp"
#include "libslic3r/PresetBundle.hpp"

#include <wx/dcmemory.h>
#include <wx/graphics.h>
#include <wx/statbmp.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>

namespace Slic3r { namespace GUI {

namespace {

static const std::vector<wxString> k_brand_prio{ "Bambu Lab", "Generic", "Polymaker" };

std::vector<wxString> order_brands(std::vector<wxString> keys, const wxString& other)
{
    const auto& prio = k_brand_prio;
    std::vector<wxString> out;
    for (const wxString& p : prio)
        if (std::find(keys.begin(), keys.end(), p) != keys.end()) out.push_back(p);
    std::vector<wxString> tail;
    for (const wxString& k : keys) {
        if (k == other) continue;
        if (std::find(prio.begin(), prio.end(), k) != prio.end()) continue;
        tail.push_back(k);
    }
    std::sort(tail.begin(), tail.end());
    for (const wxString& t : tail) out.push_back(t);
    if (std::find(keys.begin(), keys.end(), other) != keys.end()) out.push_back(other);
    return out;
}

StateColor sc(const wxColour& c) { return StateColor(std::make_pair(c, (int)StateColor::Normal)); }

// dark-mode color helpers
// Evaluated at call time so each data-fill method picks up the current theme.

inline wxColour dlg_bg()           { return StateColor::darkModeColorFor(*wxWHITE); }
inline wxColour dlg_separator()    { return wxGetApp().dark_mode() ? wxColour(0x50,0x52,0x54) : wxColour(230,230,230); }
inline wxColour dlg_divider()      { return wxGetApp().dark_mode() ? wxColour(0x50,0x52,0x54) : wxColour(220,220,220); }
inline wxColour dlg_row_hl()       { return wxGetApp().dark_mode() ? wxColour(0x24,0x5C,0x3A) : wxColour(238,248,242); }
inline wxColour dlg_text_primary()  { return wxGetApp().dark_mode() ? wxColour(0xF0,0xF0,0xF0) : AMS_MATERIALS_SETTING_GREY900; }
inline wxColour dlg_text_secondary(){ return wxGetApp().dark_mode() ? wxColour(0xC8,0xCA,0xCC) : AMS_MATERIALS_SETTING_GREY800; }

} // namespace


FilamentSelectDialog::FilamentSelectDialog(wxWindow* parent)
    : DPIDialog(parent, wxID_ANY, _L("Select Filament"), wxDefaultPosition, wxDefaultSize,
                wxCAPTION | wxCLOSE_BOX)
{
    create();
    wxGetApp().UpdateDlgDarkUI(this);
}

void FilamentSelectDialog::create()
{
    SetBackgroundColour(dlg_bg());

    m_btn_bg_green = StateColor(
        std::pair<wxColour, int>(wxColour(27,  136, 68),  StateColor::Pressed),
        std::pair<wxColour, int>(wxColour(61,  203, 115), StateColor::Hovered),
        std::pair<wxColour, int>(wxColour(0,   174, 66),  StateColor::Normal));

    m_btn_bg_gray = StateColor(
        std::pair<wxColour, int>(wxColour(206, 206, 206), StateColor::Pressed),
        std::pair<wxColour, int>(*wxWHITE,                StateColor::Focused),
        std::pair<wxColour, int>(wxColour(238, 238, 238), StateColor::Hovered),
        std::pair<wxColour, int>(*wxWHITE,                StateColor::Normal));

    auto* main = new wxBoxSizer(wxVERTICAL);

    // Full-width 1px grey separator below the title bar
    auto* sep = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(1)));
    sep->SetBackgroundColour(dlg_separator());
    main->Add(sep, 0, wxEXPAND);

    main->Add(build_default_page(this), 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(20));

    // bottom buttons
    auto* btn_sizer = new wxBoxSizer(wxHORIZONTAL);
    btn_sizer->AddStretchSpacer(1);

    auto* ok = new Button(this, _L("Confirm"));
    ok->SetBackgroundColor(m_btn_bg_green);
    ok->SetBorderColor(sc(wxColour(0, 174, 66)));
    ok->SetTextColor(sc(wxColour("#FFFFFE")));
    ok->SetMinSize(AMS_MATERIALS_SETTING_BUTTON_SIZE);
    ok->SetCornerRadius(FromDIP(12));
    ok->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_confirm(); });

    auto* close = new Button(this, _L("Close"));
    close->SetBackgroundColor(m_btn_bg_gray);
    close->SetBorderColor(sc(AMS_MATERIALS_SETTING_GREY900));
    close->SetTextColor(sc(AMS_MATERIALS_SETTING_GREY900));
    close->SetMinSize(AMS_MATERIALS_SETTING_BUTTON_SIZE);
    close->SetCornerRadius(FromDIP(12));
    close->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });

    btn_sizer->Add(ok,    0, wxALIGN_CENTER | wxRIGHT, FromDIP(20));
    btn_sizer->Add(close, 0, wxALIGN_CENTER, 0);
    main->Add(btn_sizer, 0, wxEXPAND | wxALL, FromDIP(12));

    SetSizer(main);
    SetMinSize(FromDIP(wxSize(493, 556)));
    SetSize(FromDIP(wxSize(493, 556)));
    Layout();
}



wxWindow* FilamentSelectDialog::build_default_page(wxWindow* parent)
{
    auto* page = new wxPanel(parent, wxID_ANY);
    page->SetBackgroundColour(dlg_bg());
    auto* h = new wxBoxSizer(wxHORIZONTAL);

    m_brand_list = new wxScrolledWindow(page, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    m_brand_list->SetScrollRate(0, 10);
    m_brand_list->SetBackgroundColour(dlg_bg());
    m_brand_list_sizer = new wxBoxSizer(wxVERTICAL);
    m_brand_list->SetSizer(m_brand_list_sizer);

    m_type_list = new wxScrolledWindow(page, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    m_type_list->SetScrollRate(0, 10);
    m_type_list->SetBackgroundColour(dlg_bg());
    m_type_list_sizer = new wxBoxSizer(wxVERTICAL);
    m_type_list->SetSizer(m_type_list_sizer);

    auto* divider = new wxPanel(page, wxID_ANY, wxDefaultPosition, wxSize(1, -1));
    divider->SetBackgroundColour(dlg_divider());

    h->Add(m_brand_list, 2, wxEXPAND | wxTOP | wxBOTTOM | wxLEFT, FromDIP(4));
    h->Add(divider,      0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(4));
    h->Add(m_type_list,  3, wxEXPAND | wxTOP | wxBOTTOM | wxRIGHT, FromDIP(4));
    page->SetSizer(h);
    return page;
}

// data population

void FilamentSelectDialog::Popup(const wxArrayString&                          filament_items,
                                 const std::unordered_map<wxString, wxString>& vendors,
                                 const std::unordered_map<wxString, wxString>& types,
                                 const wxString&                               current_alias,
                                 std::set<std::string>                         printer_names)
{
    m_filament_items = filament_items;
    m_vendors        = vendors;
    m_types          = types;
    m_printer_names  = std::move(printer_names);
    m_checked_alias  = current_alias;

    fill_default_tab();

    Layout();
    wxGetApp().UpdateDlgDarkUI(this);
    ShowModal();
}


void FilamentSelectDialog::on_confirm()
{
    m_result = SelectionResult{};
    if (!m_checked_alias.IsEmpty())
        m_result.preset_alias = m_checked_alias;   // caller resolves filament_id/type/temp from alias
    if (!m_result.is_valid()) return;
    EndModal(wxID_OK);
}






void FilamentSelectDialog::fill_default_tab()
{
    m_brand_to_aliases.clear();
    m_brand_list_sizer->Clear(true);
    m_type_list_sizer->Clear(true);

    const wxString other = _L("Other");
    for (const wxString& alias : m_filament_items) {
        wxString vendor;
        auto it = m_vendors.find(alias);
        if (it != m_vendors.end()) vendor = it->second;
        if (vendor.IsEmpty()) vendor = other;
        m_brand_to_aliases[vendor].push_back(alias);
    }

    static const std::vector<wxString> type_order{ "PLA", "PETG", "ABS", "TPU" };
    for (auto& kv : m_brand_to_aliases) {
        std::sort(kv.second.begin(), kv.second.end(), [this](const wxString& l, const wxString& r) {
            wxString lt, rt;
            auto il = m_types.find(l), ir = m_types.find(r);
            if (il != m_types.end()) lt = il->second;
            if (ir != m_types.end()) rt = ir->second;
            auto i1 = std::find(type_order.begin(), type_order.end(), lt);
            auto i2 = std::find(type_order.begin(), type_order.end(), rt);
            if (i1 != i2) return i1 < i2;
            return l < r;
        });
    }

    std::vector<wxString> keys;
    for (auto& kv : m_brand_to_aliases) keys.push_back(kv.first);
    m_ordered_brands = order_brands(keys, other);

    for (const wxString& b : m_ordered_brands) {
        auto* row = new wxPanel(m_brand_list, wxID_ANY);
        row->SetBackgroundColour(dlg_bg());
        auto* h = new wxBoxSizer(wxHORIZONTAL);

        auto* lbl = new Label(row, b);
        lbl->SetFont(Label::Body_14);
        lbl->SetForegroundColour(dlg_text_secondary());
        h->Add(lbl, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(12));

        wxBitmap arrow_bmp = create_scaled_bitmap("chip_arrow_right", row, 16);
        auto* arrow = new wxStaticBitmap(row, wxID_ANY, arrow_bmp);
        h->Add(arrow, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));

        row->SetMinSize(wxSize(-1, FromDIP(36)));
        row->SetSizer(h);

        const wxString brand = b;
        auto on_click = [this, brand](wxMouseEvent&) { select_brand(brand); };
        row->Bind(wxEVT_LEFT_DOWN, on_click);
        for (auto* c : row->GetChildren()) c->Bind(wxEVT_LEFT_DOWN, on_click);

        m_brand_list_sizer->Add(row, 0, wxEXPAND);
    }
    m_brand_list->FitInside();

    // show first brand (prefer the one containing the current preset)
    wxString target = m_ordered_brands.empty() ? wxString() : m_ordered_brands.front();
    if (!m_checked_alias.IsEmpty()) {
        for (const auto& kv : m_brand_to_aliases) {
            if (std::find(kv.second.begin(), kv.second.end(), m_checked_alias) != kv.second.end()) {
                target = kv.first;
                break;
            }
        }
    }
    if (!target.IsEmpty()) select_brand(target);
}

void FilamentSelectDialog::select_brand(const wxString& brand)
{
    const wxString prev_brand = m_selected_brand;
    m_selected_brand = brand;

    // Freeze to suppress intermediate repaints during the list rebuild.
    m_type_list->Freeze();
    m_type_list_sizer->Clear(true);

    auto it = m_brand_to_aliases.find(brand);
    if (it != m_brand_to_aliases.end()) {
        for (const wxString& alias : it->second) {
            auto* row = new wxPanel(m_type_list, wxID_ANY);
            row->SetBackgroundColour(dlg_bg());
            auto* h = new wxBoxSizer(wxHORIZONTAL);

            auto* lbl = new Label(row, alias);
            lbl->SetFont(Label::Body_14);
            lbl->SetForegroundColour(dlg_text_primary());
            h->Add(lbl, 1, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(8));

            row->SetSizer(h);
            const wxColour row_bg = (alias == m_checked_alias) ? dlg_row_hl() : dlg_bg();
            row->SetBackgroundColour(row_bg);
            for (auto* c : row->GetChildren()) c->SetBackgroundColour(row_bg);

            const wxString cur_brand = brand;
            auto on_type_click = [this, alias, cur_brand](wxMouseEvent&) {
                m_checked_alias = alias;
                CallAfter([this, cur_brand]() { select_brand(cur_brand); });
            };
            row->Bind(wxEVT_LEFT_DOWN, on_type_click);
            for (auto* c : row->GetChildren()) c->Bind(wxEVT_LEFT_DOWN, on_type_click);

            m_type_list_sizer->Add(row, 0, wxEXPAND);
        }
    }
    m_type_list->FitInside();
    m_type_list->Layout();
    m_type_list->Thaw();

    for (auto* w : m_brand_list->GetChildren()) {
        auto* row = dynamic_cast<wxPanel*>(w);
        if (!row) continue;

        // Extract brand name from the first Label child.
        wxString row_brand;
        for (auto* c : row->GetChildren()) {
            if (auto* lbl = dynamic_cast<Label*>(c)) { row_brand = lbl->GetLabel(); break; }
        }
        const bool sel     = (row_brand == brand);
        const bool was_sel = (row_brand == prev_brand);
        if (sel == was_sel) continue;
        const wxColour sel_bg = dlg_row_hl();
        const wxColour unsel_bg = dlg_bg();
        row->SetBackgroundColour(sel ? sel_bg : unsel_bg);

        const wxColour fg = sel ? dlg_text_primary() : dlg_text_secondary();
        for (auto* c : row->GetChildren()) {
            // Children must stay transparent so the custom-painted bg shows through.
            c->SetBackgroundColour(sel ? sel_bg : unsel_bg);
            if (auto* lbl = dynamic_cast<Label*>(c))
                lbl->SetForegroundColour(fg);
            // Arrow colour unchanged — always GREY300
        }
        row->Refresh();
    }
}

void FilamentSelectDialog::on_dpi_changed(const wxRect& /*suggested_rect*/)
{
    Layout();
}

}} // namespace Slic3r::GUI
