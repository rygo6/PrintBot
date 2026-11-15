// print_bot core: slicing event keys and typed payloads published on Topic::Slice by
// the background slicing process. The wx Plater subscribes (dispatching through
// CallAfter) and turns them back into its existing wx events. GUI-free (no wxWidgets).
#ifndef printbot_core_SliceEvents_hpp_
#define printbot_core_SliceEvents_hpp_

#include <exception>
#include <string>

namespace printbot::core::slice_events {

// Slicing finished; G-code export continues. Payload: SlicingCompleted.
inline constexpr const char *kSlicingCompleted = "slice.slicing_completed";
// The background task finished, was cancelled or failed. Payload: ProcessCompleted.
inline constexpr const char *kProcessCompleted = "slice.process_completed";
// G-code export to a user path started. Payload: none.
inline constexpr const char *kExportBegan = "slice.export_began";
// G-code export to a user path finished. Payload: ExportFinished.
inline constexpr const char *kExportFinished = "slice.export_finished";
// Progress of a headless slice job. Detail: {job, percent, text}.
inline constexpr const char *kProgress = "slice.progress";
// Headless slice job state change. Detail: snapshot summary.
inline constexpr const char *kState = "slice.state";

struct SlicingCompleted
{
    int timestamp = 0;
};

struct ProcessCompleted
{
    enum Status { Finished, Cancelled, Error };
    Status             status = Finished;
    std::exception_ptr exception;
};

struct ExportFinished
{
    std::string path;
};

} // namespace printbot::core::slice_events

#endif
