/*
 * texteditor/main.cpp - Entry point for the VibeOS Text Editor.
 *
 * The TextEditor class owns all state and rendering/event logic.
 * It opens its own window inside run(), so main() only needs to
 * construct, initialise, and run the application.
 */

#include "texteditor.h"

int main() {
    static TextEditor editor;
    editor.run();
    return 0;
}
