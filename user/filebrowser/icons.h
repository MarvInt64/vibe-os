#ifndef FILEBROWSER_ICONS_H
#define FILEBROWSER_ICONS_H

// Compact, crisp SVGs matching the VibeOS style (typically 28x28 viewBox).
// We use fill="currentColor" or stroke="currentColor" to allow dynamic theme coloring.

static const char* SVG_ARROW_LEFT = 
    "<svg width=\"28\" height=\"28\" viewBox=\"0 0 28 28\">"
    "<path d=\"M18 6 L10 14 L18 22\" stroke=\"currentColor\" stroke-width=\"2.5\" fill=\"none\" stroke-linecap=\"round\" stroke-linejoin=\"round\"/>"
    "</svg>";

static const char* SVG_ARROW_RIGHT = 
    "<svg width=\"28\" height=\"28\" viewBox=\"0 0 28 28\">"
    "<path d=\"M10 6 L18 14 L10 22\" stroke=\"currentColor\" stroke-width=\"2.5\" fill=\"none\" stroke-linecap=\"round\" stroke-linejoin=\"round\"/>"
    "</svg>";

static const char* SVG_ARROW_UP = 
    "<svg width=\"28\" height=\"28\" viewBox=\"0 0 28 28\">"
    "<path d=\"M6 18 L14 10 L22 18\" stroke=\"currentColor\" stroke-width=\"2.5\" fill=\"none\" stroke-linecap=\"round\" stroke-linejoin=\"round\"/>"
    "</svg>";

static const char* SVG_REFRESH = 
    "<svg width=\"28\" height=\"28\" viewBox=\"0 0 28 28\">"
    "<path d=\"M20 8.5 A8 8 0 1 1 8 8.5 M20 4.5 v4 h-4\" stroke=\"currentColor\" stroke-width=\"2.5\" fill=\"none\" stroke-linecap=\"round\" stroke-linejoin=\"round\"/>"
    "</svg>";

static const char* SVG_NEW_FOLDER = 
    "<svg width=\"28\" height=\"28\" viewBox=\"0 0 28 28\">"
    "<path d=\"M3 6h7l2 3h13v13H3V6z\" stroke=\"currentColor\" stroke-width=\"2\" fill=\"none\" stroke-linejoin=\"round\"/>"
    "<path d=\"M14 11v6M11 14h6\" stroke=\"currentColor\" stroke-width=\"2\" stroke-linecap=\"round\"/>"
    "</svg>";

static const char* SVG_LIST_VIEW = 
    "<svg width=\"28\" height=\"28\" viewBox=\"0 0 28 28\">"
    "<path d=\"M4 7h20M4 14h20M4 21h20\" stroke=\"currentColor\" stroke-width=\"2.5\" stroke-linecap=\"round\"/>"
    "</svg>";

static const char* SVG_GRID_VIEW = 
    "<svg width=\"28\" height=\"28\" viewBox=\"0 0 28 28\">"
    "<rect x=\"4\" y=\"4\" width=\"8\" height=\"8\" stroke=\"currentColor\" stroke-width=\"2.5\" fill=\"none\" rx=\"1\"/>"
    "<rect x=\"16\" y=\"4\" width=\"8\" height=\"8\" stroke=\"currentColor\" stroke-width=\"2.5\" fill=\"none\" rx=\"1\"/>"
    "<rect x=\"4\" y=\"16\" width=\"8\" height=\"8\" stroke=\"currentColor\" stroke-width=\"2.5\" fill=\"none\" rx=\"1\"/>"
    "<rect x=\"16\" y=\"16\" width=\"8\" height=\"8\" stroke=\"currentColor\" stroke-width=\"2.5\" fill=\"none\" rx=\"1\"/>"
    "</svg>";

static const char* SVG_SEARCH = 
    "<svg width=\"28\" height=\"28\" viewBox=\"0 0 28 28\">"
    "<circle cx=\"12\" cy=\"12\" r=\"7\" stroke=\"currentColor\" stroke-width=\"2\" fill=\"none\"/>"
    "<path d=\"M17 17 L24 24\" stroke=\"currentColor\" stroke-width=\"2.5\" stroke-linecap=\"round\"/>"
    "</svg>";

static const char* SVG_HOME = 
    "<svg width=\"28\" height=\"28\" viewBox=\"0 0 28 28\">"
    "<path d=\"M3 12 L14 3 L25 12 M6 10 v12 h16 V10 M11 22 v-6 h6 v6\" stroke=\"currentColor\" stroke-width=\"2\" fill=\"none\" stroke-linecap=\"round\" stroke-linejoin=\"round\"/>"
    "</svg>";

static const char* SVG_RECENT = 
    "<svg width=\"28\" height=\"28\" viewBox=\"0 0 28 28\">"
    "<circle cx=\"14\" cy=\"14\" r=\"10\" stroke=\"currentColor\" stroke-width=\"2\" fill=\"none\"/>"
    "<path d=\"M14 8 v6 h5\" stroke=\"currentColor\" stroke-width=\"2\" stroke-linecap=\"round\" stroke-linejoin=\"round\"/>"
    "</svg>";

static const char* SVG_STAR = 
    "<svg width=\"28\" height=\"28\" viewBox=\"0 0 28 28\">"
    "<path d=\"M14 3 L17.5 10.5 L25 11.5 L19.5 16.8 L21 24 L14 20 L7 24 L8.5 16.8 L3 11.5 L10.5 10.5 Z\" stroke=\"currentColor\" stroke-width=\"2\" fill=\"none\" stroke-linejoin=\"round\"/>"
    "</svg>";

static const char* SVG_FOLDER = 
    "<svg width=\"28\" height=\"28\" viewBox=\"0 0 28 28\">"
    "<path d=\"M3 6h7l2 3h13v13H3V6z\" fill=\"currentColor\" stroke=\"currentColor\" stroke-width=\"1\" stroke-linejoin=\"round\" fill-opacity=\"0.2\"/>"
    "</svg>";

static const char* SVG_FILE = 
    "<svg width=\"28\" height=\"28\" viewBox=\"0 0 28 28\">"
    "<path d=\"M6 3 h11 l6 6 v16 H6 Z\" fill=\"currentColor\" stroke=\"currentColor\" stroke-width=\"2\" fill-opacity=\"0.1\" stroke-linejoin=\"round\"/>"
    "<path d=\"M17 3 v6 h6\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"2\" stroke-linejoin=\"round\"/>"
    "</svg>";

static const char* SVG_FILE_MD = 
    "<svg width=\"28\" height=\"28\" viewBox=\"0 0 28 28\">"
    "<path d=\"M6 3 h11 l6 6 v16 H6 Z\" fill=\"currentColor\" stroke=\"currentColor\" stroke-width=\"2\" fill-opacity=\"0.15\" stroke-linejoin=\"round\"/>"
    "<path d=\"M17 3 v6 h6\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"2\" stroke-linejoin=\"round\"/>"
    "<path d=\"M9 12 v6 M9 12 l3 3.5 l3 -3.5 v6\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"1.5\" stroke-linecap=\"round\" stroke-linejoin=\"round\"/>"
    "<path d=\"M17 12 v6 M15 15 h4\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"1.5\" stroke-linecap=\"round\" stroke-linejoin=\"round\"/>"
    "</svg>";

static const char* SVG_FILE_CODE = 
    "<svg width=\"28\" height=\"28\" viewBox=\"0 0 28 28\">"
    "<path d=\"M6 3 h11 l6 6 v16 H6 Z\" fill=\"currentColor\" stroke=\"currentColor\" stroke-width=\"2\" fill-opacity=\"0.15\" stroke-linejoin=\"round\"/>"
    "<path d=\"M17 3 v6 h6\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"2\" stroke-linejoin=\"round\"/>"
    "<path d=\"M8 15 l-3 3 l3 3 M13 15 l3 3 l-3 3 M11 14 l2 8\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"1.5\" stroke-linecap=\"round\" stroke-linejoin=\"round\"/>"
    "</svg>";

static const char* SVG_FILE_LOG = 
    "<svg width=\"28\" height=\"28\" viewBox=\"0 0 28 28\">"
    "<path d=\"M6 3 h11 l6 6 v16 H6 Z\" fill=\"currentColor\" stroke=\"currentColor\" stroke-width=\"2\" fill-opacity=\"0.15\" stroke-linejoin=\"round\"/>"
    "<path d=\"M17 3 v6 h6\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"2\" stroke-linejoin=\"round\"/>"
    "<path d=\"M9 13 h8 M9 17 h5\" stroke=\"currentColor\" stroke-width=\"2\" stroke-linecap=\"round\"/>"
    "</svg>";

static const char* SVG_SETTINGS = 
    "<svg width=\"28\" height=\"28\" viewBox=\"0 0 28 28\">"
    "<circle cx=\"14\" cy=\"14\" r=\"4\" stroke=\"currentColor\" stroke-width=\"2\" fill=\"none\"/>"
    "<path d=\"M14 2 v4 M14 22 v4 M2 14 h4 M22 14 h4 M6 6 l3 3 M19 19 l3 3 M6 22 l3 -3 M19 9 l3 -3\" stroke=\"currentColor\" stroke-width=\"2.5\" stroke-linecap=\"round\"/>"
    "</svg>";

static const char* SVG_ROOT = 
    "<svg width=\"28\" height=\"28\" viewBox=\"0 0 28 28\">"
    "<rect x=\"3\" y=\"3\" width=\"22\" height=\"6\" rx=\"1\" stroke=\"currentColor\" stroke-width=\"2\" fill=\"none\"/>"
    "<circle cx=\"6\" cy=\"6\" r=\"1\" fill=\"currentColor\"/>"
    "<rect x=\"3\" y=\"11\" width=\"22\" height=\"6\" rx=\"1\" stroke=\"currentColor\" stroke-width=\"2\" fill=\"none\"/>"
    "<circle cx=\"6\" cy=\"14\" r=\"1\" fill=\"currentColor\"/>"
    "<rect x=\"3\" y=\"19\" width=\"22\" height=\"6\" rx=\"1\" stroke=\"currentColor\" stroke-width=\"2\" fill=\"none\"/>"
    "<circle cx=\"6\" cy=\"22\" r=\"1\" fill=\"currentColor\"/>"
    "</svg>";

static const char* SVG_NETWORK = 
    "<svg width=\"28\" height=\"28\" viewBox=\"0 0 28 28\">"
    "<circle cx=\"14\" cy=\"14\" r=\"10\" stroke=\"currentColor\" stroke-width=\"2\" fill=\"none\"/>"
    "<path d=\"M4 14 h20 M14 4 A16 16 0 0 1 14 24 M14 4 A16 16 0 0 0 14 24\" stroke=\"currentColor\" stroke-width=\"1.5\" fill=\"none\"/>"
    "</svg>";

static const char* SVG_TRASH = 
    "<svg width=\"28\" height=\"28\" viewBox=\"0 0 28 28\">"
    "<path d=\"M5 7 h18 M9 7 v-3 h10 v3 M7 7 v16 a2 2 0 0 0 2 2 h10 a2 2 0 0 0 2 -2 v-16 M11 11 v10 M17 11 v10\" stroke=\"currentColor\" stroke-width=\"2\" fill=\"none\" stroke-linecap=\"round\" stroke-linejoin=\"round\"/>"
    "</svg>";

#endif // FILEBROWSER_ICONS_H
