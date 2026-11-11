#include <algorithm>
#include <wx/display.h>
#include <wx/sizer.h>
#include <wx/choice.h>
#include <wx/grid.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <cmath>
#include <memory>
#include "Widgets/Button.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/StateColor.hpp"
#include "libslic3r/FlushVolCalc.hpp"
#include "WipeTowerDialog.hpp"
#include "GUI.hpp"
#include "I18N.hpp"
#include "GUI_App.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "MainFrame.hpp"
#include "DeviceCore/DevConfigUtil.h"


using namespace Slic3r;
using namespace Slic3r::GUI;

static const float g_min_flush_multiplier = 0.f;
static const float g_max_flush_multiplier = 3.f;

bool is_flush_config_modified()
{
    const auto &project_config    = wxGetApp().preset_bundle->project_config;
    const std::vector<double> &config_matrix     = (project_config.option<ConfigOptionFloats>("flush_volumes_matrix"))->values;
    bool                       use_fast_multiplier = project_config.option<ConfigOptionEnum<PrimeVolumeMode>>("prime_volume_mode")->value == PrimeVolumeMode::pvmFast;
    const std::vector<double> &config_multiplier   = (project_config.option<ConfigOptionFloats>(use_fast_multiplier ? "flush_multiplier_fast" : "flush_multiplier"))->values;
    auto physical_indices = wxGetApp().preset_bundle->physical_filament_config_indices();
    size_t full_n = project_config.option<ConfigOptionStrings>("filament_colour")->values.size();

    bool has_modify = false;
    for (int i = 0; i < config_multiplier.size(); i++) {
        if (config_multiplier[i] != 1) {
            has_modify = true;
            break;
        }
        std::vector<std::vector<double>> default_matrix = WipingDialog::CalcFlushingVolumes(i);
        size_t p_len = default_matrix.size();
        size_t nozzle_offset = i * full_n * full_n;
        for (size_t m = 0; m < p_len; m++) {
            for (size_t n = 0; n < p_len; n++) {
                size_t cfg_idx = nozzle_offset + physical_indices[m] * full_n + physical_indices[n];
                if (cfg_idx < config_matrix.size() && config_matrix[cfg_idx] != default_matrix[m][n] * config_multiplier[i]) {
                    has_modify = true;
                    break;
                }
            }
            if (has_modify) break;
        }
        if (has_modify) break;
    }
    return has_modify;
}

void open_flushing_dialog(wxEvtHandler *parent, const wxEvent &event)
{
    auto                      &project_config = wxGetApp().preset_bundle->project_config;

    WipingDialog dlg(static_cast<wxWindow *>(wxGetApp().mainframe));
    dlg.ShowModal();
    if (dlg.GetSubmitFlag()) {
        auto matrix = dlg.GetFlattenMatrix();
        auto flush_multipliers = dlg.GetMultipliers();
        (project_config.option<ConfigOptionFloats>("flush_volumes_matrix"))->values = std::vector<double>(matrix.begin(), matrix.end());
        bool        use_fast_multiplier = project_config.option<ConfigOptionEnum<PrimeVolumeMode>>("prime_volume_mode")->value == PrimeVolumeMode::pvmFast;
        const char *multiplier_key      = use_fast_multiplier ? "flush_multiplier_fast" : "flush_multiplier";
        (project_config.option<ConfigOptionFloats>(multiplier_key))->values = std::vector<double>(flush_multipliers.begin(), flush_multipliers.end());
        bool flushing_volume_modify = is_flush_config_modified();
        wxGetApp().sidebar().set_flushing_volume_warning(flushing_volume_modify);
        wxGetApp().preset_bundle->export_selections(*wxGetApp().app_config);
        wxGetApp().plater()->update_project_dirty_from_presets();
        wxPostEvent(parent, event);
    }
}

static std::vector<double> extract_physical_sub_matrix(
    const std::vector<double>& full_matrix, size_t full_n,
    const std::vector<size_t>& indices)
{
    size_t p = indices.size();
    std::vector<double> sub(p * p, 0.0);
    if (full_matrix.size() < full_n * full_n)
        return sub;
    for (size_t pi = 0; pi < p; ++pi)
        for (size_t pj = 0; pj < p; ++pj)
            sub[pi * p + pj] = full_matrix[indices[pi] * full_n + indices[pj]];
    return sub;
}

static std::vector<double> expand_physical_to_full_matrix(
    const std::vector<double>& sub_matrix,
    const std::vector<size_t>& indices, size_t full_n,
    const std::vector<double>& original_matrix)
{
    std::vector<double> full = original_matrix;
    if (full.size() < full_n * full_n)
        return full;
    size_t p = indices.size();
    for (size_t pi = 0; pi < p; ++pi)
        for (size_t pj = 0; pj < p; ++pj)
            full[indices[pi] * full_n + indices[pj]] = sub_matrix[pi * p + pj];
    return full;
}

namespace {

const wxColour& dlg_bg()       { static wxColour l("#FFFFFF"), d("#2D2D31"); return wxGetApp().dark_mode() ? d : l; }
const wxColour& row_bg(int r)  { static wxColour l0("#EEEEEE"), l1("#FFFFFF"), d0("#4C4C55"), d1("#2D2D31");
                                 return wxGetApp().dark_mode() ? (r % 2 == 0 ? d0 : d1) : (r % 2 == 0 ? l0 : l1); }
const wxColour& label_bg()     { static wxColour l("#EEEEEE"), d("#3B3B40"); return wxGetApp().dark_mode() ? d : l; }
const wxColour& line_col()     { static wxColour l("#CCCCCC"), d("#555555"); return wxGetApp().dark_mode() ? d : l; }
const wxColour& text_col()     { static wxColour l("#000000"), d("#E0E0E0"); return wxGetApp().dark_mode() ? d : l; }
const wxColour& tip_bg()       { static wxColour l("#EEEEEE"), d("#4C4C55"); return wxGetApp().dark_mode() ? d : l; }
const wxColour& tip_fg()       { static wxColour l("#333333"), d("#CCCCCC"); return wxGetApp().dark_mode() ? d : l; }

double luminance(const wxColour& c) { return (0.299 * c.Red() + 0.587 * c.Green() + 0.114 * c.Blue()) / 255.0; }

// Filament-number badge in the coloured header cells (like the old .icon-button).
void draw_badge(const wxGrid& grid, wxDC& dc, const wxRect& rect, const wxString& value, const wxColour& colour)
{
    const int  sz = grid.FromDIP(18);
    wxRect     r(rect.x + (rect.width - sz) / 2, rect.y + (rect.height - sz) / 2, sz, sz);
    dc.SetPen(wxPen(wxColour("#999999"), 1));
    dc.SetBrush(wxBrush(colour));
    dc.DrawRoundedRectangle(r, grid.FromDIP(4));
    dc.SetTextForeground(luminance(colour) > 0.5 ? *wxBLACK : *wxWHITE);
    wxFont f = grid.GetLabelFont();
    f.SetPointSize(std::max(7, f.GetPointSize() - 1));
    dc.SetFont(f);
    dc.DrawLabel(value, r, wxALIGN_CENTER);
}

class BadgeColRenderer : public wxGridColumnHeaderRendererDefault
{
public:
    explicit BadgeColRenderer(const wxColour& c) : m_colour(c) {}
    void DrawBorder(const wxGrid& grid, wxDC& dc, wxRect& rect) const override
    {
        dc.SetPen(wxPen(line_col()));
        dc.SetBrush(wxBrush(label_bg()));
        dc.DrawRectangle(rect);
    }
    void DrawLabel(const wxGrid& grid, wxDC& dc, const wxString& value, const wxRect& rect, int, int, int) const override
    { draw_badge(grid, dc, rect, value, m_colour); }
private:
    wxColour m_colour;
};

class BadgeRowRenderer : public wxGridRowHeaderRendererDefault
{
public:
    explicit BadgeRowRenderer(const wxColour& c) : m_colour(c) {}
    void DrawBorder(const wxGrid& grid, wxDC& dc, wxRect& rect) const override
    {
        dc.SetPen(wxPen(line_col()));
        dc.SetBrush(wxBrush(label_bg()));
        dc.DrawRectangle(rect);
    }
    void DrawLabel(const wxGrid& grid, wxDC& dc, const wxString& value, const wxRect& rect, int, int, int) const override
    { draw_badge(grid, dc, rect, value, m_colour); }
private:
    wxColour m_colour;
};

class CornerRenderer : public wxGridCornerHeaderRendererDefault
{
public:
    void DrawBorder(const wxGrid& grid, wxDC& dc, wxRect& rect) const override
    {
        dc.SetPen(wxPen(line_col()));
        dc.SetBrush(wxBrush(label_bg()));
        dc.DrawRectangle(rect);
        // wxGrid never calls DrawLabel() for the corner, so draw the caption here.
        wxFont f = grid.GetLabelFont();
        f.SetWeight(wxFONTWEIGHT_NORMAL);
        dc.SetFont(f);
        dc.SetTextForeground(text_col());
        dc.DrawLabel("from/to", rect, wxALIGN_CENTER);
    }
};

class FlushAttrProvider : public wxGridCellAttrProvider
{
public:
    explicit FlushAttrProvider(const std::vector<std::string>& colours)
    {
        for (const auto& c : colours) {
            wxColour col(wxString::FromUTF8(c));
            if (!col.IsOk()) col = *wxWHITE;
            m_cols.emplace_back(new BadgeColRenderer(col));
            m_rows.emplace_back(new BadgeRowRenderer(col));
        }
    }
    const wxGridColumnHeaderRenderer& GetColumnHeaderRenderer(int col) override
    { return (col >= 0 && size_t(col) < m_cols.size()) ? *m_cols[col] : wxGridCellAttrProvider::GetColumnHeaderRenderer(col); }
    const wxGridRowHeaderRenderer& GetRowHeaderRenderer(int row) override
    { return (row >= 0 && size_t(row) < m_rows.size()) ? *m_rows[row] : wxGridCellAttrProvider::GetRowHeaderRenderer(row); }
    const wxGridCornerHeaderRenderer& GetCornerRenderer() override { return m_corner; }
private:
    std::vector<std::unique_ptr<BadgeColRenderer>> m_cols;
    std::vector<std::unique_ptr<BadgeRowRenderer>> m_rows;
    CornerRenderer m_corner;
};

wxString format_multiplier(double v)
{
    // Same look as the old JS number input (1 -> "1", 1.5 -> "1.5").
    wxString s = wxString::FromCDouble(v, 2);
    if (s.Contains(".")) {
        while (s.EndsWith("0")) s.RemoveLast();
        if (s.EndsWith(".")) s.RemoveLast();
    }
    return s;
}

} // namespace

void WipingDialog::LoadData()
{
    auto full_config = wxGetApp().preset_bundle->full_config();
    auto all_filament_colors = full_config.option<ConfigOptionStrings>("filament_colour")->values;
    bool use_fast_multiplier  = full_config.option<ConfigOptionEnum<PrimeVolumeMode>>("prime_volume_mode")->value == PrimeVolumeMode::pvmFast;
    auto flush_multiplier     = use_fast_multiplier ? full_config.option<ConfigOptionFloats>("flush_multiplier_fast")->values :
                                                      full_config.option<ConfigOptionFloats>("flush_multiplier")->values;
    int nozzle_num = full_config.option<ConfigOptionFloatsNullable>("nozzle_diameter")->values.size();
    auto raw_matrix_data = full_config.option<ConfigOptionFloats>("flush_volumes_matrix")->values;
    auto nozzle_flush_dataset = full_config.option<ConfigOptionIntsNullable>("nozzle_flush_dataset")->values;

    m_physical_indices = wxGetApp().preset_bundle->physical_filament_config_indices();
    size_t full_n = all_filament_colors.size();

    m_colors.clear();
    for (size_t idx : m_physical_indices)
        m_colors.push_back(all_filament_colors[idx]);

    std::vector<std::vector<double>> full_matrixs;
    for (int idx = 0; idx < nozzle_num; ++idx)
        full_matrixs.emplace_back(get_flush_volumes_matrix(raw_matrix_data, idx, nozzle_num));

    m_edit_matrixs.clear();
    for (const auto& fm : full_matrixs)
        m_edit_matrixs.emplace_back(extract_physical_sub_matrix(fm, full_n, m_physical_indices));

    flush_multiplier.resize(nozzle_num, 1);

    m_default_matrixs.clear();
    for (int idx = 0; idx < nozzle_num; ++idx) {
        std::vector<double> flat;
        for (const auto& row : CalcFlushingVolumes(idx))
            flat.insert(flat.end(), row.begin(), row.end());
        m_default_matrixs.emplace_back(std::move(flat));
    }

    m_raw_matrixs       = full_matrixs;
    m_flush_multipliers = flush_multiplier;
    m_edit_multipliers  = flush_multiplier;
    m_extruder_num      = nozzle_num;

    m_min_flush_volumes.clear();
    m_max_flush_volumes.clear();
    for (int idx = 0; idx < nozzle_num; ++idx) {
        const std::vector<int> &min_flush_volumes = get_min_flush_volumes(full_config, idx);
        int min_flush_from_nozzle_volume = *min_element(min_flush_volumes.begin(), min_flush_volumes.end());
        GenericFlushPredictor pd(nozzle_flush_dataset[idx]);
        int min_flush_from_flush_data = pd.get_min_flush_volume();
        m_min_flush_volumes.push_back(std::min(min_flush_from_flush_data, min_flush_from_nozzle_volume));
        m_max_flush_volumes.push_back(m_max_flush_volume);
    }
}

WipingDialog::WipingDialog(wxWindow* parent, const int max_flush_volume) :
    wxDialog(parent, wxID_ANY, _(L("Flushing volumes for filament change")),
    wxDefaultPosition, wxDefaultSize,
    wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
    m_max_flush_volume(max_flush_volume)
{
    std::string icon_path = (boost::format("%1%/images/BambuStudioTitle.ico") % Slic3r::resources_dir()).str();
    SetIcon(wxIcon(Slic3r::encode_path(icon_path.c_str()), wxBITMAP_TYPE_ICO));
    LoadData();
    BuildUi();
}

void WipingDialog::BuildUi()
{
    const bool dark = wxGetApp().dark_mode();
    m_text_normal = text_col();
    m_text_warn   = wxColour("#FFA500");                       // JS: orange
    m_text_error  = dark ? wxColour("#F44336") : wxColour("#FF0000");
    SetBackgroundColour(dlg_bg());
    SetForegroundColour(text_col());

    auto *main_sizer = new wxBoxSizer(wxVERTICAL);
    const int pad    = FromDIP(20);

    // Auto-calculate tip.
    auto *tip_panel = new wxPanel(this);
    tip_panel->SetBackgroundColour(tip_bg());
    auto *tip = new wxStaticText(tip_panel, wxID_ANY,
        _L("Studio would re-calculate your flushing volumes everytime the filaments color changed or filaments changed. You could disable the auto-calculate in Bambu Studio > Preferences"));
    tip->SetForegroundColour(tip_fg());
    tip->SetBackgroundColour(tip_bg());
    tip->SetFont(Label::Body_12);
    auto *tip_sizer = new wxBoxSizer(wxVERTICAL);
    tip_sizer->Add(tip, 0, wxALL, FromDIP(10));
    tip_panel->SetSizer(tip_sizer);
    main_sizer->Add(tip_panel, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, pad);

    StateColor btn_bg_green(std::pair<wxColour, int>(wxColour(144, 144, 144), StateColor::Disabled),
                            std::pair<wxColour, int>(wxColour(27, 136, 68), StateColor::Pressed),
                            std::pair<wxColour, int>(wxColour(61, 203, 115), StateColor::Hovered),
                            std::pair<wxColour, int>(dark ? wxColour("#3A8F44") : wxColour("#21A452"), StateColor::Normal));
    StateColor btn_text_white(std::pair<wxColour, int>(wxColour(255, 255, 254), StateColor::Normal));
    auto make_green = [&](const wxString& label) {
        auto *b = new Button(this, label);
        b->SetBackgroundColor(btn_bg_green);
        b->SetBorderColor(btn_bg_green);
        b->SetTextColor(btn_text_white);
        b->SetFont(Label::Body_12);
        b->SetCornerRadius(FromDIP(12));
        b->SetMinSize(wxSize(FromDIP(60), FromDIP(26)));
        return b;
    };

    // Re-calculate + extruder (nozzle) selector.
    auto *ctrl_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto *calc_btn   = make_green(_L("Re-calculate"));
    calc_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OnRecalculate(); });
    ctrl_sizer->Add(calc_btn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));

    m_extruder_choice = new wxChoice(this, wxID_ANY);
    {
        std::string pt = wxGetApp().preset_bundle->printers.get_edited_preset().get_printer_type(wxGetApp().preset_bundle);
        // Same order/labels as the web dialog: entry 0 = deputy (left), entry 1 = main (right).
        m_extruder_choice->Append(_L(DevPrinterConfigUtil::get_toolhead_display_name(pt, DEPUTY_EXTRUDER_ID, ToolHeadComponent::Extruder, ToolHeadNameCase::SentenceCase)));
        m_extruder_choice->Append(_L(DevPrinterConfigUtil::get_toolhead_display_name(pt, MAIN_EXTRUDER_ID, ToolHeadComponent::Extruder, ToolHeadNameCase::SentenceCase)));
        for (int i = 2; i < m_extruder_num; ++i)
            m_extruder_choice->Append(wxString::Format(_L("Nozzle %d"), i + 1));
    }
    m_extruder_choice->SetSelection(0);
    m_extruder_choice->Show(m_extruder_num > 1);
    m_extruder_choice->Bind(wxEVT_CHOICE, [this](wxCommandEvent& e) { OnExtruderSelect(e.GetSelection()); });
    ctrl_sizer->Add(m_extruder_choice, 0, wxALIGN_CENTER_VERTICAL);
    main_sizer->AddSpacer(FromDIP(10));
    main_sizer->Add(ctrl_sizer, 0, wxLEFT | wxRIGHT, pad);

    // Matrix.
    const int n = int(m_colors.size());
    m_grid = new wxGrid(this, wxID_ANY);
    m_grid->CreateGrid(n, n);
    m_grid->GetTable()->SetAttrProvider(new FlushAttrProvider(m_colors));
    m_grid->EnableDragRowSize(false);
    m_grid->EnableDragColSize(false);
    m_grid->EnableDragGridSize(false);
    m_grid->DisableDragColMove();
    m_grid->SetDefaultCellAlignment(wxALIGN_CENTER, wxALIGN_CENTER);
    m_grid->SetDefaultCellFont(Label::Body_12);
    m_grid->SetLabelFont(Label::Body_12);
    m_grid->SetGridLineColour(line_col());
    m_grid->SetLabelBackgroundColour(label_bg());
    m_grid->SetLabelTextColour(text_col());
    m_grid->SetDefaultCellBackgroundColour(dlg_bg());
    m_grid->SetDefaultCellTextColour(text_col());
    m_grid->SetBackgroundColour(dlg_bg());
    m_grid->SetCellHighlightColour(wxColour("#00AE42"));
    m_grid->SetSelectionBackground(dark ? wxColour("#3A5F44") : wxColour("#EDFAF2"));
    m_grid->SetSelectionMode(wxGrid::wxGridSelectNone); // keep warning text colours visible (no selection overlay)
    m_grid->SetSelectionForeground(text_col());
    const int cell_w = FromDIP(52), cell_h = FromDIP(28);
    m_grid->SetDefaultColSize(cell_w, true);
    m_grid->SetDefaultRowSize(cell_h, true);
    m_grid->SetRowLabelSize(FromDIP(60));
    m_grid->SetColLabelSize(cell_h);
    for (int r = 0; r < n; ++r) {
        m_grid->SetRowLabelValue(r, wxString::Format("%d", r + 1));
        m_grid->SetColLabelValue(r, wxString::Format("%d", r + 1));
        for (int c = 0; c < n; ++c) {
            m_grid->SetCellBackgroundColour(r, c, row_bg(r));
            if (r == c) m_grid->SetReadOnly(r, c, true);
        }
    }
    m_grid->Bind(wxEVT_GRID_CELL_CHANGED, &WipingDialog::OnCellChanged, this);
    // Matrix area up to 1000x500 DIP, then scroll (headers stay in place like the sticky web table).
    const wxSize want(FromDIP(60) + n * cell_w + FromDIP(4), cell_h * (n + 1) + FromDIP(4));
    m_grid->SetMinSize(wxSize(std::min(want.x, FromDIP(1000)), std::min(want.y, FromDIP(500))));
    main_sizer->AddSpacer(FromDIP(10));
    main_sizer->Add(m_grid, 1, wxEXPAND | wxLEFT | wxRIGHT, pad);

    // Description + ranges + multiplier.
    auto *desc = new wxStaticText(this, wxID_ANY, _L("Flushing volume (mm³) for each filament pair."));
    desc->SetFont(Label::Body_12);
    main_sizer->AddSpacer(FromDIP(10));
    main_sizer->Add(desc, 0, wxLEFT | wxRIGHT, pad);
    m_volume_range = new wxStaticText(this, wxID_ANY, "");
    m_volume_range->SetFont(Label::Body_12);
    main_sizer->AddSpacer(FromDIP(5));
    main_sizer->Add(m_volume_range, 0, wxLEFT | wxRIGHT, pad);

    auto *mult_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto *mult_label = new wxStaticText(this, wxID_ANY, _L("Multiplier"));
    mult_label->SetFont(Label::Body_12);
    m_multiplier_input = new wxTextCtrl(this, wxID_ANY, format_multiplier(m_edit_multipliers.empty() ? 1. : m_edit_multipliers[0]),
                                        wxDefaultPosition, wxSize(FromDIP(70), -1));
    if (dark) m_multiplier_input->SetBackgroundColour(wxColour("#4C4C55"));
    m_multiplier_input->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { OnMultiplierText(); });
    mult_sizer->Add(mult_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    mult_sizer->Add(m_multiplier_input, 0, wxALIGN_CENTER_VERTICAL);
    main_sizer->AddSpacer(FromDIP(8));
    main_sizer->Add(mult_sizer, 0, wxLEFT | wxRIGHT, pad);

    main_sizer->AddSpacer(FromDIP(5));
    m_multiplier_range = new wxStaticText(this, wxID_ANY, "");
    m_multiplier_range->SetFont(Label::Body_12);
    m_multiplier_range->SetForegroundColour(dark ? wxColour("#A0A0A0") : wxColour("#666666"));
    main_sizer->Add(m_multiplier_range, 0, wxLEFT | wxRIGHT, pad);

    // Save / Cancel.
    auto *btn_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto *ok_btn    = make_green(_L("OK"));
    auto *cancel_btn = new Button(this, _L("Cancel"));
    cancel_btn->SetFont(Label::Body_12);
    cancel_btn->SetCornerRadius(FromDIP(12));
    cancel_btn->SetMinSize(wxSize(FromDIP(60), FromDIP(26)));
    if (dark) {
        cancel_btn->SetBackgroundColor(StateColor(std::pair<wxColour, int>(wxColour("#2D2D31"), StateColor::Normal)));
        cancel_btn->SetBorderColor(StateColor(std::pair<wxColour, int>(wxColour("#E0E0E0"), StateColor::Normal)));
        cancel_btn->SetTextColor(StateColor(std::pair<wxColour, int>(wxColour("#E0E0E0"), StateColor::Normal)));
    }
    ok_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OnSave(); });
    cancel_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });
    btn_sizer->AddStretchSpacer();
    btn_sizer->Add(ok_btn, 0, wxRIGHT, FromDIP(10));
    btn_sizer->Add(cancel_btn, 0);
    btn_sizer->AddStretchSpacer();
    main_sizer->Add(btn_sizer, 0, wxEXPAND | wxALL, pad);

    SetSizer(main_sizer);
    {
        // GTK under-reports a wrapped wxStaticText's width; pin it so the dialog keeps its right margin.
        const int wrap_w = std::max(m_grid->GetMinSize().x, FromDIP(420));
        tip->Wrap(wrap_w);
        tip->SetMinSize(wxSize(wrap_w, tip->GetBestSize().y));
    }
    RefreshTable();
    UpdateRangeTexts();
    main_sizer->SetSizeHints(this);
    Fit();
    CenterOnParent();
}

int WipingDialog::DisplayValue(double raw) const
{
    const double ratio = m_cur < (int)m_edit_multipliers.size() ? m_edit_multipliers[m_cur] : 1.;
    return int(std::floor(raw * ratio + 0.5)); // JS Math.round
}

int WipingDialog::LimitDisplay(double val) const
{
    if (std::isnan(val) || val < 0) return 0;
    const int mx = m_max_flush_volumes.empty() ? m_max_flush_volume : m_max_flush_volumes[m_cur];
    return val > mx ? mx : int(val);
}

void WipingDialog::RefreshTable()
{
    const int n = int(m_colors.size());
    if (m_cur >= (int)m_edit_matrixs.size()) return;
    const auto& raw = m_edit_matrixs[m_cur];
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) {
            const int v = (i == j) ? 0 : LimitDisplay(DisplayValue(raw[i * n + j]));
            m_grid->SetCellValue(i, j, wxString::Format("%d", v));
        }
    UpdateWarningTexts();
    m_grid->ForceRefresh();
}

void WipingDialog::UpdateRangeTexts()
{
    const int mn = m_min_flush_volumes.empty() ? 0 : m_min_flush_volumes[m_cur];
    const int mx = m_max_flush_volumes.empty() ? m_max_flush_volume : m_max_flush_volumes[m_cur];
    m_volume_range->SetLabel(wxString::Format(_L("Suggestion: Flushing Volume in range [%d, %d]"), mn, mx));
    m_multiplier_range->SetLabel(wxString::Format(_L("The multiplier should be in range [%.2f, %.2f]."), g_min_flush_multiplier, g_max_flush_multiplier));
}

void WipingDialog::UpdateWarningTexts()
{
    double mult = 1.;
    m_multiplier_input->GetValue().ToCDouble(&mult);
    m_multiplier_input->SetForegroundColour(mult != 1.0 ? m_text_warn : m_text_normal);
    m_multiplier_input->Refresh();

    const int n  = int(m_colors.size());
    const int mn = m_min_flush_volumes.empty() ? 0 : m_min_flush_volumes[m_cur];
    const int mx = m_max_flush_volumes.empty() ? m_max_flush_volume : m_max_flush_volumes[m_cur];
    bool has_exception = false;
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) {
            if (i == j) { m_grid->SetCellTextColour(i, j, m_text_normal); continue; }
            long val = 0;
            if (!m_grid->GetCellValue(i, j).ToLong(&val)) val = 0;
            const int def = (m_cur < (int)m_default_matrixs.size() && size_t(i * n + j) < m_default_matrixs[m_cur].size()) ?
                                DisplayValue(m_default_matrixs[m_cur][i * n + j]) : int(val);
            if (val < mn || val > mx) { m_grid->SetCellTextColour(i, j, m_text_error); has_exception = true; }
            else if (val != def)      m_grid->SetCellTextColour(i, j, m_text_warn);
            else                      m_grid->SetCellTextColour(i, j, m_text_normal);
        }
    m_volume_range->SetForegroundColour(has_exception ? m_text_error : m_text_normal);
    m_volume_range->Refresh();
    m_grid->ForceRefresh();
}

void WipingDialog::OnCellChanged(wxGridEvent& evt)
{
    const int i = evt.GetRow(), j = evt.GetCol(), n = int(m_colors.size());
    long parsed = 0;
    double val = m_grid->GetCellValue(i, j).Trim().Trim(false).ToLong(&parsed) ? double(parsed) : std::nan("");
    const int shown = LimitDisplay(val);
    m_grid->SetCellValue(i, j, wxString::Format("%d", shown));
    if (i != j) {
        const double ratio = m_edit_multipliers[m_cur];
        m_edit_matrixs[m_cur][i * n + j] = (ratio == 0) ? 0 : std::floor(shown / ratio + 0.5);
    }
    UpdateWarningTexts();
}

void WipingDialog::OnMultiplierText()
{
    double val = 1.0;
    if (!m_multiplier_input->GetValue().ToCDouble(&val) || std::isnan(val)) val = 1.0;
    if (val < g_min_flush_multiplier) {
        val = g_min_flush_multiplier;
        m_multiplier_input->ChangeValue(format_multiplier(val));
    } else if (val > g_max_flush_multiplier) {
        val = g_max_flush_multiplier;
        m_multiplier_input->ChangeValue(format_multiplier(val));
    }
    if (m_cur < (int)m_edit_multipliers.size()) m_edit_multipliers[m_cur] = val;
    RefreshTable();
}

void WipingDialog::OnRecalculate()
{
    std::vector<double> flat;
    for (const auto& row : CalcFlushingVolumes(m_cur))
        flat.insert(flat.end(), row.begin(), row.end());
    if (m_cur < (int)m_edit_matrixs.size() && flat.size() == m_edit_matrixs[m_cur].size())
        m_edit_matrixs[m_cur] = flat;
    RefreshTable();
}

void WipingDialog::OnExtruderSelect(int idx)
{
    if (idx < 0 || idx >= m_extruder_num) return;
    if (m_grid->IsCellEditControlEnabled()) m_grid->SaveEditControlValue();
    m_cur = idx;
    UpdateRangeTexts();
    m_multiplier_input->ChangeValue(format_multiplier(m_edit_multipliers[m_cur]));
    RefreshTable();
    Layout();
}

void WipingDialog::OnSave()
{
    if (m_grid->IsCellEditControlEnabled()) {
        m_grid->SaveEditControlValue();
        m_grid->HideCellEditControl();
    }
    std::vector<std::vector<double>> store_matrixs = m_edit_matrixs;
    std::vector<double> store_multipliers = m_edit_multipliers;
    {
        size_t cols_temp_matrix = 0;
        if (!store_matrixs.empty()) { cols_temp_matrix = store_matrixs[0].size(); }
        if (store_multipliers.size() == store_matrixs.size() && cols_temp_matrix>0)
        {
            for (size_t idx = 0; idx < store_multipliers.size(); ++idx) {
                double m_max_flush_volume_t = (double)m_max_flush_volume, m_store_multipliers=store_multipliers[idx];
                std::transform(store_matrixs[idx].begin(), store_matrixs[idx].end(),
                               store_matrixs[idx].begin(),
                               [m_max_flush_volume_t, m_store_multipliers](double inputx) {
                                   return std::clamp(inputx, 0.0, m_max_flush_volume_t / m_store_multipliers);
                               });
            }
        }
    }
    if (!m_physical_indices.empty()) {
        size_t full_n = wxGetApp().preset_bundle->project_config.option<ConfigOptionStrings>("filament_colour")->values.size();
        for (size_t nozzle_idx = 0; nozzle_idx < store_matrixs.size() && nozzle_idx < m_raw_matrixs.size(); ++nozzle_idx)
            store_matrixs[nozzle_idx] = expand_physical_to_full_matrix(
                store_matrixs[nozzle_idx], m_physical_indices, full_n, m_raw_matrixs[nozzle_idx]);
    }
    this->StoreFlushData(m_extruder_num, store_matrixs, store_multipliers);
    m_submit_flag = true;
    EndModal(wxID_OK);
}


int WipingDialog::CalcFlushingVolume(const wxColour& from, const wxColour& to, int min_flush_volume , int nozzle_flush_dataset, const std::string& from_filament_id, const std::string& to_filament_id)
{
    Slic3r::FlushVolCalculator calculator(min_flush_volume, Slic3r::g_max_flush_volume, nozzle_flush_dataset);
    return calculator.calc_flush_vol(from_filament_id, to_filament_id, from.Alpha(), from.Red(), from.Green(), from.Blue(), to.Alpha(), to.Red(), to.Green(), to.Blue());
}

WipingDialog::VolumeMatrix WipingDialog::CalcFlushingVolumes(int extruder_id)
{
    auto& preset_bundle = wxGetApp().preset_bundle;
    auto full_config = preset_bundle->full_config();
    auto& ams_multi_color_filament = preset_bundle->ams_multi_color_filment;
    auto physical_indices = preset_bundle->physical_filament_config_indices();

    std::vector<std::string> all_color_strs = full_config.option<ConfigOptionStrings>("filament_colour")->values;
    int flush_dataset_value = full_config.option<ConfigOptionIntsNullable>("nozzle_flush_dataset")->values[extruder_id];
    const std::vector<int> min_flush_volumes = get_min_flush_volumes(full_config, extruder_id);
    const std::vector<std::string>& filament_ids = full_config.option<ConfigOptionStrings>("filament_ids")->values;
    auto get_filament_id = [&filament_ids](int cfg_idx) -> std::string {
        return (cfg_idx >= 0 && cfg_idx < (int)filament_ids.size()) ? filament_ids[cfg_idx] : std::string();
    };

    std::vector<std::vector<wxColour>> multi_colors;
    for (size_t cfg_idx : physical_indices) {
        std::vector<wxColour> single_filament;
        if (cfg_idx < ams_multi_color_filament.size() && !ams_multi_color_filament[cfg_idx].empty()) {
            for (const auto& c : ams_multi_color_filament[cfg_idx])
                single_filament.push_back(wxColour(c));
        } else {
            single_filament.push_back(wxColour(all_color_strs[cfg_idx]));
        }
        multi_colors.push_back(single_filament);
    }

    VolumeMatrix matrix;
    for (size_t pi = 0; pi < physical_indices.size(); ++pi) {
        int from_cfg = (int)physical_indices[pi];
        bool is_from_support = is_support_filament(from_cfg);
        matrix.emplace_back();
        for (size_t pj = 0; pj < physical_indices.size(); ++pj) {
            int to_cfg = (int)physical_indices[pj];
            if (from_cfg == to_cfg) {
                matrix.back().emplace_back(0);
                continue;
            }

            bool is_to_support = is_support_filament(to_cfg);
            int flushing_volume = 0;
            if (is_to_support) {
                flushing_volume = Slic3r::g_flush_volume_to_support;
            } else {
                int min_flush_from = (from_cfg < (int)min_flush_volumes.size()) ? min_flush_volumes[from_cfg] : 0;
                std::string from_filament_id = get_filament_id(from_cfg);
                std::string to_filament_id = get_filament_id(to_cfg);
                for (size_t i = 0; i < multi_colors[pi].size(); ++i) {
                    const wxColour& from = multi_colors[pi][i];
                    for (size_t j = 0; j < multi_colors[pj].size(); ++j) {
                        const wxColour& to = multi_colors[pj][j];
                        int volume = CalcFlushingVolume(from, to, min_flush_from, flush_dataset_value, from_filament_id, to_filament_id);
                        flushing_volume = std::max(flushing_volume, volume);
                    }
                }
                if (is_from_support)
                    flushing_volume = std::max(Slic3r::g_min_flush_volume_from_support, flushing_volume);
            }
            matrix.back().emplace_back(flushing_volume);
        }
    }
    return matrix;
}

void WipingDialog::StoreFlushData(int extruder_num, const std::vector<std::vector<double>>& flush_volume_vecs, const std::vector<double>&flush_multipliers)
{
    m_flush_multipliers = flush_multipliers;
    m_raw_matrixs = flush_volume_vecs;
}

std::vector<double> WipingDialog::GetFlattenMatrix()const
{
    std::vector<double> ret;
    for (auto& matrix : m_raw_matrixs) {
        ret.insert(ret.end(), matrix.begin(), matrix.end());
    }
    return ret;
}


std::vector<double> WipingDialog::GetMultipliers()const
{
    return m_flush_multipliers;
}