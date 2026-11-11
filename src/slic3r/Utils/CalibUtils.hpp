#pragma once
#include "libslic3r/Calib.hpp"
#include "../GUI/DeviceManager.hpp"
#include "../GUI/Jobs/PrintJob.hpp"

namespace Slic3r {

class ProgressIndicator;
class Preset;

namespace GUI {
extern const double MIN_PA_K_VALUE;
extern const double MAX_PA_K_VALUE;

// print_bot: trimmed with the Calibration tab removal; only helpers still used by
// Device/Monitor/AMS/Tab code are kept.
enum CalibrationMethod {
    CALI_METHOD_MANUAL = 0,
    CALI_METHOD_AUTO,
    CALI_METHOD_NEW_AUTO,
    CALI_METHOD_NONE,
};

// Detect which calibration (if any) the printer is currently running.
CalibMode get_obj_calibration_mode(const MachineObject* obj);
CalibMode get_obj_calibration_mode(const MachineObject* obj, int& cali_stage);
CalibMode get_obj_calibration_mode(const MachineObject* obj, CalibrationMethod& method, int& cali_stage);

class CalibUtils
{
public:
    CalibUtils(){};

    static CalibMode get_calib_mode_by_name(const std::string name, int &cali_stage);

    static void select_PA_calib_result(const PACalibIndexInfo &pa_calib_info);

    // Get all supported nozzle diameters for a given printer model
    static std::vector<std::string> get_supported_nozzle_diameters_by_model(const MachineObject *obj);

    // Get all supported NozzleVolumeTypes for a given printer model and nozzle diameter
    static std::vector<NozzleVolumeType> get_supported_nozzle_volume_types_by_model_and_nozzle(const MachineObject *obj, const std::string &nozzle_diameter);

    // key:supported volume type; value: volume type supported diameter
    static std::map<NozzleVolumeType, std::set<NozzleDiameterType>> get_supported_nozzle_volume_and_diameters(const MachineObject *obj, bool with_hybrid=true);

    static int get_selected_calib_idx(const std::vector<PACalibResult> &pa_calib_values, int cali_idx);
    static bool get_pa_k_n_value_by_cali_idx(const MachineObject* obj, int cali_idx, float& out_k, float& out_n);
};

extern std::map<int, DynamicPrintConfig> build_filament_ams_list(MachineObject* obj);


extern void get_default_k_n_value(const std::string &filament_id, float &k, float &n);
extern wxString get_nozzle_volume_type_name(NozzleVolumeType type);


}
}