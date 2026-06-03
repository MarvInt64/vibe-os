#include "filebrowser.h"

// Entry point for the VibeOS File Browser.
//
// The FileBrowser class owns all state and the rendering/event logic. It opens
// its own window inside run() (mirroring the Browser app's design), so main()
// only needs to construct, initialise, and run the application.
int main() {
    static FileBrowser app;
    app.init();
    app.run();
    return 0;
}
