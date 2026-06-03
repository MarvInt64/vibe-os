/*
 * filedialog/main.cpp — Entry point for the standard file dialog.
 */

#include "filedialog.h"
#include <vibeos.h>
#include <string.h>

int main() {
    char arg[256];
    char title[64] = "Select File";
    char path[256] = "/home/user";
    bool save = false;

    char res_file[128] = "/tmp/fd_result";

    /* Parse argument: "MODE;PATH;RESULT_FILE" */
    if (vos_getarg(arg, sizeof(arg)) > 0) {
        char *semi1 = strchr(arg, ';');
        if (semi1) {
            *semi1 = '\0';
            char *p1 = semi1 + 1;
            char *semi2 = strchr(p1, ';');
            if (semi2) {
                *semi2 = '\0';
                char *p2 = semi2 + 1;
                strncpy(res_file, p2, sizeof(res_file) - 1);
            }
            strncpy(path, p1, sizeof(path) - 1);
            
            if (strcmp(arg, "SAVE") == 0) {
                save = true;
                strcpy(title, "Save As");
            } else {
                strcpy(title, "Open File");
            }
        }
    }

    FileDialog dlg;
    dlg.init(title, path, save, res_file);
    dlg.run();

    return 0;
}
