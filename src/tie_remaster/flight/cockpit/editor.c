#include "tie_remaster/flight/cockpit/editor.h"

#include "aeron/input.h"
#include "aeron/log.h"
#include "aeron/paths.h"
#include "aeron/scene/draw_list2d.h"
#include "tie_runtime/input/input.h"
#include "tie_runtime/presentation/presentation.h"
#include "tie_runtime/snapshot/hud_types.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TIE_COCKPIT_EDITOR_PAD 8.0f /* handle padding around a group's anchors, cockpit px */
/* Default view: the XWAU cockpits' normal horizontal FOV. */
#define TIE_COCKPIT_EDITOR_FOV_DEG 88.0f
#define TIE_COCKPIT_EDITOR_PAN_PER_PX 0.05f /* OPT units the cockpit slides per cockpit px dragged */
#define TIE_COCKPIT_EDITOR_PAN_MAX 20.0f   /* OPT units */
/* The cockpit view handle, in the top-right corner of the screen (cockpit px). */
#define TIE_COCKPIT_EDITOR_VIEW_W 70.0f
#define TIE_COCKPIT_EDITOR_VIEW_H 18.0f
#define TIE_COCKPIT_EDITOR_PLAY_GAP 6.0f /* PLAY button sits this far under the view handle */
/* Right-click menu and edge handles, cockpit px. */
#define TIE_COCKPIT_EDITOR_MENU_W 96.0f
#define TIE_COCKPIT_EDITOR_MENU_ITEM_H 11.0f
#define TIE_COCKPIT_EDITOR_HANDLE 3.0f /* half size of an edge handle */
#define TIE_COCKPIT_EDITOR_OBJECTIVES_WIDTH 220.0f

#define TIE_COCKPIT_EDITOR_FOV_STEP_DEG 2.0f /* per mouse-wheel notch */
#define TIE_COCKPIT_EDITOR_SCALE_STEP 1.05f  /* HUD group scale per notch */
#define TIE_COCKPIT_EDITOR_ANGLE_STEP_DEG 5.0f /* HUD group rotation per Shift+notch */

typedef struct TieCockpitEditorRange {
	int first, last; /* last < first marks an unused slot */
} TieCockpitEditorRange;

/* Instruments that move together, grouped by where the original cockpits
 * place them. The gunsight stays fixed so it keeps marking where the lasers
 * fire; damage cracks and subsystem covers belong to the cockpit art and are
 * not drawn over a TFTC cockpit. */
typedef struct TieCockpitEditorGroup {
	TieCockpitEditorRange ranges[3];
} TieCockpitEditorGroup;

#define TIE_COCKPIT_EDITOR_NONE { 0, -1 }

static const TieCockpitEditorGroup k_groups[] = {
	/* Targeting computer: the 3D view with its readouts around it. */
	{ { { TIE_HUDI_CMD_3D_CRT, TIE_HUDI_CMD_3D_CRT },
		{ TIE_HUDI_TARGET_SUBSYSTEM_PCT, TIE_HUDI_TARGET_SUBSYSTEM_FOCUS },
		{ 79, 90 } } },
	{ { { TIE_HUDI_MISSILE_HP_FIRST, TIE_HUDI_MISSILE_AMMO_LAST },
		{ TIE_HUDI_WEAPON_FIRE_FIRST, TIE_HUDI_WEAPON_FIRE_LAST },
		TIE_COCKPIT_EDITOR_NONE } },
	{ { { TIE_HUDI_SHIELD_FWD_NORMAL, TIE_HUDI_HULL_DAMAGE_LEVER }, TIE_COCKPIT_EDITOR_NONE,
		TIE_COCKPIT_EDITOR_NONE } },
	{ { { TIE_HUDI_SPEED_DIGITS, TIE_HUDI_THROTTLE_DIGITS }, TIE_COCKPIT_EDITOR_NONE, TIE_COCKPIT_EDITOR_NONE } },
	{ { { TIE_HUDI_REC_LED, TIE_HUDI_REC_PCT }, TIE_COCKPIT_EDITOR_NONE, TIE_COCKPIT_EDITOR_NONE } },
	{ { { TIE_HUDI_BEAM_ARC, TIE_HUDI_BEAM_ARC }, { TIE_HUDI_BEAM_FIRE, TIE_HUDI_BEAM_FIRE },
		TIE_COCKPIT_EDITOR_NONE } },
	{ { { TIE_HUDI_WARN_INCOMING, TIE_HUDI_WARN_IMPACT }, TIE_COCKPIT_EDITOR_NONE, TIE_COCKPIT_EDITOR_NONE } },
	{ { { TIE_HUDI_THREAT_SHIELD_PCT, TIE_HUDI_THREAT_HULL_PCT }, TIE_COCKPIT_EDITOR_NONE,
		TIE_COCKPIT_EDITOR_NONE } },
};
#define TIE_COCKPIT_EDITOR_GROUP_COUNT ((int)(sizeof k_groups / sizeof k_groups[0]))

static const char* const k_group_names[] = {
	"TARGETING COMPUTER", "WARHEADS", "SHIELDS AND HULL", "SPEED AND THROTTLE",
	"RECORDER",           "BEAM WEAPON", "WARNINGS",       "THREAT DISPLAY",
};
_Static_assert(sizeof k_group_names / sizeof k_group_names[0] == sizeof k_groups / sizeof k_groups[0],
			   "one name per group");

/* Any other instrument is a group of its own. */
static bool TieCockpitEditor_Fixed(int id) {
	return id == TIE_HUDI_GUNSIGHT || (id >= TIE_HUDI_DAMAGE_CRACK_FIRST && id <= TIE_HUDI_DAMAGE_CRACK_LAST) ||
		   (id >= TIE_HUDI_COVER_SHIELDS && id <= TIE_HUDI_COVER_BEAM_DOWN);
}

static void TieCockpitEditor_GroupName(int group, char* out, size_t capacity) {
	if (group < TIE_COCKPIT_EDITOR_GROUP_COUNT) {
		snprintf(out, capacity, "%s", k_group_names[group]);
		return;
	}
	const int id = group - TIE_COCKPIT_EDITOR_GROUP_COUNT;
	if (id >= TIE_HUDI_LASER_LED_FIRST && id <= TIE_HUDI_LASER_LED_LAST) {
		snprintf(out, capacity, "LASER CHARGE %d", id - TIE_HUDI_LASER_LED_FIRST + 1);
		return;
	}
	switch (id) {
		case TIE_COCKPIT_EDITOR_REPAIRS_ID: snprintf(out, capacity, "REPAIRS"); break;
		case TIE_COCKPIT_EDITOR_OBJECTIVES_ID: snprintf(out, capacity, "OBJECTIVES"); break;
		case TIE_HUDI_RADAR_LEFT: snprintf(out, capacity, "FRONT RADAR"); break;
		case TIE_HUDI_RADAR_RIGHT: snprintf(out, capacity, "REAR RADAR"); break;
		case TIE_HUDI_POWER_BALANCE: snprintf(out, capacity, "ENGINE POWER"); break;
		case TIE_HUDI_POWER_LASERS: snprintf(out, capacity, "LASER POWER"); break;
		case TIE_HUDI_POWER_SHIELDS: snprintf(out, capacity, "SHIELD POWER"); break;
		case TIE_HUDI_POWER_BEAM: snprintf(out, capacity, "BEAM POWER"); break;
		case TIE_HUDI_CLOCK_DIGITS: snprintf(out, capacity, "MISSION CLOCK"); break;
		case TIE_HUDI_THREAT_DIST_KM_INT:
		case TIE_HUDI_THREAT_DIST_KM_FRAC: snprintf(out, capacity, "THREAT DISTANCE"); break;
		case TIE_HUDI_THREAT_ION: snprintf(out, capacity, "ION THREAT"); break;
		case TIE_HUDI_THREAT_TORP: snprintf(out, capacity, "TORPEDO THREAT"); break;
		case TIE_HUDI_THREAT_MISSILE: snprintf(out, capacity, "MISSILE THREAT"); break;
		case TIE_HUDI_THREAT_BEAM: snprintf(out, capacity, "BEAM THREAT"); break;
		default: snprintf(out, capacity, "INSTRUMENT %d", id); break;
	}
}

static int TieCockpitEditor_GroupOf(int id) {
	for (int g = 0; g < TIE_COCKPIT_EDITOR_GROUP_COUNT; ++g)
		for (int r = 0; r < 3; ++r) {
			const TieCockpitEditorRange* range = &k_groups[g].ranges[r];
			if (id >= range->first && id <= range->last)
				return g;
		}
	return TieCockpitEditor_Fixed(id) ? -1 : TIE_COCKPIT_EDITOR_GROUP_COUNT + id;
}

static struct {
	AeronVfs* vfs;
	const char* model;
	float dx[TIE_COCKPIT_EDITOR_ID_COUNT];
	float dy[TIE_COCKPIT_EDITOR_ID_COUNT];
	float sx[TIE_COCKPIT_EDITOR_ID_COUNT], sy[TIE_COCKPIT_EDITOR_ID_COUNT];
	float angle[TIE_COCKPIT_EDITOR_ID_COUNT]; /* degrees, clockwise on screen */
	bool surface[TIE_COCKPIT_EDITOR_ID_COUNT];
	float pan_x, pan_y;                   /* cockpit slide right / up, OPT units */
	float fov;                            /* degrees */
	bool open;
	bool open_requested;
	bool dirty;
	int drag_group; /* -1 = none, TIE_COCKPIT_EDITOR_DRAG_VIEW = the view handle */
	float drag_x, drag_y;
	/* Input of the current frame, in render-target pixels once mapped. */
	float mouse_x, mouse_y;
	uint32_t buttons, pressed;
	int wheel;
	bool shift;
	bool help_toggled;
	bool help_hidden;
	bool objectives_wrap, objectives_headings;
	float objectives_width, objectives_height; /* on screen, cockpit px; height 0 = every row */
	/* Edge handle drag (TIE_COCKPIT_EDITOR_DRAG_RESIZE): bit 0 width, bit 1 height. */
	int resize_group, resize_axes;
	float resize_x, resize_y, resize_sx, resize_sy, resize_hw, resize_hh, resize_w, resize_h;
	int menu_group; /* box whose right-click menu is open, -1 = none */
	float menu_x, menu_y;
	bool target_requested;
	int target_wait; /* frames since the target key was sent */
} s_editor = {
	.fov = TIE_COCKPIT_EDITOR_FOV_DEG,
	.objectives_wrap = true,
	.objectives_headings = true,
	.objectives_width = TIE_COCKPIT_EDITOR_OBJECTIVES_WIDTH,
	.menu_group = -1,
	.drag_group = -1,
};

#define TIE_COCKPIT_EDITOR_DRAG_VIEW (-2)
#define TIE_COCKPIT_EDITOR_DRAG_RESIZE (-3) /* an edge handle */

static void TieCockpitEditor_ResetView(void) {
	s_editor.pan_x = s_editor.pan_y = 0.0f;
	s_editor.fov = TIE_COCKPIT_EDITOR_FOV_DEG;
}

static void TieCockpitEditor_Path(char* out, size_t capacity) {
	snprintf(out, capacity, "cockpit3d/%s.hud.yaml", s_editor.model);
}

static void TieCockpitEditor_Load(void) {
	memset(s_editor.dx, 0, sizeof s_editor.dx);
	memset(s_editor.dy, 0, sizeof s_editor.dy);
	memset(s_editor.angle, 0, sizeof s_editor.angle);
	memset(s_editor.surface, 0, sizeof s_editor.surface);
	for (int id = 0; id < TIE_COCKPIT_EDITOR_ID_COUNT; ++id)
		s_editor.sx[id] = s_editor.sy[id] = 1.0f;
	TieCockpitEditor_ResetView();
	s_editor.objectives_wrap = s_editor.objectives_headings = true;
	s_editor.objectives_width = TIE_COCKPIT_EDITOR_OBJECTIVES_WIDTH;
	s_editor.objectives_height = 0.0f;
	char path[128];
	TieCockpitEditor_Path(path, sizeof path);
	uint8_t* bytes = NULL;
	size_t size = 0;
	if (!AeronVfs_ReadAll(s_editor.vfs, AERON_VFS_ROOT_RESOURCE, path, 1u << 20, &bytes, &size) &&
		!AeronVfs_ReadAll(s_editor.vfs, AERON_VFS_ROOT_USER, path, 1u << 20, &bytes, &size))
		return;
	char* text = malloc(size + 1);
	if (!text) {
		free(bytes);
		return;
	}
	memcpy(text, bytes, size);
	text[size] = '\0';
	free(bytes);
	for (char* line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
		int id, wrap, headings, surface;
		float x, y, z, scale, scale_y, angle;
		if (sscanf(line, "objectives: { wrap: %d, headings: %d, width: %f, height: %f }", &wrap, &headings, &x,
				   &y) >= 3) {
			s_editor.objectives_wrap = wrap != 0;
			s_editor.objectives_headings = headings != 0;
			s_editor.objectives_width = x;
			if (strstr(line, "height:"))
				s_editor.objectives_height = y;
			continue;
		}
		if (sscanf(line, " - { id: %d, x: %f, y: %f, sx: %f, sy: %f, angle: %f, surface: %d }", &id, &x, &y, &scale,
				   &scale_y, &angle, &surface) == 7 &&
			id >= 0 && id < TIE_COCKPIT_EDITOR_ID_COUNT && scale > 0.0f && scale_y > 0.0f) {
			s_editor.dx[id] = x;
			s_editor.dy[id] = y;
			s_editor.sx[id] = scale;
			s_editor.sy[id] = scale_y;
			s_editor.angle[id] = angle;
			s_editor.surface[id] = surface != 0;
		} else if (sscanf(line, " - { id: %d, x: %f, y: %f, scale: %f, angle: %f }", &id, &x, &y, &scale, &angle) ==
					   5 &&
				   id >= 0 && id < TIE_COCKPIT_EDITOR_ID_COUNT && scale > 0.0f) {
			s_editor.dx[id] = x;
			s_editor.dy[id] = y;
			s_editor.sx[id] = s_editor.sy[id] = scale;
			s_editor.angle[id] = angle;
		} else if (sscanf(line, "view: { x: %f, y: %f, fov: %f }", &x, &y, &z) == 3) {
			s_editor.pan_x = x;
			s_editor.pan_y = y;
			s_editor.fov = z;
		} else if (sscanf(line, " - { id: %d, x: %f, y: %f }", &id, &x, &y) == 3 && id >= 0 &&
			id < TIE_COCKPIT_EDITOR_ID_COUNT) {
			s_editor.dx[id] = x;
			s_editor.dy[id] = y;
		}
	}
	free(text);
	/* The objectives panel scales its text evenly and never turns. */
	s_editor.sy[TIE_COCKPIT_EDITOR_OBJECTIVES_ID] = s_editor.sx[TIE_COCKPIT_EDITOR_OBJECTIVES_ID];
	s_editor.angle[TIE_COCKPIT_EDITOR_OBJECTIVES_ID] = 0.0f;
	Aeron_LogInfo("tie.cockpit.editor", "loaded HUD positions from %s", path);
}

static void TieCockpitEditor_Save(void) {
	if (!s_editor.model || !s_editor.vfs)
		return;
	char text[8192];
	size_t length = (size_t)snprintf(text, sizeof text,
									 "# HUD instrument offsets for %s, written by the cockpit editor (--cockpit-editor).\n"
									 "# View: cockpit slide in OPT units and FOV in degrees; instrument offsets in pixels of the 640x480 frame.\n"
									 "view: { x: %.2f, y: %.2f, fov: %.1f }\n"
									 "objectives: { wrap: %d, headings: %d, width: %.1f, height: %.1f }\n"
									 "instruments:\n",
									 s_editor.model, s_editor.pan_x, s_editor.pan_y, s_editor.fov,
									 s_editor.objectives_wrap ? 1 : 0, s_editor.objectives_headings ? 1 : 0,
									 s_editor.objectives_width, s_editor.objectives_height);
	for (int id = 0; id < TIE_COCKPIT_EDITOR_ID_COUNT && length < sizeof text; ++id) {
		if (s_editor.dx[id] == 0.0f && s_editor.dy[id] == 0.0f && s_editor.sx[id] == 1.0f && s_editor.sy[id] == 1.0f &&
			s_editor.angle[id] == 0.0f && !s_editor.surface[id])
			continue;
		length += (size_t)snprintf(text + length, sizeof text - length,
								   "  - { id: %d, x: %.1f, y: %.1f, sx: %.3f, sy: %.3f, angle: %.1f, surface: %d }\n", id,
								   s_editor.dx[id], s_editor.dy[id], s_editor.sx[id], s_editor.sy[id], s_editor.angle[id],
								   s_editor.surface[id] ? 1 : 0);
	}
	if (length >= sizeof text)
		length = sizeof text - 1;
	char path[128];
	TieCockpitEditor_Path(path, sizeof path);
	/* The VFS keeps the resource root read-only, but the layout belongs beside
	 * the cockpit model, so it is written there directly. */
	s_editor.dirty = false;
	const char* root = Aeron_ResourceRoot();
	const size_t root_length = root ? strlen(root) : 0;
	char full[1024];
	snprintf(full, sizeof full, "%s%s%s", root ? root : "",
			 root_length && root[root_length - 1] != '/' && root[root_length - 1] != '\\' ? "/" : "", path);
	FILE* file = root_length ? fopen(full, "wb") : NULL;
	if (file) {
		const bool ok = fwrite(text, 1, length, file) == length;
		if (fclose(file) == 0 && ok) {
			Aeron_LogInfo("tie.cockpit.editor", "saved HUD positions to %s", full);
			return;
		}
	}
	AeronVfs_CreateDirectory(s_editor.vfs, AERON_VFS_ROOT_USER, "cockpit3d");
	if (AeronVfs_WriteAllAtomic(s_editor.vfs, AERON_VFS_ROOT_USER, path, text, length))
		Aeron_LogInfo("tie.cockpit.editor", "could not write %s; saved HUD positions to the user folder as %s", full,
					  path);
	else
		Aeron_LogWarn("tie.cockpit.editor", "could not save %s", path);
}

bool TieCockpitEditor_Available(void) { return s_editor.model != NULL; }

bool TieCockpitEditor_Open(void) { return s_editor.open; }

void TieCockpitEditor_SetOpen(bool open) {
	if (open && !s_editor.model)
		return;
	if (!open && s_editor.open && s_editor.dirty)
		TieCockpitEditor_Save();
	s_editor.open = open;
	s_editor.drag_group = -1;
}

void TieCockpitEditor_SetInput(const AeronInputSnapshot* input) {
	s_editor.buttons = input ? input->mouse.buttons : 0;
	s_editor.pressed = input ? input->mouse.pressed_buttons : 0;
	s_editor.wheel = input ? input->mouse.wheel_y : 0;
	s_editor.shift = input && (input->key_down[AERON_KEY_LSHIFT] || input->key_down[AERON_KEY_RSHIFT]);
	s_editor.help_toggled = input && input->key_pressed[AERON_KEY_A + ('H' - 'A')];
	/* Logical units for now; TieCockpitEditor_Frame maps them onto the render target. */
	s_editor.mouse_x = input ? (float)input->mouse.x : 0.0f;
	s_editor.mouse_y = input ? (float)input->mouse.y : 0.0f;
}

void TieCockpitEditor_UseModel(AeronVfs* vfs, const char* model, bool settled, bool has_target) {
	if (model != s_editor.model) {
		TieCockpitEditor_SetOpen(false);
		s_editor.vfs = vfs;
		s_editor.model = model;
		if (model)
			TieCockpitEditor_Load();
	}
	if (!model || !settled || !s_editor.open_requested)
		return;
	/* Target something first ("t", next target) so the targeting computer
	 * shows a craft while the HUD is laid out; open once it has one. */
	if (!s_editor.target_requested && !has_target) {
		TieInput_EnqueueKey('t');
		s_editor.target_requested = true;
		s_editor.target_wait = 0;
		return;
	}
	if (!has_target && ++s_editor.target_wait < 60)
		return;
	s_editor.open_requested = false;
	TieCockpitEditor_SetOpen(true);
}

void TieCockpitEditor_RequestOpen(void) { s_editor.open_requested = true; }

void TieCockpitEditor_View(float* pan_x, float* pan_y, float* fov_deg) {
	*pan_x = s_editor.model ? s_editor.pan_x : 0.0f;
	*pan_y = s_editor.model ? s_editor.pan_y : 0.0f;
	*fov_deg = s_editor.model ? s_editor.fov : TIE_COCKPIT_EDITOR_FOV_DEG;
}

static float TieCockpitEditor_Clamp(float value, float lo, float hi) {
	return value < lo ? lo : value > hi ? hi : value;
}

void TieCockpitEditor_Offset(int id, float* dx, float* dy) {
	const bool set = s_editor.model && id >= 0 && id < TIE_COCKPIT_EDITOR_ID_COUNT;
	*dx = set ? s_editor.dx[id] : 0.0f;
	*dy = set ? s_editor.dy[id] : 0.0f;
}

void TieCockpitEditor_ObjectivesLayout(bool* wrap, bool* headings, float* width, float* height) {
	*wrap = s_editor.objectives_wrap;
	*headings = s_editor.objectives_headings;
	*width = s_editor.objectives_width;
	*height = s_editor.objectives_height;
}

void TieCockpitEditor_Transform(int id, float* scale_x, float* scale_y, float* angle_deg) {
	const bool set = s_editor.model && id >= 0 && id < TIE_COCKPIT_EDITOR_ID_COUNT;
	*scale_x = set ? s_editor.sx[id] : 1.0f;
	*scale_y = set ? s_editor.sy[id] : 1.0f;
	*angle_deg = set && id != TIE_COCKPIT_EDITOR_OBJECTIVES_ID ? s_editor.angle[id] : 0.0f;
}

int TieCockpitEditor_Group(int id) { return TieCockpitEditor_GroupOf(id); }

bool TieCockpitEditor_Surface(int id) {
	return s_editor.model && id >= 0 && id < TIE_COCKPIT_EDITOR_ID_COUNT && s_editor.surface[id];
}

typedef struct TieCockpitEditorBox {
	float x0, y0, x1, y1; /* cockpit px */
	int member;           /* an instrument of the group, for its scale */
	bool used;
} TieCockpitEditorBox;

enum { TIE_COCKPIT_EDITOR_BOX_COUNT = TIE_COCKPIT_EDITOR_GROUP_COUNT + TIE_COCKPIT_EDITOR_ID_COUNT };

/* Bounds of each group's active anchors, indexed by group. */
static void TieCockpitEditor_Boxes(const float* anchor_x, const float* anchor_y, const float* extent_w,
								   const float* extent_h, const bool* active, int count, TieCockpitEditorBox* boxes) {
	memset(boxes, 0, sizeof *boxes * TIE_COCKPIT_EDITOR_BOX_COUNT);
	for (int id = 0; id < count && id < TIE_COCKPIT_EDITOR_ID_COUNT; ++id) {
		const int group = active[id] ? TieCockpitEditor_GroupOf(id) : -1;
		if (group < 0)
			continue;
		TieCockpitEditorBox* box = &boxes[group];
		const float x1 = anchor_x[id] + (extent_w ? extent_w[id] : 0.0f);
		const float y1 = anchor_y[id] + (extent_h ? extent_h[id] : 0.0f);
		if (!box->used) {
			*box = (TieCockpitEditorBox) { anchor_x[id], anchor_y[id], x1, y1, id, true };
			continue;
		}
		box->x0 = anchor_x[id] < box->x0 ? anchor_x[id] : box->x0;
		box->y0 = anchor_y[id] < box->y0 ? anchor_y[id] : box->y0;
		box->x1 = x1 > box->x1 ? x1 : box->x1;
		box->y1 = y1 > box->y1 ? y1 : box->y1;
	}
}

void TieCockpitEditor_Pivots(const float* anchor_x, const float* anchor_y, const float* extent_w,
							 const float* extent_h, const bool* active, int count, float* pivot_x, float* pivot_y) {
	TieCockpitEditorBox boxes[TIE_COCKPIT_EDITOR_BOX_COUNT];
	TieCockpitEditor_Boxes(anchor_x, anchor_y, extent_w, extent_h, active, count, boxes);
	for (int id = 0; id < count && id < TIE_COCKPIT_EDITOR_ID_COUNT; ++id) {
		const int group = active[id] ? TieCockpitEditor_GroupOf(id) : -1;
		pivot_x[id] = group >= 0 ? (boxes[group].x0 + boxes[group].x1) * 0.5f : anchor_x[id];
		pivot_y[id] = group >= 0 ? (boxes[group].y0 + boxes[group].y1) * 0.5f : anchor_y[id];
	}
}

static void TieCockpitEditor_Move(int group, float dx, float dy) {
	for (int id = 0; id < TIE_COCKPIT_EDITOR_ID_COUNT; ++id)
		if (TieCockpitEditor_GroupOf(id) == group) {
			s_editor.dx[id] += dx;
			s_editor.dy[id] += dy;
		}
	s_editor.dirty = true;
}

static void TieCockpitEditor_Reset(int group) {
	for (int id = 0; id < TIE_COCKPIT_EDITOR_ID_COUNT; ++id)
		if (TieCockpitEditor_GroupOf(id) == group) {
			s_editor.dx[id] = s_editor.dy[id] = 0.0f;
			s_editor.sx[id] = s_editor.sy[id] = 1.0f;
			s_editor.angle[id] = 0.0f;
			s_editor.surface[id] = false;
		}
	s_editor.dirty = true;
}

static void TieCockpitEditor_ScaleAndTurn(int group, float factor, float degrees) {
	for (int id = 0; id < TIE_COCKPIT_EDITOR_ID_COUNT; ++id)
		if (TieCockpitEditor_GroupOf(id) == group) {
			s_editor.sx[id] = TieCockpitEditor_Clamp(s_editor.sx[id] * factor, 0.2f, 6.0f);
			s_editor.sy[id] = TieCockpitEditor_Clamp(s_editor.sy[id] * factor, 0.2f, 6.0f);
			s_editor.angle[id] += degrees;
			if (s_editor.angle[id] > 180.0f)
				s_editor.angle[id] -= 360.0f;
			if (s_editor.angle[id] <= -180.0f)
				s_editor.angle[id] += 360.0f;
		}
	s_editor.dirty = true;
}

static void TieCockpitEditor_Label(TieCockpitEditorLabel* labels, int cap, int* n, float x, float y,
								   TieCockpitEditorTextColor color, const char* text) {
	if (*n >= cap)
		return;
	TieCockpitEditorLabel* label = &labels[(*n)++];
	snprintf(label->text, sizeof label->text, "%s", text);
	label->x = x;
	label->y = y;
	label->color = color;
}

int TieCockpitEditor_Frame(AeronDrawList2D* list, const float* anchor_x, const float* anchor_y,
						   const float* extent_w, const float* extent_h, const bool* active, int count, float fit_x,
						   float fit_y, float scale_x, float scale_y, int rt_w, int rt_h,
						   TieCockpitEditorLabel* labels, int label_cap) {
	if (!s_editor.open || !list || scale_x <= 0.0f || scale_y <= 0.0f)
		return 0;
	int label_count = 0;
	if (s_editor.help_toggled)
		s_editor.help_hidden = !s_editor.help_hidden;

	enum { BOX_COUNT = TIE_COCKPIT_EDITOR_BOX_COUNT };
	TieCockpitEditorBox boxes[BOX_COUNT];
	TieCockpitEditor_Boxes(anchor_x, anchor_y, extent_w, extent_h, active, count, boxes);
	/* A scaled group draws larger or smaller about its centre; so does its handle. */
	for (int b = 0; b < BOX_COUNT; ++b) {
		TieCockpitEditorBox* box = &boxes[b];
		if (!box->used)
			continue;
		const float s_x = s_editor.sx[box->member], s_y = s_editor.sy[box->member];
		const float cx = (box->x0 + box->x1) * 0.5f, cy = (box->y0 + box->y1) * 0.5f;
		const float hw = ((box->x1 - box->x0) * 0.5f + TIE_COCKPIT_EDITOR_PAD) * s_x - TIE_COCKPIT_EDITOR_PAD;
		const float hh = ((box->y1 - box->y0) * 0.5f + TIE_COCKPIT_EDITOR_PAD) * s_y - TIE_COCKPIT_EDITOR_PAD;
		box->x0 = cx - hw;
		box->x1 = cx + hw;
		box->y0 = cy - hh;
		box->y1 = cy + hh;
	}

	/* Mouse in cockpit px, through the flight render target's place in the logical frame. */
	float mx = -1.0e6f, my = -1.0e6f;
	const TiePresentationLayout* layout = TiePresentation_Layout();
	if (layout && layout->modern.width > 0 && layout->modern.height > 0) {
		const float rt_x = (s_editor.mouse_x - (float)layout->modern.x) * (float)rt_w / (float)layout->modern.width;
		const float rt_y = (s_editor.mouse_y - (float)layout->modern.y) * (float)rt_h / (float)layout->modern.height;
		mx = (rt_x - fit_x) / scale_x;
		my = (rt_y - fit_y) / scale_y;
	}

	/* The smallest box under the cursor wins, so nested groups stay reachable. */
	int hover = -1;
	float hover_area = 0.0f;
	for (int b = 0; b < BOX_COUNT; ++b) {
		const TieCockpitEditorBox* box = &boxes[b];
		if (!box->used || mx < box->x0 - TIE_COCKPIT_EDITOR_PAD || mx > box->x1 + TIE_COCKPIT_EDITOR_PAD ||
			my < box->y0 - TIE_COCKPIT_EDITOR_PAD || my > box->y1 + TIE_COCKPIT_EDITOR_PAD)
			continue;
		const float area = (box->x1 - box->x0 + 1.0f) * (box->y1 - box->y0 + 1.0f);
		if (hover < 0 || area < hover_area) {
			hover = b;
			hover_area = area;
		}
	}

	/* The cockpit view has its own handle so the HUD cannot move it by accident. */
	const float view_x = ((float)rt_w - 12.0f - fit_x) / scale_x - TIE_COCKPIT_EDITOR_VIEW_W;
	const float view_y = (12.0f - fit_y) / scale_y;
	const bool on_view = mx >= view_x && mx <= view_x + TIE_COCKPIT_EDITOR_VIEW_W && my >= view_y &&
						 my <= view_y + TIE_COCKPIT_EDITOR_VIEW_H;
	const float play_y = view_y + TIE_COCKPIT_EDITOR_VIEW_H + TIE_COCKPIT_EDITOR_PLAY_GAP;
	const bool on_play = mx >= view_x && mx <= view_x + TIE_COCKPIT_EDITOR_VIEW_W && my >= play_y &&
						 my <= play_y + TIE_COCKPIT_EDITOR_VIEW_H;
	if (on_view || on_play)
		hover = -1;
	/* PLAY hands the game back to try the layout; F12 returns to the editor. */
	if ((s_editor.pressed & AERON_MOUSE_BUTTON_LEFT) && on_play) {
		TieCockpitEditor_SetOpen(false);
		return 0;
	}

	/* Right-click menu: the next click picks an item or closes it. */
	const int objectives_group = TIE_COCKPIT_EDITOR_GROUP_COUNT + TIE_COCKPIT_EDITOR_OBJECTIVES_ID;
	enum { MENU_WRAP, MENU_HEADINGS, MENU_SURFACE, MENU_RESET_SIZE, MENU_RESET };
	int menu_items[5], menu_count = 0, menu_hover = -1;
	const int menu_member = s_editor.menu_group >= 0 && boxes[s_editor.menu_group].used
								? boxes[s_editor.menu_group].member
								: -1;
	if (s_editor.menu_group >= 0) {
		if (s_editor.menu_group == objectives_group) {
			menu_items[menu_count++] = MENU_WRAP;
			menu_items[menu_count++] = MENU_HEADINGS;
		}
		menu_items[menu_count++] = MENU_SURFACE;
		menu_items[menu_count++] = MENU_RESET_SIZE;
		menu_items[menu_count++] = MENU_RESET;
		if (mx >= s_editor.menu_x && mx <= s_editor.menu_x + TIE_COCKPIT_EDITOR_MENU_W && my >= s_editor.menu_y)
			menu_hover = (int)((my - s_editor.menu_y) / TIE_COCKPIT_EDITOR_MENU_ITEM_H);
		if (menu_hover >= menu_count)
			menu_hover = -1;
		if (s_editor.pressed & (AERON_MOUSE_BUTTON_LEFT | AERON_MOUSE_BUTTON_RIGHT)) {
			if ((s_editor.pressed & AERON_MOUSE_BUTTON_LEFT) && menu_hover >= 0) {
				if (menu_items[menu_hover] == MENU_WRAP)
					s_editor.objectives_wrap = !s_editor.objectives_wrap;
				else if (menu_items[menu_hover] == MENU_HEADINGS)
					s_editor.objectives_headings = !s_editor.objectives_headings;
				else if (menu_items[menu_hover] == MENU_SURFACE && menu_member >= 0) {
					const bool on = !s_editor.surface[menu_member];
					for (int id = 0; id < TIE_COCKPIT_EDITOR_ID_COUNT; ++id)
						if (TieCockpitEditor_GroupOf(id) == s_editor.menu_group)
							s_editor.surface[id] = on;
				} else if (menu_items[menu_hover] == MENU_RESET_SIZE) {
					for (int id = 0; id < TIE_COCKPIT_EDITOR_ID_COUNT; ++id)
						if (TieCockpitEditor_GroupOf(id) == s_editor.menu_group)
							s_editor.sx[id] = s_editor.sy[id] = 1.0f;
					if (s_editor.menu_group == objectives_group) {
						s_editor.objectives_width = TIE_COCKPIT_EDITOR_OBJECTIVES_WIDTH;
						s_editor.objectives_height = 0.0f;
					}
				} else
					TieCockpitEditor_Reset(s_editor.menu_group);
				TieCockpitEditor_Save();
			}
			s_editor.menu_group = -1;
			s_editor.pressed = 0;
		}
		hover = -1;
	}

	/* Edge handles of the box under the cursor (or being stretched): right edge
	 * = width, bottom edge = height, corner = both. On the objectives panel
	 * they set its wrap width and visible height instead of stretching it. */
	const int handle_group = s_editor.drag_group == TIE_COCKPIT_EDITOR_DRAG_RESIZE ? s_editor.resize_group : hover;
	float handle_x[3] = { 0 }, handle_y[3] = { 0 };
	int on_handle = -1; /* 0 right, 1 bottom, 2 corner */
	if (handle_group >= 0 && boxes[handle_group].used && s_editor.menu_group < 0) {
		const TieCockpitEditorBox* box = &boxes[handle_group];
		const float right = box->x1 + TIE_COCKPIT_EDITOR_PAD, bottom = box->y1 + TIE_COCKPIT_EDITOR_PAD;
		const float mid_x = (box->x0 + box->x1) * 0.5f, mid_y = (box->y0 + box->y1) * 0.5f;
		handle_x[0] = right;
		handle_y[0] = mid_y;
		handle_x[1] = mid_x;
		handle_y[1] = bottom;
		handle_x[2] = right;
		handle_y[2] = bottom;
		for (int h = 2; h >= 0 && on_handle < 0; --h)
			if (mx >= handle_x[h] - TIE_COCKPIT_EDITOR_HANDLE && mx <= handle_x[h] + TIE_COCKPIT_EDITOR_HANDLE &&
				my >= handle_y[h] - TIE_COCKPIT_EDITOR_HANDLE && my <= handle_y[h] + TIE_COCKPIT_EDITOR_HANDLE)
				on_handle = h;
	}
	if ((s_editor.pressed & AERON_MOUSE_BUTTON_LEFT) && on_handle >= 0) {
		const TieCockpitEditorBox* box = &boxes[handle_group];
		s_editor.drag_group = TIE_COCKPIT_EDITOR_DRAG_RESIZE;
		s_editor.resize_group = handle_group;
		s_editor.resize_axes = on_handle == 0 ? 1 : on_handle == 1 ? 2 : 3;
		s_editor.resize_x = mx;
		s_editor.resize_y = my;
		s_editor.resize_sx = s_editor.sx[box->member];
		s_editor.resize_sy = s_editor.sy[box->member];
		s_editor.resize_hw = (box->x1 - box->x0) * 0.5f + TIE_COCKPIT_EDITOR_PAD;
		s_editor.resize_hh = (box->y1 - box->y0) * 0.5f + TIE_COCKPIT_EDITOR_PAD;
		s_editor.resize_w = s_editor.objectives_width;
		s_editor.resize_h = s_editor.objectives_height > 0.0f ? s_editor.objectives_height : box->y1 - box->y0;
		s_editor.pressed &= ~(uint32_t)AERON_MOUSE_BUTTON_LEFT;
	}
	if (s_editor.drag_group == TIE_COCKPIT_EDITOR_DRAG_RESIZE && (s_editor.buttons & AERON_MOUSE_BUTTON_LEFT)) {
		const float drag_x = mx - s_editor.resize_x, drag_y = my - s_editor.resize_y;
		if (s_editor.resize_group == objectives_group) {
			if (s_editor.resize_axes & 1)
				s_editor.objectives_width = TieCockpitEditor_Clamp(s_editor.resize_w + drag_x, 60.0f, 500.0f);
			if (s_editor.resize_axes & 2)
				s_editor.objectives_height = TieCockpitEditor_Clamp(s_editor.resize_h + drag_y, 12.0f, 480.0f);
		} else {
			/* Stretching about the centre: the dragged edge follows the cursor. */
			const float new_sx = s_editor.resize_sx * (s_editor.resize_hw + drag_x) / s_editor.resize_hw;
			const float new_sy = s_editor.resize_sy * (s_editor.resize_hh + drag_y) / s_editor.resize_hh;
			for (int id = 0; id < TIE_COCKPIT_EDITOR_ID_COUNT; ++id) {
				if (TieCockpitEditor_GroupOf(id) != s_editor.resize_group)
					continue;
				if (s_editor.resize_axes & 1)
					s_editor.sx[id] = TieCockpitEditor_Clamp(new_sx, 0.2f, 6.0f);
				if (s_editor.resize_axes & 2)
					s_editor.sy[id] = TieCockpitEditor_Clamp(new_sy, 0.2f, 6.0f);
			}
		}
		s_editor.dirty = true;
	}

	if (on_handle >= 0)
		hover = handle_group;
	if (s_editor.pressed & AERON_MOUSE_BUTTON_LEFT) {
		s_editor.drag_group = on_view ? TIE_COCKPIT_EDITOR_DRAG_VIEW : hover;
		s_editor.drag_x = mx;
		s_editor.drag_y = my;
	} else if (!(s_editor.buttons & AERON_MOUSE_BUTTON_LEFT)) {
		s_editor.drag_group = -1;
		if (s_editor.dirty)
			TieCockpitEditor_Save();
	}
	/* Dragging the view handle slides the cockpit (no rotation); its wheel zooms. */
	if (s_editor.drag_group == TIE_COCKPIT_EDITOR_DRAG_VIEW && (mx != s_editor.drag_x || my != s_editor.drag_y)) {
		s_editor.pan_x = TieCockpitEditor_Clamp(s_editor.pan_x + (mx - s_editor.drag_x) * TIE_COCKPIT_EDITOR_PAN_PER_PX,
												-TIE_COCKPIT_EDITOR_PAN_MAX, TIE_COCKPIT_EDITOR_PAN_MAX);
		s_editor.pan_y = TieCockpitEditor_Clamp(s_editor.pan_y - (my - s_editor.drag_y) * TIE_COCKPIT_EDITOR_PAN_PER_PX,
												-TIE_COCKPIT_EDITOR_PAN_MAX, TIE_COCKPIT_EDITOR_PAN_MAX);
		s_editor.drag_x = mx;
		s_editor.drag_y = my;
		s_editor.dirty = true;
	}
	/* The wheel over a handle scales its group, or with Shift rotates it
	 * (the objectives panel only scales its text). */
	if (s_editor.wheel && hover >= 0) {
		if (s_editor.shift && hover != objectives_group)
			TieCockpitEditor_ScaleAndTurn(hover, 1.0f, (float)s_editor.wheel * TIE_COCKPIT_EDITOR_ANGLE_STEP_DEG);
		else
			TieCockpitEditor_ScaleAndTurn(hover, powf(TIE_COCKPIT_EDITOR_SCALE_STEP, (float)s_editor.wheel), 0.0f);
		TieCockpitEditor_Save();
	} else if (s_editor.wheel && on_view && s_editor.menu_group < 0) {
		s_editor.fov = TieCockpitEditor_Clamp(s_editor.fov - (float)s_editor.wheel * TIE_COCKPIT_EDITOR_FOV_STEP_DEG,
											  40.0f, 130.0f);
		TieCockpitEditor_Save();
	}
	if (s_editor.drag_group >= 0 && (mx != s_editor.drag_x || my != s_editor.drag_y)) {
		TieCockpitEditor_Move(s_editor.drag_group, mx - s_editor.drag_x, my - s_editor.drag_y);
		TieCockpitEditorBox* box = &boxes[s_editor.drag_group];
		box->x0 += mx - s_editor.drag_x;
		box->x1 += mx - s_editor.drag_x;
		box->y0 += my - s_editor.drag_y;
		box->y1 += my - s_editor.drag_y;
		s_editor.drag_x = mx;
		s_editor.drag_y = my;
	}
	if ((s_editor.pressed & AERON_MOUSE_BUTTON_RIGHT) && on_view) {
		TieCockpitEditor_ResetView();
		TieCockpitEditor_Save();
	} else if ((s_editor.pressed & AERON_MOUSE_BUTTON_RIGHT) && hover >= 0) {
		s_editor.menu_group = hover;
		s_editor.menu_x = mx;
		s_editor.menu_y = my;
	}

	/* Editor frame around the screen, then one handle per group. */
	static const float k_frame[4] = { 0.9f, 0.6f, 0.0f, 0.9f };
	AeronDrawList_AddFrame(list, 1.0f, 1.0f, (float)rt_w - 2.0f, (float)rt_h - 2.0f, 4.0f, k_frame,
						   AERON_BLIT2D_BLEND_PMA, NULL);
	const int hot_group = s_editor.drag_group >= 0 ? s_editor.drag_group : s_editor.drag_group < -1 ? -1 : hover;
	for (int b = 0; b < BOX_COUNT; ++b) {
		const TieCockpitEditorBox* box = &boxes[b];
		if (!box->used)
			continue;
		const bool hot = b == hot_group;
		char name[40];
		TieCockpitEditor_GroupName(b, name, sizeof name);
		TieCockpitEditor_Label(labels, label_cap, &label_count, box->x0 - TIE_COCKPIT_EDITOR_PAD,
							   box->y0 - TIE_COCKPIT_EDITOR_PAD - 15.0f * TIE_COCKPIT_EDITOR_TEXT_SCALE,
							   hot ? TIE_COCKPIT_EDITOR_TEXT_HOT : TIE_COCKPIT_EDITOR_TEXT_PLAIN, name);
		const float x = fit_x + (box->x0 - TIE_COCKPIT_EDITOR_PAD) * scale_x;
		const float y = fit_y + (box->y0 - TIE_COCKPIT_EDITOR_PAD) * scale_y;
		const float w = (box->x1 - box->x0 + 2.0f * TIE_COCKPIT_EDITOR_PAD) * scale_x;
		const float h = (box->y1 - box->y0 + 2.0f * TIE_COCKPIT_EDITOR_PAD) * scale_y;
		const float fill[4] = { 0.0f, hot ? 0.25f : 0.08f, hot ? 0.35f : 0.12f, hot ? 0.35f : 0.12f };
		const float edge[4] = { 0.0f, hot ? 1.0f : 0.6f, hot ? 1.0f : 0.8f, 1.0f };
		AeronDrawList_AddFill(list, x, y, w, h, fill, AERON_BLIT2D_BLEND_PMA, NULL);
		AeronDrawList_AddFrame(list, x, y, w, h, hot ? 3.0f : 2.0f, edge, AERON_BLIT2D_BLEND_PMA, NULL);
	}

	for (int h = 0; h < 3 && handle_group >= 0 && boxes[handle_group].used && s_editor.menu_group < 0; ++h) {
		const bool hot = h == on_handle || (s_editor.drag_group == TIE_COCKPIT_EDITOR_DRAG_RESIZE &&
											s_editor.resize_axes == (h == 0 ? 1 : h == 1 ? 2 : 3));
		const float handle[4] = { 0.1f, hot ? 1.0f : 0.7f, 0.1f, 1.0f };
		AeronDrawList_AddFill(list, fit_x + (handle_x[h] - TIE_COCKPIT_EDITOR_HANDLE) * scale_x,
							  fit_y + (handle_y[h] - TIE_COCKPIT_EDITOR_HANDLE) * scale_y,
							  2.0f * TIE_COCKPIT_EDITOR_HANDLE * scale_x, 2.0f * TIE_COCKPIT_EDITOR_HANDLE * scale_y,
							  handle, AERON_BLIT2D_BLEND_PMA, NULL);
	}
	if (s_editor.menu_group >= 0) {
		static const float k_menu[4] = { 0.02f, 0.03f, 0.05f, 0.92f };
		static const float k_menu_edge[4] = { 0.0f, 0.8f, 1.0f, 1.0f };
		static const float k_menu_hot[4] = { 0.0f, 0.3f, 0.4f, 0.6f };
		const float x = fit_x + s_editor.menu_x * scale_x, y = fit_y + s_editor.menu_y * scale_y;
		const float w = TIE_COCKPIT_EDITOR_MENU_W * scale_x, item_h = TIE_COCKPIT_EDITOR_MENU_ITEM_H * scale_y;
		AeronDrawList_AddFill(list, x, y, w, item_h * (float)menu_count, k_menu, AERON_BLIT2D_BLEND_PMA, NULL);
		if (menu_hover >= 0)
			AeronDrawList_AddFill(list, x, y + item_h * (float)menu_hover, w, item_h, k_menu_hot,
								  AERON_BLIT2D_BLEND_PMA, NULL);
		AeronDrawList_AddFrame(list, x, y, w, item_h * (float)menu_count, 2.0f, k_menu_edge, AERON_BLIT2D_BLEND_PMA,
							   NULL);
		for (int i = 0; i < menu_count; ++i) {
			const bool surface = menu_member >= 0 && s_editor.surface[menu_member];
			const char* text = menu_items[i] == MENU_WRAP
								   ? (s_editor.objectives_wrap ? "WORD WRAP: ON" : "WORD WRAP: OFF")
							   : menu_items[i] == MENU_HEADINGS
								   ? (s_editor.objectives_headings ? "HEADINGS: ON" : "HEADINGS: OFF")
							   : menu_items[i] == MENU_SURFACE ? (surface ? "MATCH SURFACE: ON" : "MATCH SURFACE: OFF")
							   : menu_items[i] == MENU_RESET_SIZE ? "RESET SIZE"
																  : "RESET ALL";
			TieCockpitEditor_Label(labels, label_cap, &label_count, s_editor.menu_x + 4.0f,
								   s_editor.menu_y + (float)i * TIE_COCKPIT_EDITOR_MENU_ITEM_H + 2.0f,
								   i == menu_hover ? TIE_COCKPIT_EDITOR_TEXT_HOT : TIE_COCKPIT_EDITOR_TEXT_PLAIN, text);
		}
	}

	/* Cockpit view handle. */
	{
		const bool hot = s_editor.drag_group == TIE_COCKPIT_EDITOR_DRAG_VIEW || (s_editor.drag_group == -1 && on_view);
		const float x = fit_x + view_x * scale_x, y = fit_y + view_y * scale_y;
		const float w = TIE_COCKPIT_EDITOR_VIEW_W * scale_x, h = TIE_COCKPIT_EDITOR_VIEW_H * scale_y;
		const float fill[4] = { hot ? 0.45f : 0.25f, hot ? 0.3f : 0.15f, 0.0f, hot ? 0.45f : 0.25f };
		const float edge[4] = { 1.0f, hot ? 0.8f : 0.6f, 0.0f, 1.0f };
		AeronDrawList_AddFill(list, x, y, w, h, fill, AERON_BLIT2D_BLEND_PMA, NULL);
		AeronDrawList_AddFrame(list, x, y, w, h, hot ? 3.0f : 2.0f, edge, AERON_BLIT2D_BLEND_PMA, NULL);
		TieCockpitEditor_Label(labels, label_cap, &label_count, view_x + 4.0f, view_y + 3.0f,
							   hot ? TIE_COCKPIT_EDITOR_TEXT_HOT : TIE_COCKPIT_EDITOR_TEXT_PLAIN, "COCKPIT VIEW");
	}

	/* PLAY button. */
	{
		const float x = fit_x + view_x * scale_x, y = fit_y + play_y * scale_y;
		const float w = TIE_COCKPIT_EDITOR_VIEW_W * scale_x, h = TIE_COCKPIT_EDITOR_VIEW_H * scale_y;
		const float fill[4] = { 0.0f, on_play ? 0.45f : 0.25f, 0.0f, on_play ? 0.45f : 0.25f };
		const float edge[4] = { 0.2f, 1.0f, 0.2f, 1.0f };
		AeronDrawList_AddFill(list, x, y, w, h, fill, AERON_BLIT2D_BLEND_PMA, NULL);
		AeronDrawList_AddFrame(list, x, y, w, h, on_play ? 3.0f : 2.0f, edge, AERON_BLIT2D_BLEND_PMA, NULL);
		TieCockpitEditor_Label(labels, label_cap, &label_count, view_x + 4.0f, play_y + 3.0f,
							   on_play ? TIE_COCKPIT_EDITOR_TEXT_HOT : TIE_COCKPIT_EDITOR_TEXT_PLAIN,
							   "PLAY (F12: EDIT)");
	}

	/* Help panel in the top-left corner of the screen. */
	static const char* const k_help[] = {
		"DRAG A BOX: MOVE IT",
		"WHEEL ON A BOX: SCALE IT",
		"SHIFT + WHEEL ON A BOX: ROTATE IT",
		"GREEN HANDLES ON A BOX: WIDTH, HEIGHT, BOTH",
		"RIGHT-CLICK A BOX: OPTIONS (SURFACE, RESET...)",
		"DRAG COCKPIT VIEW (TOP RIGHT): MOVE THE COCKPIT",
		"WHEEL ON COCKPIT VIEW: ZOOM",
		"RIGHT-CLICK COCKPIT VIEW: RESET THE VIEW",
		"PLAY: TRY THE LAYOUT. F12: BACK TO THE EDITOR",
		"CHANGES SAVE AS YOU GO. H: HIDE HELP",
	};
	const int help_lines = s_editor.help_hidden ? 0 : (int)(sizeof k_help / sizeof k_help[0]);
	const float line_h = 15.0f * TIE_COCKPIT_EDITOR_TEXT_SCALE; /* cockpit px */
	const float left = (12.0f - fit_x) / scale_x, top = (12.0f - fit_y) / scale_y + 2.0f;
	const int lines = 3 + help_lines;
	static const float k_panel[4] = { 0.0f, 0.0f, 0.0f, 0.7f };
	AeronDrawList_AddFill(list, fit_x + (left - 4.0f) * scale_x, fit_y + (top - 4.0f) * scale_y, 140.0f * scale_x,
						  ((float)lines * line_h + 6.0f) * scale_y, k_panel, AERON_BLIT2D_BLEND_PMA, NULL);
	float y = top;
	TieCockpitEditor_Label(labels, label_cap, &label_count, left, y, TIE_COCKPIT_EDITOR_TEXT_HEADING,
						   s_editor.help_hidden ? "COCKPIT EDITOR. H: SHOW HELP" : "COCKPIT EDITOR");
	y += line_h;
	for (int i = 0; i < help_lines; ++i, y += line_h)
		TieCockpitEditor_Label(labels, label_cap, &label_count, left, y, TIE_COCKPIT_EDITOR_TEXT_PLAIN, k_help[i]);
	char status[48];
	snprintf(status, sizeof status, "VIEW: X %.1f  Y %.1f  FOV %.0f", s_editor.pan_x, s_editor.pan_y,
			 s_editor.fov);
	TieCockpitEditor_Label(labels, label_cap, &label_count, left, y, TIE_COCKPIT_EDITOR_TEXT_PLAIN, status);
	y += line_h;
	if (hot_group >= 0 && boxes[hot_group].used) {
		char name[40];
		TieCockpitEditor_GroupName(hot_group, name, sizeof name);
		const int member = boxes[hot_group].member;
		if (hot_group == objectives_group)
			snprintf(status, sizeof status, "%s: TEXT %.2f  W %.0f  H %.0f", name, s_editor.sx[member],
					 s_editor.objectives_width, s_editor.objectives_height);
		else
			snprintf(status, sizeof status, "%s: W %.2f  H %.2f  ANGLE %.0f", name, s_editor.sx[member],
					 s_editor.sy[member], s_editor.angle[member]);
	} else if (on_play) {
		snprintf(status, sizeof status, "PLAY: RESUME THE GAME");
	} else if (on_view) {
		snprintf(status, sizeof status, "COCKPIT VIEW: DRAG, WHEEL OR RIGHT-CLICK");
	} else {
		snprintf(status, sizeof status, "POINT AT A BOX TO EDIT IT");
	}
	TieCockpitEditor_Label(labels, label_cap, &label_count, left, y, TIE_COCKPIT_EDITOR_TEXT_HOT, status);
	return label_count;
}
