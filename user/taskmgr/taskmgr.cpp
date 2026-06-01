/* taskmgr — VibeOS live process monitor.
 *
 * Entry point only.  All application logic lives in TaskManager;
 * crt0 handles _start, global constructors, and _exit. */
#include "task_manager.h"

int main() {
    static TaskManager app;
    TaskManager::s_instance_ = &app;
    app.run();
}
