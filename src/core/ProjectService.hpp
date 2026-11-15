// print_bot core: the project (model objects on the plate). Phase 1 keeps a single
// plate. GUI-free (no wxWidgets).
#ifndef printbot_core_ProjectService_hpp_
#define printbot_core_ProjectService_hpp_

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "Result.hpp"
#include "Snapshots.hpp"

namespace Slic3r {
class Model;
}

namespace printbot::core {

class Core;

class ProjectService
{
public:
    struct LoadOptions
    {
        std::string path;
        bool        append = false;
        // Place loaded objects on the bed (center one / auto-arrange several).
        // Default: on for meshes, off for 3MF (keeps the file's placement).
        std::optional<bool> arrange;
    };
    struct LoadResult
    {
        std::vector<ObjectInfo>  added;
        std::vector<std::string> warnings;
        ProjectSnapshot          project;
    };

    explicit ProjectService(Core &core);
    ~ProjectService();

    // --- thread-safe API ---
    std::shared_ptr<const ProjectSnapshot> snapshot() const;
    Result<LoadResult>                     load(const LoadOptions &opts);
    Result<ProjectSnapshot>                clear();
    Result<ProjectSnapshot>                remove_object(ObjectId id);
    Result<LoadResult>                     arrange(double spacing_mm);

    // --- core-thread only ---
    const Slic3r::Model &model() const { return *m_model; }
    Revision             revision() const { return m_rev; }
    void                 publish(const char *why);

private:
    std::vector<std::string> place(const std::vector<size_t> &new_objects, bool arrange_all, double spacing_mm);
    void                     update_print_volume_state();

    Core                                   &m_core;
    std::unique_ptr<Slic3r::Model>          m_model;
    mutable std::mutex                      m_snap_mutex;
    std::shared_ptr<const ProjectSnapshot>  m_snapshot;
    Revision                                m_rev = 0;
};

} // namespace printbot::core

#endif
