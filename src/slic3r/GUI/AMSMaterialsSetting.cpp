#include "AMSMaterialsSetting.hpp"
#include "ExtrusionCalibration.hpp"
#include "MsgDialog.hpp"
#include "GUI_App.hpp"
#include "libslic3r/Preset.hpp"
#include "I18N.hpp"
#include <boost/log/trivial.hpp>
#include <iterator>
#include <wx/colordlg.h>
#include <wx/dcgraph.h>
#include <wx/display.h>
#include "CalibUtils.hpp"
#include "../Utils/ColorSpaceConvert.hpp"
#include "../Utils/BBLUtil.hpp"
#include "EncodedFilament.hpp"
#include "FilamentBitmapUtils.hpp"
#include "FilamentSelectDialog.hpp"


#include "DeviceCore/DevConfig.h"
#include "DeviceCore/DevConfigUtil.h"
#include "DeviceCore/DevExtruderSystem.h"
#include "DeviceCore/DevFilaBlackList.h"
#include "DeviceCore/DevFilaSystem.h"

#include "Widgets/Label.hpp"

#include <wx/dcmemory.h>
#include <wx/graphics.h>
#include <algorithm>
#include <cctype>
#include <cmath>

#define FILAMENT_MAX_TEMP       300
#define FILAMENT_MIN_TEMP       120

namespace Slic3r { namespace GUI {

wxDEFINE_EVENT(EVT_SELECTED_COLOR, wxCommandEvent);

static std::string float_to_string_with_precision(float value, int precision = 3)
{
    if (value < 0)
        return std::string();

    std::stringstream stream;
    stream << std::fixed << std::setprecision(precision) << value;
    return stream.str();
}

static std::string colour_to_ams_string(const wxColour& color)
{
    char col_buf[10];
    sprintf(col_buf, "%02X%02X%02X%02X", (int)color.Red(), (int)color.Green(), (int)color.Blue(), (int)color.Alpha());
    return col_buf;
}

static int filament_color_type_to_ams_ctype(FilamentColor::ColorType type, size_t color_count)
{
    if (color_count < 2) return 2;
    return type == FilamentColor::ColorType::GRADIENT_CLR ? 0 : 1;
}

static wxColour mix_colour(const wxColour& left, const wxColour& right, double ratio)
{
    ratio = std::max(0.0, std::min(1.0, ratio));
    auto mix_channel = [ratio](unsigned char l, unsigned char r) {
        return static_cast<unsigned char>(std::round(l + (r - l) * ratio));
    };
    return wxColour(mix_channel(left.Red(), right.Red()),
                    mix_channel(left.Green(), right.Green()),
                    mix_channel(left.Blue(), right.Blue()),
                    mix_channel(left.Alpha(), right.Alpha()));
}

static bool is_light_colour_for_border(const wxColour& c)
{
    return c.Red() > 224 && c.Green() > 224 && c.Blue() > 224;
}

static bool is_dark_colour_for_border(const wxColour& c)
{
    return c.Red() < 45 && c.Green() < 45 && c.Blue() < 45;
}

static std::vector<ColorPickerPopup::ColorItem> collect_ams_color_items(DevFilaSystem* fila_system)
{
    std::vector<ColorPickerPopup::ColorItem> items;
    if (!fila_system) return items;

    for (const auto& ams_pair : fila_system->GetAmsList()) {
        DevAms* ams = ams_pair.second;
        if (!ams) continue;

        for (const auto& tray_pair : ams->GetTrays()) {
            DevAmsTray* tray = tray_pair.second;
            if (!tray || !tray->is_tray_info_ready()) continue;

            ColorPickerPopup::ColorItem item;
            for (const std::string& col : tray->cols) {
                item.colors.emplace_back(DevAmsTray::decode_color(col));
            }
            if (item.colors.empty() && !tray->color.empty()) {
                item.colors.emplace_back(DevAmsTray::decode_color(tray->color));
            }
            if (item.colors.empty()) continue;

            item.ctype = item.colors.size() > 1 ? static_cast<int>(tray->ctype) : 2;
            items.emplace_back(std::move(item));
        }
    }

    return items;
}

AMSMaterialsSetting::AMSMaterialsSetting(wxWindow *parent, wxWindowID id)
    : AMSTraySettingBase(parent, id, _L("AMS Materials Setting"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
{
    create();
    wxGetApp().UpdateDlgDarkUI(this);
}

void AMSMaterialsSetting::create()
{
    SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));
    wxBoxSizer *m_sizer_main = new wxBoxSizer(wxVERTICAL);

    m_panel_normal = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    m_panel_normal->SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));
    create_panel_normal(m_panel_normal);
    m_panel_kn = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    m_panel_kn->SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));
    create_panel_kn(m_panel_kn);

    wxBoxSizer *m_sizer_button = new wxBoxSizer(wxHORIZONTAL);

    m_sizer_button->Add(0, 0, 1, wxEXPAND, 0);

    m_button_confirm = new Button(this, _L("Confirm"));
    m_btn_bg_green   = StateColor(std::pair<wxColour, int>(wxColour(27, 136, 68), StateColor::Pressed), std::pair<wxColour, int>(wxColour(61, 203, 115), StateColor::Hovered),
                            std::pair<wxColour, int>(wxColour(0, 174, 66), StateColor::Normal));
    m_button_confirm->SetBackgroundColor(m_btn_bg_green);
    m_button_confirm->SetBorderColor(wxColour(0, 174, 66));
    m_button_confirm->SetTextColor(wxColour("#FFFFFE"));
    m_button_confirm->SetMinSize(AMS_MATERIALS_SETTING_BUTTON_SIZE);
    m_button_confirm->SetCornerRadius(FromDIP(12));
    m_button_confirm->Bind(wxEVT_BUTTON, &AMSMaterialsSetting::on_select_ok, this);

    m_button_reset = new Button(this, _L("Reset"));
    m_btn_bg_gray = StateColor(std::pair<wxColour, int>(wxColour(206, 206, 206), StateColor::Pressed), std::pair<wxColour, int>(*wxWHITE, StateColor::Focused),
        std::pair<wxColour, int>(wxColour(238, 238, 238), StateColor::Hovered),
        std::pair<wxColour, int>(*wxWHITE, StateColor::Normal));
    m_button_reset->SetBackgroundColor(m_btn_bg_gray);
    m_button_reset->SetBorderColor(AMS_MATERIALS_SETTING_GREY900);
    m_button_reset->SetTextColor(AMS_MATERIALS_SETTING_GREY900);
    m_button_reset->SetMinSize(AMS_MATERIALS_SETTING_BUTTON_SIZE);
    m_button_reset->SetCornerRadius(FromDIP(12));
    m_button_reset->Bind(wxEVT_BUTTON, &AMSMaterialsSetting::on_select_reset, this);

    m_button_close = new Button(this, _L("Close"));
    m_button_close->SetBackgroundColor(m_btn_bg_gray);
    m_button_close->SetBorderColor(AMS_MATERIALS_SETTING_GREY900);
    m_button_close->SetTextColor(AMS_MATERIALS_SETTING_GREY900);
    m_button_close->SetMinSize(AMS_MATERIALS_SETTING_BUTTON_SIZE);
    m_button_close->SetCornerRadius(FromDIP(12));
    m_button_close->Bind(wxEVT_BUTTON, &AMSMaterialsSetting::on_select_close, this);

    m_sizer_button->Add(m_button_confirm, 0, wxALIGN_CENTER | wxRIGHT, FromDIP(20));
    m_sizer_button->Add(m_button_reset, 0, wxALIGN_CENTER | wxRIGHT, FromDIP(20));
    m_sizer_button->Add(m_button_close, 0, wxALIGN_CENTER, 0);

    m_sizer_main->Add(m_panel_normal, 0, wxEXPAND, 0);

    m_sizer_main->Add(m_panel_kn, 0, wxEXPAND, 0);

    wxBoxSizer* m_tip_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_tip_readonly = new Label(this, _L(""));
    m_tip_readonly->SetForegroundColour(wxColour("#FF6F00"));
    m_tip_readonly->SetMinSize(wxSize(AMS_MATERIALS_SETTING_TIP_WIDTH, -1));
    m_tip_readonly->SetMaxSize(wxSize(AMS_MATERIALS_SETTING_TIP_WIDTH, -1));
    m_tip_readonly->Hide();
    m_tip_sizer->Add(m_tip_readonly, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(20));
    m_sizer_main->Add(m_tip_sizer, 0, wxLEFT | wxTOP, FromDIP(20));

    m_sizer_main->Add(0, 0, 0, wxTOP, FromDIP(24));
    m_sizer_main->Add(m_sizer_button, 0,  wxEXPAND | wxLEFT | wxRIGHT, FromDIP(20));
    m_sizer_main->Add(0, 0, 0,  wxTOP, FromDIP(16));

    SetSizer(m_sizer_main);
    Layout();
    apply_dialog_size();

    Bind(EVT_SELECTED_COLOR, &AMSMaterialsSetting::on_picker_color, this);

    m_comboBox_cali_result->Bind(wxEVT_COMBOBOX, &AMSMaterialsSetting::on_select_cali_result, this);
}

void AMSMaterialsSetting::create_panel_normal(wxWindow* parent)
{
    auto sizer = new wxBoxSizer(wxVERTICAL);

    wxBoxSizer* m_sizer_filament = new wxBoxSizer(wxVERTICAL);

    m_title_filament = new wxStaticText(parent, wxID_ANY, _L("Filament"), wxDefaultPosition, wxDefaultSize, 0);
    m_title_filament->SetFont(::Label::Head_14);
    m_title_filament->SetForegroundColour(AMS_MATERIALS_SETTING_GREY800);
    m_title_filament->Wrap(-1);
    m_sizer_filament->Add(m_title_filament, 0, wxALIGN_LEFT, 0);
    m_sizer_filament->AddSpacer(FromDIP(6));

    m_filament_box = new StaticBox(parent);
    m_filament_box->SetMinSize(AMS_MATERIALS_SETTING_COMBOX_WIDTH);
    m_filament_box->SetCornerRadius(FromDIP(4));
    m_filament_box->SetBorderColor(StateColor(std::make_pair(wxColour(0xDB, 0xDB, 0xDB), (int)StateColor::Normal)));
    m_filament_box->SetBackgroundColor(StateColor(std::make_pair(wxColour(0xF0, 0xF0, 0xF1), (int)StateColor::Disabled),
                                                  std::make_pair(*wxWHITE, (int)StateColor::Normal)));

    auto* box_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_filament_text = new Label(m_filament_box, wxEmptyString);
    m_filament_text->SetFont(::Label::Body_14);
    m_filament_text->SetForegroundColour(StateColor::darkModeColorFor(AMS_MATERIALS_SETTING_GREY900));
    m_filament_text->SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));   // match box fill; avoid grey default label bg
    box_sizer->Add(m_filament_text, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));

    m_filament_arrow = new wxStaticBitmap(m_filament_box, wxID_ANY,
        create_scaled_bitmap("filament_select_arrow", m_filament_box, 10));
    m_filament_arrow->SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));
    box_sizer->Add(m_filament_arrow, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    m_filament_box->SetSizer(box_sizer);

    auto open_filament_dialog = [this](wxMouseEvent&) { on_open_filament_select_dialog(); };
    m_filament_box->Bind(wxEVT_LEFT_DOWN, open_filament_dialog);
    m_filament_text->Bind(wxEVT_LEFT_DOWN, open_filament_dialog);
    m_filament_arrow->Bind(wxEVT_LEFT_DOWN, open_filament_dialog);

    m_sizer_filament->Add(m_filament_box, 0, wxEXPAND, 0);


    wxBoxSizer* m_sizer_colour = new wxBoxSizer(wxHORIZONTAL);

    m_title_colour = new wxStaticText(parent, wxID_ANY, _L("Colour"), wxDefaultPosition, wxDefaultSize, 0);
    m_title_colour->SetFont(::Label::Head_14);
    m_title_colour->SetForegroundColour(AMS_MATERIALS_SETTING_GREY800);
    m_title_colour->Wrap(-1);
    m_sizer_colour->Add(m_title_colour, 0, wxALIGN_CENTER_VERTICAL, 0);

    m_clr_picker = new ColorPicker(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize);
    m_clr_picker->set_show_full(true);
    m_clr_picker->SetBackgroundColour(*wxWHITE);

    m_sizer_colour->Add(m_clr_picker, 0, wxRESERVE_SPACE_EVEN_IF_HIDDEN | wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(10));
    m_clr_name = new Label(parent, wxEmptyString);
    m_clr_name->SetForegroundColour(*wxBLACK);
    m_clr_name->SetBackgroundColour(*wxWHITE);
    m_clr_name->SetFont(Label::Body_13);
    m_sizer_colour->Add(m_clr_name, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(10));

    m_color_picker_popup = new ColorPickerPopup(parent, this);
    m_color_picker_popup->SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));
    m_color_picker_popup->Hide();

    wxBoxSizer* m_sizer_temperature = new wxBoxSizer(wxHORIZONTAL);

    m_panel_temperature = new StaticBox(parent);
    m_panel_temperature->SetCornerRadius(FromDIP(10));
    m_panel_temperature->SetBackgroundColor(StateColor(std::make_pair(wxColour(248, 248, 248), (int)StateColor::Normal)));
    m_panel_temperature->SetBorderColor(StateColor(std::make_pair(wxColour(248, 248, 248), (int)StateColor::Normal)));

    wxBoxSizer* temp_box_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_nozzle_temp_label = new Label(m_panel_temperature, wxEmptyString);
    m_nozzle_temp_label->SetFont(::Label::Body_13);
    m_nozzle_temp_label->SetForegroundColour(AMS_MATERIALS_SETTING_GREY700);
    temp_box_sizer->Add(m_nozzle_temp_label, 1, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(8));
    m_panel_temperature->SetSizer(temp_box_sizer);
    m_panel_temperature->Layout();

    m_sizer_temperature->Add(m_panel_temperature, 0, 0, 0);

    m_panel_SN = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    wxBoxSizer* m_sizer_SN = new wxBoxSizer(wxVERTICAL);
    m_sizer_SN->AddSpacer(FromDIP(16));

    wxBoxSizer* m_sizer_SN_inside = new wxBoxSizer(wxHORIZONTAL);

    auto m_title_SN = new wxStaticText(m_panel_SN, wxID_ANY, _L("SN"), wxDefaultPosition, wxSize(AMS_MATERIALS_SETTING_LABEL_WIDTH, -1), 0);
    m_title_SN->SetFont(::Label::Body_13);
    m_title_SN->SetForegroundColour(AMS_MATERIALS_SETTING_GREY800);
    m_title_SN->Wrap(-1);
    m_sizer_SN_inside->Add(m_title_SN, 0, wxALIGN_CENTER_VERTICAL, 0);

    m_sn_number = new wxStaticText(m_panel_SN, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize);
    m_sn_number->SetForegroundColour(*wxBLACK);
    m_sizer_SN_inside->Add(m_sn_number, 0, wxALIGN_CENTER, 0);
    m_sizer_SN->Add(m_sizer_SN_inside);

    m_panel_SN->SetSizer(m_sizer_SN);
    m_panel_SN->Layout();
    m_panel_SN->Fit();
    m_panel_SN->Hide();

    sizer->Add(0, 0, 0, wxTOP, FromDIP(16));
    sizer->Add(m_sizer_filament, 0, wxLEFT | wxRIGHT | wxEXPAND, FromDIP(20));
    sizer->Add(0, 0, 0, wxTOP, FromDIP(8));
    sizer->Add(m_sizer_temperature, 0, wxLEFT | wxRIGHT | wxEXPAND, FromDIP(20));
    sizer->Add(0, 0, 0, wxTOP, FromDIP(16));
    sizer->Add(m_sizer_colour, 0, wxLEFT | wxRIGHT | wxEXPAND, FromDIP(20));
    sizer->Add(0, 0, 0, wxTOP, FromDIP(8));
    wxBoxSizer* m_sizer_color_popup_row = new wxBoxSizer(wxHORIZONTAL);
    m_sizer_color_popup_row->Add(m_color_picker_popup, 1, wxEXPAND, 0);
    sizer->Add(m_sizer_color_popup_row, 0, wxLEFT | wxRIGHT | wxEXPAND, FromDIP(20));
    sizer->Add(0, 0, 0, wxTOP, FromDIP(16));
    sizer->Add(m_panel_SN, 0, wxLEFT, FromDIP(20));
    parent->SetSizer(sizer);

    update_nozzle_temp_display();
}

void AMSMaterialsSetting::create_panel_kn(wxWindow* parent)
{
    auto sizer = new wxBoxSizer(wxVERTICAL);
    m_ratio_text   = new wxStaticText(parent, wxID_ANY, _L("Factors of Flow Dynamics Calibration"));
    m_ratio_text->SetForegroundColour(wxColour(50, 58, 61));
    m_ratio_text->SetFont(Label::Head_14);
    m_ratio_text->Wrap(-1);

    std::string language = wxGetApp().app_config->get("language");
    wxString    region   = "en";
    if (language.find("zh") == 0)
        region = "zh";
    wxString link_url = wxString::Format("https://wiki.bambulab.com/%s/software/bambu-studio/calibration_pa", region);
    m_wiki_ctrl = new Label(parent, _L("View Wiki for details"));
    m_wiki_ctrl->SetFont(Label::Body_13);
    m_wiki_ctrl->SetForegroundColour(wxColour(0, 174, 66));
    m_wiki_ctrl->Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent &e) {
        e.Skip();
        m_wiki_ctrl->SetForegroundColour(wxColour(0, 150, 57));
        m_wiki_ctrl->SetCursor(wxCURSOR_HAND);
        m_wiki_ctrl->Refresh();
    });
    m_wiki_ctrl->Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent &e) {
        e.Skip();
        m_wiki_ctrl->SetForegroundColour(wxColour(0, 174, 66));
        m_wiki_ctrl->SetCursor(wxCURSOR_ARROW);
        m_wiki_ctrl->Refresh();
    });
    m_wiki_ctrl->Bind(wxEVT_LEFT_UP, [link_url](wxMouseEvent &) {
        wxLaunchDefaultBrowser(link_url);
    });

    wxBoxSizer *m_sizer_nozzle_type = new wxBoxSizer(wxHORIZONTAL);
    // Nozzle Type
    m_title_nozzle_type = new wxStaticText(parent, wxID_ANY, _L("Nozzle Type"), wxDefaultPosition, wxSize(AMS_MATERIALS_SETTING_LABEL_WIDTH, -1), 0);
    m_title_nozzle_type->SetMinSize(wxSize(FromDIP(80), -1));
    m_title_nozzle_type->SetMaxSize(wxSize(FromDIP(80), -1));
    m_title_nozzle_type->SetFont(::Label::Body_13);
    m_title_nozzle_type->SetForegroundColour(AMS_MATERIALS_SETTING_GREY800);
    m_title_nozzle_type->Wrap(-1);
    m_sizer_nozzle_type->Add(m_title_nozzle_type, 0, wxALIGN_CENTER, 0);
    m_sizer_nozzle_type->Add(0, 0, 0, wxEXPAND, 0);

    m_comboBox_nozzle_type = new ::ComboBox(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, AMS_MATERIALS_SETTING_COMBOX_WIDTH, 0, nullptr, wxCB_READONLY);
    m_comboBox_nozzle_type->Bind(wxEVT_COMMAND_COMBOBOX_SELECTED, &AMSMaterialsSetting::on_select_nozzle_pos_id, this);
    m_sizer_nozzle_type->Add(m_comboBox_nozzle_type, 1, wxALIGN_CENTER, 0);


    wxBoxSizer *m_sizer_cali_resutl = new wxBoxSizer(wxHORIZONTAL);
    // pa profile
    m_title_pa_profile = new wxStaticText(parent, wxID_ANY, _L("PA Profile"), wxDefaultPosition, wxSize(AMS_MATERIALS_SETTING_LABEL_WIDTH, -1), 0);
    m_title_pa_profile->SetMinSize(wxSize(FromDIP(80), -1));
    m_title_pa_profile->SetMaxSize(wxSize(FromDIP(80), -1));
    m_title_pa_profile->SetFont(::Label::Body_13);
    m_title_pa_profile->SetForegroundColour(AMS_MATERIALS_SETTING_GREY800);
    m_title_pa_profile->Wrap(-1);
    m_sizer_cali_resutl->Add(m_title_pa_profile, 0, wxALIGN_CENTER, 0);
    m_sizer_cali_resutl->Add(0, 0, 0, wxEXPAND, 0);

    m_comboBox_cali_result = new ::ComboBox(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, AMS_MATERIALS_SETTING_COMBOX_WIDTH, 0, nullptr, wxCB_READONLY);
    m_sizer_cali_resutl->Add(m_comboBox_cali_result, 1, wxALIGN_CENTER, 0);

    auto kn_val_sizer = new wxFlexGridSizer(0, 2, 0, 0);
    kn_val_sizer->SetFlexibleDirection(wxBOTH);
    kn_val_sizer->SetNonFlexibleGrowMode(wxFLEX_GROWMODE_SPECIFIED);
    kn_val_sizer->AddGrowableCol(1);

    // k params input
    m_k_param = new wxStaticText(parent, wxID_ANY, _L("Factor K"), wxDefaultPosition, wxDefaultSize, 0);
    m_k_param->SetMinSize(wxSize(FromDIP(80), -1));
    m_k_param->SetMaxSize(wxSize(FromDIP(80), -1));
    m_k_param->SetFont(::Label::Body_13);
    m_k_param->SetForegroundColour(wxColour(50, 58, 61));
    m_k_param->Wrap(-1);
    kn_val_sizer->Add(m_k_param, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(0));

    m_input_k_val = new TextInput(parent, wxEmptyString, wxEmptyString, wxEmptyString, wxDefaultPosition, AMS_MATERIALS_SETTING_COMBOX_WIDTH, wxTE_CENTRE | wxTE_PROCESS_ENTER);
    m_input_k_val->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));
    kn_val_sizer->Add(m_input_k_val, 0, wxALL | wxEXPAND | wxALIGN_CENTER_VERTICAL, FromDIP(0));

    // n params input
    wxBoxSizer* n_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_n_param = new wxStaticText(parent, wxID_ANY, _L("Factor N"), wxDefaultPosition, wxDefaultSize, 0);
    m_n_param->SetFont(::Label::Body_13);
    m_n_param->SetForegroundColour(wxColour(50, 58, 61));
    m_n_param->Wrap(-1);
    kn_val_sizer->Add(m_n_param, 1, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(5));
    m_input_n_val = new TextInput(parent, wxEmptyString, wxEmptyString, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxTE_CENTRE | wxTE_PROCESS_ENTER);
    m_input_n_val->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));
    kn_val_sizer->Add(m_input_n_val, 0, wxALL | wxEXPAND | wxALIGN_CENTER_VERTICAL, FromDIP(5));
    m_n_param->Hide();
    m_input_n_val->Hide();

    // Heading row: section title on the left, wiki link right after it.
    wxBoxSizer* cali_heading_sizer = new wxBoxSizer(wxHORIZONTAL);
    cali_heading_sizer->Add(m_ratio_text, 0, wxALIGN_CENTER_VERTICAL, 0);
    cali_heading_sizer->Add(m_wiki_ctrl, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(10));

    wxBoxSizer* cali_col_sizer = new wxBoxSizer(wxVERTICAL);
    cali_col_sizer->Add(cali_heading_sizer, 0, wxEXPAND, 0);
    cali_col_sizer->Add(0, 0, 0, wxTOP, FromDIP(10));
    cali_col_sizer->Add(m_sizer_nozzle_type, 0, wxEXPAND, 0);
    m_nozzle_type_spacer_item = cali_col_sizer->Add(0, FromDIP(10), 0, 0);
    cali_col_sizer->Add(m_sizer_cali_resutl, 0, wxEXPAND, 0);
    cali_col_sizer->Add(0, 0, 0, wxTOP, FromDIP(10));
    cali_col_sizer->Add(kn_val_sizer, 0, wxEXPAND, 0);

    sizer->Add(0, 0, 0, wxTOP, FromDIP(10));
    sizer->Add(cali_col_sizer, 0, wxLEFT | wxRIGHT | wxEXPAND, FromDIP(20));
    sizer->Add(0, 0, 0, wxTOP, FromDIP(10));
    parent->SetSizer(sizer);
}


AMSMaterialsSetting::~AMSMaterialsSetting() {}

void AMSMaterialsSetting::update_nozzle_temp_display()
{
    if (!m_nozzle_temp_label) return;

    wxString tmin = m_nozzle_temp_min_str.IsEmpty() ? wxString("0") : m_nozzle_temp_min_str;
    wxString tmax = m_nozzle_temp_max_str.IsEmpty() ? wxString("0") : m_nozzle_temp_max_str;

    wxString title = _L("Nozzle\nTemperature");
    title.Replace("\n", " ");

    wxString text = title + ": " + tmin + "-" + tmax + " " + wxString::FromUTF8("\xe2\x84\x83");
    m_nozzle_temp_label->SetLabel(text);
    if (m_panel_temperature) m_panel_temperature->Layout();
    if (m_panel_normal)      m_panel_normal->Layout();   // 让自适应宽度的圆角框按新文字重新取宽
}

void AMSMaterialsSetting::update()
{
    if (obj) {
        update_widgets();
        update_filament_editing(obj->is_in_printing() || obj->can_resume());
    }
}

void AMSMaterialsSetting::update_filament_editing(bool is_printing)
{
    if (is_printing) {
        enable_filament_box(obj->is_support_filament_setting_inprinting);
        m_comboBox_nozzle_type->Enable(obj->is_support_filament_setting_inprinting);
        m_comboBox_cali_result->Enable(obj->is_support_filament_setting_inprinting);
        m_button_confirm->Show(obj->is_support_filament_setting_inprinting);
        m_button_reset->Show(obj->is_support_filament_setting_inprinting);
    }
    else {
        enable_filament_box(true);
        m_comboBox_nozzle_type->Enable(true);
        m_comboBox_cali_result->Enable(true);
        m_button_reset->Show(true);
        m_button_confirm->Show(true);
    }

    if (!m_is_third) {
        m_tip_readonly->SetLabelText(wxEmptyString);
        m_tip_readonly->Hide();
    }
    else {
        if (!obj->is_support_filament_setting_inprinting) {
            if (!is_virtual_tray()) {
                m_tip_readonly->SetLabelText(_L("Tip: Setting AMS slot information while printing is not supported"));
            } else {
                m_tip_readonly->SetLabelText(_L("Tip: Setting Virtual slot information while printing is not supported"));
            }
        } else {
            m_tip_readonly->SetLabelText(wxEmptyString);
        }

        m_tip_readonly->SetForegroundColour(wxColour("#FF6F00"));   // reassert: UpdateDlgDarkUI may recolor labels
        m_tip_readonly->Wrap(AMS_MATERIALS_SETTING_TIP_WIDTH);
        m_tip_readonly->Show(is_printing);
    }

    if (m_view_only) {
        enable_filament_box(false);
        m_comboBox_nozzle_type->Enable(false);
        m_comboBox_cali_result->Enable(false);
        m_input_k_val->Enable(false);
        m_input_n_val->Enable(false);
        m_button_confirm->Hide();
        m_button_reset->Hide();
    }
}

std::vector<ColorPickerPopup::ColorItem> AMSMaterialsSetting::get_preset_color_items(const std::string& filament_id) const
{
    std::vector<ColorPickerPopup::ColorItem> items;
    if (filament_id.empty()) return items;

    auto* clr_query = GUI::wxGetApp().get_filament_color_code_query();
    if (!clr_query) return items;

    FilamentColorCodes* color_codes = clr_query->GetFilaInfoMap(wxString::FromUTF8(filament_id));
    if (!color_codes || !color_codes->GetFilamentColor2CodeMap()) return items;

    std::set<std::string> seen;
    for (const auto& color_pair : *color_codes->GetFilamentColor2CodeMap()) {
        const FilamentColor& fila_color = color_pair.first;
        FilamentColorCode* color_code = color_pair.second;
        if (!color_code || fila_color.GetColors().empty()) continue;

        ColorPickerPopup::ColorItem item;
        item.ctype = filament_color_type_to_ams_ctype(fila_color.m_color_type, fila_color.ColorCount());
        item.name = color_code->GetFilaColorName();

        std::string key = std::to_string(item.ctype);
        for (const wxColour& color : fila_color.GetColors()) {
            item.colors.emplace_back(color);
            key += "|" + colour_to_ams_string(color);
        }

        if (item.colors.empty() || !seen.insert(key).second) continue;
        items.emplace_back(std::move(item));
    }

    return items;
}

void AMSMaterialsSetting::on_select_reset(wxCommandEvent& event) {
    // View-only mode never commits changes.
    if (m_view_only) {
        return;
    }

    MessageDialog msg_dlg(nullptr, _L("Are you sure you want to clear the filament information?"), wxEmptyString, wxICON_WARNING | wxOK | wxCANCEL);
    auto result = msg_dlg.ShowModal();
    if (result != wxID_OK)
        return;

    m_nozzle_temp_min_str = wxString();
    m_nozzle_temp_max_str = wxString();
    update_nozzle_temp_display();
    m_has_initial_filament_weight = false;
    ams_filament_id = "";
    ams_setting_id = "";

    m_filament_type = "";
    long nozzle_temp_min_int = 0;
    long nozzle_temp_max_int = 0;
    wxColour color = *wxWHITE;
    char col_buf[10];
    sprintf(col_buf, "%02X%02X%02X00", (int)color.Red(), (int)color.Green(), (int)color.Blue());
    std::string color_str;  // reset use empty string

    std::string   selected_ams_id;
    PresetBundle *preset_bundle = wxGetApp().preset_bundle;
    if (preset_bundle) {
        std::string filament_id;
        if (filament_id.empty()) {
            auto it = map_filament_items.find(into_u8(m_current_filament_alias));
            if (it != map_filament_items.end())
                filament_id = it->second.filament_id;
        }
        for (auto it = preset_bundle->filaments.begin(); it != preset_bundle->filaments.end(); ++it) {
            if (!filament_id.empty() && it->filament_id.compare(filament_id) == 0) {
                selected_ams_id = it->filament_id;
                break;
            }
        }
    }

    if (obj) {
        if(m_is_third){
            obj->command_ams_filament_settings(ams_id, slot_id, ams_filament_id, ams_setting_id, std::string(col_buf), m_filament_type, nozzle_temp_min_int,
                                               nozzle_temp_max_int);
        }

        reset_calibration(selected_ams_id);

    }
    trigger_select_filament(std::string(), /*from_printer=*/true, wxEmptyString);
}


static DevFilaBlacklist::CheckResult
sCheckFilamentInfo(PresetBundle* preset_bundle,
                   MachineObject* obj,
                   int ams_id, int slot_id,
                   const std::string& filament_id,
                   std::string& ams_filament_id,
                   std::string& ams_setting_id)
{
    DevFilaBlacklist::CheckResult result;
    if (!preset_bundle) {
        return result;
    }

    auto it = preset_bundle->get_filament_by_filament_id(filament_id);
    if (!it.has_value()) {
        return result;
    }

    if (wxGetApp().app_config->get("skip_ams_blacklist_check") != "true") {
        DevFilaBlacklist::CheckFilamentInfo check_info;
        check_info.dev_id = obj->get_dev_id();
        check_info.model_id = obj->printer_type;
        check_info.fila_id = it->filament_id;
        check_info.fila_type = it->filament_type;

        auto option = GUI::wxGetApp().preset_bundle->get_filament_by_filament_id(check_info.fila_id);
        check_info.fila_name = option ? option->filament_name : "";
        check_info.fila_vendor = option ? option->vendor : "";
        check_info.has_filament_switch = obj->GetFilaSwitch()->IsInstalled();

        check_info.ams_id = ams_id;
        check_info.slot_id = slot_id;
        check_info.extruder_id = obj->GetFilaSystem()->GetExtruderIdByAmsId(std::to_string(ams_id));

        if (check_info.extruder_id == MAIN_EXTRUDER_ID && obj->GetNozzleRack()->IsSupported()) {
            ;// the extruder have serval nozzles, do nothing here
        } else {
            check_info.nozzle_flow = obj->GetFilaSystem()->GetNozzleFlowStringByAmsId(std::to_string(ams_id));
            auto nozzle = obj->GetNozzleSystem()->GetNozzleByPosId(check_info.extruder_id.value_or(-1));
            if (!nozzle.IsEmpty()) {
                check_info.nozzle_diameter = nozzle.GetNozzleDiameter();
            }
        }

        result = DevFilaBlacklist::check_filaments_in_blacklist(check_info);
    }

    ams_filament_id = it->filament_id;
    ams_setting_id = it->setting_id;
    return result;
}

namespace {

void remember_ams_recent_filament_preset(
    const wxString& selected);


} // namespace

void AMSMaterialsSetting::on_select_ok(wxCommandEvent& event)
{
    if (!obj) {
        return;
    }

    // View-only mode never commits changes.
    if (m_view_only) {
        return;
    }

    PresetBundle* preset_bundle = wxGetApp().preset_bundle;
    if (!preset_bundle) {
        return;
    }


    // the combobox item
    auto filament_item = map_filament_items[into_u8(m_current_filament_alias)];

    //get filament id
    ams_filament_id = "";
    ams_setting_id = "";



    // check filament info
    const auto& fila_check_res = sCheckFilamentInfo(preset_bundle, obj, ams_id, slot_id, filament_item.filament_id, ams_filament_id, ams_setting_id);
    bool can_set_fila = fila_check_res.get_items_by_action("prohibition").empty();

    // check if need to set usr_has_setup_tpu
    auto fila_item = preset_bundle ? preset_bundle->get_filament_by_filament_id(filament_item.filament_id) : std::nullopt;
    if (fila_item.has_value() && can_set_fila && DevPrinterConfigUtil::support_user_first_setup_tpu_check(obj->printer_type)) {
        if ((fila_item->filament_type == "TPU" || fila_item->filament_type == "TPU-AMS") &&
            wxGetApp().app_config->get("usr_has_setup_tpu") != "true") {

            MessageDialog dlg(this, _L("TPU needs a different feeding path and loading procedure. Otherwise clogging or jam may happen. Please read the tutorial before using TPU."),
                              SLIC3R_APP_NAME + _L("Info"), wxICON_INFORMATION);
            dlg.AddButton(wxID_CANCEL, _L("Cancel"), false);
            dlg.AddButton(wxID_OK, _L("Go to Check"), true);
            int rtn = dlg.ShowModal();
            if (rtn != wxID_OK) {
                return;
            }

            wxGetApp().app_config->set("usr_has_setup_tpu", "true");

            auto tpu_check_url = DevPrinterConfigUtil::support_user_first_setup_tpu_check_url(obj->printer_type);
            if (!tpu_check_url.empty()) {
                wxLaunchDefaultBrowser(tpu_check_url);
            }
        };
    }

    if (!fila_check_res.action_items.empty()) {
        if (const auto& prohibit_items = fila_check_res.get_items_by_action("prohibition"); !prohibit_items.empty()) {
            wxString info_msg;
            for (auto item : prohibit_items) {
                info_msg += item.info_msg + "\n";
            }

            MessageDialog msg_wingow(nullptr, info_msg, _L("Error"), wxICON_WARNING | wxOK);
            msg_wingow.ShowModal();
            return;
        }

        if (const auto& warning_items = fila_check_res.get_items_by_action("warning"); !warning_items.empty()) {
            std::vector<FilamentWarningInfo> infos;

            for (auto item : warning_items) {
                FilamentWarningInfo info;
                info.info_msg = item.info_msg;
                info.wiki_url = item.wiki_url;
                infos.emplace_back(info);
            }

            FilamentWarningDialog msg_window(nullptr, _("Warning"), infos);
            msg_window.ShowModal();
        }
    }

    wxString nozzle_temp_min = m_nozzle_temp_min_str;
    auto     filament = m_current_filament_alias;

    wxString nozzle_temp_max = m_nozzle_temp_max_str;

    long nozzle_temp_min_int, nozzle_temp_max_int;
    nozzle_temp_min.ToLong(&nozzle_temp_min_int);
    nozzle_temp_max.ToLong(&nozzle_temp_max_int);
    wxColour color = m_clr_picker->m_colour;
    std::string tray_color = colour_to_ams_string(color);
    std::vector<std::string> tray_colors;
    int tray_ctype = 2;
    // Only printers that advertise manual multi-color editing (fun2[23]) receive the
    // cols/ctype fields; otherwise fall back to the legacy single-color command.
    if (obj->is_support_filament_manual_multi_color) {
        for (const wxColour& selected_color : m_clr_picker->m_cols) {
            tray_colors.emplace_back(colour_to_ams_string(selected_color));
        }
        if (tray_colors.empty()) {
            tray_colors.emplace_back(tray_color);
        }
        tray_ctype = tray_colors.size() > 1 ? m_clr_picker->ctype : 2;
    }

    if (ams_filament_id.empty() || nozzle_temp_min.empty() || nozzle_temp_max.empty() || m_filament_type.empty()) {
        BOOST_LOG_TRIVIAL(trace) << "Invalid Setting id";
        MessageDialog msg_dlg(nullptr, _L("You need to select the material type and color first."), wxEmptyString, wxICON_WARNING | wxOK);
        msg_dlg.ShowModal();
        return;
    }


    // set filament
    if (m_is_third) {
        obj->command_ams_filament_settings(ams_id, slot_id, ams_filament_id, ams_setting_id, tray_color, m_filament_type, nozzle_temp_min_int, nozzle_temp_max_int,
                                           tray_colors, tray_ctype);

        // Optimistic local update
        DevAmsTray* the_tray = nullptr;
        if (auto* ams_obj = obj->GetFilaSystem()->GetAmsById(std::to_string(ams_id))) {
            auto tray_it = ams_obj->GetTrays().find(std::to_string(slot_id));
            if (tray_it != ams_obj->GetTrays().end()) {
                the_tray = tray_it->second;
            }
        } else if (auto vt_tray = obj->get_vt_tray(std::to_string(ams_id))) {
            the_tray = vt_tray;
        }

        the_tray->setting_id = ams_setting_id;
        the_tray->m_fila_type = m_filament_type;
        the_tray->color = tray_color;
        the_tray->cols  = tray_colors;
        the_tray->ctype = static_cast<DevFilaColorType>(tray_ctype);
        the_tray->set_hold_count();

        // Filament Manager entries are identified by their spool ID. Only
        // remember actual system-preset aliases in the shared recent list.
        if (m_selected_spool_id.empty()) {
            remember_ams_recent_filament_preset(
                m_current_filament_alias);
        }
    }

    if (!save_pa_profile_selection())
        return;


    Close();
}

void AMSMaterialsSetting::on_select_close(wxCommandEvent &event)
{
    Close();
}

void AMSMaterialsSetting::set_color(wxColour color)
{
    //m_clrData->SetColour(color);
    m_clr_picker->is_empty(false);
    m_clr_picker->Show();
    m_clr_picker->set_color(color);

    FilamentColor fila_color;
    fila_color.AddColor(color);
    fila_color.EndSet(m_clr_picker->ctype);
    auto clr_query = GUI::wxGetApp().get_filament_color_code_query();
    m_clr_name->SetLabelText(clr_query->GetFilaColorName(ams_filament_id, fila_color));
    if (m_clr_picker->GetParent()) m_clr_picker->GetParent()->Layout();
}

void AMSMaterialsSetting::set_empty_color(wxColour color)
{
    m_clr_picker->is_empty(true);
    m_clr_picker->set_color(color);
    m_clr_picker->set_colors({ color });
    m_clr_picker->Hide();
    m_clr_name->SetLabelText(_L("Please select a color"));
    if (m_clr_picker->GetParent()) m_clr_picker->GetParent()->Layout();
}

void AMSMaterialsSetting::set_colors(std::vector<wxColour> colors)
{
    //m_clrData->SetColour(color);
    m_clr_picker->set_colors(colors);

    if (!colors.empty())
    {
        m_clr_picker->is_empty(false);
        m_clr_picker->Show();
        FilamentColor fila_color;
        for (const auto& clr : colors) { fila_color.AddColor(clr); }
        fila_color.EndSet(m_clr_picker->ctype);
        auto clr_query = GUI::wxGetApp().get_filament_color_code_query();
        m_clr_name->SetLabelText(clr_query->GetFilaColorName(ams_filament_id, fila_color));
        if (m_clr_picker->GetParent()) m_clr_picker->GetParent()->Layout();
    }
}

void AMSMaterialsSetting::set_ctype(int ctype)
{
    m_clr_picker->ctype = ctype;
}

void AMSMaterialsSetting::on_picker_color(wxCommandEvent& event)
{
    std::vector<wxColour> colors = m_color_picker_popup->get_selected_colours();
    if (colors.empty()) {
        unsigned int color_num = event.GetInt();
        colors.emplace_back(color_num >> 24 & 0xFF, color_num >> 16 & 0xFF, color_num >> 8 & 0xFF, color_num & 0xFF);
    }

    set_ctype(m_color_picker_popup->get_selected_ctype());
    set_color(colors.front());
    set_colors(colors);
}

bool AMSMaterialsSetting::colour_editable() const
{
    if (!obj) return false;
    if (m_view_only || !m_is_third) return false;
    if (obj->is_in_printing() || obj->can_resume())
        return obj->is_support_filament_setting_inprinting;
    return true;
}

bool AMSMaterialsSetting::colour_palette_visible() const
{
    if (!obj) return false;
    if (m_view_only || !m_is_third) return false;
    return true;
}

void AMSMaterialsSetting::apply_dialog_size()
{
    SetMinSize(wxSize(AMS_MATERIALS_SETTING_DIALOG_SIZE.GetWidth(), -1));
    Fit();
}

bool AMSTraySettingBase::is_virtual_tray()
{
    if (ams_id == VIRTUAL_TRAY_MAIN_ID || ams_id == VIRTUAL_TRAY_DEPUTY_ID)
        return true;
    return false;
}

void AMSTraySettingBase::update_kval_editability()
{
    bool editable = !obj || !obj->GetCalib()->IsVersionInited();
    if (m_input_k_val) m_input_k_val->Enable(editable);
    if (m_input_n_val) m_input_n_val->Enable(editable);
}

void AMSMaterialsSetting::update_widgets()
{
    if (obj && obj->get_printer_series() == PrinterSeries::SERIES_X1 && !obj->GetCalib()->IsVersionInited()) {
        // Low version firmware does not display k value
        m_panel_normal->Show();
        m_panel_kn->Hide();
    }
    else if(is_virtual_tray()) // virtual tray
    {
        if (obj)
            m_panel_normal->Show();
        else
            m_panel_normal->Hide();
        m_panel_kn->Show();
    } else if (obj && (obj->ams_support_virtual_tray || obj->GetCalib()->IsVersionInited())) {
        m_panel_normal->Show();
        m_panel_kn->Show();
    } else {
        m_panel_normal->Show();
        m_panel_kn->Hide();
    }
    Layout();
}

bool AMSMaterialsSetting::Show(bool show)
{
    if (!show) {
        m_pa_data_pending = false;
    }

    if (show) {
        m_button_confirm->SetMinSize(AMS_MATERIALS_SETTING_BUTTON_SIZE);
        //m_clr_picker->set_color(m_clr_picker->GetParent()->GetBackgroundColour());

        m_ratio_text->Show();
        m_wiki_ctrl->Show();
        m_k_param->Show();
        m_input_k_val->Show();
        m_filament_arrow->SetBitmap(create_scaled_bitmap("filament_select_arrow", m_filament_box, 10));
        Layout();
        apply_dialog_size();
        wxGetApp().UpdateDlgDarkUI(this);
    }
    return DPIDialog::Show(show);
}



namespace {

constexpr size_t AMS_RECENT_FILAMENT_PRESETS_MAX = 5;
constexpr const char* AMS_RECENT_FILAMENT_PRESETS_KEY =
    "ams_recent_filament_presets";

std::vector<wxString> parse_ams_recent_filament_presets(
    const std::string& value)
{
    std::vector<wxString> result;
    std::stringstream stream(value);
    std::string item;

    while (std::getline(stream, item, '\n')) {
        if (!item.empty())
            result.emplace_back(from_u8(item));
    }

    return result;
}

std::string serialize_ams_recent_filament_presets(
    const std::vector<wxString>& items)
{
    std::string value;

    for (const wxString& item : items) {
        if (item.empty())
            continue;

        if (!value.empty())
            value += '\n';

        value += into_u8(item);
    }

    return value;
}

std::vector<wxString> load_ams_recent_filament_presets()
{
    return parse_ams_recent_filament_presets(
        wxGetApp().app_config->get(
            AMS_RECENT_FILAMENT_PRESETS_KEY));
}

void remember_ams_recent_filament_preset(
    const wxString& selected)
{
    if (selected.empty())
        return;

    std::vector<wxString> items =
        load_ams_recent_filament_presets();

    items.erase(
        std::remove(items.begin(), items.end(), selected),
        items.end());

    items.insert(items.begin(), selected);

    if (items.size() > AMS_RECENT_FILAMENT_PRESETS_MAX)
        items.resize(AMS_RECENT_FILAMENT_PRESETS_MAX);

    wxGetApp().app_config->set(
        AMS_RECENT_FILAMENT_PRESETS_KEY,
        serialize_ams_recent_filament_presets(items));
}

} // namespace


static void _collect_filament_info(const wxString& shown_name,
                                   const Preset& filament,
                                   std::unordered_map<wxString, wxString>& query_filament_vendors,
                                   std::unordered_map<wxString, wxString>& query_filament_types)
{
    query_filament_vendors[shown_name] = filament.config.get_filament_vendor();
    query_filament_types[shown_name] = filament.config.get_filament_type();
}

void AMSMaterialsSetting::get_filaments_info(const MachineObject*                     obj,
                                             const std::string&                       nozzle_diameter_str,
                                             wxArrayString&                           filament_items,
                                             std::map<std::string, FilamentInfos>&    map_filament_items,
                                             std::unordered_map<wxString, wxString>&  query_filament_vendors,
                                             std::unordered_map<wxString, wxString>&  query_filament_types)
{
    if (!obj) return;

    PresetBundle *  preset_bundle = wxGetApp().preset_bundle;
    if (!preset_bundle) return;

    auto& filaments = preset_bundle->filaments;
    BOOST_LOG_TRIVIAL(trace) << "system_preset_bundle filament number=" << filaments.size();

    auto printer_names = preset_bundle->get_printer_names_by_printer_type_and_nozzle(
        DevPrinterConfigUtil::get_printer_display_name(obj->printer_type), nozzle_diameter_str);

    auto collect_pass = [&](bool want_system) {
        std::set<std::string> filament_id_set; // Deduplicate by filament_id.
        for (auto filament_it = filaments.begin(); filament_it != filaments.end(); ++filament_it) {
            Preset& preset = *filament_it;
            // Skip non-root presets.
            if (filaments.get_preset_base(preset) != &preset) continue;
            // First pass: only system presets. Second pass: only user presets, and

            // skip user fila in system preset mode
            if (want_system && !preset.is_system) continue;
            // skip system fila in user preset mode
            if (!want_system && preset.is_system) continue;

            ConfigOption*        printer_opt  = filament_it->config.option("compatible_printers");
            ConfigOptionStrings* printer_strs = dynamic_cast<ConfigOptionStrings*>(printer_opt);
            if (!printer_strs) continue;

            for (const auto& printer_str : printer_strs->values) {
                if (printer_names.find(printer_str) == printer_names.end()) continue;
                // This preset is compatible with the current printer. Append it to the out containers, if not duplicated.
                if (filament_it->filament_id.empty()) break;
                if (!filament_id_set.insert(filament_it->filament_id).second) break;

                auto fialment_alias = filaments.get_preset_alias(preset, true);
                if (fialment_alias.empty()) break;

                filament_items.push_back(from_u8(fialment_alias));
                _collect_filament_info(fialment_alias, preset, query_filament_vendors, query_filament_types);

                FilamentInfos filament_infos;
                filament_infos.filament_id         = filament_it->filament_id;
                filament_infos.setting_id          = filament_it->setting_id;
                map_filament_items[fialment_alias] = filament_infos;
                break;
            }
        }
    };

    // First pass: system presets.
    collect_pass(true);
    // Second pass: user presets.
    if (obj->is_support_user_preset) {
        collect_pass(false);
    }
}

Preset* AMSMaterialsSetting::get_filament_by_id(const std::string& filament_id, bool is_system)
{
    PresetBundle *  preset_bundle = wxGetApp().preset_bundle;
    if (!preset_bundle) return nullptr;

    if (filament_id.empty()) return nullptr;
    auto& filaments = preset_bundle->filaments;
    for (auto it = filaments.begin(); it != filaments.end(); ++it) {
        Preset& preset = *it;
        if (filaments.get_preset_base(preset) != &preset) continue;
        if (is_system && !preset.is_system) continue;
        if (preset.filament_id == filament_id) return &preset;
    }
    return nullptr;
}

std::optional<DevNozzle> AMSTraySettingBase::get_slot_nozzle() const
{
    if (!obj)
        return std::nullopt;

    if (is_nozzle_combo_selector())
        return std::nullopt;

    if (auto rack = obj->GetNozzleRack(); rack && rack->IsSupported()) {
        return std::nullopt;
    }

    const int extruder_id = obj->GetFilaSystem()->GetExtruderIdByAmsId(std::to_string(ams_id));
    if (extruder_id == MAIN_EXTRUDER_ID || extruder_id == DEPUTY_EXTRUDER_ID) {
        auto nozzle = obj->get_nozzle_by_id_code(extruder_id);
        if (!nozzle.IsEmpty())
            return nozzle;
    }

    return std::nullopt;
}

std::set<int> AMSTraySettingBase::slot_bound_extruder_ids() const
{
    std::set<int> extruder_ids;
    if (obj && obj->GetFilaSystem()) {
        if (ams_id == VIRTUAL_TRAY_MAIN_ID || ams_id == VIRTUAL_TRAY_DEPUTY_ID) {
            extruder_ids.insert(obj->GetFilaSystem()->GetExtruderIdByAmsId(std::to_string(ams_id)));
        } else if (DevAms* curr_ams = obj->GetFilaSystem()->GetAmsById(std::to_string(ams_id))) {
            auto extruder_id_set = curr_ams->GetBindedExtruderSet();
            extruder_ids.insert(extruder_id_set.begin(), extruder_id_set.end());
        }
    }
    return extruder_ids;
}

bool AMSTraySettingBase::is_nozzle_combo_selector() const
{
    if (obj && obj->GetNozzleSystem()) {
        auto rack = obj->GetNozzleSystem()->GetNozzleRack();
        // Upstream gates the combo on RackType_AB; this branch models a single rack,
        // so any supported rack is combo-driven.
        if (rack && rack->IsSupported()) {
            const auto extruder_ids = slot_bound_extruder_ids();
            auto switcher = obj->GetFilaSwitch();
            if (switcher && switcher->IsInstalled())
                return !extruder_ids.empty();
            return extruder_ids == std::set<int>{MAIN_EXTRUDER_ID};
        }
    }
    return false;
}

std::optional<AMSTraySettingBase::EffectiveNozzle> AMSTraySettingBase::get_combo_nozzle() const
{
    if (m_comboBox_nozzle_type) {
        const int sel = m_comboBox_nozzle_type->GetSelection();
        if (sel != wxNOT_FOUND) {
            auto* sel_pair = (std::pair<NozzleDiameterType, NozzleFlowType>*)m_comboBox_nozzle_type->GetClientData(sel);
            if (sel_pair && sel_pair->first != NozzleDiameterType::NONE_DIAMETER_TYPE)
                return EffectiveNozzle{sel_pair->first, sel_pair->second};
        }
    }
    return std::nullopt;
}

std::optional<AMSTraySettingBase::EffectiveNozzle> AMSTraySettingBase::get_effective_nozzle() const
{
    if (is_nozzle_combo_selector())
        return get_combo_nozzle();

    if (auto slot_nozzle = get_slot_nozzle()) {
        const auto dia = slot_nozzle->GetNozzleDiameterType();
        if (dia != NozzleDiameterType::NONE_DIAMETER_TYPE)
            return EffectiveNozzle{dia, slot_nozzle->GetNozzleFlowType()};
    }

    return std::nullopt;
}

void AMSMaterialsSetting::set_filament_box_text(const wxString& text)
{
    m_filament_text->SetLabel(text);
    m_filament_box->Layout();
}

void AMSMaterialsSetting::enable_filament_box(bool enable)
{
    m_filament_box_editable = enable;
}

void AMSMaterialsSetting::on_open_filament_select_dialog()
{
    if (!obj) return;
    if (m_view_only || !m_filament_box_editable) return;

    std::string nozzle_diameter_str;
    if (auto nozzle_opt = get_slot_nozzle()) {
        std::ostringstream stream;
        stream << std::fixed << std::setprecision(1) << nozzle_opt->GetNozzleDiameter();
        nozzle_diameter_str = stream.str();
    }

    wxArrayString                          filament_items;
    std::map<std::string, FilamentInfos>   tmp_map;
    std::unordered_map<wxString, wxString> query_filament_vendors;
    std::unordered_map<wxString, wxString> query_filament_types;
    get_filaments_info(obj, nozzle_diameter_str, filament_items, tmp_map,
                       query_filament_vendors, query_filament_types);

    // Current alias for preselection in FilamentSelectDialog's System Presets tab.
    // m_selected_spool_id is empty when the current selection is a system preset.
    wxString current_alias;
    if (m_selected_spool_id.empty())
        current_alias = m_current_filament_alias;

    std::set<std::string> printer_names;
    if (PresetBundle* preset_bundle = wxGetApp().preset_bundle)
        printer_names = preset_bundle->get_printer_names_by_printer_type_and_nozzle(
            DevPrinterConfigUtil::get_printer_display_name(obj->printer_type), nozzle_diameter_str);

    FilamentSelectDialog dlg(this);

    const int     cascade  = FromDIP(20);
    const wxPoint base_pos = this->GetScreenPosition();
    const wxSize  dlg_size = dlg.GetSize();
    const wxRect  area     = wxDisplay(wxDisplay::GetFromWindow(this)).GetClientArea();
    int pos_x = base_pos.x + cascade;
    int pos_y = base_pos.y + cascade;
    if (pos_x + dlg_size.GetWidth()  > area.GetRight())  pos_x = area.GetRight()  - dlg_size.GetWidth();
    if (pos_y + dlg_size.GetHeight() > area.GetBottom()) pos_y = area.GetBottom() - dlg_size.GetHeight();
    if (pos_x < area.GetLeft()) pos_x = area.GetLeft();
    if (pos_y < area.GetTop())  pos_y = area.GetTop();
    dlg.Move(wxPoint(pos_x, pos_y));

    dlg.Popup(filament_items, query_filament_vendors, query_filament_types, current_alias,
              std::move(printer_names));

    const auto& res = dlg.get_result();
    if (!res.is_valid()) return;

    trigger_select_filament(std::string{}, /*from_printer=*/false, res.preset_alias);
}

void AMSMaterialsSetting::Popup(wxString filament, wxString sn, wxString temp_min, wxString temp_max, wxString k, wxString n)
{
    if (!obj) return;
    BOOST_LOG_TRIVIAL(info) << "AMSMaterialsSetting::Popup dev_id=" << BBLCrossTalk::Crosstalk_DevId(obj->get_dev_id())
                             << ", ams_id=" << ams_id << ", slot_id=" << slot_id
                             << ", calib_version_inited=" << obj->GetCalib()->IsVersionInited()
                             << ", pa_history_ready=" << obj->GetCalib()->IsPAHistoryReady()
                             << ", pa_history_status=" << (int)obj->GetCalib()->GetPAHistoryStatus();


    update_widgets();
    // set default value
    if (k.IsEmpty())
        k = "0.000";
    if (n.IsEmpty())
        n = "0.000";

    m_input_k_val->GetTextCtrl()->SetValue(k);
    m_input_n_val->GetTextCtrl()->SetValue(n);

    wxString bambu_filament_name;
    wxString hint_filament_name; // the hint type to be selected

    PresetBundle *preset_bundle = wxGetApp().preset_bundle;
    m_filament_type             = "";

    std::string nozzle_diameter_str;
    auto nozzle_opt = get_slot_nozzle();
    if (nozzle_opt) {
        std::ostringstream stream;
        stream << std::fixed << std::setprecision(1) << nozzle_opt->GetNozzleDiameter();
        nozzle_diameter_str = stream.str();
    }

    {
        wxArrayString                          unused_items;
        std::unordered_map<wxString, wxString> unused_vendors;
        std::unordered_map<wxString, wxString> unused_types;
        get_filaments_info(obj, nozzle_diameter_str, unused_items, map_filament_items, unused_vendors, unused_types);
    }

    if (Preset* ams_fila_it = get_filament_by_id(ams_filament_id, !m_is_third)) {
        auto fialment_alias = preset_bundle->filaments.get_preset_alias(*ams_fila_it, true);
        if (!fialment_alias.empty()) {
            hint_filament_name  = from_u8(fialment_alias);
            bambu_filament_name = from_u8(fialment_alias);
        }

        // update if nozzle_temperature_range is found
        if (ConfigOption *opt_min = ams_fila_it->config.option("nozzle_temperature_range_low")) {
            ConfigOptionInts *opt_min_ints = dynamic_cast<ConfigOptionInts *>(opt_min);
            if (opt_min_ints) {
                wxString text_nozzle_temp_min = wxString::Format("%d", opt_min_ints->get_at(0));
                m_nozzle_temp_min_str = text_nozzle_temp_min;
            }
        }
        if (ConfigOption *opt_max = ams_fila_it->config.option("nozzle_temperature_range_high")) {
            ConfigOptionInts *opt_max_ints = dynamic_cast<ConfigOptionInts *>(opt_max);
            if (opt_max_ints) {
                wxString text_nozzle_temp_max = wxString::Format("%d", opt_max_ints->get_at(0));
                m_nozzle_temp_max_str = text_nozzle_temp_max;
            }
        }
        update_nozzle_temp_display();
        std::string display_filament_type;
        m_filament_type = ams_fila_it->config.get_filament_type(display_filament_type);
    }

    if (obj) {
        if (!m_is_third) {
            enable_filament_box(false);
            wxString display = bambu_filament_name.empty() ? "Bambu " + filament : bambu_filament_name;
            m_current_filament_alias = display;
            set_filament_box_text(display);

            m_nozzle_temp_min_str = temp_min;
            m_nozzle_temp_max_str = temp_max;
            update_nozzle_temp_display();
        }
        else {
            enable_filament_box(true);
        }

        if (obj->GetCalib()->IsVersionInited()) {
            m_title_pa_profile->Show();
            m_comboBox_cali_result->Show();
            m_input_k_val->Disable();
        }
        else {
            m_title_pa_profile->Hide();
            m_comboBox_cali_result->Hide();
            m_input_k_val->Enable();
        }

        m_button_reset->Show();
        //m_button_confirm->Show();
    }


    m_selected_spool_id.clear();

    std::string  init_spool_id;
    wxString     init_display_text = hint_filament_name;  // preset alias or empty

    // Do not RequestPAHistory here: StatusPanel ticks PrepareFetchQueue/SendNextFetch.
    // If the cache is not ready yet, TryRefreshPAProfiles rebuilds the PA list when it is.
    m_pa_data_pending = !obj->GetCalib()->IsPAHistoryReady();

    BOOST_LOG_TRIVIAL(info) << "AMSMaterialsSetting::on_select_filament pa history"
                             << ", dev_id=" << BBLCrossTalk::Crosstalk_DevId(obj->get_dev_id())
                             << ", ams_id=" << ams_id << ", slot_id=" << slot_id
                             << ", history_ready=" << obj->GetCalib()->IsPAHistoryReady();

    m_snap_taken = false;
    trigger_select_filament(init_spool_id, /*from_printer=*/true, init_display_text);

    m_clr_picker->Enable(true);

    update();
    apply_dialog_size();
    Layout();
    ShowModal();
}

void AMSMaterialsSetting::trigger_select_filament(const std::string& spool_id,
                                                   bool               from_printer,
                                                   const wxString&    display_text)
{
    m_pending_spool_id     = spool_id;
    m_pending_from_printer = from_printer;
    m_current_filament_alias = display_text;
    set_filament_box_text(display_text);
    apply_filament_selection();
}

bool AMSTraySettingBase::save_pa_profile_selection()
{
    if (!obj) return false;

    wxString k_text = m_input_k_val ? m_input_k_val->GetTextCtrl()->GetValue() : wxString("0");
    wxString n_text = m_input_n_val ? m_input_n_val->GetTextCtrl()->GetValue() : wxString("0");

    if (!obj->GetCalib()->IsVersionInited()
        && (obj->get_printer_series() != PrinterSeries::SERIES_X1)
        && !ExtrusionCalibration::check_k_validation(k_text)) {
        wxString k_tips = wxString::Format(_L("Please input a valid value (K in %.1f~%.1f)"), MIN_PA_K_VALUE, MAX_PA_K_VALUE);
        MessageDialog msg_dlg(nullptr, k_tips, wxEmptyString, wxICON_WARNING | wxOK);
        msg_dlg.ShowModal();
        return false;
    }

    double k = 0.0;
    double n = 0.0;
    try {
        k_text.ToDouble(&k);
        n_text.ToDouble(&n);
    }
    catch (...) {
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << " Convert k/n value failed, k_text: " << k_text << ", n_text: " << n_text;
    }

    if (is_virtual_tray()) {
        auto vt_tray_id = obj->GetFilaSystem()->GetTrayIdByAmsSlotId(ams_id, 0);
        if (obj->GetCalib()->IsVersionInited()) {
            PACalibIndexInfo select_index_info;
            select_index_info.tray_id         = vt_tray_id;
            select_index_info.ams_id          = ams_id;
            select_index_info.slot_id         = 0;
            select_index_info.nozzle_diameter = obj->GetExtderSystem()->GetNozzleDiameter(0);
            auto cali_select_id = m_comboBox_cali_result ? m_comboBox_cali_result->GetSelection() : 0;
            if (!m_pa_profile_items.empty() && cali_select_id >= 0) {
                select_index_info.cali_idx    = m_pa_profile_items[cali_select_id].cali_idx;
                select_index_info.filament_id = m_pa_profile_items[cali_select_id].filament_id;
            } else {
                select_index_info.cali_idx    = -1;
                select_index_info.filament_id = ams_filament_id;
            }
            CalibUtils::select_PA_calib_result(select_index_info);
        } else {
            obj->command_extrusion_cali_set(vt_tray_id, "", "", k, n);
        }
    } else {
        int cali_tray_id = obj->GetFilaSystem()->GetTrayIdByAmsSlotId(ams_id, slot_id);
        if (obj->GetCalib()->IsVersionInited()) {
            PACalibIndexInfo select_index_info;
            select_index_info.tray_id         = cali_tray_id;
            select_index_info.ams_id          = ams_id;
            select_index_info.slot_id         = slot_id;
            select_index_info.nozzle_diameter = obj->GetExtderSystem()->GetNozzleDiameter(0);
            auto cali_select_id = m_comboBox_cali_result ? m_comboBox_cali_result->GetSelection() : 0;
            if (!m_pa_profile_items.empty() && cali_select_id > 0) {
                select_index_info.cali_idx    = m_pa_profile_items[cali_select_id].cali_idx;
                select_index_info.filament_id = m_pa_profile_items[cali_select_id].filament_id;
            } else {
                select_index_info.cali_idx    = -1;
                select_index_info.filament_id = ams_filament_id;
            }
            CalibUtils::select_PA_calib_result(select_index_info);
        } else {
            obj->command_extrusion_cali_set(cali_tray_id, "", "", k, n);
        }
    }
    return true;
}

void AMSTraySettingBase::TryRefreshPAProfiles()
{
    if (!m_pa_data_pending || !obj) return;
    if (!obj->GetCalib()->IsPAHistoryReady()) {
        BOOST_LOG_TRIVIAL(info) << "AMSTraySettingBase::TryRefreshPAProfiles PA history not ready yet, dev_id="
                                 << BBLCrossTalk::Crosstalk_DevId(obj->get_dev_id())
                                 << ", pa_history_status=" << (int)obj->GetCalib()->GetPAHistoryStatus();
        return;
    }

    m_pa_data_pending = false;

    BOOST_LOG_TRIVIAL(info) << "AMSTraySettingBase::TryRefreshPAProfiles PA history ready, dev_id="
                             << BBLCrossTalk::Crosstalk_DevId(obj->get_dev_id());
    on_pa_history_ready();
}

void AMSMaterialsSetting::on_pa_history_ready()
{
    BOOST_LOG_TRIVIAL(info) << "AMSMaterialsSetting::on_pa_history_ready rebuilding pa profiles"
                             << ", ams_id=" << ams_id << ", slot_id=" << slot_id
                             << ", text=" << m_current_filament_alias.ToStdString();
    const bool from_printer = m_snap_taken && m_selected_spool_id == m_open_spool_id && ams_filament_id == m_open_filament_id;
    trigger_select_filament(m_selected_spool_id, from_printer, m_current_filament_alias);
}

void AMSTraySettingBase::reset_calibration(const std::string& selected_filament_id)
{
    if (!obj) return;
    if (!obj->GetCalib()->IsVersionInited() && obj->get_printer_series() == PrinterSeries::SERIES_P1P) {
        int cali_tray_id = obj->GetFilaSystem()->GetTrayIdByAmsSlotId(ams_id, slot_id);
        obj->command_extrusion_cali_set(cali_tray_id, "", "", 0.0, 0.0);
    } else {
        PACalibIndexInfo select_index_info;
        select_index_info.tray_id         = obj->GetFilaSystem()->GetTrayIdByAmsSlotId(ams_id, slot_id);
        select_index_info.ams_id          = ams_id;
        select_index_info.slot_id         = slot_id;
        select_index_info.nozzle_diameter = obj->GetExtderSystem()->GetNozzleDiameter(0);
        select_index_info.cali_idx        = -1;
        select_index_info.filament_id     = selected_filament_id;
        CalibUtils::select_PA_calib_result(select_index_info);
    }
}

void AMSTraySettingBase::on_select_cali_result(wxCommandEvent &evt)
{
    m_pa_cali_select_id = evt.GetSelection();
    if (m_pa_cali_select_id >= 0 && m_pa_profile_items.size() > (size_t)m_pa_cali_select_id) {
        if (m_input_k_val)
            m_input_k_val->GetTextCtrl()->SetValue(float_to_string_with_precision(m_pa_profile_items[m_pa_cali_select_id].k_value));
        if (m_input_n_val)
            m_input_n_val->GetTextCtrl()->SetValue(float_to_string_with_precision(m_pa_profile_items[m_pa_cali_select_id].n_coef));
    }
    else {
        if (m_input_k_val)
            m_input_k_val->GetTextCtrl()->SetValue(std::to_string(0.00));
        if (m_input_n_val)
            m_input_n_val->GetTextCtrl()->SetValue(std::to_string(0.00));
    }
}

void AMSTraySettingBase::on_select_nozzle_pos_id(wxCommandEvent &evt)
{
    int selected_id = evt.GetSelection();

    if(selected_id == -1){
        m_comboBox_cali_result->Disable();
    } else{
        m_comboBox_cali_result->Enable();
        update_pa_profile_items();
    }
}

void AMSTraySettingBase::update_pa_profile_items()
{
    if (!obj || !obj->GetNozzleSystem()) return;

    wxArrayString items;
    {
        m_pa_profile_items.clear();
        if (m_comboBox_cali_result) m_comboBox_cali_result->SetValue(wxEmptyString);
        if (m_input_k_val) m_input_k_val->GetTextCtrl()->SetValue(wxEmptyString);
        // add default item
        PACalibResult default_item;
        default_item.cali_idx    = -1;
        default_item.filament_id = ams_filament_id;
        if (obj->GetConfig()->SupportCalibrationPA_FlowAuto()) {
            default_item.k_value = -1;
            default_item.n_coef  = -1;
        } else {
            get_default_k_n_value(ams_filament_id, default_item.k_value, default_item.n_coef);
        }
        m_pa_profile_items.emplace_back(default_item);
        items.push_back(_L("Default"));
    }

    auto rack = obj->GetNozzleSystem()->GetNozzleRack();

    BOOST_LOG_TRIVIAL(info) << "AMSTraySettingBase::update_pa_profile_items dev_id=" << BBLCrossTalk::Crosstalk_DevId(obj->get_dev_id())
                             << ", ams_id=" << ams_id << ", ams_filament_id=" << ams_filament_id
                             << ", calib_version_inited=" << obj->GetCalib()->IsVersionInited()
                             << ", pa_history_ready=" << obj->GetCalib()->IsPAHistoryReady();

    const auto extruder_ids = slot_bound_extruder_ids();
    const auto effective = get_effective_nozzle();
    if (!effective && is_nozzle_combo_selector()) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " no effective nozzle, show Default only"
                                 << ", ams_id=" << ams_id
                                 << ", combo_selector=true"
                                 << ", extruder_ids=" << extruder_ids.size();
        if (m_comboBox_cali_result) {
            m_comboBox_cali_result->Set(items);
            if (m_comboBox_cali_result->GetCount() > 0)
                m_comboBox_cali_result->SetSelection(0);
        }
        return;
    }

    for (int extruder_id : extruder_ids) {
        NozzleDiameterType dia_type = effective ? effective->diameter : NozzleDiameterType::NONE_DIAMETER_TYPE;
        NozzleFlowType nozzle_flow_type = effective ? effective->flow_type : NozzleFlowType::NONE_FLOWTYPE;
        if (!effective) {
            dia_type = obj->GetExtderSystem()->GetNozzleDiameterType(extruder_id);
            nozzle_flow_type = obj->GetExtderSystem()->GetNozzleFlowType(extruder_id);
        }
        if (dia_type == NozzleDiameterType::NONE_DIAMETER_TYPE) {
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " skip unknown nozzle diameter, extruder_id=" << extruder_id;
            continue;
        }
        // Empty tray filament: no historical profile is offered.
        if (ams_filament_id.empty()) {
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " skip empty filament_id, extruder_id=" << extruder_id;
            continue;
        }

        PaHistoryFilter pa_history_filter = obj->GetCalib()->GetPaHistoryFilter();
        pa_history_filter.set_filament_id(ams_filament_id)
                .set_nozzle_diameter(dia_type)
                .set_extruder_id(obj->is_multi_extruders() ? std::optional<int>(extruder_id) : std::nullopt)
                .set_nozzle_volume_type(nozzle_flow_type == NozzleFlowType::NONE_FLOWTYPE ? std::nullopt : std::optional<NozzleVolumeType>(DevNozzle::ToNozzleVolumeType(nozzle_flow_type)));

        std::vector<PACalibResult> matched = pa_history_filter.get();
        std::sort(matched.begin(), matched.end(), [](const PACalibResult &left, const PACalibResult &right) { return left.nozzle_pos_id < right.nozzle_pos_id; });

        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " PaHistoryFilter extruder_id=" << extruder_id
                                 << ", nozzle_diameter=" << DevNozzle::ToNozzleDiameterFloat(dia_type)
                                 << ", filament_id=" << ams_filament_id
                                 << ", matched=" << matched.size();
        for (const auto &cali_item : matched) {
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " cali_item: cali_idx=" << cali_item.cali_idx
                                     << ", name=" << cali_item.name
                                     << ", filament_id=" << cali_item.filament_id
                                     << ", k_value=" << cali_item.k_value
                                     << ", n_coef=" << cali_item.n_coef
                                     << ", nozzle_diameter=" << cali_item.nozzle_diameter
                                     << ", nozzle_volume_type=" << (int) cali_item.nozzle_volume_type
                                     << ", nozzle_pos_id=" << cali_item.nozzle_pos_id
                                     << ", extruder_id=" << cali_item.extruder_id;
        }

        for (auto cali_item : matched) {
            if(rack->IsSupported())
            {
                if(cali_item.nozzle_pos_id == 0) {
                    items.push_back(wxString::Format("R | %s", from_u8(cali_item.name)));
                } else if(cali_item.nozzle_pos_id == 1) {
                    items.push_back(wxString::Format("L | %s", from_u8(cali_item.name)));
                } else if(cali_item.nozzle_pos_id >= 0x10){
                    items.push_back(wxString::Format("%d | %s", (cali_item.nozzle_pos_id & 0x0f) + 1, from_u8(cali_item.name)));
                } else {
                    items.push_back(wxString::Format("N/A | %s", from_u8(cali_item.name)));
                    BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << "Nozzle position id is -1 or invalid.";
                }
            } else {
                items.push_back(wxString::Format("%s", from_u8(cali_item.name)));
            }

            m_pa_profile_items.push_back(cali_item);
        }
    }

    if (m_comboBox_cali_result) {
        m_comboBox_cali_result->Set(items);
        m_comboBox_cali_result->SetSelection(0);
    }
    update_kval_editability();
}

void AMSTraySettingBase::update_nozzle_combo(MachineObject* obj){
    if (!obj || !obj->GetNozzleSystem() || !m_comboBox_nozzle_type)
        return;

    auto rack = obj->GetNozzleSystem()->GetNozzleRack();
    auto switcher = obj->GetFilaSwitch();

    if (is_nozzle_combo_selector()) {
        std::set<std::pair<NozzleDiameterType, NozzleFlowType>> nozzle_type_set;
        if (auto r_extder = obj->GetExtderSystem()->GetExtderById(MAIN_EXTRUDER_ID)) {
            auto r_nozzle = obj->GetNozzleSystem()->GetExtNozzle(r_extder->GetNozzleId());
            if (r_nozzle.IsNormal())
                nozzle_type_set.insert(std::make_pair(r_nozzle.GetNozzleDiameterType(), r_nozzle.GetNozzleFlowType()));
        }
        if (switcher && switcher->IsInstalled() && ams_id != VIRTUAL_TRAY_MAIN_ID) {
            if (auto l_extder = obj->GetExtderSystem()->GetExtderById(DEPUTY_EXTRUDER_ID)) {
                auto l_nozzle = obj->GetNozzleSystem()->GetExtNozzle(l_extder->GetNozzleId());
                if (l_nozzle.IsNormal())
                    nozzle_type_set.insert(std::make_pair(l_nozzle.GetNozzleDiameterType(), l_nozzle.GetNozzleFlowType()));
            }
        }
        auto nozzle_map = rack->GetRackNozzles();
        for (auto &nozzle : nozzle_map) { // Add the nozzle of the rack
            if (nozzle.second.IsNormal()) {
                nozzle_type_set.insert(std::make_pair(nozzle.second.GetNozzleDiameterType(), nozzle.second.GetNozzleFlowType()));
            }
        }

        std::vector<std::pair<NozzleDiameterType, NozzleFlowType>> nozzle_type_vec(nozzle_type_set.begin(), nozzle_type_set.end());
        std::sort(nozzle_type_vec.begin(), nozzle_type_vec.end(), [](const std::pair<NozzleDiameterType, NozzleFlowType> & left, const std::pair<NozzleDiameterType, NozzleFlowType>& right) -> bool {
            if(left.first == right.first) {
                return left.second < right.second;
            } else {
                return left.first < right.first;
            }
        });

        /* make nozzle type combobox item */
        m_comboBox_nozzle_type->Clear();
        for(auto pair : nozzle_type_vec){
            wxString item = DevNozzle::ToNozzleDiameterStr(pair.first);
            item += " ";
            item += DevNozzle::GetNozzleFlowTypeStr(pair.second);
            m_comboBox_nozzle_type->Append(item, wxNullBitmap, new std::pair<NozzleDiameterType, NozzleFlowType>(pair));
        }
        if (m_title_nozzle_type) m_title_nozzle_type->Show();
        m_comboBox_nozzle_type->Show();
        if (m_nozzle_type_spacer_item) m_nozzle_type_spacer_item->Show(true);

        /* set nozzle pos tooltip */
        if (m_title_pa_profile) {
            auto font = m_title_pa_profile->GetFont();
            font.SetUnderlined(true);
            m_title_pa_profile->SetFont(font);
            std::string ams_mat_pt = wxGetApp().preset_bundle->printers.get_edited_preset().get_printer_type(wxGetApp().preset_bundle);
            wxString ams_ext_name = _L(DevPrinterConfigUtil::get_toolhead_display_name(ams_mat_pt, MAIN_EXTRUDER_ID, ToolHeadComponent::Extruder, ToolHeadNameCase::LowerCase));
            m_title_pa_profile->SetToolTip(wxString::Format(_L("Note: The hotend number on the %s is tied to the holder. When the hotend is moved to a new holder, its number will update automatically."), ams_ext_name));
        }
    } else{
        m_comboBox_nozzle_type->Clear();
        m_comboBox_nozzle_type->SetSelection(-1);
        m_comboBox_nozzle_type->SetValue(wxEmptyString);
        if (m_title_nozzle_type) m_title_nozzle_type->Hide();
        m_comboBox_nozzle_type->Hide();
        if (m_nozzle_type_spacer_item) m_nozzle_type_spacer_item->Show(false);

        if (m_title_pa_profile) {
            auto font = m_title_pa_profile->GetFont();
            font.SetUnderlined(false);
            m_title_pa_profile->SetFont(font);
            m_title_pa_profile->SetToolTip(wxEmptyString);
        }
    }

    Layout();
    Fit();
}

int AMSMaterialsSetting::get_nozzle_combo_id_code() const{
    auto sel = m_comboBox_nozzle_type->GetSelection();
    if (sel != wxNOT_FOUND) return *(reinterpret_cast<int*>(m_comboBox_nozzle_type->GetClientData(sel)));

    return -1;
}

int AMSMaterialsSetting::get_nozzle_sel_by_sn(MachineObject* obj, const std::string& sn){
    if(!obj) return -1;

    auto nozzle = obj->get_nozzle_by_sn(sn);

    for(unsigned int i = 0; i<m_comboBox_nozzle_type->GetCount(); i++){
        auto sel_pair = (std::pair<NozzleDiameterType, NozzleFlowType>*)m_comboBox_nozzle_type->GetClientData(i);

        if(sel_pair->first == nozzle.GetNozzleDiameterType() && sel_pair->second == nozzle.GetNozzleFlowType())
            return i;
    }

    return -1;
}

bool AMSTraySettingBase::switch_nozzle_combo_to_target(NozzleVolumeType volume_type, float nozzle_diameter)
{
    NozzleFlowType  target_flow     = DevNozzle::ToNozzleFlowType(volume_type);
    NozzleDiameterType target_diam  = DevNozzle::ToNozzleDiameterType(nozzle_diameter);
    for (unsigned int i = 0; i < m_comboBox_nozzle_type->GetCount(); ++i) {
        auto* p = (std::pair<NozzleDiameterType, NozzleFlowType>*)m_comboBox_nozzle_type->GetClientData(i);
        if (p && p->first == target_diam && p->second == target_flow) {
            m_comboBox_nozzle_type->SetSelection(i);
            return true;
        }
    }
    m_comboBox_nozzle_type->SetSelection(-1);
    m_comboBox_nozzle_type->SetValue(wxEmptyString);
    return false;
}

int AMSMaterialsSetting::get_cali_index_by_ams_slot(MachineObject *obj, int ams_id, int slot_id)
{
    if (!obj) return -1;

    if (!m_pending_from_printer) return -1;

    if (obj->GetCalib()->IsVersionInited()) {
        if (ams_id == VIRTUAL_TRAY_MAIN_ID || ams_id == VIRTUAL_TRAY_DEPUTY_ID) {
            for (auto slot : obj->vt_slot) {
                if (slot.id == std::to_string(ams_id)) return slot.cali_idx;
            }
        } else {
            DevAmsTray *selected_tray = obj->GetFilaSystem()->GetAmsTray(std::to_string(ams_id), std::to_string(slot_id));
            if (!selected_tray) {
                return -1;
            } else {
                return selected_tray->cali_idx;
            }
        }
    }

    return -1;
}

void AMSMaterialsSetting::apply_filament_selection()
{
    const bool from_printer_flag = m_pending_from_printer;

    m_filament_type = "";
    PresetBundle* preset_bundle = wxGetApp().preset_bundle;
    if (preset_bundle) {
        std::ostringstream stream;
        if (obj)
            stream << std::fixed << std::setprecision(1) << obj->GetExtderSystem()->GetNozzleDiameter(0);
        std::string nozzle_diameter_str = stream.str();
        std::set<std::string> printer_names = preset_bundle->get_printer_names_by_printer_type_and_nozzle(DevPrinterConfigUtil::get_printer_display_name(obj->printer_type),
                                                                                                          nozzle_diameter_str);
        for (auto it = preset_bundle->filaments.begin(); it != preset_bundle->filaments.end(); it++) {
            if (!m_current_filament_alias.IsEmpty()) {
                auto filament_item = map_filament_items[into_u8(m_current_filament_alias)];
                std::string filament_id   = filament_item.filament_id;

                if (it->filament_id.compare(filament_id) == 0) {
                    bool has_compatible_printer = false;
                    ConfigOption* printer_opt = it->config.option("compatible_printers");
                    ConfigOptionStrings* fila_compatible_printer_strs = dynamic_cast<ConfigOptionStrings*>(printer_opt);
                    if (fila_compatible_printer_strs) {
                        for (const auto& fila_compatible_printer_str : fila_compatible_printer_strs->values) {
                            for (std::string printer_name : printer_names) {
                                if (fila_compatible_printer_str.find(printer_name) != std::string::npos) {
                                    has_compatible_printer = true;
                                    break;
                                }
                            }

                            if (has_compatible_printer) {
                                break;
                            }
                        }
                    }

                    if (!it->is_system && !has_compatible_printer) continue;
                    // ) if nozzle_temperature_range is found
                    ConfigOption* opt_min = it->config.option("nozzle_temperature_range_low");
                    if (opt_min) {
                        ConfigOptionInts* opt_min_ints = dynamic_cast<ConfigOptionInts*>(opt_min);
                        if (opt_min_ints) {
                            wxString text_nozzle_temp_min = wxString::Format("%d", opt_min_ints->get_at(0));
                            m_nozzle_temp_min_str = text_nozzle_temp_min;
                        }
                    }
                    ConfigOption* opt_max = it->config.option("nozzle_temperature_range_high");
                    if (opt_max) {
                        ConfigOptionInts* opt_max_ints = dynamic_cast<ConfigOptionInts*>(opt_max);
                        if (opt_max_ints) {
                            wxString text_nozzle_temp_max = wxString::Format("%d", opt_max_ints->get_at(0));
                            m_nozzle_temp_max_str = text_nozzle_temp_max;
                        }
                    }
                    ConfigOption* opt_type = it->config.option("filament_type");
                    bool found_filament_type = false;
                    if (opt_type) {
                        ConfigOptionStrings* opt_type_strs = dynamic_cast<ConfigOptionStrings*>(opt_type);
                        if (opt_type_strs) {
                            found_filament_type = true;
                            //m_filament_type = opt_type_strs->get_at(0);
                            std::string display_filament_type;
                            m_filament_type = it->config.get_filament_type(display_filament_type);
                        }
                    }
                    if (!found_filament_type)
                        m_filament_type = "";

                    break;
                }
            }
        }
    }
    if (m_nozzle_temp_min_str.IsEmpty()) {
         m_nozzle_temp_min_str = "0";
    }
    if (m_nozzle_temp_max_str.IsEmpty()) {
         m_nozzle_temp_max_str = "0";
    }
    update_nozzle_temp_display();

    m_selected_spool_id = m_pending_spool_id;

    //reset cali
    int cali_select_idx = -1;

    update_nozzle_combo(obj);

    const bool no_selection = m_selected_spool_id.empty() && m_current_filament_alias.IsEmpty();
    if ( !this->obj || no_selection) {
        m_input_k_val->Enable(false);
        m_input_n_val->Enable(false);
        m_button_confirm->Disable();
        m_button_confirm->SetBackgroundColor(wxColour(0x90, 0x90, 0x90));
        m_button_confirm->SetBorderColor(wxColour(0x90, 0x90, 0x90));
        m_comboBox_cali_result->Clear();
        m_comboBox_cali_result->SetValue(wxEmptyString);
        m_comboBox_nozzle_type->Clear();
        m_comboBox_nozzle_type->SetValue(wxEmptyString);
        m_input_k_val->GetTextCtrl()->SetValue(wxEmptyString);
        m_input_n_val->GetTextCtrl()->SetValue(wxEmptyString);
        m_pending_spool_id.clear();
        m_pending_from_printer = false;

        set_empty_color(*wxWHITE);
        if (m_color_picker_popup) {
            const bool visible  = colour_palette_visible();
            const bool editable = colour_editable();
            m_color_picker_popup->Show(visible);
            m_color_picker_popup->Enable(editable);   // disabled == read-only, palette stays visible
            if (visible) {
                m_color_picker_popup->set_ams_colours(collect_ams_color_items(obj->GetFilaSystem().get()));
                m_color_picker_popup->set_preset_colours({});
            }
            m_panel_normal->Layout();
            Layout();
            apply_dialog_size();
        }
        return;
    }
    else {
        m_button_confirm->SetBackgroundColor(m_btn_bg_green);
        m_button_confirm->SetBorderColor(wxColour(0, 174, 66));
        m_button_confirm->SetTextColor(wxColour("#FFFFFE"));
        m_button_confirm->Enable(true);
    }

    //filament id
    ams_filament_id = "";
    ams_setting_id = "";

    if (preset_bundle) {
        for (auto it = preset_bundle->filaments.begin(); it != preset_bundle->filaments.end(); it++) {
            auto itor = map_filament_items.find(into_u8(m_current_filament_alias));
            if ( itor != map_filament_items.end()) {
                ams_filament_id = itor->second.filament_id;
                ams_setting_id  = itor->second.setting_id;
                break;
            }

            if (it->alias.compare(into_u8(m_current_filament_alias)) == 0) {
                ams_filament_id = it->filament_id;
                ams_setting_id = it->setting_id;
                break;
            }
        }
    }

    auto get_cali_index = [this](const std::string& str) -> int{
        for (int i = 0; i < int(m_pa_profile_items.size()); ++i) {
            if (m_pa_profile_items[i].name == str)
                return i;
        }
        return 0;
    };

    if (auto ams_item = obj->GetFilaSystem()->GetAmsById(std::to_string(ams_id))) {
        if (ams_item->GetBindedExtruderSet().size() == 1) {
            int extruder_id = *ams_item->GetBindedExtruderSet().begin();
            if (obj->is_nozzle_flow_type_supported() && (obj->GetExtderSystem()->GetNozzleFlowType(extruder_id) == NozzleFlowType::NONE_FLOWTYPE)) {
                MessageDialog dlg(nullptr, _L("The nozzle flow is not set. Please set the nozzle flow rate before editing the filament.\n'Device -> Print parts'"), _L("Warning"), wxICON_WARNING | wxOK);
                dlg.ShowModal();
            }
        }
    }

    if (is_nozzle_combo_selector()) {
        PaHistoryFilter pa_history_filter = obj->GetCalib()->GetPaHistoryFilter();
        int cur_cali_idx = get_cali_index_by_ams_slot(obj, ams_id, slot_id); // calib_idx == -1 is select default
        if (const PACalibResult *iter = pa_history_filter.find_by_cali_idx(cur_cali_idx)) {
            switch_nozzle_combo_to_target(iter->nozzle_volume_type, iter->nozzle_diameter);
        } else if (m_comboBox_nozzle_type) {
            m_comboBox_nozzle_type->SetSelection(-1);
            m_comboBox_nozzle_type->SetValue(wxEmptyString);
        }
    }

    if (obj->GetCalib()->IsVersionInited()) {
        update_pa_profile_items();

        int cali_idx = get_cali_index_by_ams_slot(obj, ams_id, slot_id);
        /* get sel_idx of cali idex in pa_profile_items */
        cali_select_idx = CalibUtils::get_selected_calib_idx(m_pa_profile_items, cali_idx);

        if(from_printer_flag) {
            if (cali_select_idx >= 0) {
                m_comboBox_cali_result->SetSelection(cali_select_idx);
            } else {
                m_comboBox_cali_result->SetSelection(0);
                BOOST_LOG_TRIVIAL(info) << "extrusion_cali_status_error: cannot find pa profile"
                                        << ", ams_id = " << ams_id
                                        << ", slot_id = " << slot_id
                                        << ", cali_idx = " << cali_idx;
            }
        } else {
                cali_select_idx = get_cali_index(m_current_filament_alias.ToStdString());
                m_comboBox_cali_result->SetSelection(cali_select_idx);
        }

        if (cali_select_idx >= 0) {
            m_input_k_val->GetTextCtrl()->SetValue(float_to_string_with_precision(m_pa_profile_items[cali_select_idx].k_value));
            m_input_n_val->GetTextCtrl()->SetValue(float_to_string_with_precision(m_pa_profile_items[cali_select_idx].n_coef));
        }
        else {
            m_input_k_val->GetTextCtrl()->SetValue(float_to_string_with_precision(m_pa_profile_items[0].k_value));
            m_input_n_val->GetTextCtrl()->SetValue(float_to_string_with_precision(m_pa_profile_items[0].n_coef));
        }
        BOOST_LOG_TRIVIAL(info) << "AMSMaterialsSetting::on_select_filament k/n resolved, dev_id="
                                 << BBLCrossTalk::Crosstalk_DevId(obj->get_dev_id())
                                 << ", ams_id=" << ams_id << ", slot_id=" << slot_id
                                 << ", from_printer=" << from_printer_flag
                                 << ", cali_select_idx=" << cali_select_idx
                                 << ", k=" << m_input_k_val->GetTextCtrl()->GetValue().ToStdString()
                                 << ", n=" << m_input_n_val->GetTextCtrl()->GetValue().ToStdString();
    }
    else {
        //m_input_k_val->GetTextCtrl()->SetValue("0.00");
        m_input_k_val->Enable(!ams_filament_id.empty());
        BOOST_LOG_TRIVIAL(info) << "AMSMaterialsSetting::on_select_filament calib version not inited, k input left as-is, dev_id="
                                 << BBLCrossTalk::Crosstalk_DevId(obj->get_dev_id())
                                 << ", ams_id=" << ams_id << ", slot_id=" << slot_id;
    }

    m_pending_spool_id.clear();
    m_pending_from_printer = false;

    if (!m_snap_taken) {
        m_open_spool_id    = m_selected_spool_id;
        m_open_colour      = m_clr_picker->m_colour;
        m_open_filament_id = ams_filament_id;
        m_snap_taken       = true;
    }

    if (m_color_picker_popup) {
        const bool visible  = colour_palette_visible();
        const bool editable = colour_editable();
        m_color_picker_popup->Show(visible);
        m_color_picker_popup->Enable(editable);   // disabled == read-only, palette stays visible
        if (visible) {
            m_color_picker_popup->set_ams_colours(collect_ams_color_items(obj->GetFilaSystem().get()));
            // 兜底: fall back to the default palette when read-only (printing), presets when editable
            if (editable)
                m_color_picker_popup->set_preset_colours(get_preset_color_items(ams_filament_id));
            else
                m_color_picker_popup->set_preset_colours({});
            m_color_picker_popup->set_def_colour(m_clr_picker->m_colour, m_clr_picker->m_cols, m_clr_picker->ctype);
        }
        m_panel_normal->Layout();
        Layout();
        apply_dialog_size();
    }
}

void AMSMaterialsSetting::on_dpi_changed(const wxRect &suggested_rect)
{
    m_input_k_val->GetTextCtrl()->SetSize(wxSize(-1, FromDIP(20)));
    m_clr_picker->msw_rescale();
    m_filament_box->SetMinSize(AMS_MATERIALS_SETTING_COMBOX_WIDTH);
    m_filament_box->SetCornerRadius(FromDIP(4));
    m_filament_arrow->SetBitmap(create_scaled_bitmap("filament_select_arrow", m_filament_box, 10));
    m_button_reset->SetMinSize(AMS_MATERIALS_SETTING_BUTTON_SIZE);
    m_button_reset->SetCornerRadius(FromDIP(12));
    m_button_confirm->SetMinSize(AMS_MATERIALS_SETTING_BUTTON_SIZE);
    m_button_confirm->SetCornerRadius(FromDIP(12));
    m_button_close->SetMinSize(AMS_MATERIALS_SETTING_BUTTON_SIZE);
    m_button_close->SetCornerRadius(FromDIP(12));
    apply_dialog_size();
    this->Refresh();
}

ColorPicker::ColorPicker(wxWindow* parent, wxWindowID id, const wxPoint& pos /*= wxDefaultPosition*/, const wxSize& size /*= wxDefaultSize*/)
{
    wxWindow::Create(parent, id, pos, size);
    SetBackgroundStyle(wxBG_STYLE_PAINT);

    SetSize(wxSize(FromDIP(25), FromDIP(25)));
    SetMinSize(wxSize(FromDIP(25), FromDIP(25)));
    SetMaxSize(wxSize(FromDIP(25), FromDIP(25)));

    Bind(wxEVT_PAINT, &ColorPicker::paintEvent, this);

    m_bitmap_border = create_scaled_bitmap("color_picker_border", nullptr, 25);
    m_bitmap_border_dark = create_scaled_bitmap("color_picker_border_dark", nullptr, 25);
    m_bitmap_transparent_def = ScalableBitmap(this, "transparent_color_picker", 25);
    m_bitmap_transparent = create_scaled_bitmap("transparent_color_picker", nullptr, 25);
}

ColorPicker::~ColorPicker(){}

void ColorPicker::msw_rescale()
{
    m_bitmap_border = create_scaled_bitmap("color_picker_border", nullptr, 25);
    m_bitmap_border_dark = create_scaled_bitmap("color_picker_border_dark", nullptr, 25);
    m_bitmap_transparent_def.msw_rescale();

    Refresh();
}

void ColorPicker::set_color(wxColour col)
{
    if (m_colour != col && col.Alpha() != 0 && col.Alpha() != 255 && col.Alpha() != 254) {
        transparent_changed = true;
    }
    m_colour = col;
    // Keep m_cols in sync so doRender's `m_cols.size() > 1` branch does not
    // paint a stale multi-color pie after switching to a single-color spool.
    m_cols = { col };
    Refresh();
}

void ColorPicker::set_colors(std::vector<wxColour> cols)
{
    m_cols = cols;
    if (!cols.empty())
        m_colour = cols[0];
    Refresh();
}

void ColorPicker::paintEvent(wxPaintEvent& evt)
{
    wxPaintDC dc(this);
    render(dc);
}

void ColorPicker::render(wxDC& dc)
{
#ifdef __WXMSW__
    wxSize     size = GetSize();
    wxMemoryDC memdc;
    wxBitmap   bmp(size.x, size.y);
    memdc.SelectObject(bmp);
    memdc.Blit({ 0, 0 }, size, &dc, { 0, 0 });

    {
        wxGCDC dc2(memdc);
        doRender(dc2);
    }

    memdc.SelectObject(wxNullBitmap);
    dc.DrawBitmap(bmp, 0, 0);
#else
    doRender(dc);
#endif
}

void ColorPicker::doRender(wxDC& dc)
{
    wxSize     size = GetSize();
    dc.SetBackground(wxBrush(GetBackgroundColour()));
    dc.Clear();
    auto alpha = m_colour.Alpha();
    // Keep the outer edge inside the bitmap for even pixel sizes produced by DPI scaling.
    const int outer_radius = (std::min(size.x, size.y) - 1) / 2;
    auto radius = m_show_full ? outer_radius - FromDIP(1) : outer_radius;
    if (m_selected) radius -= FromDIP(1);
    const int diam = std::max(0, 2 * radius);

    auto draw_state = [&]() {
        if (m_selected) {
            dc.SetPen(wxPen(m_colour));
            dc.SetBrush(*wxTRANSPARENT_BRUSH);
            dc.DrawCircle(size.x / 2, size.y / 2, outer_radius);
        }

        if (m_show_full) {
            dc.SetPen(wxPen(wxColour("#6B6B6B")));
            dc.SetBrush(*wxTRANSPARENT_BRUSH);
            dc.DrawCircle(size.x / 2, size.y / 2, radius);
        }

        if (m_is_empty) {
            dc.SetTextForeground(*wxBLACK);
            auto tsize = dc.GetTextExtent("?");
            auto pot = wxPoint((size.x - tsize.x) / 2, (size.y - tsize.y) / 2);
            dc.DrawText("?", pot);
        }

        if (!m_label.empty()) {
            dc.SetFont(::Label::Body_9);
            dc.SetTextForeground(m_colour.GetLuminance() < 0.6 ? *wxWHITE : wxColour(0x26, 0x2E, 0x30));
            auto tsize = dc.GetTextExtent(m_label);
            dc.DrawText(m_label, wxPoint((size.x - tsize.x) / 2, (size.y - tsize.y) / 2));
        }
    };

    if (m_cols.size() > 1) {
        if (ctype == 0) {
            const double center_x = size.x / 2;
            const double center_y = size.y / 2;
            const double draw_radius = radius;
            dc.SetPen(*wxTRANSPARENT_PEN);
            for (int x = 0; x < size.x; ++x) {
                const double dx = x + 0.5 - center_x;
                if (std::abs(dx) > draw_radius + 1.0) continue;

                const double half_height = std::sqrt(std::max(0.0, (draw_radius + 1.0) * (draw_radius + 1.0) - dx * dx));
                const int top = std::max(0, static_cast<int>(std::floor(center_y - half_height)));
                const int bottom = std::min(size.y - 1, static_cast<int>(std::ceil(center_y + half_height)));
                const double pos = size.x > 1 ? static_cast<double>(x) / (size.x - 1) : 0.0;
                const double scaled = pos * (m_cols.size() - 1);
                const size_t idx = std::min(static_cast<size_t>(scaled), m_cols.size() - 2);
                const wxColour col = mix_colour(m_cols[idx], m_cols[idx + 1], scaled - idx);

                for (int y = top; y <= bottom; ++y) {
                    const double dy = y + 0.5 - center_y;
                    const double cover = std::max(0.0, std::min(1.0, draw_radius + 0.5 - std::sqrt(dx * dx + dy * dy)));
                    if (cover <= 0.0) continue;
                    dc.SetBrush(wxBrush(wxColour(col.Red(), col.Green(), col.Blue(),
                        static_cast<unsigned char>(cover * col.Alpha() + 0.5))));
                    dc.DrawRectangle(x, y, 1, 1);
                }
            }
        }
        else {
            float ev_angle = 360.0 / m_cols.size();
            const float overlap = 2.0f;
            wxPoint center(size.x / 2, size.y / 2);
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(m_cols.front()));
            dc.DrawCircle(center.x, center.y, radius);
            for (int i = 0; i < m_cols.size(); i++) {
                dc.SetBrush(m_cols[i]);
                float startAngle = 90.0f + i * ev_angle;
                float endAngle = startAngle + ev_angle;
                dc.DrawEllipticArc(center.x - radius, center.y - radius, 2 * radius, 2 * radius, startAngle - overlap, endAngle + overlap);
            }
        }

        draw_state();
        return;
    }

    if (alpha == 0) {
        if (diam > 0 && m_bitmap_transparent_def_scaled.GetWidth() != diam)
            m_bitmap_transparent_def_scaled = scale_bitmap_to_diameter(m_bitmap_transparent_def.bmp(), diam);
        if (m_bitmap_transparent_def_scaled.IsOk()) {
            int center_x = (size.x - diam) / 2;
            int center_y = (size.y - diam) / 2;
            dc.DrawBitmap(m_bitmap_transparent_def_scaled, center_x, center_y);
        }
    }
    else if (alpha != 254 && alpha != 255) {
        if (diam > 0 && (transparent_changed || m_bitmap_transparent.GetWidth() != diam)) {
            m_bitmap_transparent = create_translucent_circle_bitmap(m_colour, diam, FromDIP(1));
            transparent_changed = false;
        }
        if (m_bitmap_transparent.IsOk()) {
            int center_x = (size.x - diam) / 2;
            int center_y = (size.y - diam) / 2;
            dc.DrawBitmap(m_bitmap_transparent, center_x, center_y);
        }
    }
    else {
        dc.SetPen(wxPen(m_colour));
        dc.SetBrush(wxBrush(m_colour));
        dc.DrawCircle(size.x / 2, size.y / 2, radius);

        if (!m_show_full) {
            bool is_dark_mode = wxGetApp().dark_mode();
            if (!is_dark_mode && is_light_colour_for_border(m_colour)) {
                dc.SetPen(wxPen(wxColour(130, 130, 128), 1, wxPENSTYLE_SOLID));
                dc.SetBrush(*wxTRANSPARENT_BRUSH);
                dc.DrawCircle(size.x / 2, size.y / 2, radius);
            } else if (is_dark_mode && is_dark_colour_for_border(m_colour)) {
                dc.SetPen(wxPen(wxColour(207, 207, 207), 1, wxPENSTYLE_SOLID));
                dc.SetBrush(*wxTRANSPARENT_BRUSH);
                dc.DrawCircle(size.x / 2, size.y / 2, radius);
            }
        }
    }

    draw_state();
}

ColorPickerPopup::ColorPickerPopup(wxWindow* parent, wxWindow* evt_target)
    :wxPanel(parent, wxID_ANY)
    , m_evt_target(evt_target ? evt_target : parent)
{
    m_def_colors.clear();
    m_def_colors.push_back(wxColour("#FFFFFF"));
    m_def_colors.push_back(wxColour("#fff144"));
    m_def_colors.push_back(wxColour("#DCF478"));
    m_def_colors.push_back(wxColour("#0ACC38"));
    m_def_colors.push_back(wxColour("#057748"));
    m_def_colors.push_back(wxColour("#0d6284"));
    m_def_colors.push_back(wxColour("#0EE2A0"));
    m_def_colors.push_back(wxColour("#76D9F4"));
    m_def_colors.push_back(wxColour("#46a8f9"));
    m_def_colors.push_back(wxColour("#2850E0"));
    m_def_colors.push_back(wxColour("#443089"));
    m_def_colors.push_back(wxColour("#A03CF7"));
    m_def_colors.push_back(wxColour("#F330F9"));
    m_def_colors.push_back(wxColour("#D4B1DD"));
    m_def_colors.push_back(wxColour("#f95d73"));
    m_def_colors.push_back(wxColour("#f72323"));
    m_def_colors.push_back(wxColour("#7c4b00"));
    m_def_colors.push_back(wxColour("#f98c36"));
    m_def_colors.push_back(wxColour("#fcecd6"));
    m_def_colors.push_back(wxColour("#D3C5A3"));
    m_def_colors.push_back(wxColour("#AF7933"));
    m_def_colors.push_back(wxColour("#898989"));
    m_def_colors.push_back(wxColour("#BCBCBC"));
    m_def_colors.push_back(wxColour("#161616"));


    SetBackgroundColour(wxColour(*wxWHITE));

    wxBoxSizer* m_sizer_main = new wxBoxSizer(wxVERTICAL);
    wxBoxSizer* m_sizer_box = new wxBoxSizer(wxVERTICAL);

    m_def_color_box = new StaticBox(this);
    m_def_color_box->SetCornerRadius(FromDIP(10));
    m_def_color_box->SetBackgroundColor(StateColor(std::pair<wxColour, int>(wxColour(248, 248, 248), StateColor::Normal)));
    m_def_color_box->SetBorderColor(StateColor(std::pair<wxColour, int>(wxColour(248, 248, 248), StateColor::Normal)));
    m_def_color_box->SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));

    // single continuous colour grid: AMS -> other/preset -> "+" custom entry
    m_color_fg_sizer = new wxFlexGridSizer(0, 10, 0, 0);
    m_color_fg_sizer->SetFlexibleDirection(wxBOTH);
    m_color_fg_sizer->SetNonFlexibleGrowMode(wxFLEX_GROWMODE_SPECIFIED);

    for (wxColour col : m_def_colors) {
        auto cp = new ColorPicker(m_def_color_box, wxID_ANY, wxDefaultPosition, wxDefaultSize);
        cp->set_color(col);
        cp->set_colors({ col });
        cp->set_selected(false);
        cp->SetBackgroundColour(StateColor::darkModeColorFor(wxColour(248,248,248)));
        m_color_pickers.push_back(cp);
        m_default_color_pickers.push_back(cp);
        cp->Bind(wxEVT_LEFT_DOWN, [this, cp](auto& e) {
            set_def_colour(cp->m_colour, cp->m_cols, cp->ctype);

            wxCommandEvent evt(EVT_SELECTED_COLOR);
            unsigned long g_col = ((cp->m_colour.Red() & 0xff) << 24) + ((cp->m_colour.Green() & 0xff) << 16) + ((cp->m_colour.Blue() & 0xff) << 8) + (cp->m_colour.Alpha() & 0xff);
            evt.SetInt(g_col);
            wxPostEvent(m_evt_target, evt);
        });
    }

    // custom colour entry rendered as a trailing "+" tile blended into the box
    m_custom_cp =  new StaticBox(m_def_color_box);
    m_custom_cp->SetSize(FromDIP(25), FromDIP(25));
    m_custom_cp->SetMinSize(wxSize(FromDIP(25), FromDIP(25)));
    m_custom_cp->SetMaxSize(wxSize(FromDIP(25), FromDIP(25)));
    m_custom_cp->SetBackgroundColor(StateColor(std::pair<wxColour, int>(wxColour(248, 248, 248), StateColor::Normal)));
    m_custom_cp->SetBorderColor(StateColor(std::pair<wxColour, int>(wxColour(248, 248, 248), StateColor::Normal)));
    m_custom_cp->Bind(wxEVT_LEFT_DOWN, &ColorPickerPopup::on_custom_clr_picker, this);
    m_custom_cp->Bind(wxEVT_ENTER_WINDOW, [this](auto& e) {
        SetCursor(wxCURSOR_HAND);
    });
    m_custom_cp->Bind(wxEVT_LEAVE_WINDOW, [this](auto& e) {
        SetCursor(wxCURSOR_ARROW);
    });

    m_custom_plus = new wxStaticBitmap(m_custom_cp, wxID_ANY,
        ScalableBitmap(m_custom_cp, "color_picker_add", 14).bmp());
    m_custom_plus->Bind(wxEVT_LEFT_DOWN, &ColorPickerPopup::on_custom_clr_picker, this);
    m_custom_plus->Bind(wxEVT_ENTER_WINDOW, [this](auto& e) {
        SetCursor(wxCURSOR_HAND);
        });
    m_custom_plus->Bind(wxEVT_LEAVE_WINDOW, [this](auto& e) {
        SetCursor(wxCURSOR_ARROW);
        });

    auto sizer_custom = new wxBoxSizer(wxVERTICAL);
    m_custom_cp->SetSizer(sizer_custom);
    sizer_custom->AddStretchSpacer();
    sizer_custom->Add(m_custom_plus, 0, wxALIGN_CENTER_HORIZONTAL, 0);
    sizer_custom->AddStretchSpacer();
    m_custom_cp->Layout();

    m_clrData = new wxColourData();
    m_clrData->SetChooseFull(true);
    m_clrData->SetChooseAlpha(false);


    m_sizer_box->Add(0, 0, 0, wxTOP, FromDIP(10));
    m_sizer_box->Add(m_color_fg_sizer, 0, wxEXPAND|wxLEFT|wxRIGHT, FromDIP(10));
    m_sizer_box->Add(0, 0, 0, wxTOP, FromDIP(10));


    m_def_color_box->SetSizer(m_sizer_box);
    m_def_color_box->Layout();
    m_def_color_box->Fit();

    m_sizer_main->Add(m_def_color_box, 0, wxALL | wxEXPAND, 0);
    SetSizer(m_sizer_main);

    relayout_colours();

    Layout();
    Fit();

    wxGetApp().UpdateDarkUIWin(this);
}

void ColorPickerPopup::on_custom_clr_picker(wxMouseEvent& event)
{
    std::vector<std::string> colors = wxGetApp().app_config->get_custom_color_from_config();
    for (int i = 0; i < colors.size(); i++) {
        m_clrData->SetCustomColour(i, string_to_wxColor(colors[i]));
    }
    auto clr_dialog = new wxColourDialog(nullptr, m_clrData);
    wxColour picker_color;

    if (clr_dialog->ShowModal() == wxID_OK) {
        m_clrData = &(clr_dialog->GetColourData());
        if (colors.size() != CUSTOM_COLOR_COUNT) {
            colors.resize(CUSTOM_COLOR_COUNT);
        }
        for (int i = 0; i < CUSTOM_COLOR_COUNT; i++) {
            colors[i] = color_to_string(m_clrData->GetCustomColour(i));
        }
        wxGetApp().app_config->save_custom_color_to_config(colors);

        picker_color = wxColour(
            m_clrData->GetColour().Red(),
            m_clrData->GetColour().Green(),
            m_clrData->GetColour().Blue(),
            255
        );

        set_def_colour(picker_color, { picker_color }, 2);
        wxCommandEvent evt(EVT_SELECTED_COLOR);
        unsigned long g_col = ((picker_color.Red() & 0xff) << 24) + ((picker_color.Green() & 0xff) << 16) + ((picker_color.Blue() & 0xff) << 8) + (picker_color.Alpha() & 0xff);
        evt.SetInt(g_col);
        wxPostEvent(m_evt_target, evt);
    }
}

void ColorPickerPopup::relayout_colours()
{
    m_color_fg_sizer->Clear(false); // detach items, keep the windows alive

    const bool use_presets = !m_preset_color_pickers.empty();
    for (ColorPicker* cp : m_default_color_pickers) cp->Show(!use_presets);
    for (ColorPicker* cp : m_preset_color_pickers)  cp->Show(true);

    for (ColorPicker* cp : m_ams_color_pickers)
        m_color_fg_sizer->Add(cp, 0, wxALL, FromDIP(3));
    for (ColorPicker* cp : (use_presets ? m_preset_color_pickers : m_default_color_pickers))
        m_color_fg_sizer->Add(cp, 0, wxALL, FromDIP(3));
    m_color_fg_sizer->Add(m_custom_cp, 0, wxALL, FromDIP(3));

    m_def_color_box->Layout();
    m_def_color_box->Fit();
    m_def_color_box->Refresh();
    Layout();
    Fit();
}

void ColorPickerPopup::set_ams_colours(const std::vector<ColorItem>& ams)
{
    if (m_ams_color_pickers.size() > 0) {
        for (ColorPicker* col_pick:m_ams_color_pickers) {

            std::vector<ColorPicker*>::iterator iter = find(m_color_pickers.begin(), m_color_pickers.end(), col_pick);
            if (iter != m_color_pickers.end()) {
                col_pick->Destroy();
                m_color_pickers.erase(iter);
            }
        }

        m_ams_color_pickers.clear();
    }


    m_ams_color_items = ams;
    for (const ColorItem& item : m_ams_color_items) {
        if (item.colors.empty()) continue;

        auto cp = new ColorPicker(m_def_color_box, wxID_ANY, wxDefaultPosition, wxDefaultSize);
        cp->set_color(item.colors.front());
        cp->set_colors(item.colors);
        cp->ctype = item.colors.size() > 1 ? item.ctype : 2;
        cp->set_selected(false);
        cp->set_label("AMS");
        cp->SetBackgroundColour(StateColor::darkModeColorFor(wxColour(248,248,248)));
        m_color_pickers.push_back(cp);
        m_ams_color_pickers.push_back(cp);
        cp->Bind(wxEVT_LEFT_DOWN, [this, cp](auto& e) {
            set_def_colour(cp->m_colour, cp->m_cols, cp->ctype);

            wxCommandEvent evt(EVT_SELECTED_COLOR);
            unsigned long g_col = ((cp->m_colour.Red() & 0xff) << 24) + ((cp->m_colour.Green() & 0xff) << 16) + ((cp->m_colour.Blue() & 0xff) << 8) + (cp->m_colour.Alpha() & 0xff);
            evt.SetInt(g_col);
            wxPostEvent(m_evt_target, evt);
        });
    }
    relayout_colours();
}

void ColorPickerPopup::set_preset_colours(const std::vector<ColorItem>& preset_colors)
{
    if (!m_preset_color_pickers.empty()) {
        for (ColorPicker* col_pick : m_preset_color_pickers) {
            auto iter = std::find(m_color_pickers.begin(), m_color_pickers.end(), col_pick);
            if (iter != m_color_pickers.end()) {
                col_pick->Destroy();
                m_color_pickers.erase(iter);
            }
        }
        m_preset_color_pickers.clear();
    }

    for (const ColorItem& item : preset_colors) {
        if (item.colors.empty()) continue;

        auto cp = new ColorPicker(m_def_color_box, wxID_ANY, wxDefaultPosition, wxDefaultSize);
        cp->set_color(item.colors.front());
        cp->set_colors(item.colors);
        cp->ctype = item.colors.size() > 1 ? item.ctype : 2;
        cp->set_selected(false);
        cp->SetBackgroundColour(StateColor::darkModeColorFor(wxColour(248,248,248)));
        if (!item.name.empty()) {
            cp->SetToolTip(item.name);
        }

        m_color_pickers.push_back(cp);
        m_preset_color_pickers.push_back(cp);
        cp->Bind(wxEVT_LEFT_DOWN, [this, cp](auto& e) {
            set_def_colour(cp->m_colour, cp->m_cols, cp->ctype);

            wxCommandEvent evt(EVT_SELECTED_COLOR);
            unsigned long g_col = ((cp->m_colour.Red() & 0xff) << 24) + ((cp->m_colour.Green() & 0xff) << 16) + ((cp->m_colour.Blue() & 0xff) << 8) + (cp->m_colour.Alpha() & 0xff);
            evt.SetInt(g_col);
            wxPostEvent(m_evt_target, evt);
        });
    }

    relayout_colours();
}

void ColorPickerPopup::set_def_colour(wxColour col, std::vector<wxColour> cols, int ctype)
{
    m_def_col = col;
    if (cols.empty()) {
        cols.push_back(col);
    }
    m_def_cols = cols;
    m_def_ctype = m_def_cols.size() > 1 ? ctype : 2;

    for (ColorPicker* cp : m_color_pickers) {
        if (cp->m_selected) {
            cp->set_selected(false);
        }
    }

    for (ColorPicker* cp : m_color_pickers) {
        if (!cp->IsShown()) continue;
        if (cp->ctype == m_def_ctype && cp->m_cols == m_def_cols) {
            cp->set_selected(true);
            break;
        }
    }
}

}} // namespace Slic3r::GUI
