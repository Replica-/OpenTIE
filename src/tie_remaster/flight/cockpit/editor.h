#ifndef TIE_REMASTER_FLIGHT_COCKPIT_EDITOR_H
#define TIE_REMASTER_FLIGHT_COCKPIT_EDITOR_H

/* Cockpit editor: started with --cockpit-editor, it pauses flight once a TFTC
 * cockpit is on screen and lets the HUD instruments be dragged around in
 * groups; the mouse wheel over a group scales it and Shift+wheel rotates it,
 * and the handles on its right edge, bottom edge and corner stretch it.
 * Right-click opens a group's menu (match surface, reset; for the objectives
 * panel also word wrap and headings, its handles setting width and height). The COCKPIT VIEW handle in the top-right
 * corner slides the cockpit when dragged, zooms with the wheel and resets on
 * right-click. PLAY, under it, resumes the game to try the layout and F12
 * returns to the editor. Positions
 * are kept as per-instrument offsets in the cockpit's 640x480 frame and,
 * with the view, saved to resources/cockpit3d/<Model>.hud.yaml after every
 * change. */

#include <stdbool.h>

#include "aeron/vfs.h"
#include "tie_runtime/snapshot/hud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AeronInputSnapshot AeronInputSnapshot;
typedef struct AeronDrawList2D AeronDrawList2D;

/* Text the editor wants drawn: handle names and the help panel. */
typedef enum TieCockpitEditorTextColor {
	TIE_COCKPIT_EDITOR_TEXT_PLAIN,
	TIE_COCKPIT_EDITOR_TEXT_HEADING,
	TIE_COCKPIT_EDITOR_TEXT_HOT,
} TieCockpitEditorTextColor;

typedef struct TieCockpitEditorLabel {
	char text[48];
	float x, y; /* cockpit frame px, top-left of the text */
	TieCockpitEditorTextColor color;
} TieCockpitEditorLabel;

#define TIE_COCKPIT_EDITOR_MAX_LABELS 128
/* Editor ids past the HUD instruments for the TFTC-only panels. */
#define TIE_COCKPIT_EDITOR_REPAIRS_ID TIE_MAX_HUD_INSTRUMENTS
#define TIE_COCKPIT_EDITOR_OBJECTIVES_ID (TIE_MAX_HUD_INSTRUMENTS + 1)
#define TIE_COCKPIT_EDITOR_ID_COUNT (TIE_MAX_HUD_INSTRUMENTS + 2)
/* Editor text is drawn smaller than the cockpit's own, to keep it out of the way. */
#define TIE_COCKPIT_EDITOR_TEXT_SCALE 0.6f

/* True while a TFTC cockpit with editable HUD is on screen. */
bool TieCockpitEditor_Available(void);
bool TieCockpitEditor_Open(void);
/* Closing saves any changes. */
void TieCockpitEditor_SetOpen(bool open);
void TieCockpitEditor_SetInput(const AeronInputSnapshot* input);
/* Opens the editor as soon as a TFTC cockpit is on screen (--cockpit-editor). */
void TieCockpitEditor_RequestOpen(void);

/* Selects the offsets of the TFTC model on screen, loading them on change.
 * NULL means no TFTC cockpit: offsets are zero and the editor closes. Once
 * `settled`, after the first flight frames, a requested editor selects a
 * target (so the targeting computer has something to show) and opens when
 * `has_target` or after a short wait. */
void TieCockpitEditor_UseModel(AeronVfs* vfs, const char* model, bool settled, bool has_target);
/* View of the TFTC cockpit: how far the model slides right and up from the
 * eye, in OPT units, and the horizontal field of view in degrees. */
void TieCockpitEditor_View(float* pan_x, float* pan_y, float* fov_deg);
/* Offset of instrument `id` in cockpit frame pixels; zero when unset. */
void TieCockpitEditor_Offset(int id, float* dx, float* dy);
/* Horizontal and vertical scale (1 when unset) and clockwise rotation in
 * degrees of instrument `id`, applied about its group's pivot. */
void TieCockpitEditor_Transform(int id, float* scale_x, float* scale_y, float* angle_deg);
/* True when instrument `id` is drawn onto the cockpit surface behind it. */
bool TieCockpitEditor_Surface(int id);
/* The group instrument `id` moves with, or -1 for one the editor keeps fixed. */
int TieCockpitEditor_Group(int id);
/* Objectives panel options: word wrap at `width` and at most `height` (0 =
 * every row) on screen, in cockpit px, and whether the goals screen's
 * category headings show. The panel scales its text but never turns. */
void TieCockpitEditor_ObjectivesLayout(bool* wrap, bool* headings, float* width, float* height);
/* Pivot of each instrument's group: the centre of its active anchors, each
 * widened by its extent (zero for an instrument, the size of a panel). */
void TieCockpitEditor_Pivots(const float* anchor_x, const float* anchor_y, const float* extent_w,
							 const float* extent_h, const bool* active, int count, float* pivot_x, float* pivot_y);

/* Runs the drag interaction and draws the group handles. `anchor_x/y` are the
 * instruments' positions in cockpit frame pixels, offsets included, and
 * `active` marks the ones drawn this frame. The fit maps cockpit frame pixels
 * to render-target pixels. Fills `labels` with the text to draw over it and
 * returns how many. */
int TieCockpitEditor_Frame(AeronDrawList2D* list, const float* anchor_x, const float* anchor_y,
						   const float* extent_w, const float* extent_h, const bool* active, int count, float fit_x,
						   float fit_y, float scale_x, float scale_y, int rt_w, int rt_h,
						   TieCockpitEditorLabel* labels, int label_cap);

#ifdef __cplusplus
}
#endif

#endif
