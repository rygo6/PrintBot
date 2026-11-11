#include "PresetUpdater.hpp"

#include <algorithm>
#include <atomic>
#include <thread>
#include <unordered_map>
#include <ostream>
#include <utility>
#include <chrono>
#include <vector>
#include <stdexcept>
#include <boost/format.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/log/trivial.hpp>
#include <tbb/parallel_for.h>

#include <wx/app.h>
#include <wx/msgdlg.h>

#include "libslic3r/libslic3r.h"
#include "libslic3r/format.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/EncodedFilament.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/UpdateDialogs.hpp"
#include "slic3r/GUI/ConfigWizard.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/Config/Version.hpp"
#include "libslic3r/miniz_extension.hpp"
#include "slic3r/GUI/GUI_Utils.hpp"

namespace fs = boost::filesystem;
using Slic3r::GUI::Config::Index;
using Slic3r::GUI::Config::Version;


// FIXME: Incompat bundle resolution doesn't deal with inherited user presets

namespace Slic3r {


static const char *INDEX_FILENAME = "index.idx";
static const char *TMP_EXTENSION = ".data";
static const char *PRESET_SUBPATH = "presets";
static const char *PLUGINS_SUBPATH = "plugins";

long long getTimestamp()
{
    auto now          = std::chrono::system_clock::now();
    auto duration     = now.time_since_epoch();
    auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
    return milliseconds;
}

int copy_file_fix(const fs::path &source, const fs::path &target, std::string& error_message)
{
    BOOST_LOG_TRIVIAL(debug) << format("PresetUpdater: Copying %1% -> %2%", PathSanitizer::sanitize(source), PathSanitizer::sanitize(target));

	//CopyFileResult cfr = Slic3r::GUI::copy_file_gui(source.string(), target.string(), error_message, false);
	CopyFileResult cfr = copy_file(source.string(), target.string(), error_message, false);
	if (cfr != CopyFileResult::SUCCESS) {
		BOOST_LOG_TRIVIAL(error) << "Copying failed(" << cfr << "): " << error_message;
		return -1;
	}
	// Permissions should be copied from the source file by copy_file(). We are not sure about the source
	// permissions, let's rewrite them with 644.
	static constexpr const auto perms = fs::owner_read | fs::owner_write | fs::group_read | fs::others_read;
	fs::permissions(target, perms);

    return 0;
}

//BBS: add directory copy
int copy_directory_fix(const fs::path &source, const fs::path &target, std::string& error_message, std::vector<std::pair<fs::path, fs::path>>& files)
{
    int ret = 0;
    BOOST_LOG_TRIVIAL(debug) << format("PresetUpdater: Copying %1% -> %2%", PathSanitizer::sanitize(source), PathSanitizer::sanitize(target));

    boost::system::error_code ec;
    fs::create_directories(target, ec);
    if (ec) {
        error_message = ec.message();
        BOOST_LOG_TRIVIAL(error) << "copy_directory_fix: Failed to create target directory: " + error_message;
        return -2;
    }
    for (auto &dir_entry : fs::directory_iterator(source))
    {
        fs::path source_file = dir_entry.path();
        fs::path target_file = target / dir_entry.path().filename();

        std::string name = dir_entry.path().filename().string();
        // Skip IDE / VCS junk that may land under resources/profiles (e.g. Visual
        // Studio's ".vs" cache with locked sqlite / vsidx files). Copying those
        // into the user system/ preset tree fails with "Error: open src file"
        // and aborts the whole GuideFrame install.
        if (!name.empty() && name[0] == '.')
            continue;

        if (fs::is_directory(dir_entry)) {
            ret = copy_directory_fix(source_file, target_file, error_message, files);
            if (ret)
                return ret;
        }
        else {
           files.emplace_back(std::pair{source_file, target_file});
        }
    }
    return 0;
}

int copy_directory_inner(const fs::path &source, const fs::path &target, std::string &error_message)
{
    if (fs::exists(target)) {
        boost::system::error_code ec;
        fs::remove_all(target, ec);
        if (ec) {
            error_message = ec.message();
            BOOST_LOG_TRIVIAL(error) << "copy_directory_fix: Failed to remove existing target directory: " + error_message;
            return -1;
        }
    }
    std::vector<std::pair<fs::path, fs::path>> files;

    int ret = copy_directory_fix(source, target, error_message, files);
    if (ret) { return ret; }
    std::atomic<int> retVal = 0;
    tbb::parallel_for(tbb::blocked_range<int>(0, files.size()), [&files, &error_message, &retVal](const tbb::blocked_range<int> &range) {
        for (auto i = range.begin(); i != range.end(); ++i) {
            if (retVal.load()) return;
            auto &pair = files[i];
            // CopyFileResult cfr = Slic3r::GUI::copy_file_gui(source_file, target_file, error_message, false);
            CopyFileResult cfr = copy_file(pair.first.string(), pair.second.string(), error_message, false);
            if (cfr != CopyFileResult::SUCCESS) {
                BOOST_LOG_TRIVIAL(error) << "Copying failed(" << cfr << "): " << error_message
                                        << ", src=" << PathSanitizer::sanitize(pair.first)
                                        << ", dst=" << PathSanitizer::sanitize(pair.second);
                retVal.store(-3);
            }
        }
    });
    return retVal.load();
}

struct Update
{
	fs::path source;
	fs::path target;

	Version version;
	std::string vendor;
	//BBS: use changelog string instead of url
	std::string change_log;
	std::string descriptions;

	bool forced_update;
	//BBS: add directory support
	bool is_directory {false};

	Update() {}
	//BBS: add directory support
	//BBS: use changelog string instead of url
	Update(fs::path &&source, fs::path &&target, const Version &version, std::string vendor, std::string changelog, std::string description, bool forced = false, bool is_dir = false)
		: source(std::move(source))
		, target(std::move(target))
		, version(version)
		, vendor(std::move(vendor))
		, change_log(std::move(changelog))
		, descriptions(std::move(description))
		, forced_update(forced)
		, is_directory(is_dir)
	{}

    //BBS: add directory support
    int install() const
    {
        int ret = 0;
        std::string error_message;

        if (is_directory) {
            ret = copy_directory_inner(source, target, error_message);
        }
        else {
            ret = copy_file_fix(source, target, error_message);
        }

        /*if (ret) {
            throw Slic3r::CriticalException(GUI::format(
                _L("Copying of file %1% to %2% failed: %3%"),
                source, target, error_message));
        }*/

        return ret;
    }

	friend std::ostream& operator<<(std::ostream& os, const Update &self)
	{
        os << "Update(" << PathSanitizer::sanitize(self.source.string()) << " -> " << PathSanitizer::sanitize(self.target.string()) << ')';
		return os;
	}
};

struct Incompat
{
	fs::path bundle;
	Version version;
	std::string vendor;
	//BBS: add directory support
	bool is_directory {false};

	Incompat(fs::path &&bundle, const Version &version, std::string vendor, bool is_dir = false)
		: bundle(std::move(bundle))
		, version(version)
		, vendor(std::move(vendor))
		, is_directory(is_dir)
	{}

    void remove() {
        // Remove the bundle file
        if (is_directory) {
            if (fs::exists(bundle)) {
                fs::path newPath(bundle.string() + std::to_string(getTimestamp()));
                boost::system::error_code ec;
                fs::rename(bundle, newPath, ec);
                if (!ec) {
                    std::thread thread([newPath]() {
                        boost::system::error_code ec;
                        fs::remove_all(newPath, ec);
                    });
                    thread.detach();
                } else {
                    fs::remove_all(bundle);
                }
            }
        } else {
            if (fs::exists(bundle)) fs::remove(bundle);
        }
    }

	friend std::ostream& operator<<(std::ostream& os , const Incompat &self) {
		os << "Incompat(" << self.bundle.string() << ')';
		return os;
	}
};

struct Updates
{
	std::vector<Incompat> incompats;
	std::vector<Update> updates;
};


wxDEFINE_EVENT(EVT_SLIC3R_EXPERIMENTAL_VERSION_ONLINE, wxCommandEvent);


struct PresetUpdater::priv
{
	std::vector<Index> index_db;

	bool enabled_version_check;
	bool enabled_config_update;
	std::string version_check_url;

	fs::path cache_path;
	fs::path rsrc_path;
	fs::path vendor_path;

    // Written by the main thread (in ~PresetUpdater) and polled by the sync worker
    // thread, so it must be atomic to avoid a data race / cached read.
    std::atomic<bool> cancel;
    std::thread       thread;

    bool    has_waiting_updates{false};
    Updates waiting_updates;
	bool has_waiting_printer_updates { false };
    Updates waiting_printer_updates;

    struct Resource
    {
        std::string              version;
        std::string              description;
        std::string              url;
        std::string              vendor;
        bool                     force{false};
        std::string              cache_root;
        std::vector<std::string> sub_caches;
    };

    priv();

	//BBS: refine preset update logic

    void remove_config_files(std::string vendor, std::string sub_path) const;
    //compare_count means how many sub-versions in AA.BB.CC.DD need to be checked
    bool get_valid_ota_version(Semver& app_version, Semver& current_version, Semver& cached_version, int compare_count) const;

    // is_arm64 is captured on the main thread before the sync worker starts, so the
    // worker never has to call GUI::wxGetApp() (which can be null during shutdown).

	//BBS: refine preset update logic
	bool install_bundles_rsrc(std::vector<std::string> bundles, bool snapshot) const;
	void check_installed_vendor_profiles() const;
    Updates get_printer_config_updates(bool update = false) const;
	Updates get_config_updates(const Semver& old_slic3r_version) const;
	bool perform_updates(Updates &&updates, bool snapshot = true) const;
	void set_waiting_updates(Updates u);
};

//BBS: change directories by design
PresetUpdater::priv::priv()
	: cache_path(fs::path(Slic3r::data_dir()) / "ota")
	, rsrc_path(fs::path(resources_dir()) / "profiles")
	, vendor_path(fs::path(Slic3r::data_dir()) / PRESET_SYSTEM_DIR)
	, cancel(false)
{
	//BBS: refine preset updater logic
	enabled_version_check = true;
	version_check_url = GUI::wxGetApp().app_config->version_check_url();
	enabled_config_update = version_check_url.empty();
	// Install indicies from resources. Only installs those that are either missing or older than in resources.
	check_installed_vendor_profiles();
    perform_updates(get_printer_config_updates(), false);
	// Load indices from the cache directory.
	//index_db = Index::load_db();
}

// Pull relevant preferences from AppConfig

//BBS: refine the Preset Updater logic
// Downloads a file (http get operation). Cancels if the Updater is being destroyed.

//BBS: refine preset update logic

// Remove leftover paritally downloaded files, if any.

//BBS: refine the Preset Updater logic
// Get Slic3rPE version available online, save in AppConfig.

// Version string must contain release version on first line. Follows non-mandatory alpha / beta releases on following lines (alpha=2.0.0-alpha1).

//BBS: refine the Preset Updater logic
// Download vendor indices. Also download new bundles if an index indicates there's a new one available.
// Both are saved in cache.

bool PresetUpdater::priv::get_valid_ota_version(Semver& app_version, Semver& current_version, Semver& cached_version, int compare_count) const
{
    bool valid = false;
    int app_patch_cc = app_version.patch()/100;
    int cached_patch_cc = cached_version.patch()/100;
    int curent_patch_cc = current_version.patch()/100;

    int app_patch_dd = app_version.patch()%100;
    int cached_patch_dd = cached_version.patch() % 100;
    int curent_patch_dd = current_version.patch()%100;

    if (compare_count <= 1) {
        if ((cached_version.maj() == app_version.maj())
            && ((cached_version.min() > current_version.min())
              || ((cached_version.min() == current_version.min()) && (cached_patch_cc > curent_patch_cc))
              || ((cached_version.min() == current_version.min()) && (cached_patch_cc == curent_patch_cc)  && (cached_patch_dd > curent_patch_dd) )))
              valid = true;
    }
    else if (compare_count == 2) {
        if ((cached_version.maj() == app_version.maj()) && (cached_version.min() == app_version.min())
            && ((cached_patch_cc > curent_patch_cc)
              || ((cached_patch_cc == curent_patch_cc)  && (cached_patch_dd > curent_patch_dd) )))
              valid = true;
    }
    else if (compare_count == 3) {
        if ((cached_version.maj() == app_version.maj()) && (cached_version.min() == app_version.min()) && (cached_patch_cc == app_patch_cc)
            && (cached_patch_dd > curent_patch_dd))
              valid = true;
    }
    return valid;
}



void PresetUpdater::priv::remove_config_files(std::string vendor, std::string sub_path) const
{
    fs::path cache_folder = sub_path.empty()?cache_path : (cache_path / sub_path);
    fs::path vendor_folder = sub_path.empty()?(cache_path / vendor): (cache_path / sub_path / vendor);
    std::string vendor_ota_json = cache_folder.string() + "/"+vendor+".changelog";
    std::string vendor_json = cache_folder.string() + "/"+vendor+".json";
    if (fs::exists(vendor_ota_json)) {
        try {
            fs::remove(vendor_ota_json);
        } catch (...) {
            BOOST_LOG_TRIVIAL(error) << "Failed  removing vendor ota json: " << vendor_ota_json;
        }
    }

    if (fs::exists(vendor_json)) {
        try {
            fs::remove(vendor_json);
        } catch (...) {
            BOOST_LOG_TRIVIAL(error) << "Failed  removing vendor json: " << vendor_json;
        }
    }

    if (fs::exists(vendor_folder)) {
        try {
            fs::remove_all(vendor_folder);
        } catch (...) {
            BOOST_LOG_TRIVIAL(error) << "Failed  removing the vendor directory: " << vendor_folder.string();
        }
    }
    return;
}

//BBS: refine the Preset Updater logic
// Download vendor indices. Also download new bundles if an index indicates there's a new one available.
// Both are saved in cache.


// return true means there are plugins files



bool PresetUpdater::priv::install_bundles_rsrc(std::vector<std::string> bundles, bool snapshot) const
{
    Updates updates;

    BOOST_LOG_TRIVIAL(info) << format("Installing %1% bundles from resources ...", bundles.size());

    for (const auto &bundle : bundles) {
        //BBS: add directory support
        auto print_in_rsrc = this->rsrc_path / bundle;
        auto print_in_vendors = this->vendor_path / bundle;
        fs::path print_folder(print_in_vendors);

        auto path_in_rsrc = (this->rsrc_path / bundle).replace_extension(".json");
        auto path_in_vendors = (this->vendor_path / bundle).replace_extension(".json");

        //delete the json at first
        boost::system::error_code ec;
        if (fs::exists(path_in_vendors)) {
            fs::remove(path_in_vendors, ec);
            if (ec) {
                BOOST_LOG_TRIVIAL(error) << boost::format("install_bundles_rsrc: Failed to remove file %1%, error %2% ") % path_in_vendors.string() % ec.message();
                return false;
            }
        }

        if (fs::exists(print_folder)) {
            auto print_rename_folder = this->vendor_path / (bundle + std::to_string(getTimestamp()));
            fs::rename(print_folder, print_rename_folder, ec);
            if (ec) {
                BOOST_LOG_TRIVIAL(error) << boost::format("install_bundles_rsrc: Failed to remove directory %1%, error %2% ") % print_folder.string() % ec.message();
                return false;
            }
            std::thread thread([print_rename_folder]() {
                boost::system::error_code ec;
                fs::remove_all(print_rename_folder, ec);
            });
            // remove the fold in background thread
            thread.detach();
        }
        updates.updates.emplace_back(std::move(print_in_rsrc), std::move(print_in_vendors), Version(), bundle, "", "", false, true);

        //copy json at the last
        updates.updates.emplace_back(std::move(path_in_rsrc), std::move(path_in_vendors), Version(), bundle, "", "");
    }

    return perform_updates(std::move(updates), snapshot);
}


//BBS: refine preset update logic
// Install indicies from resources. Only installs those that are either missing or older than in resources.
void PresetUpdater::priv::check_installed_vendor_profiles() const
{
    BOOST_LOG_TRIVIAL(info) << "[BBL Updater]:Checking whether the profile from resource is newer";

    AppConfig *app_config = GUI::wxGetApp().app_config;
    const auto enabled_vendors = app_config->vendors();

    //BBS: refine the init check logic
    std::vector<std::string> bundles;
    for (auto &dir_entry : fs::directory_iterator(rsrc_path)) {
        const auto &path = dir_entry.path();
        std::string file_path = path.string();
        if (is_json_file(file_path)) {
            const auto path_in_vendor = vendor_path / path.filename();
            std::string vendor_name = path.filename().string();
            // Remove the .json suffix.
            vendor_name.erase(vendor_name.size() - 5);
            if (enabled_config_update) {
                if ( fs::exists(path_in_vendor)) {
                    if ((vendor_name == PresetBundle::BBL_BUNDLE) || (enabled_vendors.find(vendor_name) != enabled_vendors.end())) {
                        Semver resource_ver = get_version_from_json(file_path);
                        Semver vendor_ver = get_version_from_json(path_in_vendor.string());

                        bool version_match = ((resource_ver.maj() == vendor_ver.maj()) && (resource_ver.min() == vendor_ver.min()));
                        if (!version_match || (vendor_ver < resource_ver)) {
                            BOOST_LOG_TRIVIAL(info) << "[BBL Updater]:found vendor "<<vendor_name<<" different version "<<resource_ver.to_string() <<" from resource, old version "<<vendor_ver.to_string()<<", will copy from resource";
                            bundles.push_back(vendor_name);
                        }
                    }
                    else {
                        //need to be removed because not installed
                        fs::remove(path_in_vendor);
                        const auto path_of_vendor = vendor_path / vendor_name;
                        if (fs::exists(path_of_vendor))
                            fs::remove_all(path_of_vendor);
                    }
                }
                else if ((vendor_name == PresetBundle::BBL_BUNDLE) || (enabled_vendors.find(vendor_name) != enabled_vendors.end())) {//if vendor has no file, copy it from resource for BBL
                    bundles.push_back(vendor_name);
                }
            }
            else if ((vendor_name == PresetBundle::BBL_BUNDLE) || (enabled_vendors.find(vendor_name) != enabled_vendors.end())) { //always update configs from resource to vendor for BBL
                bundles.push_back(vendor_name);
            }
        }
    }

    if (bundles.size() > 0)
        install_bundles_rsrc(bundles, false);
}

Updates PresetUpdater::priv::get_printer_config_updates(bool update) const
{
    std::string             data_dir_str = data_dir();
    fs::path data_dir_path(data_dir_str);
    fs::path resc_dir_path(resources_dir());
    auto                    config_folder = data_dir_path / "printers";
    auto                    resc_folder   = (update ? cache_path : resc_dir_path) / "printers";
    std::string             curr_version;
    std::string             resc_version;
    try {
        if (fs::exists(resc_folder / "version.txt")) {
            Slic3r::load_string_file(resc_folder / "version.txt", resc_version);
            boost::algorithm::trim(resc_version);
        }
    } catch (...) {}
    try {
        if (fs::exists(config_folder / "version.txt")) {
            Slic3r::load_string_file(config_folder / "version.txt", curr_version);
            boost::algorithm::trim(curr_version);
        }
    } catch (...) {}

    if (!curr_version.empty()) {
        Semver curr_ver = curr_version;
        Semver resc_ver   = resc_version;

        bool version_match = ((resc_ver.maj() == curr_ver.maj()) && (resc_ver.min() == curr_ver.min()));

        if (!version_match || (curr_ver < resc_ver)) {
            BOOST_LOG_TRIVIAL(info) << "[BBL Updater]:found different version " << resc_version << " from resource, old version " << curr_version<<", will copy from resource";
        } else {
            return {};
        }
    }
    Updates updates;
    Version version;
    version.config_version = resc_version;
    std::string change_log;
    if (update) {
        std::string changelog_file = (resc_folder / "printer.json").string();
        try {
            boost::nowide::ifstream ifs(changelog_file);
            json                    j;
            ifs >> j;
            version.comment = j["description"];
        } catch (...) {}
    }
    updates.updates.emplace_back(std::move(resc_folder), std::move(config_folder), version, "bbl", change_log, version.comment, false, true);
    return updates;
}

// Generates a list of bundle updates that are to be performed.
// Version of slic3r that was running the last time and which was read out from PrusaSlicer.ini is provided
// as a parameter.
//BBS: refine the Preset Updater logic
Updates PresetUpdater::priv::get_config_updates(const Semver &old_slic3r_version) const
{
    Updates updates;
    auto cache_folder = cache_path / PRESET_SUBPATH;

    BOOST_LOG_TRIVIAL(info) << "[BBL Updater]:Checking for cached configuration updates...";

    //remove previous old datas
    remove_config_files("BBL", std::string());

    if (fs::exists(cache_folder)) {
        for (auto& dir_entry : fs::directory_iterator(cache_folder)) {
            const auto& path = dir_entry.path();
            std::string file_path = path.string();
            if (is_json_file(file_path)) {
                const auto path_in_vendor = vendor_path / path.filename();
                std::string vendor_name = path.filename().string(), cached_version, description;
                // Remove the .json suffix.
                vendor_name.erase(vendor_name.size() - 5);
                auto print_in_cache = (cache_folder / vendor_name / PRESET_PRINT_NAME);
                auto filament_in_cache = (cache_folder / vendor_name / PRESET_FILAMENT_NAME);
                auto machine_in_cache = (cache_folder / vendor_name / PRESET_PRINTER_NAME);

                std::string changelog_file = (cache_folder / (vendor_name + ".changelog")).string();
                bool force_update;

                if ((fs::exists(path_in_vendor))
                    && (fs::exists(print_in_cache))
                    && (fs::exists(filament_in_cache))
                    && (fs::exists(machine_in_cache))) {

                    if (!cached_version.empty()) {
                        BOOST_LOG_TRIVIAL(info) << boost::format("[BBL Updater] found new presets of vendor: %1%, version %2%, force_upgrade %3%") % vendor_name % cached_version % force_update;
                        std::string app_version = SLIC3R_VERSION;
                        Semver app_semver = app_version;
                        Semver cached_semver = cached_version;
                        Semver current_preset_semver = get_version_from_json(path_in_vendor.string());

                        bool valid_version = get_valid_ota_version(app_semver, current_preset_semver, cached_semver, 2);

                        if (valid_version) {
                            BOOST_LOG_TRIVIAL(info) << boost::format("[BBL Updater] need to update vendor: %1%'s settings from version %2%  to newer version %3%, force_upgrade %4%")
                                % vendor_name % current_preset_semver.to_string() % cached_semver.to_string() % force_update;
                            Version version;
                            version.config_version = cached_semver;
                            //version.comment = description;

                            //BBS: add directory support
                            updates.updates.emplace_back(cache_folder / vendor_name, vendor_path / vendor_name, Version(), vendor_name, "", "", force_update, true);

                            updates.updates.emplace_back(std::move(file_path), std::move(path_in_vendor.string()), std::move(version), vendor_name, description, "", force_update, false);
                        }
                    }
                }
            }
        }
    }

	return updates;
}

//BBS: switch to new BBL.json configs
bool PresetUpdater::priv::perform_updates(Updates &&updates, bool snapshot) const
{
    int ret = 0;
    //std::string vendor_path;
    //std::string vendor_name;
    if (updates.incompats.size() > 0) {
        //if (snapshot) {
        //	BOOST_LOG_TRIVIAL(info) << "Taking a snapshot...";
        //	if (! GUI::Config::take_config_snapshot_cancel_on_error(*GUI::wxGetApp().app_config, Snapshot::SNAPSHOT_DOWNGRADE, "",
        //		_u8L("Continue and install configuration updates?")))
        //		return false;
        //}
        BOOST_LOG_TRIVIAL(info) << format("[BBL Updater]:Deleting %1% incompatible bundles", updates.incompats.size());

        for (auto &incompat : updates.incompats) {
            BOOST_LOG_TRIVIAL(info) << '\t' << incompat;
            incompat.remove();
        }
    } else if (updates.updates.size() > 0) {
        //if (snapshot) {
        //	BOOST_LOG_TRIVIAL(info) << "Taking a snapshot...";
        //	if (! GUI::Config::take_config_snapshot_cancel_on_error(*GUI::wxGetApp().app_config, Snapshot::SNAPSHOT_UPGRADE, "",
        //		_u8L("Continue and install configuration updates?")))
        //		return false;
        //}

        BOOST_LOG_TRIVIAL(info) << format("[BBL Updater]:Performing %1% updates", updates.updates.size());

        for (const auto &update : updates.updates) {
            BOOST_LOG_TRIVIAL(info) << '\t' << update;

            ret = update.install();
            if (ret) {
                BOOST_LOG_TRIVIAL(error) << boost::format("[BBL Updater]:perform_updates to %1% failed, ret=%2%")% update.target.string() % ret;
                break;
            }
            //if (!update.is_directory) {
            //    vendor_path = update.source.parent_path().string();
            //    vendor_name = update.vendor;
            //}
        }

        //if (!vendor_path.empty()) {
        //    PresetBundle bundle;
        //    // Throw when parsing invalid configuration. Only valid configuration is supposed to be provided over the air.
        //    bundle.load_vendor_configs_from_json(vendor_path, vendor_name, PresetBundle::LoadConfigBundleAttribute::LoadSystem, ForwardCompatibilitySubstitutionRule::Disable);

        //    BOOST_LOG_TRIVIAL(info) << format("Deleting %1% conflicting presets", bundle.prints.size() + bundle.filaments.size() + bundle.printers.size());

        //    auto preset_remover = [](const Preset& preset) {
        //        BOOST_LOG_TRIVIAL(info) << '\t' << preset.file;
        //        fs::remove(preset.file);
        //    };

        //    for (const auto &preset : bundle.prints)    { preset_remover(preset); }
        //    for (const auto &preset : bundle.filaments) { preset_remover(preset); }
        //    for (const auto &preset : bundle.printers)  { preset_remover(preset); }
        //}
    }

    return (ret == 0);
}

void PresetUpdater::priv::set_waiting_updates(Updates u)
{
	waiting_updates = u;
	has_waiting_updates = true;
}

PresetUpdater::PresetUpdater() :
	p(new priv())
{}


// Public

PresetUpdater::~PresetUpdater()
{
	if (p && p->thread.joinable()) {
		// This will stop transfers being done by the thread, if any.
		// Cancelling takes some time, but should complete soon enough.
		p->cancel = true;
		p->thread.join();
	}
}

//BBS: change directories by design
//BBS: refine the preset updater logic


static bool reload_configs_update_gui()
{
	wxString header = _L("Need to check the unsaved changes before configuration updates.");
	if (!GUI::wxGetApp().check_and_save_current_preset_changes(_L("Configuration updates"), header, false ))
		return false;

	// Reload global configuration
	auto* app_config = GUI::wxGetApp().app_config;
	// System profiles should not trigger any substitutions, user profiles may trigger substitutions, but these substitutions
	// were already presented to the user on application start up. Just do substitutions now and keep quiet about it.
	// However throw on substitutions in system profiles, those shall never happen with system profiles installed over the air.
	GUI::wxGetApp().preset_bundle->load_presets(*app_config, ForwardCompatibilitySubstitutionRule::EnableSilentDisableSystem);
	// AppConfig-restored filament colors may predate the JSON primary-color alignment.
	Slic3r::align_project_filament_primary_colors_with_json(GUI::wxGetApp().preset_bundle);
	GUI::wxGetApp().load_current_presets();
	GUI::wxGetApp().plater()->set_bed_shape();

	return true;
}

//BBS: refine the preset updater logic
PresetUpdater::UpdateResult PresetUpdater::config_update(const Semver& old_slic3r_version, UpdateParams params) const
{
    if (! p->enabled_config_update) { return R_NOOP; }

    auto updates = p->get_config_updates(old_slic3r_version);
    //if (updates.incompats.size() > 0) {
    //	BOOST_LOG_TRIVIAL(info) << format("%1% bundles incompatible. Asking for action...", updates.incompats.size());

    //	std::unordered_map<std::string, wxString> incompats_map;
    //	for (const auto &incompat : updates.incompats) {
    //		const auto min_slic3r = incompat.version.min_slic3r_version;
    //		const auto max_slic3r = incompat.version.max_slic3r_version;
    //		wxString restrictions;
    //		if (min_slic3r != Semver::zero() && max_slic3r != Semver::inf()) {
    //               restrictions = GUI::format_wxstr(_L("requires min. %s and max. %s"),
    //                   min_slic3r.to_string(),
    //                   max_slic3r.to_string());
    //		} else if (min_slic3r != Semver::zero()) {
    //			restrictions = GUI::format_wxstr(_L("requires min. %s"), min_slic3r.to_string());
    //			BOOST_LOG_TRIVIAL(debug) << "Bundle is not downgrade, user will now have to do whole wizard. This should not happen.";
    //		} else {
    //               restrictions = GUI::format_wxstr(_L("requires max. %s"), max_slic3r.to_string());
    //		}

    //		incompats_map.emplace(std::make_pair(incompat.vendor, std::move(restrictions)));
    //	}

    //	GUI::MsgDataIncompatible dlg(std::move(incompats_map));
    //	const auto res = dlg.ShowModal();
    //	if (res == wxID_REPLACE) {
    //		BOOST_LOG_TRIVIAL(info) << "User wants to re-configure...";

    //		// This effectively removes the incompatible bundles:
    //		// (snapshot is taken beforehand)
    //		if (! p->perform_updates(std::move(updates)) ||
    //			! GUI::wxGetApp().run_wizard(GUI::ConfigWizard::RR_DATA_INCOMPAT))
    //			return R_INCOMPAT_EXIT;

    //		return R_INCOMPAT_CONFIGURED;
    //	}
    //	else {
    //		BOOST_LOG_TRIVIAL(info) << "User wants to exit Slic3r, bye...";
    //		return R_INCOMPAT_EXIT;
    //	}

    //} else
    if (updates.updates.size() > 0) {

        bool force_update = false;
        for (const auto& update : updates.updates) {
            force_update = (update.forced_update ? true : force_update);
            //td::cout << update.forced_update << std::endl;
            //BOOST_LOG_TRIVIAL(info) << format("Update requires higher version.");
        }

        //forced update
        if (force_update)
        {
            BOOST_LOG_TRIVIAL(info) << format("[BBL Updater]:Force updating will start, size %1% ", updates.updates.size());
            bool ret = p->perform_updates(std::move(updates));
            if (!ret) {
                BOOST_LOG_TRIVIAL(warning) << format("[BBL Updater]:perform_updates failed");
                return R_INCOMPAT_EXIT;
            }

            ret = reload_configs_update_gui();
            if (!ret) {
                BOOST_LOG_TRIVIAL(warning) << format("[BBL Updater]:reload_configs_update_gui failed");
                return R_ALL_CANCELED;
            }
            Semver cur_ver = GUI::wxGetApp().preset_bundle->get_vendor_profile_version(PresetBundle::BBL_BUNDLE);

            GUI::wxGetApp().plater()->get_notification_manager()->push_notification(GUI::NotificationType::PresetUpdateFinished, GUI::NotificationManager::NotificationLevel::ImportantNotificationLevel,  _u8L("Configuration package updated to ")+cur_ver.to_string());

            return R_UPDATE_INSTALLED;
        }

        // regular update
        if (params == UpdateParams::SHOW_NOTIFICATION) {
            p->set_waiting_updates(updates);
            GUI::wxGetApp().plater()->get_notification_manager()->push_notification(GUI::NotificationType::PresetUpdateAvailable);
        }
        else {
            BOOST_LOG_TRIVIAL(info) << format("[BBL Updater]:Configuration package available. size %1%, need to confirm...", p->waiting_updates.updates.size());

            std::vector<GUI::MsgUpdateConfig::Update> updates_msg;
            for (const auto& update : updates.updates) {
                //BBS: skip directory
                if (update.is_directory)
                    continue;
                std::string changelog = update.change_log;
                updates_msg.emplace_back(update.vendor, update.version.config_version, update.descriptions, std::move(changelog));
            }

            GUI::MsgUpdateConfig dlg(updates_msg, params == UpdateParams::FORCED_BEFORE_WIZARD);

            const auto res = dlg.ShowModal();
            if (res == wxID_OK) {
                BOOST_LOG_TRIVIAL(debug) << "[BBL Updater]:selected yes to update";
                if (!p->perform_updates(std::move(updates)))
                    return R_INCOMPAT_EXIT;

                if (!reload_configs_update_gui())
                    return R_ALL_CANCELED;
                return R_UPDATE_INSTALLED;
            }
            else {
                BOOST_LOG_TRIVIAL(info) << "[BBL Updater]:selected no for updating";
                if (params == UpdateParams::FORCED_BEFORE_WIZARD && res == wxID_CANCEL)
                    return R_ALL_CANCELED;
                return R_UPDATE_REJECT;
            }
        }

        // MsgUpdateConfig will show after the notificaation is clicked
    } else {
        BOOST_LOG_TRIVIAL(info) << "[BBL Updater]:No configuration updates available.";
    }

	return R_NOOP;
}

//BBS: add json related logic
bool PresetUpdater::install_bundles_rsrc(std::vector<std::string> bundles, bool snapshot) const
{
	return p->install_bundles_rsrc(bundles, snapshot);
}

void PresetUpdater::on_update_notification_confirm()
{
	if (!p->has_waiting_updates)
		return;
	BOOST_LOG_TRIVIAL(info) << format("Update of %1% bundles available. Asking for confirmation ...", p->waiting_updates.updates.size());

	std::vector<GUI::MsgUpdateConfig::Update> updates_msg;
	for (const auto& update : p->waiting_updates.updates) {
		//BBS: skip directory
		if (update.is_directory)
			continue;
		std::string changelog = update.change_log;
		updates_msg.emplace_back(update.vendor, update.version.config_version, update.descriptions, std::move(changelog));
	}

	GUI::MsgUpdateConfig dlg(updates_msg);

	const auto res = dlg.ShowModal();
	if (res == wxID_OK) {
		BOOST_LOG_TRIVIAL(debug) << "User agreed to perform the update";
		if (p->perform_updates(std::move(p->waiting_updates)) &&
			reload_configs_update_gui()) {
			p->has_waiting_updates = false;
		}
	}
	else {
		BOOST_LOG_TRIVIAL(info) << "User refused the update";
	}
}

void PresetUpdater::do_printer_config_update()
{
    if (!p->has_waiting_printer_updates)
        return;
    BOOST_LOG_TRIVIAL(info) << "Update of printer configs available. Asking for confirmation ...";

    std::vector<GUI::MsgUpdateConfig::Update> updates_msg;
    for (const auto &update : p->waiting_printer_updates.updates) {
        std::string changelog = update.change_log;
        updates_msg.emplace_back(update.vendor, update.version.config_version, update.descriptions, std::move(changelog));
    }

    GUI::MsgUpdateConfig dlg(updates_msg);

    const auto res = dlg.ShowModal();
    if (res == wxID_OK) {
        BOOST_LOG_TRIVIAL(debug) << "User agreed to perform the update";
        if (p->perform_updates(std::move(p->waiting_printer_updates)))
            p->has_waiting_printer_updates = false;
    } else {
        BOOST_LOG_TRIVIAL(info) << "User refused the update";
    }
}

bool PresetUpdater::version_check_enabled() const
{
	return p->enabled_version_check;
}

}
