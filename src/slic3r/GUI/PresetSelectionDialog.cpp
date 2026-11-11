#include "PresetSelectionDialog.hpp"

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "MsgDialog.hpp"
#include "GUI.hpp"
#include "Plater.hpp"
#include "UnsavedChangesDialog.hpp"
#include "EncodedFilament.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/StateColor.hpp"

#include <wx/checkbox.h>
#include <wx/checklst.h>
#include <wx/simplebook.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/statline.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include <algorithm>
#include <functional>
#include <cstdlib>

#include <boost/algorithm/string.hpp>
#include <boost/log/trivial.hpp>

namespace Slic3r { namespace GUI {

static std::string first_string(const Preset &p, const char *key)
{
    auto *opt = p.config.option<ConfigOptionStrings>(key);
    return (opt && !opt->values.empty()) ? opt->values.front() : std::string();
}

PresetSelectionDialog::PresetSelectionDialog(wxWindow *parent, Page start_page)
    : DPIDialog(parent, wxID_ANY, _L("Printers and filaments"), wxDefaultPosition, wxDefaultSize,
                wxCAPTION | wxCLOSE_BOX | wxRESIZE_BORDER)
{
    // Theme: explicit colours from the app palette for both modes. Native GTK
    // widgets otherwise keep the system theme's text colour, which ended up
    // dark-on-dark (and the filament checklist white) in the app's dark mode.
    // Follow the same switch the rest of the themed UI uses (StateColor's dark
    // flag, set in GUI_App::init_label_colours): the "dark_color_mode" config
    // key can be rewritten from the GTK appearance at runtime on Linux, which
    // would leave this dialog light inside a dark main window.
    const bool dark = StateColor::darkModeColorFor(ThemeColor::White) != ThemeColor::White;
    m_bg       = dark ? wxColour("#2D2D31") : ThemeColor::White;
    m_fg       = dark ? wxColour("#EFEFF0") : ThemeColor::TextPrimary;
    m_fg_muted = dark ? wxColour("#B3B3B5") : ThemeColor::TextMuted;
    m_field_bg = dark ? wxColour("#36363C") : ThemeColor::White;
    SetBackgroundColour(m_bg);
    SetForegroundColour(m_fg);

    auto *top = new wxBoxSizer(wxVERTICAL);

    // Tab strip (themed buttons instead of a native wxNotebook, whose GTK tab
    // labels ignore the app's dark palette).
    auto *tab_bar = new wxBoxSizer(wxHORIZONTAL);
    for (const wxString &label : {_L("Printers"), _L("Filaments")}) {
        auto *tab = new ::Button(this, label);
        tab->SetFont(Label::Body_14);
        tab->SetCornerRadius(FromDIP(4));
        tab->SetPaddingSize(FromDIP(wxSize(14, 5)));
        tab->SetCanFocus(false);
        const int idx = int(m_tabs.size());
        tab->Bind(wxEVT_BUTTON, [this, idx](wxCommandEvent &) { select_page(idx); });
        tab_bar->Add(tab, 0, wxRIGHT, FromDIP(6));
        m_tabs.push_back(tab);
    }
    top->Add(tab_bar, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(10));
    m_rule = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
    top->Add(m_rule, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(6));

    m_book = new wxSimplebook(this, wxID_ANY);
    auto *printers_page  = new wxScrolledWindow(m_book, wxID_ANY);
    auto *filaments_page = new wxPanel(m_book, wxID_ANY);
    build_printers_page(printers_page);
    build_filaments_page(filaments_page);
    m_book->AddPage(printers_page, _L("Printers"));
    m_book->AddPage(filaments_page, _L("Filaments"));
    top->Add(m_book, 1, wxEXPAND | wxALL, FromDIP(10));

    auto *btns       = new wxBoxSizer(wxHORIZONTAL);
    auto *create_btn = make_button(this, _L("Create custom filament..."), false);
    create_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        if (apply()) EndModal(ID_CREATE_FILAMENT);
    });
    btns->Add(create_btn, 0, wxALIGN_CENTER_VERTICAL);
    btns->AddStretchSpacer();
    auto *ok     = make_button(this, _L("OK"), true, wxID_OK);
    auto *cancel = make_button(this, _L("Cancel"), false, wxID_CANCEL);
    cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_CANCEL); });
    ok->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        if (apply()) EndModal(wxID_OK);
    });
    btns->Add(ok, 0, wxRIGHT, FromDIP(8));
    btns->Add(cancel, 0);
    top->Add(btns, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));

    SetSizer(top);
    SetSize(FromDIP(wxSize(640, 620)));
    SetMinSize(FromDIP(wxSize(480, 400)));
    apply_theme();
    select_page(start_page == pgFilaments ? pgFilaments : pgPrinters);
    CenterOnParent();
}

void PresetSelectionDialog::build_printers_page(wxWindow *page)
{
    auto       *scrolled = static_cast<wxScrolledWindow *>(page);
    auto       *sizer    = new wxBoxSizer(wxVERTICAL);
    AppConfig  *cfg      = wxGetApp().app_config;
    PresetBundle *bundle = wxGetApp().preset_bundle;

    sizer->Add(new wxStaticText(page, wxID_ANY, _L("Tick the printer models and nozzle sizes to show in the printer list.")),
               0, wxALL, FromDIP(8));

    // Bambu Lab first, then any other installed vendor bundle.
    std::vector<const VendorProfile *> vendors;
    for (const auto &[id, vp] : bundle->vendors)
        if (id == PresetBundle::BBL_BUNDLE) vendors.insert(vendors.begin(), &vp); else vendors.push_back(&vp);

    for (const VendorProfile *vp : vendors) {
        if (vendors.size() > 1) {
            auto *title = new wxStaticText(page, wxID_ANY, from_u8(vp->name));
            title->SetFont(title->GetFont().Bold());
            sizer->Add(title, 0, wxLEFT | wxTOP, FromDIP(8));
        }
        auto *grid = new wxFlexGridSizer(2, FromDIP(4), FromDIP(16));
        for (const VendorProfile::PrinterModel &model : vp->models) {
            if (model.technology != ptFFF || model.variants.empty()) continue;
            grid->Add(new wxStaticText(page, wxID_ANY, from_u8(model.name)), 0, wxALIGN_CENTER_VERTICAL);
            auto *row = new wxBoxSizer(wxHORIZONTAL);
            // Same column per nozzle size on every row (profiles list them in varying order).
            std::vector<VendorProfile::PrinterVariant> variants(model.variants.begin(), model.variants.end());
            std::stable_sort(variants.begin(), variants.end(), [](const auto &a, const auto &b) {
                return std::atof(a.name.c_str()) < std::atof(b.name.c_str());
            });
            for (const VendorProfile::PrinterVariant &var : variants) {
                auto *box = new wxCheckBox(page, wxID_ANY, from_u8(var.name) + " mm");
                box->SetValue(cfg->get_variant(vp->id, model.id, var.name));
                row->Add(box, 0, wxRIGHT, FromDIP(12));
                m_variants.push_back({vp->id, model.id, var.name, box});
            }
            grid->Add(row, 0, wxALIGN_CENTER_VERTICAL);
        }
        sizer->Add(grid, 0, wxALL, FromDIP(8));
    }
    scrolled->SetSizer(sizer);
    scrolled->SetScrollRate(0, FromDIP(10));
}

void PresetSelectionDialog::build_filaments_page(wxWindow *page)
{
    AppConfig    *cfg    = wxGetApp().app_config;
    PresetBundle *bundle = wxGetApp().preset_bundle;
    const auto    enabled = cfg->has_section(AppConfig::SECTION_FILAMENTS) ? cfg->get_section(AppConfig::SECTION_FILAMENTS)
                                                                          : std::map<std::string, std::string>();

    std::map<std::string, size_t> by_alias;
    for (const Preset &p : bundle->filaments) {
        if (!p.is_system || p.alias.empty()) continue;
        auto it = by_alias.find(p.alias);
        if (it == by_alias.end()) {
            it = by_alias.emplace(p.alias, m_groups.size()).first;
            m_groups.push_back({p.alias, first_string(p, "filament_vendor"), first_string(p, "filament_type"), {}, false});
        }
        FilamentGroup &g = m_groups[it->second];
        g.names.push_back(p.name);
        if (enabled.count(p.name)) g.checked = true;
    }
    std::sort(m_groups.begin(), m_groups.end(), [](const FilamentGroup &a, const FilamentGroup &b) {
        const bool ab = boost::istarts_with(a.vendor, "Bambu"), bb = boost::istarts_with(b.vendor, "Bambu");
        if (ab != bb) return ab;
        if (a.vendor != b.vendor) return a.vendor < b.vendor;
        return a.alias < b.alias;
    });

    auto *sizer = new wxBoxSizer(wxVERTICAL);
    auto *bar   = new wxBoxSizer(wxHORIZONTAL);
    m_search    = new wxTextCtrl(page, wxID_ANY);
    m_search->SetHint(_L("Search"));
    m_only_compatible = new wxCheckBox(page, wxID_ANY, _L("Only filaments for the ticked printers"));
    m_only_compatible->SetValue(true);
    bar->Add(m_search, 1, wxRIGHT, FromDIP(8));
    bar->Add(m_only_compatible, 0, wxALIGN_CENTER_VERTICAL);
    sizer->Add(bar, 0, wxEXPAND | wxALL, FromDIP(8));

    m_list = new wxCheckListBox(page, wxID_ANY);
    sizer->Add(m_list, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));

    auto *sel_bar = new wxBoxSizer(wxHORIZONTAL);
    auto *all     = make_button(page, _L("Select all shown"), false, wxID_ANY);
    auto *none    = make_button(page, _L("Clear all shown"), false, wxID_ANY);
    auto  set_all = [this](bool v) {
        for (unsigned i = 0; i < m_shown.size(); ++i) { m_groups[m_shown[i]].checked = v; m_list->Check(i, v); }
    };
    all->Bind(wxEVT_BUTTON, [set_all](wxCommandEvent &) { set_all(true); });
    none->Bind(wxEVT_BUTTON, [set_all](wxCommandEvent &) { set_all(false); });
    sel_bar->Add(all, 0, wxRIGHT, FromDIP(8));
    sel_bar->Add(none, 0);
    sizer->Add(sel_bar, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
    page->SetSizer(sizer);

    m_search->Bind(wxEVT_TEXT, [this](wxCommandEvent &) { refresh_filament_list(); });
    m_only_compatible->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) { refresh_filament_list(); });
    m_list->Bind(wxEVT_CHECKLISTBOX, [this](wxCommandEvent &e) {
        const int row = e.GetInt();
        if (row >= 0 && size_t(row) < m_shown.size()) m_groups[m_shown[row]].checked = m_list->IsChecked(row);
    });
}

std::set<std::string> PresetSelectionDialog::compatible_aliases()
{
    PresetBundle *bundle = wxGetApp().preset_bundle;
    std::set<std::string> out;
    for (const VariantBox &v : m_variants) {
        if (!v.box->GetValue()) continue;
        for (const Preset &printer : bundle->printers) {
            if (!printer.is_system || !printer.vendor || printer.vendor->id != v.vendor) continue;
            if (printer.config.opt_string("printer_model") != v.model || printer.config.opt_string("printer_variant") != v.variant) continue;
            auto it = m_compat_cache.find(printer.name);
            if (it == m_compat_cache.end()) {
                std::set<std::string> aliases;
                const PresetWithVendorProfile pp = bundle->printers.get_preset_with_vendor_profile(printer);
                for (const Preset &f : bundle->filaments)
                    if (f.is_system && !f.alias.empty() && !aliases.count(f.alias) &&
                        is_compatible_with_printer(bundle->filaments.get_preset_with_vendor_profile(f), pp))
                        aliases.insert(f.alias);
                it = m_compat_cache.emplace(printer.name, std::move(aliases)).first;
            }
            out.insert(it->second.begin(), it->second.end());
        }
    }
    return out;
}

void PresetSelectionDialog::refresh_filament_list()
{
    if (!m_list) return;
    const std::string     needle = boost::to_lower_copy(into_u8(m_search->GetValue()));
    const bool            filter = m_only_compatible->GetValue();
    std::set<std::string> compat;
    if (filter) compat = compatible_aliases();

    m_list->Freeze();
    m_list->Clear();
    m_shown.clear();
    for (size_t i = 0; i < m_groups.size(); ++i) {
        const FilamentGroup &g = m_groups[i];
        if (filter && !compat.count(g.alias)) continue;
        const std::string label = g.alias + "   (" + g.vendor + (g.type.empty() ? "" : " " + g.type) + ")";
        if (!needle.empty() && boost::to_lower_copy(label).find(needle) == std::string::npos) continue;
        const int row = m_list->Append(from_u8(label));
        m_list->Check(row, g.checked);
        m_shown.push_back(i);
    }
    m_list->Thaw();
}

bool PresetSelectionDialog::apply()
{
    GUI_App      &app    = wxGetApp();
    AppConfig    *cfg    = app.app_config;
    PresetBundle *bundle = app.preset_bundle;

    // Vendors: rebuild the vendors shown in the dialog from the checkboxes, keep any others.
    AppConfig::VendorMap new_vendors = cfg->vendors();
    for (const VariantBox &v : m_variants) new_vendors.erase(v.vendor);
    std::string preferred_model, preferred_variant;
    bool        any = false;
    for (const VariantBox &v : m_variants) {
        if (!v.box->GetValue()) continue;
        any = true;
        new_vendors[v.vendor][v.model].insert(v.variant);
        if (preferred_model.empty() && !cfg->get_variant(v.vendor, v.model, v.variant)) {
            preferred_model   = v.model;
            preferred_variant = v.variant;
        }
    }
    if (!any) {
        MessageDialog(this, _L("Please select at least one printer nozzle."), _L("Printers and filaments"), wxOK | wxICON_WARNING).ShowModal();
        select_page(pgPrinters);
        return false;
    }

    // Filaments: start from the current section (keeps user/unknown entries), then apply the groups.
    std::map<std::string, std::string> new_filaments = cfg->has_section(AppConfig::SECTION_FILAMENTS) ?
                                                           cfg->get_section(AppConfig::SECTION_FILAMENTS) : std::map<std::string, std::string>();
    for (const FilamentGroup &g : m_groups)
        for (const std::string &name : g.names) {
            if (g.checked) new_filaments[name] = "true"; else new_filaments.erase(name);
        }

    const auto old_filaments = cfg->has_section(AppConfig::SECTION_FILAMENTS) ? cfg->get_section(AppConfig::SECTION_FILAMENTS)
                                                                             : std::map<std::string, std::string>();
    const bool changed = new_vendors != cfg->vendors() || new_filaments != old_filaments;
    if (!changed) return true;

    bool apply_keeped_changes = false;
    int  act_btns             = UnsavedChangesDialog::ActionButtons::KEEP | UnsavedChangesDialog::ActionButtons::SAVE;
    if (!app.check_and_keep_current_preset_changes(_L("Configuration package changed"), _L("The visible printers or filaments are about to change"),
                                                   act_btns, &apply_keeped_changes))
        return false;

    bundle->export_selections(*cfg);
    cfg->set_section(AppConfig::SECTION_FILAMENTS, new_filaments);
    cfg->set_vendors(std::move(new_vendors));

    std::string errors;
    PresetsConfigSubstitutions subs;
    std::tie(subs, errors) = bundle->load_presets(*cfg, ForwardCompatibilitySubstitutionRule::EnableSilentDisableSystem,
                                                  {preferred_model, preferred_variant, std::string(), std::string()});
    if (!errors.empty()) show_error(nullptr, errors);
    align_project_filament_primary_colors_with_json(bundle);
    bundle->export_selections(*cfg);
    if (apply_keeped_changes) app.apply_keeped_preset_modifications();
    cfg->save();
    BOOST_LOG_TRIVIAL(info) << "PresetSelectionDialog: applied printer/filament selection";
    return true;
}

bool PresetSelectionDialog::run(Page start_page)
{
    GUI_App &app = wxGetApp();
    app.preset_bundle->export_selections(*app.app_config);
    PresetSelectionDialog dlg(app.mainframe, start_page);
    const int res = dlg.ShowModal();
    if (res != wxID_OK && res != ID_CREATE_FILAMENT) return false;
    app.load_current_presets();
    if (res == ID_CREATE_FILAMENT)
        wxQueueEvent(app.plater(), new SimpleEvent(EVT_CREATE_FILAMENT));
    return true;
}

// Same StateColor recipe as the other native dialogs (PurgeModeDialog etc.);
// StateColor maps these light colours to their dark equivalents itself.
::Button *PresetSelectionDialog::make_button(wxWindow *parent, const wxString &label, bool primary, wxWindowID id)
{
    StateColor bg = primary ?
        StateColor(std::pair<wxColour, int>(ThemeColor::BrandGreenPressed, StateColor::Pressed),
                   std::pair<wxColour, int>(ThemeColor::BrandGreenHovered, StateColor::Hovered),
                   std::pair<wxColour, int>(ThemeColor::BrandGreen, StateColor::Normal)) :
        StateColor(std::pair<wxColour, int>(ThemeColor::Grey400, StateColor::Pressed),
                   std::pair<wxColour, int>(ThemeColor::Grey300, StateColor::Hovered),
                   std::pair<wxColour, int>(ThemeColor::White, StateColor::Normal));
    StateColor bd(std::pair<wxColour, int>(primary ? ThemeColor::BrandGreen : ThemeColor::TextPrimary, StateColor::Normal));
    StateColor fg(std::pair<wxColour, int>(primary ? wxColour("#FFFFFE") : ThemeColor::TextPrimary, StateColor::Normal));
    auto *b = new ::Button(parent, label);
    b->SetId(id);
    b->SetFont(Label::Body_12);
    b->SetMinSize(wxSize(FromDIP(72), FromDIP(26)));
    b->SetCornerRadius(FromDIP(13));
    b->SetBackgroundColor(bg);
    b->SetBorderColor(bd);
    b->SetTextColor(fg);
    return b;
}

void PresetSelectionDialog::select_page(int page)
{
    m_book->SetSelection(page);
    // Selected tab: brand green with white text; others: plain text on the dialog background.
    for (size_t i = 0; i < m_tabs.size(); ++i) {
        const bool sel = int(i) == page;
        StateColor bg(std::pair<wxColour, int>(sel ? ThemeColor::BrandGreen : m_bg, StateColor::Normal));
        StateColor fg(std::pair<wxColour, int>(sel ? wxColour("#FFFFFE") : m_fg, StateColor::Normal));
        m_tabs[i]->SetBackgroundColor(bg);
        m_tabs[i]->SetBorderColor(bg);
        m_tabs[i]->SetTextColor(fg);
        m_tabs[i]->Refresh();
    }
    if (page == pgFilaments) refresh_filament_list();
}

// Recursively give every native control in the dialog the palette colours.
// ::Button children are themed through their StateColors; only their window
// background (the rounded-corner fill, copied from the parent at creation) is set.
void PresetSelectionDialog::apply_theme()
{
    std::function<void(wxWindow *)> walk = [&](wxWindow *w) {
        for (wxWindow *c : w->GetChildren()) {
            if (dynamic_cast<::Button *>(c)) { c->SetBackgroundColour(m_bg); c->Refresh(); continue; }
            if (dynamic_cast<wxCheckListBox *>(c) || dynamic_cast<wxTextCtrl *>(c)) {
                c->SetBackgroundColour(m_field_bg);
                c->SetForegroundColour(m_fg);
            } else if (dynamic_cast<wxStaticText *>(c) || dynamic_cast<wxCheckBox *>(c)) {
                c->SetBackgroundColour(m_bg);
                c->SetForegroundColour(m_fg);
            } else if (c == m_rule) {
                c->SetBackgroundColour(m_bg == ThemeColor::White ? ThemeColor::Grey350 : wxColour("#4C4C55"));
            } else {
                c->SetBackgroundColour(m_bg);
                c->SetForegroundColour(m_fg);
            }
            walk(c);
        }
    };
    walk(this);
}

void PresetSelectionDialog::on_dpi_changed(const wxRect &) { Layout(); Refresh(); }

}} // namespace Slic3r::GUI
