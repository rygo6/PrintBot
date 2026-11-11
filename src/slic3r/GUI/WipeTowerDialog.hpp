#ifndef _WIPE_TOWER_DIALOG_H_
#define _WIPE_TOWER_DIALOG_H_

#include <wx/dialog.h>
#include "libslic3r/PrintConfig.hpp"

class wxChoice;
class wxGrid;
class wxGridEvent;
class wxStaticText;
class wxTextCtrl;

bool is_flush_config_modified();
void open_flushing_dialog(wxEvtHandler *parent, const wxEvent &event);

// print_bot: native wx "Purging volumes" dialog (was an HTML page around
// resources/web/flush/WipingDialog.html). Same data flow and behaviour:
// one matrix + multiplier per nozzle/extruder (H2D etc.), auto re-calculate,
// values stored to project_config by open_flushing_dialog().
class WipingDialog : public wxDialog
{
public:
	using VolumeMatrix = std::vector<std::vector<double>>;

	WipingDialog(wxWindow* parent, const int max_flush_volume = Slic3r::g_max_flush_volume);
	static VolumeMatrix CalcFlushingVolumes(int extruder_id);
	std::vector<double> GetFlattenMatrix()const;
	std::vector<double> GetMultipliers()const;
	bool GetSubmitFlag() const { return m_submit_flag; }

private:
	static int CalcFlushingVolume(const wxColour& from_, const wxColour& to_, int min_flush_volume, int nozzle_flush_dataset, const std::string& from_filament_id = std::string(), const std::string& to_filament_id = std::string());
	void LoadData();
	void BuildUi();
	void RefreshTable();          // display values from m_edit_matrixs[m_cur] and multiplier
	void UpdateWarningTexts();
	void UpdateRangeTexts();
	void OnCellChanged(wxGridEvent& evt);
	void OnMultiplierText();
	void OnRecalculate();
	void OnExtruderSelect(int idx);
	void OnSave();
	int  DisplayValue(double raw) const;
	int  LimitDisplay(double val) const;
	void StoreFlushData(int extruder_num, const std::vector<std::vector<double>>& flush_volume_vecs, const std::vector<double>& flush_multipliers);

	int m_max_flush_volume;

	// Data (same meaning as the old JS state).
	int                              m_extruder_num{1};
	std::vector<std::string>         m_colors;          // physical filament colours
	std::vector<std::vector<double>> m_edit_matrixs;    // per extruder, physical p*p ("raw", before multiplier)
	std::vector<std::vector<double>> m_default_matrixs; // per extruder, auto-calculated p*p
	std::vector<double>              m_edit_multipliers;
	std::vector<int>                 m_min_flush_volumes;
	std::vector<int>                 m_max_flush_volumes;
	int                              m_cur{0};

	// UI
	wxGrid*       m_grid{nullptr};
	wxChoice*     m_extruder_choice{nullptr};
	wxTextCtrl*   m_multiplier_input{nullptr};
	wxStaticText* m_volume_range{nullptr};
	wxStaticText* m_multiplier_range{nullptr};
	wxColour      m_text_normal, m_text_warn, m_text_error;

	// Results (full-size matrices per extruder, as stored in the config).
	VolumeMatrix m_raw_matrixs;
	std::vector<double> m_flush_multipliers;
	std::vector<size_t> m_physical_indices;
	bool m_submit_flag{ false };
};

#endif  // _WIPE_TOWER_DIALOG_H_
