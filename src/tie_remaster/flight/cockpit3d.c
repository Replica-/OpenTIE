/*
 * 3D cockpit model drawn around the camera in place of the cockpit art when
 * the 3D cockpit option is on. Models are XWA-style cockpit OPTs loaded from
 * resources/cockpit3d/<Name>Cockpit.opt, chosen by the cockpit view's craft.
 * A view with no matching model falls back to the cockpit art.
 */

#include "aeron/asset/opt_model.h"
#include "aeron/log.h"
#include "aeron/render.h"
#include "aeron/scene/mesh.h"
#include "aeron/vfs.h"
#include "opt.h"
#include "tie_remaster/flight/cockpit/editor.h"
#include "tie_remaster/flight/render_math.h"
#include "tie_remaster/flight/renderer_internal.h"
#include "tie_runtime/presentation/presentation.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* How far the view is raised inside the cockpit. */
#define TIE_FLIGHT_COCKPIT3D_PITCH_DOWN_DEG 6.0f

typedef struct TieFlightCockpit3dModel {
	const char* view_prefix; /* PanelViewDef.name prefix, uppercase */
	const char* model;       /* basename under resources/cockpit3d/ */
} TieFlightCockpit3dModel;

static const TieFlightCockpit3dModel k_cockpit3d_models[] = {
	{ "TIEFTR", "TieFighterCockpit" },	  { "TIEINT", "TieInterceptorCockpit" },
	{ "TIEBMB", "TieBomberCockpit" },	  { "TIEADV", "TieAdvancedX1Cockpit" },
	{ "TIEDLX", "TieDefenderCockpit" },	  { "GUNBT", "AssaultGunboatCockpit" },
	{ "MISSBT", "MissileBoatCockpit" },	  { "MISSUP", "MissileBoatCockpit" },
};

static const char* TieFlightCockpit3d_ModelForView(const char* view_name) {
	for (size_t i = 0; i < sizeof k_cockpit3d_models / sizeof k_cockpit3d_models[0]; ++i) {
		const char* prefix = k_cockpit3d_models[i].view_prefix;
		size_t n = strlen(prefix);
		size_t j = 0;
		for (; j < n && view_name[j]; ++j) {
			char c = view_name[j];
			if (c >= 'a' && c <= 'z')
				c = (char)(c - 32);
			if (c != prefix[j])
				break;
		}
		if (j == n)
			return k_cockpit3d_models[i].model;
	}
	return NULL;
}

/* The camera rides the craft whose own mesh SceneSubmit hides. The first
 * flight frame still carries an unset (identity) camera orientation. */
static const TieFlightObjectState* TieFlightCockpit3d_RiddenCraft(const TieSnapshot* curr) {
	if (curr->camera.zoom_active != 0 || curr->replay_mode == 2 || !curr->cockpit.panel_loaded ||
		curr->flight_frame == 0)
		return NULL;
	for (uint16_t i = 0; i < curr->flight_count; ++i) {
		if (curr->flights[i].slot == curr->camera.target_obj_slot)
			return &curr->flights[i];
	}
	return NULL;
}

void TieFlightCockpit3d_Release(TieFlightRenderer* g) {
	for (uint32_t i = 0; i < g->cockpit3d_mesh_count; ++i)
		AeronScene_MeshDestroy(g->cockpit3d_meshes[i]);
	g->cockpit3d_mesh_count = 0;
	g->cockpit3d_model = NULL;
	g->cockpit3d_active = false;
}

static bool TieFlightCockpit3d_BuildChunk(TieFlightRenderer* g, AeronCommandBuffer* cmd, const uint8_t* bytes,
										  size_t size, const char* path, int first, int count) {
	AeronFlightModel built = { 0 };
	AeronOptModelError error = { 0 };
	if (!Aeron_OptModelBuildMemory(bytes, size, path,
								   &(AeronOptModelBuildOptions) {
									   .smooth_angle_degrees = g->assets->smooth_angle_degrees,
									   .emissive_strength = 1.0f,
									   .emissive = true,
									   /* XWA cockpits carry far more texture than a ship. */
									   .max_atlas_size = 8192,
									   .mesh_first = first,
									   .mesh_count = count,
								   },
								   &built, &error))
		return false;
	AeronSceneMeshCreateStatus status = AERON_SCENE_MESH_CREATE_SUCCESS;
	AeronSceneMesh* mesh = AeronScene_MeshCreate(cmd, &built, path, &status);
	Aeron_FlightModelFree(&built);
	if (!mesh)
		return false;
	g->cockpit3d_meshes[g->cockpit3d_mesh_count++] = mesh;
	return true;
}

/* Scene meshes use 16-bit indices and an XWA cockpit converts to far more
 * vertices than that, so it is built as runs of its OPT meshes, halving a
 * run until it fits. */
static void TieFlightCockpit3d_Load(TieFlightRenderer* g, AeronCommandBuffer* cmd, const char* model) {
	TieFlightCockpit3d_Release(g);
	g->cockpit3d_model = model;

	char path[96];
	snprintf(path, sizeof path, "cockpit3d/%s.opt", model);
	uint8_t* bytes = NULL;
	size_t size = 0;
	if (!AeronVfs_ReadAll(g->assets->vfs, AERON_VFS_ROOT_RESOURCE, path, 256u * 1024u * 1024u, &bytes, &size)) {
		Aeron_LogWarn("tie.cockpit3d", "%s not found; using the cockpit art", path);
		return;
	}
	opt_error_t parse_error = { { 0 } };
	opt_file_t* opt = opt_load_memory(bytes, size, &parse_error);
	if (!opt) {
		Aeron_LogWarn("tie.cockpit3d", "%s: %s", path, parse_error.msg);
		free(bytes);
		return;
	}
	const int total = opt->mesh_count;
	opt_free(opt);

	int first = 0;
	while (first < total && g->cockpit3d_mesh_count < TIE_FLIGHT_COCKPIT3D_MAX_CHUNKS) {
		int count = total - first;
		while (!TieFlightCockpit3d_BuildChunk(g, cmd, bytes, size, path, first, count) && count > 1)
			count = (count + 1) / 2;
		if (count == 1 && g->cockpit3d_mesh_count == 0)
			Aeron_LogWarn("tie.cockpit3d", "%s: mesh %d could not be built", path, first);
		first += count;
	}
	free(bytes);
	uint32_t vertices = 0;
	for (uint32_t i = 0; i < g->cockpit3d_mesh_count; ++i)
		vertices += g->cockpit3d_meshes[i]->vertex_count;
	Aeron_LogInfo("tie.cockpit3d", "loaded %s: %d OPT meshes in %u scene meshes, %u vertices", path, total,
				  g->cockpit3d_mesh_count, vertices);
}

void TieFlightCockpit3d_Prepare(TieFlightRenderer* g, AeronCommandBuffer* cmd, const TieSnapshot* curr) {
	g->cockpit3d_active = false;
	if (!TiePresentation_Cockpit3d() || !g->scene_model_backend || !TieFlightCockpit3d_RiddenCraft(curr))
		return;
	const char* model = TieFlightCockpit3d_ModelForView(curr->cockpit.view_name);
	if (!model)
		return;
	if (model != g->cockpit3d_model)
		TieFlightCockpit3d_Load(g, cmd, model);
}

void TieFlightCockpit3d_Submit(TieFlightRenderer* g, const TieSnapshot* curr, const float cam_pos[3]) {
	const TieFlightObjectState* craft = TieFlightCockpit3d_RiddenCraft(curr);
	if (!TiePresentation_Cockpit3d() || g->cockpit3d_mesh_count == 0 || !craft ||
		TieFlightCockpit3d_ModelForView(curr->cockpit.view_name) != g->cockpit3d_model)
		return;

	/* The model origin is the pilot's eye. It turns with the craft, not the
	 * camera, so side and rear views look at the matching part of it. */
	float transform[16];
	float craft_transform[16];
	TieRenderMath_Mat4FromQuaternionTranslation(craft_transform, craft->ori, cam_pos, AERON_OPT_UNITS_PER_METER);
	/* Slide the model from the eye as the cockpit editor sets (OPT +X right,
	 * +Z up; model space is in metres), then pitch it down about the eye so
	 * the view sits higher in the cockpit while the world and reticle stay put. */
	float pan_x, pan_y, fov_deg;
	TieCockpitEditor_View(&pan_x, &pan_y, &fov_deg);
	const float pitch = TIE_FLIGHT_COCKPIT3D_PITCH_DOWN_DEG * 3.14159265f / 180.0f;
	const float cp = cosf(pitch), sp = sinf(pitch);
	const float slide_x = pan_x * AERON_OPT_METERS_PER_UNIT, slide_z = pan_y * AERON_OPT_METERS_PER_UNIT;
	const float slide_then_pitch[16] = {
		1.0f, 0.0f, 0.0f, slide_x, 0.0f, cp, -sp, 0.0f, 0.0f, sp, cp, slide_z, 0.0f, 0.0f, 0.0f, 1.0f,
	};
	TieRenderMath_Mat4Multiply(transform, craft_transform, slide_then_pitch);
	for (uint32_t i = 0; i < g->cockpit3d_mesh_count; ++i) {
		AeronSceneMeshInstance in;
		memset(&in, 0, sizeof in);
		in.mesh = g->cockpit3d_meshes[i];
		if (in.mesh->all_materials_single_sided)
			in.cull_mode = AERON_CULL_BACK;
		memcpy(in.transform, transform, sizeof in.transform);
		memcpy(in.prev_transform, transform, sizeof in.prev_transform);
		in.zero_velocity = 1;
		/* The hidden exterior hull still casts shadows and would black out
		 * the interior, and the cockpit must not shadow the world. */
		in.shadow_flags = AERON_SCENE_INSTANCE_NO_CAST_SHADOW | AERON_SCENE_INSTANCE_NO_RECEIVE_SHADOW;
		AeronScene_AddMeshInstance(g->scene, &in);
	}
	memcpy(g->cockpit3d_transform, transform, sizeof g->cockpit3d_transform);
	g->cockpit3d_active = true;
}

bool TieFlightRenderer_Cockpit3dActive(const TieFlightRenderer* g) { return g && g->cockpit3d_active; }

const char* TieFlightRenderer_Cockpit3dModel(const TieFlightRenderer* g) {
	return g && g->cockpit3d_active ? g->cockpit3d_model : NULL;
}

/* General 4x4 inverse (row-major); false when singular. */
static bool TieFlightCockpit3d_Invert(const float m[16], float out[16]) {
	float inv[16];
	inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] +
			 m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
	inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] -
			 m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
	inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] +
			 m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
	inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] -
			  m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
	inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] -
			 m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
	inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] +
			 m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
	inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] -
			 m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
	inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] +
			  m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
	inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] +
			 m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
	inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] -
			 m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
	inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] +
			  m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
	inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] -
			  m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
	inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] -
			 m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
	inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] +
			 m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
	inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] -
			  m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
	inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] +
			  m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
	const float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
	if (det == 0.0f)
		return false;
	for (int i = 0; i < 16; ++i)
		out[i] = inv[i] / det;
	return true;
}

static void TieFlightCockpit3d_Transform(const float m[16], const float p[3], float w, float out[4]) {
	for (int r = 0; r < 4; ++r)
		out[r] = m[r * 4 + 0] * p[0] + m[r * 4 + 1] * p[1] + m[r * 4 + 2] * p[2] + m[r * 4 + 3] * w;
}

void TieFlightRenderer_ProjectLocal(const TieFlightRenderer* g, const float point[3], float clip[4]) {
	TieFlightCockpit3d_Transform(g->frame.fcam.view_proj, point, 1.0f, clip);
}

bool TieFlightRenderer_ScreenRay(const TieFlightRenderer* g, float x, float y, float origin[3], float dir[3]) {
	if (!g || g->rt_w <= 0 || g->rt_h <= 0)
		return false;
	float inv[16];
	if (!TieFlightCockpit3d_Invert(g->frame.fcam.view_proj, inv))
		return false;
	/* NDC y is up; reverse-Z puts the near plane at 1. */
	const float ndc[2] = { 2.0f * x / (float)g->rt_w - 1.0f, 1.0f - 2.0f * y / (float)g->rt_h };
	float near_point[4], mid_point[4];
	TieFlightCockpit3d_Transform(inv, (const float[3]) { ndc[0], ndc[1], 1.0f }, 1.0f, near_point);
	TieFlightCockpit3d_Transform(inv, (const float[3]) { ndc[0], ndc[1], 0.5f }, 1.0f, mid_point);
	if (near_point[3] == 0.0f || mid_point[3] == 0.0f)
		return false;
	float length = 0.0f;
	for (int i = 0; i < 3; ++i) {
		origin[i] = near_point[i] / near_point[3];
		dir[i] = mid_point[i] / mid_point[3] - origin[i];
		length += dir[i] * dir[i];
	}
	length = sqrtf(length);
	if (length <= 0.0f)
		return false;
	for (int i = 0; i < 3; ++i)
		dir[i] /= length;
	return true;
}

bool TieFlightRenderer_Cockpit3dSurfaceAt(const TieFlightRenderer* g, float x, float y, float point[3],
										  float normal[3]) {
	float origin[3], dir[3];
	if (!g || !g->cockpit3d_active || !TieFlightRenderer_ScreenRay(g, x, y, origin, dir))
		return false;
	const float* m = g->cockpit3d_transform;
	float best = 1.0e30f;
	bool hit = false;
	for (uint32_t c = 0; c < g->cockpit3d_mesh_count; ++c) {
		const AeronSceneMesh* mesh = g->cockpit3d_meshes[c];
		if (!mesh->cpu_vertices || !mesh->cpu_indices)
			continue;
		for (uint32_t i = 0; i + 2 < mesh->index_count; i += 3) {
			float v[3][4];
			for (int k = 0; k < 3; ++k)
				TieFlightCockpit3d_Transform(m, mesh->cpu_vertices[mesh->cpu_indices[i + k]].pos, 1.0f, v[k]);
			/* Moller-Trumbore. */
			const float e1[3] = { v[1][0] - v[0][0], v[1][1] - v[0][1], v[1][2] - v[0][2] };
			const float e2[3] = { v[2][0] - v[0][0], v[2][1] - v[0][1], v[2][2] - v[0][2] };
			const float p[3] = { dir[1] * e2[2] - dir[2] * e2[1], dir[2] * e2[0] - dir[0] * e2[2],
								 dir[0] * e2[1] - dir[1] * e2[0] };
			const float det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
			if (fabsf(det) < 1.0e-9f)
				continue;
			const float inv_det = 1.0f / det;
			const float s[3] = { origin[0] - v[0][0], origin[1] - v[0][1], origin[2] - v[0][2] };
			const float u = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) * inv_det;
			if (u < 0.0f || u > 1.0f)
				continue;
			const float q[3] = { s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2], s[0] * e1[1] - s[1] * e1[0] };
			const float w = (dir[0] * q[0] + dir[1] * q[1] + dir[2] * q[2]) * inv_det;
			if (w < 0.0f || u + w > 1.0f)
				continue;
			const float t = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * inv_det;
			if (t <= 0.0f || t >= best)
				continue;
			best = t;
			hit = true;
			float n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
			const float n_length = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
			const float facing = n[0] * dir[0] + n[1] * dir[1] + n[2] * dir[2] > 0.0f ? -1.0f : 1.0f;
			for (int k = 0; k < 3; ++k) {
				normal[k] = n_length > 0.0f ? n[k] / n_length * facing : -dir[k];
				point[k] = origin[k] + dir[k] * t;
			}
		}
	}
	return hit;
}
