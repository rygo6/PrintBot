#ifndef slic3r_ConfigWizard_hpp_
#define slic3r_ConfigWizard_hpp_

// print_bot: the legacy ConfigWizard dialog was removed (t75u). Only the
// reason/start-page enums survive; GUI_App::run_wizard() opens the native
// "Printers and filaments" dialog.

namespace Slic3r {
namespace GUI {

struct ConfigWizard
{
    // Why is the Wizard run
    enum RunReason {
        RR_DATA_EMPTY,                  // No or empty datadir
        RR_DATA_LEGACY,                 // Pre-updating datadir
        RR_DATA_INCOMPAT,               // Incompatible datadir - Slic3r downgrade situation
        RR_USER,                        // User requested the Wizard from the menus
    };

    // What page should wizard start on
    enum StartPage {
        SP_WELCOME,
        SP_PRINTERS,
        SP_FILAMENTS,
        SP_MATERIALS,
        SP_CUSTOM,
    };
};

}
}

#endif
