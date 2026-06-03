#ifndef FILEBROWSER_ICONS_H
#define FILEBROWSER_ICONS_H

// Compact, crisp SVGs matching the VibeOS style (typically 28x28 viewBox).
// We use fill="currentColor" or stroke="currentColor" to allow dynamic theme coloring.

// Navigation arrows: thin line glyphs with a shaft + arrowhead (matches the
// flat toolbar look of the mockup, not bare chevrons).
static const char* SVG_ARROW_LEFT =
    "<svg width=\"28\" height=\"28\" viewBox=\"0 0 28 28\">"
    "<path d=\"M21 14 H8 M14 8 L8 14 L14 20\" stroke=\"currentColor\" stroke-width=\"2\" fill=\"none\" stroke-linecap=\"round\" stroke-linejoin=\"round\"/>"
    "</svg>";

static const char* SVG_ARROW_RIGHT =
    "<svg width=\"28\" height=\"28\" viewBox=\"0 0 28 28\">"
    "<path d=\"M7 14 H20 M14 8 L20 14 L14 20\" stroke=\"currentColor\" stroke-width=\"2\" fill=\"none\" stroke-linecap=\"round\" stroke-linejoin=\"round\"/>"
    "</svg>";

static const char* SVG_ARROW_UP =
    "<svg width=\"28\" height=\"28\" viewBox=\"0 0 28 28\">"
    "<path d=\"M14 21 V8 M8 14 L14 8 L20 14\" stroke=\"currentColor\" stroke-width=\"2\" fill=\"none\" stroke-linecap=\"round\" stroke-linejoin=\"round\"/>"
    "</svg>";

// NOTE: VibeOS' libsvg does NOT render elliptical-arc ('A') path commands — it
// collapses them to a straight line. Every "round" icon below is therefore
// built from cubic curves ('C', supported) or polylines, never arcs.
static const char* SVG_REFRESH =
    "<svg width=\"28\" height=\"28\" viewBox=\"0 0 28 28\">"
    "<path d=\"M12 21.7 L7.1 18 L6.3 11.9 L10 7.1 L16 6.3 L20.9 10\" stroke=\"currentColor\" stroke-width=\"2\" fill=\"none\" stroke-linecap=\"round\" stroke-linejoin=\"round\"/>"
    "<path d=\"M21 5 V10 H16\" stroke=\"currentColor\" stroke-width=\"2\" fill=\"none\" stroke-linecap=\"round\" stroke-linejoin=\"round\"/>"
    "</svg>";

// Standalone plus glyph for the "New Folder" button.
static const char* SVG_PLUS =
    "<svg width=\"28\" height=\"28\" viewBox=\"0 0 28 28\">"
    "<path d=\"M14 8 V20 M8 14 H20\" stroke=\"currentColor\" stroke-width=\"2\" fill=\"none\" stroke-linecap=\"round\"/>"
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
    "<path d=\"M4 14 H24 M14 4 C8 9 8 19 14 24 M14 4 C20 9 20 19 14 24\" stroke=\"currentColor\" stroke-width=\"1.5\" fill=\"none\"/>"
    "</svg>";

static const char* SVG_TRASH =
    "<svg width=\"28\" height=\"28\" viewBox=\"0 0 28 28\">"
    "<path d=\"M5 7 H23 M10 7 V4 H18 V7 M7.5 7 L8.5 24 H19.5 L20.5 7 M12 11 V20 M16 11 V20\" stroke=\"currentColor\" stroke-width=\"2\" fill=\"none\" stroke-linecap=\"round\" stroke-linejoin=\"round\"/>"
    "</svg>";

// --- Distinct sidebar icons (outline line-art, 28x28 viewBox) -------------- //
static const char* SVG_FOLDER_OUTLINE =
    "<svg width=\"28\" height=\"28\" viewBox=\"0 0 28 28\">"
    "<path d=\"M4 8 h6 l2 3 h12 v12 H4 Z\" stroke=\"currentColor\" stroke-width=\"2\" fill=\"none\" stroke-linejoin=\"round\"/>"
    "</svg>";

static const char* SVG_DESKTOP =
    "<svg width=\"28\" height=\"28\" viewBox=\"0 0 28 28\">"
    "<rect x=\"4\" y=\"5\" width=\"20\" height=\"13\" rx=\"1.5\" stroke=\"currentColor\" stroke-width=\"2\" fill=\"none\"/>"
    "<path d=\"M10 23 h8 M12 18 v5 M16 18 v5\" stroke=\"currentColor\" stroke-width=\"2\" fill=\"none\" stroke-linecap=\"round\" stroke-linejoin=\"round\"/>"
    "</svg>";

static const char* SVG_DOCUMENTS =
    "<svg width=\"28\" height=\"28\" viewBox=\"0 0 28 28\">"
    "<path d=\"M7 4 h9 l5 5 v15 H7 Z\" stroke=\"currentColor\" stroke-width=\"2\" fill=\"none\" stroke-linejoin=\"round\"/>"
    "<path d=\"M16 4 v5 h5\" stroke=\"currentColor\" stroke-width=\"2\" fill=\"none\" stroke-linejoin=\"round\"/>"
    "<path d=\"M10 14 h8 M10 18 h8\" stroke=\"currentColor\" stroke-width=\"1.5\" stroke-linecap=\"round\"/>"
    "</svg>";

static const char* SVG_DOWNLOAD =
    "<svg width=\"28\" height=\"28\" viewBox=\"0 0 28 28\">"
    "<path d=\"M14 4 v12 M9 11 l5 5 l5 -5\" stroke=\"currentColor\" stroke-width=\"2\" fill=\"none\" stroke-linecap=\"round\" stroke-linejoin=\"round\"/>"
    "<path d=\"M5 21 h18\" stroke=\"currentColor\" stroke-width=\"2\" stroke-linecap=\"round\"/>"
    "</svg>";

static const char* SVG_DISK =
    "<svg width=\"28\" height=\"28\" viewBox=\"0 0 28 28\">"
    "<path d=\"M4 7 C4 4.5 24 4.5 24 7 C24 9.5 4 9.5 4 7 Z\" stroke=\"currentColor\" stroke-width=\"2\" fill=\"none\"/>"
    "<path d=\"M4 7 V21 C4 23.5 24 23.5 24 21 V7\" stroke=\"currentColor\" stroke-width=\"2\" fill=\"none\"/>"
    "</svg>";

static const char* SVG_CHIP =
    "<svg width=\"28\" height=\"28\" viewBox=\"0 0 28 28\">"
    "<rect x=\"7\" y=\"7\" width=\"14\" height=\"14\" rx=\"1.5\" stroke=\"currentColor\" stroke-width=\"2\" fill=\"none\"/>"
    "<rect x=\"11\" y=\"11\" width=\"6\" height=\"6\" stroke=\"currentColor\" stroke-width=\"1.5\" fill=\"none\"/>"
    "<path d=\"M11 3 v4 M17 3 v4 M11 25 v-4 M17 25 v-4 M3 11 h4 M3 17 h4 M25 11 h-4 M25 17 h-4\" stroke=\"currentColor\" stroke-width=\"2\" stroke-linecap=\"round\"/>"
    "</svg>";

static const char* SVG_PACKAGE =
    "<svg width=\"28\" height=\"28\" viewBox=\"0 0 28 28\">"
    "<path d=\"M14 3 L24 8 V20 L14 25 L4 20 V8 Z\" stroke=\"currentColor\" stroke-width=\"2\" fill=\"none\" stroke-linejoin=\"round\"/>"
    "<path d=\"M4 8 L14 13 L24 8 M14 13 V25\" stroke=\"currentColor\" stroke-width=\"2\" fill=\"none\" stroke-linejoin=\"round\"/>"
    "</svg>";

#endif // FILEBROWSER_ICONS_H
