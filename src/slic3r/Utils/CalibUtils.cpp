#include "CalibUtils.hpp"
#include "../GUI/I18N.hpp"
#include "../GUI/GUI_App.hpp"
#include "../GUI/DeviceManager.hpp"
#include "../GUI/Jobs/ProgressIndicator.hpp"
#include "../GUI/PartPlate.hpp"
#include "../GUI/GLCanvas3D.hpp"
#include "../GUI/OpenGLManager.hpp"

#include "libslic3r/Model.hpp"
#include "../GUI/MsgDialog.hpp"

#include "../GUI/DeviceCore/DevConfig.h"
#include "../GUI/DeviceCore/DevExtruderSystem.h"
#include "../GUI/DeviceCore/DevManager.h"
#include "../GUI/DeviceCore/DevStorage.h"
#include "../GUI/DeviceCore/DevConfigUtil.h"
#include "libslic3r/FlushVolCalc.hpp"
#include "BBLUtil.hpp"
#include "../GUI/Plater.hpp"
#include "../GUI/DeviceCore/DevNozzleSystem.h"
#include "../GUI/DeviceCore/DevNozzleRack.h"
#include "../GUI/DeviceCore/DevFilaSystem.h"

namespace Slic3r {
namespace GUI {
const double MIN_PA_K_VALUE = 0.0;
const double MAX_PA_K_VALUE = 2.0;

// print_bot: the calibration-wizard print paths (calib_* model generators, PA/flow
// auto-cali MQTT drivers, pre-cali checks, send_to_print) were removed with the
// Calibration tab. Only helpers still used by Device/Monitor/AMS/Tab remain.

void get_default_k_n_value(const std::string &filament_id, float &k, float &n)
{
    if (filament_id.compare("GFU01") == 0) {
        /* TPU 95A */
        k = 0.25;
        n = 1.0;
    } else if (filament_id.compare("GFU03") == 0) {
        /* TPU 90A */
        k = 0.35;
        n = 1.0;
    } else if (filament_id.compare("GFU04") == 0) {
        /* TPU 85A */
        k = 0.65;
        n = 1.0;
    } else if (filament_id.compare("GFG00") == 0 || filament_id.compare("GFG01") == 0 || filament_id.compare("GFG60") == 0 || filament_id.compare("GFL06") == 0 ||
               filament_id.compare("GFL55") == 0 || filament_id.compare("GFG99") == 0 || filament_id.compare("GFG98") == 0 || filament_id.compare("GFG97") == 0 ||
               filament_id.compare("GFG50") == 0 || filament_id.compare("GFU02") == 0 || filament_id.compare("GFU98") == 0 || filament_id.compare("GFS00") == 0 ||
               filament_id.compare("GFS02") == 0) {
        /* 0.04 filaments */
        k = 0.04;
        n = 1.0;
    } else {
        /* other */
        k = 0.02;
        n = 1.0;
    }
}

wxString get_nozzle_volume_type_name(NozzleVolumeType type)
{
    if (NozzleVolumeType::nvtStandard == type) {
        return _L("Standard");
    } else if (NozzleVolumeType::nvtHighFlow == type) {
        return _L("High Flow");
    } else if (NozzleVolumeType::nvtHybrid == type) {
        return _L("Hybrid");
    } else if (NozzleVolumeType::nvtTPUHighFlow == type) {
        return _L("TPU High Flow");
    } else if (NozzleVolumeType::nvtE3DHighFlow == type) {
        return _L("E3D High Flow");
    }
    return wxString();
}

std::map<int, DynamicPrintConfig> build_filament_ams_list(MachineObject* obj)
{
    std::map<int, DynamicPrintConfig> filament_ams_list;
    if (!obj) return filament_ams_list;

    auto build_tray_config = [](DevAmsTray const &tray, std::string const &name, std::string ams_id, std::string slot_id) {
        BOOST_LOG_TRIVIAL(info) << boost::format("build_filament_ams_list: name %1% setting_id %2% type %3% color %4%")
                    % name % tray.setting_id % tray.m_fila_type % tray.color;
        DynamicPrintConfig tray_config;
        tray_config.set_key_value("filament_id", new ConfigOptionStrings{tray.setting_id});
        tray_config.set_key_value("tag_uid", new ConfigOptionStrings{tray.tag_uid});
        tray_config.set_key_value("ams_id", new ConfigOptionStrings{ams_id});
        tray_config.set_key_value("slot_id", new ConfigOptionStrings{slot_id});
        tray_config.set_key_value("filament_type", new ConfigOptionStrings{tray.m_fila_type});
        tray_config.set_key_value("tray_name", new ConfigOptionStrings{ name });
        tray_config.set_key_value("filament_colour", new ConfigOptionStrings{into_u8(wxColour("#" + tray.color).GetAsString(wxC2S_HTML_SYNTAX))});
        tray_config.set_key_value("filament_multi_colour", new ConfigOptionStrings{});
        tray_config.set_key_value("filament_colour_type", new ConfigOptionStrings{std::to_string(tray.ctype)});
        tray_config.set_key_value("filament_exist", new ConfigOptionBools{tray.is_exists});
        std::optional<FilamentBaseInfo> info;
        if (wxGetApp().preset_bundle) {
            info = wxGetApp().preset_bundle->get_filament_by_filament_id(tray.setting_id);
        }
        tray_config.set_key_value("filament_is_support", new ConfigOptionBools{ info.has_value() ? info->is_support : false});
        for (int i = 0; i < tray.cols.size(); ++i) {
            tray_config.opt<ConfigOptionStrings>("filament_multi_colour")->values.push_back(into_u8(wxColour("#" + tray.cols[i]).GetAsString(wxC2S_HTML_SYNTAX)));
        }
        return tray_config;
    };

    if (obj->ams_support_virtual_tray) {
        int extruder = 0x10000; // Main (first) extruder at right
        for (auto & vt_tray : obj->vt_slot) {
            filament_ams_list.emplace(extruder + stoi(vt_tray.id), build_tray_config(vt_tray, "Ext",vt_tray.id, "0"));//254 or 255
            extruder = 0;
        }
    }

    auto get_ams_name = [](int ams_id, int slot_id)->std::string {
        if (ams_id >= 0 && ams_id < 64) {
            char slot_name = slot_id + '1';
            return std::string(1, 'A' + ams_id) + std::string(1, slot_name);
        } else if (ams_id >= 128 && ams_id < 153) {
            return "HT-" + std::string(1, 'A' + (ams_id - 128));
        } else {
            assert(false);
        }
        return std::string();
    };

    auto list = obj->GetFilaSystem()->GetAmsList();

    for (const auto& ams : list) {
        for (auto extruder_id : ams.second->GetBindedExtruderSet()) {
            int extruder = extruder_id ? 0 : 0x10000; // Main (first) extruder at right
            for (auto tray : ams.second->GetTrays()) {
                int ams_id = -1;
                int slot_id = -1;
                int tray_id = -1;
                try {
                    ams_id  = std::stoi(ams.first);
                    slot_id = std::stoi(tray.first);
                    tray_id = obj->GetFilaSystem()->GetTrayIdByAmsSlotId(ams_id, slot_id);

                    filament_ams_list.emplace(extruder + tray_id, build_tray_config(*tray.second, get_ams_name(ams_id, slot_id), std::to_string(ams_id), std::to_string(slot_id)));
                } catch(...) {
                    BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << "invalid ams_id:"<< ams.first << " or slot_id:" << tray.first;
                }
            }
        }
    }
    return filament_ams_list;
}



CalibMode CalibUtils::get_calib_mode_by_name(const std::string name, int& cali_stage)
{
    if (name == "pa_line_calib_mode") {
        cali_stage = 0;
        return CalibMode::Calib_PA_Line;
    }
    else if (name == "pa_pattern_calib_mode") {
        cali_stage = 1;
        return CalibMode::Calib_PA_Line;
    }
    else if (name == "auto_pa_line_calib_mode") {
        cali_stage = 2;
        return CalibMode::Calib_PA_Line;
    }
    else if (name == "flow_rate_coarse_calib_mode") {
        cali_stage = 1;
        return CalibMode::Calib_Flow_Rate;
    }
    else if (name == "flow_rate_fine_calib_mode") {
        cali_stage = 2;
        return CalibMode::Calib_Flow_Rate;
    }
    else if (name == "temp_tower_calib_mode")
        return CalibMode::Calib_Temp_Tower;
    else if (name == "vol_speed_tower_calib_mode")
        return CalibMode::Calib_Vol_speed_Tower;
    else if (name == "vfa_tower_calib_mode")
        return CalibMode::Calib_VFA_Tower;
    else if (name == "retration_tower_calib_mode")
        return CalibMode::Calib_Retraction_tower;
    return CalibMode::Calib_None;
}

void CalibUtils::select_PA_calib_result(const PACalibIndexInfo& pa_calib_info)
{
    DeviceManager* dev = Slic3r::GUI::wxGetApp().getDeviceManager();
    if (!dev)
        return;

    MachineObject* obj_ = dev->get_selected_machine();
    if (obj_ == nullptr)
        return;

    obj_->commnad_select_pa_calibration(pa_calib_info);
}

std::vector<std::string> CalibUtils::get_supported_nozzle_diameters_by_model(const MachineObject *obj)
{
    std::vector<std::string> result;
    if (!obj) return result;

    std::string printer_model = DevPrinterConfigUtil::get_printer_display_name(obj->printer_type);

    PresetBundle *preset_bundle = wxGetApp().preset_bundle;
    if (!preset_bundle || printer_model.empty())
        return result;

    std::vector<const Preset *> machine_presets = preset_bundle->printers.find_all_presets_by_model(printer_model, true);

    std::set<std::string> nozzle_diameters_set;
    for (const Preset *preset : machine_presets) {
        std::string variant = preset->config.opt_string("printer_variant");
        if (!variant.empty())
            nozzle_diameters_set.insert(variant);
    }

    result.assign(nozzle_diameters_set.begin(), nozzle_diameters_set.end());
    return result;
}

std::vector<NozzleVolumeType> CalibUtils::get_supported_nozzle_volume_types_by_model_and_nozzle(const MachineObject *obj, const std::string &nozzle_diameter)
{
    std::vector<NozzleVolumeType> result;
    if (!obj) return result;

    std::string model_id = DevPrinterConfigUtil::get_printer_display_name(obj->printer_type);

    PresetBundle *preset_bundle = wxGetApp().preset_bundle;
    if (!preset_bundle || model_id.empty())
        return result;

    // Find the system preset by model and nozzle diameter variant
    const Preset *matched_preset = preset_bundle->printers.find_system_preset_by_model_and_variant(model_id, nozzle_diameter);
    if (!matched_preset)
        return result;

    // Get supported NozzleVolumeTypes from printer_extruder_variant
    auto variants = matched_preset->config.option<ConfigOptionStrings>("printer_extruder_variant");
    if (!variants)
        return result;

    std::set<NozzleVolumeType> unique_types;
    for (const std::string &v : variants->values) {
        NozzleVolumeType nvt = convert_to_nvt_type(v);
        unique_types.insert(nvt);
    }

    return std::vector<NozzleVolumeType>(unique_types.begin(), unique_types.end());
}

std::map<NozzleVolumeType, std::set<NozzleDiameterType>> CalibUtils::get_supported_nozzle_volume_and_diameters(const MachineObject *obj, bool with_hybrid)
{
    std::map<NozzleVolumeType, std::set<NozzleDiameterType>> volume_diameters_map;
    if (!obj) return volume_diameters_map;

    std::string printer_model = DevPrinterConfigUtil::get_printer_display_name(obj->printer_type);

    std::vector<std::string> diameters = get_supported_nozzle_diameters_by_model(obj);

    for (auto& diameter : diameters)
    {
        std::vector<NozzleVolumeType> volumes = get_supported_nozzle_volume_types_by_model_and_nozzle(obj, diameter);
        for (auto& volume : volumes)
        {
            // high flow type does not support 0.2mm nozzle
            if (diameter == "0.2") {
                if (volume == NozzleVolumeType::nvtHighFlow) continue;
                if (volume == NozzleVolumeType::nvtTPUHighFlow) continue;
            }

            volume_diameters_map[volume].insert(DevNozzle::ToNozzleDiameterType(diameter));
        }
    }
    // O1C support hybrid
    if (obj->GetNozzleRack()->IsSupported()) {
        std::set<NozzleDiameterType> hybrid_diameters_set;
        for (const auto& [volume, diameters] : volume_diameters_map) {
            hybrid_diameters_set.insert(diameters.begin(), diameters.end());
        }
        if (with_hybrid) {
            volume_diameters_map[NozzleVolumeType::nvtHybrid] = hybrid_diameters_set;
        }
    }

    return volume_diameters_map;
}

int CalibUtils::get_selected_calib_idx(const std::vector<PACalibResult> &pa_calib_values, int cali_idx) {
    for (int i = 0; i < pa_calib_values.size(); ++i) {
        if(pa_calib_values[i].cali_idx == cali_idx)
            return i;
    }
    return -1;
}

bool CalibUtils::get_pa_k_n_value_by_cali_idx(const MachineObject *obj, int cali_idx, float &out_k, float &out_n) {
    if (!obj)
        return false;

    PaHistoryFilter pa_history_filter = obj->GetCalib()->GetPaHistoryFilter();
    if (const PACalibResult *info = pa_history_filter.find_by_cali_idx(cali_idx)) {
        out_k = info->k_value;
        out_n = info->n_coef;
        return true;
    }
    return false;
}

CalibMode get_obj_calibration_mode(const MachineObject* obj)
{
    CalibrationMethod method;
    int cali_stage;
    return get_obj_calibration_mode(obj, method, cali_stage);
}

CalibMode get_obj_calibration_mode(const MachineObject* obj, int& cali_stage)
{
    CalibrationMethod method;
    return get_obj_calibration_mode(obj, method, cali_stage);
}

CalibMode get_obj_calibration_mode(const MachineObject* obj, CalibrationMethod& method, int& cali_stage)
{
    method = CalibrationMethod::CALI_METHOD_MANUAL;

    if (!obj) return CalibMode::Calib_None;

    if (boost::contains(obj->m_gcode_file, "auto_filament_cali")) {
        method = CalibrationMethod::CALI_METHOD_AUTO;
        return CalibMode::Calib_PA_Line;
    }
    if (boost::contains(obj->m_gcode_file, "user_cali_manual_pa")) {
        method = CalibrationMethod::CALI_METHOD_MANUAL;
        return CalibMode::Calib_PA_Line;
    }
    if (boost::contains(obj->m_gcode_file, "extrusion_cali")) {
        method = CalibrationMethod::CALI_METHOD_MANUAL;
        return CalibMode::Calib_PA_Line;
    }

    if (boost::contains(obj->m_gcode_file, "abs_flowcalib_cali")) {
        method = CalibrationMethod::CALI_METHOD_AUTO;
        return CalibMode::Calib_Flow_Rate;
    }

    if (obj->get_printer_series() == PrinterSeries::SERIES_P1P) {
        if (boost::contains(obj->subtask_name, "auto_filament_cali")) {
            method = CalibrationMethod::CALI_METHOD_AUTO;
            return CalibMode::Calib_PA_Line;
        }
        if (boost::contains(obj->subtask_name, "user_cali_manual_pa")) {
            method = CalibrationMethod::CALI_METHOD_MANUAL;
            return CalibMode::Calib_PA_Line;
        }
        if (boost::contains(obj->subtask_name, "extrusion_cali")) {
            method = CalibrationMethod::CALI_METHOD_MANUAL;
            return CalibMode::Calib_PA_Line;
        }

        if (boost::contains(obj->subtask_name, "abs_flowcalib_cali")) {
            method = CalibrationMethod::CALI_METHOD_AUTO;
            return CalibMode::Calib_Flow_Rate;
        }
    }

    CalibMode cali_mode = CalibUtils::get_calib_mode_by_name(obj->subtask_name, cali_stage);
    if (cali_mode == CalibMode::Calib_PA_Line && cali_stage == 2) {
        method = CalibrationMethod::CALI_METHOD_NEW_AUTO;
    }
    else if (cali_mode != CalibMode::Calib_None) {
        method = CalibrationMethod::CALI_METHOD_MANUAL;
    }
    return cali_mode;
}

}
}
