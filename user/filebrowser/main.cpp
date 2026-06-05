#include "filebrowser.h"
#include <vibeos.h>
#include <string.h>

// Entry point for the VibeOS File Browser.
//
// The FileBrowser class owns all state and the rendering/event logic. It opens
// its own window inside run() (mirroring the Browser app's design), so main()
// only needs to construct, initialise, and run the application.
//
// When launched by vui_file_dialog() the browser is spawned with a single
// argument describing the picker request: "MODE;PATH;RESULT_FILE", where MODE
// is OPEN or SAVE, PATH is the directory to start in, and RESULT_FILE is the
// file the chosen path must be written to. In that case the browser runs as a
// modal file dialog (extra action bar at the bottom); otherwise it is a normal
// standalone file manager.
int main() {
    static FileBrowser app;
    app.init();

    char arg[256];
    if (vos_getarg(arg, sizeof(arg)) > 0 && arg[0]) {
        char *semi1 = strchr(arg, ';');
        if (semi1) {
            *semi1 = '\0';
            char *path = semi1 + 1;
            char *result = nullptr;
            char *semi2 = strchr(path, ';');
            if (semi2) {
                *semi2 = '\0';
                result = semi2 + 1;
            }
            bool save = (strcmp(arg, "SAVE") == 0);
            app.set_dialog(save, path, result ? result : "/tmp/fd_result");
        }
    }

    app.run();
    return 0;
}
