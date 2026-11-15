#include "ProjectService.hpp"

#include <algorithm>
#include <cctype>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/BuildVolume.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/ModelArrange.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"

#include "Core.hpp"
#include "Json.hpp"
#include "PresetService.hpp"

namespace fs = boost::filesystem;
using namespace Slic3r;

namespace printbot::core {

ProjectService::ProjectService(Core &core) : m_core(core), m_model(std::make_unique<Model>()) { publish("init"); }
ProjectService::~ProjectService() = default;

std::shared_ptr<const ProjectSnapshot> ProjectService::snapshot() const
{
    std::lock_guard<std::mutex> lk(m_snap_mutex);
    return m_snapshot;
}

void ProjectService::update_print_volume_state()
{
    auto ps = m_core.presets().snapshot();
    if (!ps || ps->bed_max[0] <= ps->bed_min[0]) return;
    const Preset &printer = m_core.presets().bundle().printers.get_edited_preset();
    auto         *area    = printer.config.option<ConfigOptionPoints>("printable_area");
    if (!area) return;
    BuildVolume bv(area->values, ps->max_print_height, {}, {});
    m_model->update_print_volume_state(bv);
}

void ProjectService::publish(const char *why)
{
    if (m_rev > 0) update_print_volume_state();
    auto snap = std::make_shared<ProjectSnapshot>();
    snap->rev = ++m_rev;
    for (const ModelObject *o : m_model->objects) {
        ObjectInfo info;
        info.id        = o->id().id;
        info.name      = o->name;
        info.source    = o->input_file.empty() ? std::string() : fs::path(o->input_file).filename().string();
        info.instances = int(o->instances.size());
        info.volumes   = int(o->volumes.size());
        info.facets    = o->facets_count();
        if (!o->instances.empty()) {
            BoundingBoxf3 bb = o->bounding_box();
            info.min         = {bb.min.x(), bb.min.y(), bb.min.z()};
            info.max         = {bb.max.x(), bb.max.y(), bb.max.z()};
            int inside = 0, fully_out = 0;
            for (const ModelInstance *i : o->instances) {
                if (i->print_volume_state == ModelInstancePVS_Inside) ++inside;
                else if (i->print_volume_state == ModelInstancePVS_Fully_Outside) ++fully_out;
            }
            info.print_volume = inside == int(o->instances.size()) ? "inside" : fully_out == int(o->instances.size()) ? "outside" : "partly_outside";
        }
        snap->objects.push_back(std::move(info));
    }
    {
        std::lock_guard<std::mutex> lk(m_snap_mutex);
        m_snapshot = snap;
    }
    nlohmann::json detail = to_json(*snap);
    detail["reason"]      = why;
    m_core.events().publish(Topic::Project, "project.changed", std::move(detail));
}

std::vector<std::string> ProjectService::place(const std::vector<size_t> &new_objects, bool arrange_all, double spacing_mm)
{
    std::vector<std::string> warnings;
    auto                     ps = m_core.presets().snapshot();
    if (!ps || ps->bed_max[0] <= ps->bed_min[0]) {
        warnings.push_back("no printer bed known; objects left at their file positions");
        return warnings;
    }
    const Vec2d center((ps->bed_min[0] + ps->bed_max[0]) / 2., (ps->bed_min[1] + ps->bed_max[1]) / 2.);
    for (size_t idx : new_objects) {
        ModelObject *o = m_model->objects[idx];
        o->center_around_origin(false);
        for (ModelInstance *i : o->instances) i->set_offset(Vec3d(center.x(), center.y(), i->get_offset().z()));
        o->ensure_on_bed();
    }
    if (arrange_all && m_model->objects.size() > 1) {
        BoundingBox bed(Point(scaled(ps->bed_min[0]), scaled(ps->bed_min[1])), Point(scaled(ps->bed_max[0]), scaled(ps->bed_max[1])));
        arrangement::ArrangeParams params(scaled(std::max(0.0, spacing_mm)));
        bool                       overflow = false;
        try {
            arrange_objects(*m_model, bed, params, [&overflow](arrangement::ArrangePolygon &) { overflow = true; });
        } catch (const std::exception &ex) {
            warnings.push_back(std::string("arrange failed: ") + ex.what());
        }
        if (overflow) warnings.push_back("not all objects fit on the bed");
        for (ModelObject *o : m_model->objects) o->ensure_on_bed();
    }
    return warnings;
}

Result<ProjectService::LoadResult> ProjectService::load(const LoadOptions &opts)
{
    return m_core.executor().call([&]() -> Result<LoadResult> {
        const fs::path path = fs::absolute(fs::path(opts.path));
        if (!fs::exists(path) || !fs::is_regular_file(path)) return make_error("file.not_found", "no such file: " + opts.path);
        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        static const std::vector<std::string> supported{".stl", ".obj", ".3mf", ".step", ".stp", ".amf", ".svg"};
        if (std::find(supported.begin(), supported.end(), ext) == supported.end())
            return make_error("file.unsupported", "unsupported model format '" + ext + "' (stl, obj, 3mf, step, amf, svg)");
        const bool is_3mf = ext == ".3mf";

        LoadResult res;
        Model      loaded;
        try {
            DynamicPrintConfig        cfg;
            ConfigSubstitutionContext ctx(ForwardCompatibilitySubstitutionRule::EnableSilent);
            PlateDataPtrs             plate_data;
            std::vector<Preset *>     project_presets;
            bool                      is_bbs_3mf = false;
            Semver                    file_version;
            LoadStrategy strategy = is_3mf ? (LoadStrategy::LoadModel | LoadStrategy::AddDefaultInstances) : LoadStrategy::AddDefaultInstances;
            loaded = Model::read_from_file(path.string(), is_3mf ? &cfg : nullptr, is_3mf ? &ctx : nullptr, strategy, &plate_data,
                                           &project_presets, &is_bbs_3mf, &file_version);
            if (plate_data.size() > 1)
                res.warnings.push_back("3MF has " + std::to_string(plate_data.size()) +
                                       " plates; phase 1 treats everything as one plate. Project settings in the file are not applied; select presets explicitly.");
            else if (is_3mf)
                res.warnings.push_back("project settings stored in the 3MF are not applied; select presets explicitly");
            release_PlateData_list(plate_data);
            for (Preset *p : project_presets) delete p;
        } catch (const std::exception &ex) {
            return make_error("file.load_failed", std::string("failed to load ") + path.filename().string() + ": " + ex.what());
        }
        if (loaded.objects.empty()) return make_error("file.empty", "no objects found in " + path.filename().string());

        if (!opts.append) m_model->clear_objects();
        std::vector<size_t> added_idx;
        for (const ModelObject *src : loaded.objects) {
            if (src->volumes.empty()) continue;
            ModelObject *o = m_model->add_object(*src);
            o->input_file  = path.string();
            if (o->instances.empty()) o->add_instance();
            added_idx.push_back(m_model->objects.size() - 1);
        }
        const bool arrange = opts.arrange.value_or(!is_3mf);
        if (arrange) {
            auto w = place(added_idx, true, 6.0);
            res.warnings.insert(res.warnings.end(), w.begin(), w.end());
        } else {
            for (size_t i : added_idx) m_model->objects[i]->ensure_on_bed();
        }
        publish("load_model");
        res.project = *snapshot();
        for (size_t i : added_idx) {
            const auto id = m_model->objects[i]->id().id;
            for (const auto &oi : res.project.objects)
                if (oi.id == id) res.added.push_back(oi);
        }
        for (const auto &oi : res.added)
            if (oi.print_volume != "inside") res.warnings.push_back("object '" + oi.name + "' is " + oi.print_volume + " the printable volume");
        return res;
    });
}

Result<ProjectSnapshot> ProjectService::clear()
{
    return m_core.executor().call([&]() -> Result<ProjectSnapshot> {
        m_model->clear_objects();
        publish("clear");
        return *snapshot();
    });
}

Result<ProjectSnapshot> ProjectService::remove_object(ObjectId id)
{
    return m_core.executor().call([&]() -> Result<ProjectSnapshot> {
        for (size_t i = 0; i < m_model->objects.size(); ++i)
            if (m_model->objects[i]->id().id == id) {
                m_model->delete_object(i);
                publish("remove_object");
                return *snapshot();
            }
        return make_error("object.not_found", "no object with id " + std::to_string(id));
    });
}

Result<ProjectService::LoadResult> ProjectService::arrange(double spacing_mm)
{
    return m_core.executor().call([&]() -> Result<LoadResult> {
        if (m_model->objects.empty()) return make_error("project.empty", "nothing to arrange");
        LoadResult res;
        std::vector<size_t> all;
        if (m_model->objects.size() == 1) all.push_back(0);
        res.warnings = place(all, true, spacing_mm);
        publish("arrange");
        res.project = *snapshot();
        return res;
    });
}

} // namespace printbot::core
