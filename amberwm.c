#define _GNU_SOURCE 1
#include <assert.h>
#include <drm_fourcc.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>
#include <sys/stat.h>
#include <sys/inotify.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_ext_data_control_v1.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/drm.h>
#include <wlr/backend/session.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_ext_workspace_v1.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_export_dmabuf_v1.h>
#include <wlr/types/wlr_ext_image_copy_capture_v1.h>
#include <wlr/types/wlr_ext_image_capture_source_v1.h>
#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_export_dmabuf_v1.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_gamma_control_v1.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/types/wlr_output_power_management_v1.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_security_context_v1.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_session_lock_v1.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_virtual_pointer_v1.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_xdg_toplevel_icon_v1.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "vendor/stb_image.h"
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
/* SceneFX provides the scene graph (drop-in wlr_scene replacement) with
 * effects: rounded corners, blur, shadows. Must replace wlr_scene.h. */
#include <scenefx/types/wlr_scene.h>
#include <scenefx/render/fx_renderer/fx_renderer.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>
#include "ext-background-effect-v1-protocol.h"

/* For brevity's sake, struct members are annotated where they are used. */
#define AMBER_WORKSPACE_COUNT 9

/* Layout tuning defaults (amberwm.cfg overrides these). */
#define AMBER_DEFAULT_GAPS 8
#define AMBER_DEFAULT_MIN_COLUMN_WIDTH 240
#define AMBER_DEFAULT_COLUMN_FRACTION 0.5

enum amber_cursor_mode {
	AMBER_CURSOR_PASSTHROUGH,
	AMBER_CURSOR_MOVE,
	AMBER_CURSOR_RESIZE,
	AMBER_CURSOR_COLUMN_RESIZE,
};

/* Per-app window rule from the config:
 *   rule=<app-id> blur=yes,no corner-radius=N
 * An empty app-id ("rule= blur=yes") matches every window. Later
 * rules override earlier ones; rules override the global settings. */
struct amber_window_rule {
	struct wl_list link;
	char *app_id; // NULL matches all
	bool has_blur;
	bool blur;
	bool has_corner;
	int corner_radius;
	bool has_opacity;
	float opacity;
};

/* Persistent IPC connection receiving state pushes (mango-IPC style). */
struct amber_ipc_watcher {
	struct wl_list link;
	int fd;
	struct wl_event_source *source;
};

enum amber_anim_kind {
	ANIM_OPEN,       // fade + slide-in of a live window
	ANIM_LAMP_CLOSE, // magic-lamp suck-in of a closing window's snapshot
	ANIM_WS_SLIDE,   // horizontal slide between two workspace trees
	ANIM_WOBBLE,     // Compiz-style spring grid over a live dragged window
	ANIM_FLOAT_TWEEN // position glide across a float/tile toggle
};

#define ANIM_TICK_MS 16 // animation timer cadence (~60 updates/sec)

/* A single event-loop timer drives every active animation at frame rate
 * and disarms itself when the list drains (no idle wakeups). */
struct amber_animation {
	struct wl_list link;
	enum amber_anim_kind kind;
	int64_t start_usec;
	uint32_t duration_ms;
	/* ANIM_OPEN: tracks a live window (canceled on unmap/moves). */
	struct amber_toplevel *toplevel;
	struct wlr_scene_tree *tree;
	int base_x, base_y; // final layout position (parent-local px)
	int from_dx;        // horizontal offset applied at progress 0
	/* ANIM_LAMP_CLOSE: owns a locked snapshot + detached strip nodes. */
	struct wlr_buffer *snapshot;   // locked at unmap, ours until done
	struct wlr_scene_buffer **strips; // live in output->fx_tree
	int strip_count;
	struct amber_output *output;
	int win_x, win_y, win_w, win_h; // window rect at unmap (output-local)
	int tgt_x, tgt_y;               // lamp target (bottom-center)
	/* ANIM_WOBBLE: spring grid over a live dragged window. Reuses
	 * strips[] (cells), strip_count, snapshot (locked live buffer),
	 * toplevel (the live window), output, and start_usec (failsafe). */
	int gx, gy, gw, gh;             // window rect at grab (output-local)
	int cols, rows;                 // grid cell dimensions
	float *px, *py, *vx, *vy;       // point state ((cols+1)*(rows+1))
	float *wf;                      // grab-falloff weight per point
	float drag_x, drag_y;           // accumulated drag offset
	float ax, ay;                   // grab point inside window
	bool wobble_released;           // button up: settle then finish
	int64_t last_usec;              // previous tick (adaptive spring dt)
	/* Last applied rect per strip/cell [x,y,w,h]*4 — skips redundant
	 * scene property writes when the int grid didn't move. */
	int *strip_rect;
	/* ANIM_WS_SLIDE: two workspace trees sliding horizontally. */
	struct amber_workspace *ws_from, *ws_to;
	int slide_dir;    // +1 = toward higher index (content moves left)
	int origin_x, origin_y; // layout origin to restore
	/* ANIM_FLOAT_TWEEN: glides a live window from from_x/from_y to the
	 * node's current position (base_x/base_y). Canceled like ANIM_OPEN
	 * via animation_cancel_for on unmap/reparent. */
	int from_x, from_y;
};

struct amber_workspace {
	/* Scene tree holding every toplevel on this workspace. Hiding a
	 * workspace is a single node disable: hidden subtrees produce no
	 * damage and cost zero render time. */
	struct wlr_scene_tree *tree;
	struct amber_output *output;

	/* Scrollable strip: tiles ordered left-to-right (floating windows
	 * live in the same list but are skipped by arrangement). */
	struct wl_list toplevels; // amber_toplevel.link
	double view_offset;       // px scrolled rightward, >= 0
};

struct amber_server {
	struct wl_display *wl_display;
	struct wlr_backend *backend;
	struct wlr_session *session;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wlr_scene *scene;
	struct wlr_scene_output_layout *scene_layout;

	/* Config (amberwm.cfg, see config.c section below). */
	char *terminal_cmd;
	int gaps;
	int min_column_width;
	double default_column_fraction;
	struct amber_binding *bindings;
	size_t binding_count;
	char **autostart; // commands spawned once the socket is up
	size_t autostart_count;

	/* Built-in wallpaper: decoded once, uploaded once, rendered as a
	 * static background scene node (zero per-frame cost). */
	enum amber_wallpaper_mode {
		AMBER_WALLPAPER_COVER,
		AMBER_WALLPAPER_CONTAIN,
		AMBER_WALLPAPER_CENTER,
		AMBER_WALLPAPER_STRETCH,
	} wallpaper_mode;
	char *wallpaper_path;
	/* Effects (SceneFX). Blur is expensive on old GPUs: default off. */
	int corner_radius;
	bool blur_enabled;
	int blur_radius;
	struct wlr_buffer *wallpaper_buffer;

	struct wlr_xdg_shell *xdg_shell;
	struct wl_listener new_xdg_toplevel;
	struct wl_listener new_xdg_popup;

	struct wlr_layer_shell_v1 *layer_shell;
	struct wl_listener new_layer_surface;

	struct wlr_xdg_decoration_manager_v1 *xdg_decoration_mgr;
	struct wl_listener new_toplevel_decoration;

	struct wl_listener cursor_shape_request;
	struct wl_listener xdg_activation_request;

	/* Foreign toplevel (taskbars/docks enumerate + control windows). */
	struct wlr_foreign_toplevel_manager_v1 *foreign_toplevel_mgr;
	/* ext-foreign-toplevel-list: second enumeration protocol some
	 * shells (Noctalia window switcher) key their models on. */
	struct wlr_ext_foreign_toplevel_list_v1 *ext_toplevel_list;

	/* ext-workspace-v1: bars like Noctalia render + switch workspaces. */
	struct wlr_ext_workspace_manager_v1 *ext_ws_mgr;
	struct wl_listener ext_ws_commit;
	struct wl_listener ext_ws_destroy;

	/* Session lock (swaylock & friends). */
	struct wlr_session_lock_v1 *active_lock;
	struct wl_list lock_surfaces; // amber_lock_surface.link
	struct wl_listener session_lock_new_lock;
	struct wl_listener session_lock_new_surface;
	struct wl_listener session_lock_unlock;
	struct wl_listener session_lock_dead;

	/* One shared xkb context for all keyboards (created once). */
	struct xkb_context *xkb_context;

	struct amber_toplevel *focused_toplevel;
	volatile bool shutting_down; // set at every wl_display_terminate site
	bool dyn_ws; // advertise only occupied/active workspaces (config)
	struct amber_output *active_output;

	struct wlr_cursor *cursor;
	struct wlr_xcursor_manager *cursor_mgr;
	char *cursor_theme; // config cursor-theme= (XCURSOR_THEME default)
	int cursor_size;    // config cursor-size= (XCURSOR_SIZE default)
	struct wl_listener cursor_motion;
	struct wl_listener cursor_motion_absolute;
	struct wl_listener cursor_button;
	struct wl_listener cursor_axis;
	struct wl_listener cursor_frame;

	/* Protocol extras: output reconfiguration (kanshi), monitor power
	 * (wlopm), raw relative motion, and virtual input devices. */
	struct wlr_relative_pointer_manager_v1 *relative_pointer_mgr;
	struct wlr_output_manager_v1 *output_manager;
	struct wl_listener output_manager_apply;
	struct wl_listener output_manager_test;
	struct wlr_output_power_manager_v1 *output_power_mgr;
	struct wl_listener output_power_set_mode;
	struct wl_listener virtual_keyboard_new;
	struct wl_listener virtual_pointer_new;

	/* Pointer constraints (games): the compositor enforces the lock /
	 * confine itself in wlroots 0.20 (no seat/cursor helpers left). */
	struct wlr_pointer_constraints_v1 *pointer_constraints_mgr;
	struct wl_listener pointer_constraints_new;
	struct wlr_pointer_constraint_v1 *active_constraint;
	double constraint_anchor_x, constraint_anchor_y; // layout coords

	struct wlr_seat *seat;
	struct wl_listener new_input;
	struct wl_listener request_cursor;
	struct wl_listener pointer_focus_change;
	struct wl_listener request_set_selection;
	struct wl_list keyboards;
	enum amber_cursor_mode cursor_mode;
	/* IPC (mango-compatible JSON-line socket): watch clients + state. */
	struct wl_list ipc_watchers; // amber_ipc_watcher.link
	struct wl_event_source *ipc_source;
	char *ipc_path;
	long ipc_next_id;
	char *last_focused_title;
	/* Config hot-reload via inotify. */
	int config_inotify_fd;
	struct wl_event_source *config_watch_source;
	char *config_path;
	bool shadows_enabled;
	bool center_focused_column;
	/* Animations. */
	bool animations_enabled;
	uint32_t animation_duration_ms;
	bool lamp_close_enabled; // KDE magic-lamp close effect
	uint32_t lamp_close_duration_ms;
	bool ws_slide_enabled; // workspace-switch horizontal slide
	uint32_t ws_slide_duration_ms;
	bool wobbly_windows; // Compiz-style spring grid on drag
	struct wl_list animations; // amber_animation.link
	struct wl_event_source *anim_source;
	bool anim_timer_armed;
	/* Idle CPU/GPU throttle: when the desktop has been idle for
	 * idle-throttle-seconds (and nothing is playing video via
	 * idle-inhibit), drop output refresh from 60Hz to a low rate to
	 * cut GPU/CPU draw cost. Wakes back to 60Hz on any input or when
	 * media starts. */
	bool idle_throttle_enabled;
	int idle_throttle_seconds;   // inactivity before throttling, s
	int idle_throttle_hz;        // low refresh rate while idle, Hz
	int64_t last_input_usec;     // last key/pointer activity
	bool idle_throttled;         // currently in low-refresh state
	struct wl_event_source *idle_source;
	struct wlr_idle_inhibit_manager_v1 *idle_inhibit_mgr;
	struct wl_listener idle_inhibit_new;
	size_t idle_inhibitors;      // active video/media inhibitors
	/* Per-app window rules (config "rule=" lines). Matched by app-id;
	 * a rule with empty app_id matches every window. */
	struct wl_list window_rules; // amber_window_rule.link

	/* Built-in window switcher: static thumbnail grid over a dim
	 * layer on the active output; nodes live in the overlay tree. */
	bool sw_active;
	struct amber_output *sw_output;
	struct wlr_scene_rect *sw_dim;
	size_t sw_count, sw_selected, sw_cols;
	struct {
		struct amber_toplevel *tl;
		struct wlr_buffer *buf; // captured pre-dim, attached below
		struct wlr_scene_buffer *ring; // rounded white outline
		struct wlr_scene_buffer *thumb;
		struct wlr_scene_buffer *icon_node;
		struct wlr_scene_buffer *fallback; // rounded card
		struct wlr_box box; // output-local hit rect
	} sw_tiles[20];

	struct amber_toplevel *grabbed_toplevel;
	double grab_x, grab_y;
	struct wlr_box grab_geobox;
	uint32_t resize_edges;

	struct wlr_output_layout *output_layout;
	struct wl_list outputs;
	struct wl_listener new_output;

	/* ext-background-effect-v1 (hand-rolled; not in wlroots 0.20).
	 * Lets clients (e.g. dms panels) attach a blur region to a
	 * wl_surface; amber renders a scenefx optimized-blur behind it. */
	struct amber_extbe_manager *extbe_mgr;
};

/* Niri-style overview: a single row showing one workspace snapshot. */
struct overview_row {
	struct amber_output *output;
	int workspace; // index into output->workspaces
	struct wlr_scene_buffer *thumb;   // scaled workspace snapshot
	struct wlr_scene_buffer *ring;    // focus highlight border
	struct wlr_scene_buffer *fallback;  // empty-workspace placeholder
	struct wlr_scene_rect *box;       // dim backdrop behind snapshot
	struct wlr_box geom;              // on-screen box (for hit-testing)
	struct wlr_buffer *buf;           // captured snapshot buffer
};

struct amber_output {
	struct wl_list link;
	struct amber_server *server;
	struct wlr_output *wlr_output;
	/* Position and size of this output in layout coordinates. */
	struct wlr_box layout_box;
	/* Area not reserved by exclusive layer surfaces, local coords. */
	struct wlr_box usable_area;

	/* Children of the scene output tree, in stacking order:
	 * background < bottom < top(bar) < workspaces (windows) < overlay
	 * < fx_tree. Layer popups + fullscreen windows live in overlay. */
	struct wlr_scene_tree *layer_trees[4];
	struct wl_list layers[4]; // amber_layer_surface.link, per zwlr_layer_shell_v1_layer
	struct amber_workspace workspaces[AMBER_WORKSPACE_COUNT];
	int active_workspace;
	/* ext-workspace-v1 handles: one group + one handle per workspace,
	 * so bars can show pills and switch on click. */
	struct wlr_ext_workspace_group_handle_v1 *ext_group;
	struct wlr_ext_workspace_handle_v1 *ext_ws[AMBER_WORKSPACE_COUNT];
	struct wlr_scene_buffer *wallpaper_node;
	/* Animation overlay: lamp-close snapshots + workspace slides render
	 * here, above every layer. Created last => stacked on top. */
	struct wlr_scene_tree *fx_tree;

	/* Niri-style overview: all workspaces stacked vertically as
	 * scaled live snapshots over a dim layer. */
	bool ov_active;
	struct wlr_scene_rect *ov_dim;
	struct wlr_scene_tree *ov_tree;   // rows container
	struct overview_row *ov_rows[AMBER_WORKSPACE_COUNT];

	struct wl_listener frame;
	struct wl_listener request_state;
	struct wl_listener destroy;
};

struct amber_toplevel {
	struct wl_list link;
	struct amber_server *server;
	struct amber_output *output;
	int workspace; // index into output->workspaces
	struct wlr_xdg_toplevel *xdg_toplevel;
	struct wlr_scene_tree *scene_tree;

	/* Layout state. */
	bool floating;   // not part of the scrollable strip
	bool fullscreen; // true fullscreen: whole output, above bars
	bool maximized;  // fake fullscreen: covers usable area only
	int col_width;   // desired column width, px (tiles only)
	int sent_w, sent_h; // last configured size (skip duplicate configures)

	/* SceneFX: optimized-blur node behind this window's content, living
	 * inside the window tree so it follows moves for free. */
	struct wlr_scene_optimized_blur *blur_node;
	/* The scene node displaying this window's main surface buffer;
	 * cached at map so close animations can grab the scene's own
	 * locked copy of the last frame at unmap time. */
	struct wlr_scene_buffer *scene_buffer;
	/* Drop shadow (floating windows only; static scene node, so it is
	 * free once placed). */
	struct wlr_scene_shadow *shadow_node;
	/* Manual opacity override from a key binding; -1 = follow rules. */
	float fx_opacity;
	/* Stable numeric id exposed over IPC (mango-style client ids). */
	long ipc_id;
	int tile_x, tile_w; // arrangement cache (local coords)
	bool placed; // node coords are a trustworthy visual start (gates reflow glide)
	struct wlr_box pre_fs_box; // float geometry before fullscreen

	/* Foreign toplevel handle for bars/docks. */
	struct wlr_foreign_toplevel_handle_v1 *foreign_handle;
	struct wlr_ext_foreign_toplevel_handle_v1 *ext_handle;
	struct wl_listener foreign_activate;
	struct wl_listener foreign_close;
	char *foreign_title;

	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener destroy;
	struct wl_listener request_move;
	struct wl_listener request_resize;
	struct wl_listener request_maximize;
	struct wl_listener request_fullscreen;
};

/* Per-constraint destroy bookkeeping; lives in constraint->data. */
struct amber_constraint_trk {
	struct amber_server *server;
	struct wlr_pointer_constraint_v1 *constraint;
	struct wl_listener destroy;
};

struct amber_layer_surface {
	struct wl_list link;
	struct amber_server *server;
	struct amber_output *output;
	struct wlr_layer_surface_v1 *layer_surface;
	struct wlr_scene_layer_surface_v1 *scene_layer;
	bool mapped;

	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener destroy;
};

struct amber_popup {
	struct wlr_xdg_popup *xdg_popup;
	struct wl_listener commit;
	struct wl_listener destroy;
};

struct amber_lock_surface {
	struct wl_list link;
	struct amber_output *output;
	struct wlr_session_lock_surface_v1 *lock_surface;
	struct wlr_scene_tree *scene_tree;
	struct wl_listener destroy;
};

struct amber_keyboard {
	struct wl_list link;
	struct amber_server *server;
	struct wlr_keyboard *wlr_keyboard;
	/* Modifier bits that are locks (caps/num), stripped before matching
	 * bindings so NumLock does not break Super+key combos. */
	uint32_t lock_mask;
	/* Keycodes whose press was consumed by us (bind or VT combo);
	 * the matching releases are swallowed too, or clients see a
	 * release for a key they never received. */
	uint32_t swallowed[16];
	size_t n_swallowed;
	/* Physically-held keycodes; a PRESSED event for an already-held
	 * keycode is auto-repeat and must never re-trigger one-shot binds. */
	uint32_t held[32];
	size_t n_held;

	struct wl_listener modifiers;
	struct wl_listener key;
	struct wl_listener destroy;
};

/* ============================ ext-background-effect-v1 ================
 * Hand-rolled: wlroots-0.20 ships no implementation of this staging
 * protocol. A client binds ext_background_effect_manager_v1, asks for
 * ext_background_effect_surface_v1 on some wl_surface, and sets a
 * double-buffered blur region. On each surface commit we place a scenefx
 * wlr_scene_optimized_blur node right behind the containing surface's
 * scene tree, blurred over that region. Uses wlr_surface_synced so the
 * pending/current semantics are correct for free. */

#define EXTBE_VERSION 1

struct amber_extbe_state {
	pixman_region32_t blur_region;
};

struct amber_extbe_surface {
	struct wl_resource *resource;
	struct amber_server *server;
	struct wlr_surface *surface;
	struct wlr_addon addon;
	struct wlr_surface_synced synced;
	struct amber_extbe_state pending, current;
	/* Scene blur node behind the owning surface's tree. NULL until the
	 * first commit with a non-empty blur region. */
	struct wlr_scene_optimized_blur *blur_node;
};

struct amber_extbe_manager {
	struct wl_global *global;
	struct wl_list resources; // wl_resource link
	uint32_t capabilities;
	struct amber_server *server;
	struct wl_listener display_destroy;
};

/* text for a per-surface blur node is driven entirely from the state;
 * but scenefx rounded corners need a real box, so we park the owning
 * scene tree's current size here. */
static struct wlr_scene_tree *extbe_parent_tree(
		struct amber_server *server, struct wlr_surface *surface) {
	/* xdg toplevels: follow the xdg_surface -> scene_tree backref.
	 * wlroots nulls `data` before the toplevel is freed, so this never
	 * walks into a destroyed window (the naive workspace->toplevels scan
	 * could deref a stale ->xdg_toplevel on teardown). */
	struct wlr_xdg_toplevel *tl =
		wlr_xdg_toplevel_try_from_wlr_surface(surface);
	if (tl != NULL && tl->base->data != NULL) {
		return tl->base->data;
	}
	/* layer surfaces: their scene_layer is a stable tree handle. */
	struct amber_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		for (int b = 0; b < 4; b++) {
			struct amber_layer_surface *layer;
			wl_list_for_each(layer, &output->layers[b], link) {
				if (layer->layer_surface->surface == surface &&
						layer->scene_layer != NULL) {
					return layer->scene_layer->tree;
				}
			}
		}
	}
	return NULL;
}

/* Apply the committed blur region of es to its scenefx blur node.
 * Called from wlr_surface_synced commit, so `current` is up to date. */
static void extbe_apply_blur(struct amber_extbe_surface *es) {
	bool has_blur = pixman_region32_not_empty(&es->current.blur_region);
	if (!has_blur) {
		if (es->blur_node != NULL) {
			wlr_scene_node_set_enabled(&es->blur_node->node, false);
		}
		return;
	}

	struct wlr_scene_tree *tree =
		extbe_parent_tree(es->server, es->surface);
	if (tree == NULL) {
		return;
	}

	/* Size the blur to the surface's committed size (surface-local).
	 * The region itself is advisory: scenefx blurs whatever sits
	 * behind the node. */
	int w = es->surface->current.width;
	int h = es->surface->current.height;
	if (w < 1 || h < 1) {
		return;
	}

	if (es->blur_node == NULL) {
		es->blur_node = wlr_scene_optimized_blur_create(tree, w, h);
		if (es->blur_node == NULL) {
			return;
		}
	} else {
		wlr_scene_optimized_blur_set_size(es->blur_node, w, h);
	}
	wlr_scene_node_set_enabled(&es->blur_node->node, true);
}

static const struct ext_background_effect_surface_v1_interface
		extbe_surface_impl;
static const struct ext_background_effect_manager_v1_interface
		extbe_manager_impl;

static struct amber_extbe_surface *extbe_surface_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource,
		&ext_background_effect_surface_v1_interface, &extbe_surface_impl));
	return wl_resource_get_user_data(resource);
}

static void extbe_surface_destroy(struct amber_extbe_surface *es) {
	if (es == NULL) {
		return;
	}
	if (es->blur_node != NULL) {
		wlr_scene_node_destroy(&es->blur_node->node);
	}
	wlr_surface_synced_finish(&es->synced);
	wlr_addon_finish(&es->addon);
	pixman_region32_fini(&es->pending.blur_region);
	pixman_region32_fini(&es->current.blur_region);
	if (es->resource != NULL) {
		wl_resource_set_user_data(es->resource, NULL);
	}
	free(es);
}

static void extbe_surface_resource_destroy(struct wl_resource *resource) {
	extbe_surface_destroy(extbe_surface_from_resource(resource));
}

static void extbe_surface_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void extbe_surface_handle_set_blur_region(struct wl_client *client,
		struct wl_resource *resource, struct wl_resource *region_res) {
	struct amber_extbe_surface *es = extbe_surface_from_resource(resource);
	if (es == NULL) {
		wl_resource_post_error(resource,
			EXT_BACKGROUND_EFFECT_SURFACE_V1_ERROR_SURFACE_DESTROYED,
			"The wl_surface object has been destroyed");
		return;
	}
	if (region_res != NULL) {
		const pixman_region32_t *region =
			wlr_region_from_resource(region_res);
		pixman_region32_copy(&es->pending.blur_region, region);
	} else {
		pixman_region32_clear(&es->pending.blur_region);
	}
}

static const struct ext_background_effect_surface_v1_interface
		extbe_surface_impl = {
	.destroy = extbe_surface_handle_destroy,
	.set_blur_region = extbe_surface_handle_set_blur_region,
};

static void extbe_synced_init_state(void *state) {
	struct amber_extbe_state *st = state;
	pixman_region32_init(&st->blur_region);
}

static void extbe_synced_finish_state(void *state) {
	struct amber_extbe_state *st = state;
	pixman_region32_fini(&st->blur_region);
}

static void extbe_synced_move_state(void *dst, void *src) {
	struct amber_extbe_state *d = dst;
	struct amber_extbe_state *s = src;
	pixman_region32_copy(&d->blur_region, &s->blur_region);
}

static void extbe_synced_commit(struct wlr_surface_synced *synced) {
	struct amber_extbe_surface *es =
		wl_container_of(synced, es, synced);
	extbe_apply_blur(es);
}

static const struct wlr_surface_synced_impl extbe_synced_impl = {
	.state_size = sizeof(struct amber_extbe_state),
	.init_state = extbe_synced_init_state,
	.finish_state = extbe_synced_finish_state,
	.move_state = extbe_synced_move_state,
	.commit = extbe_synced_commit,
};

static void extbe_surface_addon_destroy(struct wlr_addon *addon) {
	struct amber_extbe_surface *es =
		wl_container_of(addon, es, addon);
	extbe_surface_destroy(es);
}

static const struct wlr_addon_interface extbe_surface_addon_impl = {
	.name = "ext_background_effect_surface_v1",
	.destroy = extbe_surface_addon_destroy,
};

static struct amber_extbe_surface *extbe_surface_from_wlr_surface(
		struct wlr_surface *surface) {
	struct wlr_addon *addon = wlr_addon_find(&surface->addons, NULL,
		&extbe_surface_addon_impl);
	if (addon == NULL) {
		return NULL;
	}
	struct amber_extbe_surface *es;
	return wl_container_of(addon, es, addon);
}

static void extbe_manager_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void extbe_manager_handle_get_background_effect(struct wl_client *client,
		struct wl_resource *manager_res, uint32_t id,
		struct wl_resource *surface_res) {
	struct amber_extbe_manager *mgr =
		wl_resource_get_user_data(manager_res);
	struct wlr_surface *wlr_surface =
		wlr_surface_from_resource(surface_res);

	if (extbe_surface_from_wlr_surface(wlr_surface) != NULL) {
		wl_resource_post_error(manager_res,
			EXT_BACKGROUND_EFFECT_MANAGER_V1_ERROR_BACKGROUND_EFFECT_EXISTS,
			"The wl_surface object already has a "
			"ext_background_effect_surface_v1 object");
		return;
	}

	struct amber_extbe_surface *es = calloc(1, sizeof(*es));
	if (es == NULL) {
		wl_resource_post_no_memory(manager_res);
		return;
	}
	if (!wlr_surface_synced_init(&es->synced, wlr_surface,
			&extbe_synced_impl, &es->pending, &es->current)) {
		free(es);
		wl_resource_post_no_memory(manager_res);
		return;
	}

	uint32_t version = wl_resource_get_version(manager_res);
	es->resource = wl_resource_create(client,
		&ext_background_effect_surface_v1_interface, version, id);
	if (es->resource == NULL) {
		wlr_surface_synced_finish(&es->synced);
		pixman_region32_fini(&es->pending.blur_region);
		pixman_region32_fini(&es->current.blur_region);
		free(es);
		wl_resource_post_no_memory(manager_res);
		return;
	}
	wl_resource_set_implementation(es->resource, &extbe_surface_impl, es,
		extbe_surface_resource_destroy);
	es->surface = wlr_surface;
	wlr_addon_init(&es->addon, &wlr_surface->addons, es->resource,
		&extbe_surface_addon_impl);
}

static const struct ext_background_effect_manager_v1_interface
		extbe_manager_impl = {
	.destroy = extbe_manager_handle_destroy,
	.get_background_effect = extbe_manager_handle_get_background_effect,
};

static void extbe_manager_resource_destroy(struct wl_resource *resource) {
	wl_list_remove(wl_resource_get_link(resource));
}

static void extbe_manager_bind(struct wl_client *client, void *data,
		uint32_t version, uint32_t id) {
	struct amber_extbe_manager *mgr = data;
	struct wl_resource *resource = wl_resource_create(client,
		&ext_background_effect_manager_v1_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &extbe_manager_impl, mgr,
		extbe_manager_resource_destroy);
	wl_list_insert(&mgr->resources, wl_resource_get_link(resource));

	ext_background_effect_manager_v1_send_capabilities(resource,
		mgr->capabilities);
}

static void extbe_handle_display_destroy(struct wl_listener *listener,
		void *data) {
	struct amber_extbe_manager *mgr =
		wl_container_of(listener, mgr, display_destroy);
	wl_global_destroy(mgr->global);
	wl_list_remove(&mgr->display_destroy.link);
	free(mgr);
}

static void extbe_manager_init(struct amber_server *server) {
	struct amber_extbe_manager *mgr = calloc(1, sizeof(*mgr));
	if (mgr == NULL) {
		return;
	}
	mgr->global = wl_global_create(server->wl_display,
		&ext_background_effect_manager_v1_interface, EXTBE_VERSION, mgr,
		extbe_manager_bind);
	if (mgr->global == NULL) {
		free(mgr);
		return;
	}
	mgr->capabilities = EXT_BACKGROUND_EFFECT_MANAGER_V1_CAPABILITY_BLUR;
	mgr->server = server;
	wl_list_init(&mgr->resources);
	mgr->display_destroy.notify = extbe_handle_display_destroy;
	wl_display_add_destroy_listener(server->wl_display,
		&mgr->display_destroy);
	server->extbe_mgr = mgr;
}

/* A toplevel is "alive" (safe to send state to) only while its xdg
 * surface is initialized. wlroots resets that flag on unmap, and
 * sending any set_* to a reset surface trips an assertion. */
static bool toplevel_alive(struct amber_toplevel *t) {
	return t != NULL && t->xdg_toplevel->base->initialized;
}

/* Force SERVER_SIDE mode on every decoration object of a toplevel.
 * Called from initial-commit where sending a configure is legal. */
static void apply_server_decoration_mode(struct amber_toplevel *toplevel) {
	struct wlr_xdg_decoration_manager_v1 *mgr =
		toplevel->server->xdg_decoration_mgr;
	if (mgr == NULL) {
		return;
	}
	struct wlr_xdg_toplevel_decoration_v1 *deco;
	wl_list_for_each(deco, &mgr->decorations, link) {
		if (deco->toplevel == toplevel->xdg_toplevel) {
			wlr_xdg_toplevel_decoration_v1_set_mode(deco,
				WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
		}
	}
}

static void spawn(const char *cmd) {
	/* SIGCHLD is ignored (see main), so children are reaped by the
	 * kernel automatically and never become zombies. */
	if (fork() == 0) {
		setsid();
		execl("/bin/sh", "/bin/sh", "-c", cmd, (void *)NULL);
		exit(EXIT_FAILURE);
	}
}

static struct amber_workspace *toplevel_workspace(struct amber_toplevel *toplevel) {
	return &toplevel->output->workspaces[toplevel->workspace];
}

static struct amber_output *active_output(struct amber_server *server) {
	if (server->active_output != NULL) {
		return server->active_output;
	}
	struct wlr_output *wlr_output = wlr_output_layout_output_at(
		server->output_layout, server->cursor->x, server->cursor->y);
	struct amber_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		if (output->wlr_output == wlr_output) {
			return output;
		}
	}
	return NULL;
}

static void deactivate_focused(struct amber_server *server) {
	if (toplevel_alive(server->focused_toplevel)) {
		wlr_xdg_toplevel_set_activated(
			server->focused_toplevel->xdg_toplevel, false);
	}
	server->focused_toplevel = NULL;
	wlr_seat_keyboard_clear_focus(server->seat);
}

static void focus_nothing(struct amber_server *server) {
	deactivate_focused(server);
}

static void focus_toplevel(struct amber_toplevel *toplevel);
static void ipc_broadcast(struct amber_server *server);
void animation_cancel_for(struct amber_toplevel *toplevel);
void animation_start_wobble(struct amber_toplevel *toplevel);
static void animation_start_float_glide(struct amber_toplevel *toplevel,
	int from_x, int from_y);
void animation_wobble_nudge(struct amber_toplevel *toplevel,
	int dx, int dy);
void animation_wobble_release(struct amber_toplevel *toplevel);
static void toplevel_apply_fx(struct amber_toplevel *toplevel);
struct amber_animation;
static void animation_destroy(struct amber_server *server,
	struct amber_animation *anim, bool restore);
static void animation_tree_set_opacity(struct wlr_scene_tree *tree,
	float opacity);
static int64_t anim_now_usec(void);
static void animations_kick(struct amber_server *server);

static void focus_toplevel(struct amber_toplevel *toplevel) {
	/* Note: this function only deals with keyboard focus. */
	if (toplevel == NULL) {
		return;
	}
	struct amber_server *server = toplevel->server;
	struct wlr_seat *seat = server->seat;
	struct wlr_surface *prev_surface = seat->keyboard_state.focused_surface;
	struct wlr_surface *surface = toplevel->xdg_toplevel->base->surface;
	if (prev_surface == surface) {
		/* Don't re-focus an already focused surface. */
		return;
	}
	deactivate_focused(server);
	if (prev_surface) {
		/*
		 * Deactivate the previously focused surface. This lets the client know
		 * it no longer has focus and the client will repaint accordingly, e.g.
		 * stop displaying a caret. The surface may be mid-unmap (its xdg
		 * state already reset), so only talk to it if still alive.
		 */
		struct wlr_xdg_toplevel *prev_toplevel =
			wlr_xdg_toplevel_try_from_wlr_surface(prev_surface);
		if (prev_toplevel != NULL && prev_toplevel->base->initialized) {
			wlr_xdg_toplevel_set_activated(prev_toplevel, false);
		}
	}
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
	/* Raise above overlapping floats, but never reorder the strip:
	 * tiling order is user-controlled (niri rule). */
	wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);
	if (toplevel_alive(toplevel)) {
		server->focused_toplevel = toplevel;
		wlr_xdg_toplevel_set_activated(toplevel->xdg_toplevel, true);
	}
	/*
	 * Tell the seat to have the keyboard enter this surface. wlroots will keep
	 * track of this and automatically send key events to the appropriate
	 * clients without additional work on your part.
	 */
	if (keyboard != NULL) {
		wlr_seat_keyboard_notify_enter(seat, surface,
			keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
	}
	ipc_broadcast(server);
}

/* Focus the most recently focused window of a workspace, or nothing. */
static void focus_workspace_topmost(struct amber_server *server,
		struct amber_workspace *workspace) {
	struct amber_toplevel *toplevel = NULL;
	if (!wl_list_empty(&workspace->toplevels)) {
		toplevel = wl_container_of(workspace->toplevels.next,
			toplevel, link);
	}
	if (toplevel != NULL) {
		focus_toplevel(toplevel);
	} else {
		focus_nothing(server);
	}
}

/*
 * Scrollable-tiling layout engine (niri-style).
 *
 * Tiles form a horizontal strip per workspace; a new window is inserted
 * to the right of the focused one and existing windows are NEVER resized.
 * The view (ws->view_offset) scrolls minimally so the focused column
 * stays visible. Floating windows live outside all of this.
 */

static struct amber_workspace *active_workspace(struct amber_server *server) {
	struct amber_output *output = active_output(server);
	return output ? &output->workspaces[output->active_workspace] : NULL;
}

static void workspace_update_top_layer(struct amber_output *output);

/* Send a size configure only when it actually differs: xdg-shell
 * round-trips aren't free and clients shouldn't re-render needlessly. */
static void toplevel_configure_size(struct amber_toplevel *t,
		int width, int height) {
	if (t->sent_w == width && t->sent_h == height) {
		return;
	}
	t->sent_w = width;
	t->sent_h = height;
	if (toplevel_alive(t)) {
		wlr_xdg_toplevel_set_size(t->xdg_toplevel, width, height);
	}
}

static int toplevel_effective_width(struct amber_toplevel *t,
		const struct wlr_box *usable) {
	int max = usable->width - 2 * t->server->gaps;
	if (t->fullscreen || t->maximized) {
		return t->maximized ? usable->width : max;
	}
	int w = t->col_width;
	if (w < t->server->min_column_width) {
		w = t->server->min_column_width;
	}
	if (w > max) {
		w = max;
	}
	return w;
}

static void workspace_arrange(struct amber_workspace *ws) {
	const struct wlr_box *u = &ws->output->usable_area;
	const int gap = ws->output->server->gaps;

	/* Pass 1: walk the strip assigning effective widths and local x
	 * positions; find the focused window's extent. */
	int count_tiled = 0;
	struct amber_toplevel *t;
	wl_list_for_each(t, &ws->toplevels, link) {
		if (!t->floating) {
			count_tiled++;
		}
	}

	int x = u->x + gap;
	int fx0 = 0, fx1 = 0;
	bool have_focus_extent = false;
	wl_list_for_each(t, &ws->toplevels, link) {
		if (t->floating) {
			continue;
		}
		int w = toplevel_effective_width(t, u);
		if (count_tiled == 1) {
			w = u->width - 2 * gap;
		}
		t->tile_x = x;
		t->tile_w = w;
		x += w + gap;
		if (t == ws->output->server->focused_toplevel) {
			fx0 = t->tile_x;
			fx1 = x - gap;
			have_focus_extent = true;
		}
	}

	/* Scroll so the focused column is fully visible: minimal shift, or
	 * dead-center when center_focused_column is set (niri-style). */
	double offset = ws->view_offset;
	if (have_focus_extent) {
		int view_right = u->x + u->width - gap;
		int view_left = u->x + gap;
		int inner_w = view_right - view_left;
		if (ws->output->server->center_focused_column &&
				fx1 - fx0 < inner_w) {
			offset = fx0 - view_left
				- (inner_w - (fx1 - fx0)) / 2.0;
		} else if (ws->output->server->center_focused_column) {
			offset = fx0 - view_left;
		} else {
			if (fx1 - offset > view_right) {
				offset = fx1 - view_right;
			}
			if (fx0 - offset < view_left) {
				offset = fx0 - view_left;
			}
		}
	}
	double max_offset = x - gap - (u->x + u->width);
	if (max_offset < 0) {
		max_offset = 0;
	}
	if (offset > max_offset) {
		offset = max_offset;
	}
	if (offset < 0) {
		offset = 0;
	}
	ws->view_offset = offset;

	/* Pass 2: apply positions and sizes. */
	int height = u->height - 2 * gap;
	wl_list_for_each(t, &ws->toplevels, link) {
		if (t->floating) {
			continue;
		}
		if (t->maximized && !t->fullscreen) {
			/* Fake fullscreen: the whole usable area, no gaps,
			 * client unaware (keeps its own chrome). */
			wlr_scene_node_set_position(&t->scene_tree->node,
				u->x, u->y);
			toplevel_configure_size(t, u->width, u->height);
			t->placed = true;
			continue;
		}
		int tx = t->tile_x - (int)offset;
		int ty = u->y + gap;
		/* An in-flight tween reports its visual position here, so a
		 * rearrange chains from where the window actually looks. */
		int ox = t->scene_tree->node.x;
		int oy = t->scene_tree->node.y;
		bool moved = t->placed && (ox != tx || oy != ty);
		wlr_scene_node_set_position(&t->scene_tree->node, tx, ty);
		t->placed = true;
		if (moved) {
			animation_start_float_glide(t, ox, oy);
		}
		toplevel_configure_size(t, t->tile_w, height);
	}
}

static bool modifier_held(struct amber_server *server, uint32_t mod) {
	struct amber_keyboard *k;
	wl_list_for_each(k, &server->keyboards, link) {
		uint32_t mods = wlr_keyboard_get_modifiers(k->wlr_keyboard);
		mods &= ~k->lock_mask;
		if (mods & mod) {
			return true;
		}
	}
	return false;
}

/* Turn a tiled window into a floating one, keeping its on-screen box. */
static void toplevel_to_floating(struct amber_toplevel *t) {
	if (t->floating) {
		return;
	}
	t->floating = true;
	wlr_scene_node_raise_to_top(&t->scene_tree->node);
	workspace_arrange(toplevel_workspace(t));
	toplevel_apply_fx(t); // shadow appears
}

/* Insert a floating window back into the strip, next to focus. */
static void toplevel_to_tiled(struct amber_toplevel *t) {
	if (!t->floating) {
		return;
	}
	t->fullscreen = false;
	t->maximized = false;
	struct amber_workspace *ws = toplevel_workspace(t);
	struct wlr_box *geo = &t->xdg_toplevel->base->geometry;
	t->col_width = geo->width > 0 ? geo->width
		: (int)(ws->output->usable_area.width
			* AMBER_DEFAULT_COLUMN_FRACTION);

	struct wl_list *anchor = ws->toplevels.prev; // rightmost by default
	struct amber_server *server = t->server;
	if (server->focused_toplevel != NULL &&
			server->focused_toplevel != t &&
			server->focused_toplevel->output == t->output &&
			server->focused_toplevel->workspace == t->workspace) {
		anchor = &server->focused_toplevel->link;
	} else if (server->focused_toplevel == t) {
		anchor = t->link.prev; // keep own slot
	}
	wl_list_remove(&t->link);
	wl_list_insert(anchor, &t->link); // after anchor = right of it
	t->floating = false;
	workspace_arrange(ws);
	toplevel_apply_fx(t); // shadow disappears
}

static void toplevel_set_fullscreen(struct amber_toplevel *t,
		bool fullscreen);

static void toplevel_set_fullscreen(struct amber_toplevel *t,
		bool fullscreen);
static void toplevel_apply_fx(struct amber_toplevel *toplevel);

/* Park a freshly floating window near the middle of the usable area,
 * keeping its current size; skipped until the client committed a size
 * or when the window already fills the area. */
static void toplevel_center_floating(struct amber_toplevel *t) {
	struct amber_workspace *ws = toplevel_workspace(t);
	if (ws == NULL || t->scene_tree == NULL || t->xdg_toplevel == NULL) {
		return;
	}
	struct wlr_box *u = &ws->output->usable_area;
	struct wlr_box g = t->xdg_toplevel->base->geometry;
	if (g.width <= 0 || g.height <= 0 ||
			g.width >= u->width || g.height >= u->height) {
		return;
	}
	wlr_scene_node_set_position(&t->scene_tree->node,
		u->x + (u->width - g.width) / 2,
		u->y + (u->height - g.height) / 2);
}

static void toplevel_toggle_float(struct amber_toplevel *t) {
	int pre_x = 0, pre_y = 0;
	if (t->scene_tree != NULL) {
		pre_x = t->scene_tree->node.x;
		pre_y = t->scene_tree->node.y;
	}
	if (t->fullscreen) {
		toplevel_set_fullscreen(t, false);
	}
	if (t->maximized) {
		t->maximized = false;
	}
	if (t->floating) {
		toplevel_to_tiled(t);
	} else {
		toplevel_to_floating(t);
		toplevel_center_floating(t);
	}
	animation_start_float_glide(t, pre_x, pre_y);
}

static void cycle_focus(struct amber_server *server, int dir) {
	struct amber_workspace *ws = active_workspace(server);
	if (ws == NULL || wl_list_length(&ws->toplevels) < 2) {
		return;
	}
	struct wl_list *head = &ws->toplevels;
	struct wl_list *pos = head;
	if (server->focused_toplevel != NULL &&
			server->focused_toplevel->workspace ==
				ws - ws->output->workspaces &&
			server->focused_toplevel->output == ws->output) {
		pos = &server->focused_toplevel->link;
	}
	struct wl_list *target = dir > 0 ? pos->next : pos->prev;
	while (target == head) { // wrap past the list head
		target = dir > 0 ? target->next : target->prev;
	}
	struct amber_toplevel *next;
	next = wl_container_of(target, next, link);
	focus_toplevel(next);
}

/* Move the focused tile left/right within the strip. */
static void move_focused_column(struct amber_server *server, int dir) {
	struct amber_toplevel *f = server->focused_toplevel;
	if (f == NULL || f->floating) {
		return;
	}
	struct amber_workspace *ws = toplevel_workspace(f);
	struct wl_list *head = &ws->toplevels;
	struct wl_list *a = &f->link;
	struct wl_list *b = dir > 0 ? a->next : a->prev;
	if (b == head || b == a) {
		return; // nothing on that side
	}
	wl_list_remove(a);
	if (dir > 0) {
		wl_list_insert(b, a);      // a takes b's slot (right of it)
	} else {
		wl_list_insert(b->prev, a); // a goes directly before b
	}
	workspace_arrange(ws);
}

static void resize_focused_column(struct amber_server *server, int delta) {
	struct amber_toplevel *f = server->focused_toplevel;
	if (f == NULL || f->floating) {
		return;
	}
	struct amber_workspace *ws = toplevel_workspace(f);
	f->col_width += delta;
	workspace_arrange(ws);
}

static void keyboard_handle_modifiers(
		struct wl_listener *listener, void *data) {
	/* This event is raised when a modifier key, such as shift or alt, is
	 * pressed. We simply communicate this to the client. */
	struct amber_keyboard *keyboard =
		wl_container_of(listener, keyboard, modifiers);
	/*
	 * A seat can only have one keyboard, but this is a limitation of the
	 * Wayland protocol - not wlroots. We assign all connected keyboards to the
	 * same seat. You can swap out the underlying wlr_keyboard like this and
	 * wlr_seat handles this transparently.
	 */
	wlr_seat_set_keyboard(keyboard->server->seat, keyboard->wlr_keyboard);
	/* Send modifiers to the client. */
	wlr_seat_keyboard_notify_modifiers(keyboard->server->seat,
		&keyboard->wlr_keyboard->modifiers);
}

/* Mirror the active-workspace change to ext-workspace-v1 consumers
 * (Noctalia bar pills). old_index < 0 means "initial state only". */
static void ext_workspace_sync_active(struct amber_output *output,
		int old_index, int new_index) {
	if (output->server->ext_ws_mgr == NULL) {
		return;
	}
	if (old_index >= 0 && old_index < AMBER_WORKSPACE_COUNT &&
			output->ext_ws[old_index] != NULL) {
		wlr_ext_workspace_handle_v1_set_active(
			output->ext_ws[old_index], false);
	}
	if (new_index >= 0 && new_index < AMBER_WORKSPACE_COUNT &&
			output->ext_ws[new_index] != NULL) {
		wlr_ext_workspace_handle_v1_set_active(
			output->ext_ws[new_index], true);
		/* Visiting a workspace acknowledges attention requests
		 * pointed at it; leaving the flag set would keep the bar
		 * pill lit forever. */
		wlr_ext_workspace_handle_v1_set_urgent(
			output->ext_ws[new_index], false);
	}
}

/* Mark/unmark the workspace holding this window as demanding
 * attention (bar pill lights up). */
static void ext_workspace_set_urgent_by_toplevel(
		struct amber_toplevel *t, bool urgent) {
	if (t->server->ext_ws_mgr == NULL || t->output == NULL ||
			t->workspace < 0 ||
			t->workspace >= AMBER_WORKSPACE_COUNT) {
		return;
	}
	struct wlr_ext_workspace_handle_v1 *h =
		t->output->ext_ws[t->workspace];
	if (h != NULL) {
		wlr_ext_workspace_handle_v1_set_urgent(h, urgent);
	}
}

static void workspace_switch(struct amber_output *output, int index) {
	if (index < 0 || index >= AMBER_WORKSPACE_COUNT ||
			index == output->active_workspace) {
		return;
	}
	struct amber_server *server = output->server;
	/* Finish any in-flight slide instantly so rapid switches chain. */
	struct amber_animation *anim, *tmp;
	wl_list_for_each_safe(anim, tmp, &server->animations, link) {
		if (anim->kind == ANIM_WS_SLIDE &&
				anim->output == output) {
			animation_destroy(server, anim, true);
		}
	}

	int old_index = output->active_workspace;
	struct amber_workspace *old =
		&output->workspaces[old_index];
	struct amber_workspace *new = &output->workspaces[index];
	output->active_workspace = index;
	ext_workspace_sync_active(output, old_index, index);
	workspace_arrange(new);
	workspace_update_top_layer(output);
	focus_workspace_topmost(server, new);

	bool animate = server->animations_enabled &&
		server->ws_slide_enabled &&
		server->ws_slide_duration_ms > 0 &&
		output->layout_box.width > 0;
	if (animate) {
		struct amber_animation *a = calloc(1, sizeof(*a));
		if (a != NULL) {
			a->kind = ANIM_WS_SLIDE;
			a->output = output;
			a->ws_from = old;
			a->ws_to = new;
			a->origin_x = output->layout_box.x;
			a->origin_y = output->layout_box.y;
			a->slide_dir = index > old_index ? 1 : -1;
			a->start_usec = anim_now_usec();
			a->duration_ms = server->ws_slide_duration_ms;
			/* Hidden workspaces cost nothing when idle, but
			 * both trees render during the slide; they never
			 * overlap (offset positions), so no z-order games:
			 * raising here would lift the whole workspace above
			 * the overlay/bar layers permanently and break
			 * popups/fullscreen stacking afterwards. */
			wlr_scene_node_set_enabled(&old->tree->node, true);
			wlr_scene_node_set_enabled(&new->tree->node, true);
			wlr_scene_node_set_position(&new->tree->node,
				a->origin_x + a->slide_dir *
					output->layout_box.width,
				a->origin_y); // start just off-screen
			wl_list_insert(server->animations.prev, &a->link);
			animations_kick(server);
			ipc_broadcast(server);
			return;
		}
	}
	/* Hidden workspaces cost nothing: a disabled subtree produces no
	 * damage and is skipped entirely by the scene renderer. */
	wlr_scene_node_set_enabled(&old->tree->node, false);
	wlr_scene_node_set_enabled(&new->tree->node, true);
	ipc_broadcast(server);
}

/* A bar requested a workspace switch by handle; map it back to
 * (output, index) and run the normal switch path. */
static void server_ext_ws_commit(struct wl_listener *listener, void *data) {
	struct amber_server *server =
		wl_container_of(listener, server, ext_ws_commit);
	struct wlr_ext_workspace_v1_commit_event *event = data;
	struct wlr_ext_workspace_v1_request *req, *tmp;
	wl_list_for_each_safe(req, tmp, event->requests, link) {
		if (req->type != WLR_EXT_WORKSPACE_V1_REQUEST_ACTIVATE ||
				req->activate.workspace == NULL) {
			continue;
		}
		struct amber_output *output;
		wl_list_for_each(output, &server->outputs, link) {
			for (int i = 0; i < AMBER_WORKSPACE_COUNT; i++) {
				if (output->ext_ws[i] ==
						req->activate.workspace) {
					workspace_switch(output, i);
				}
			}
		}
	}
}

static void toplevel_move_to_workspace(struct amber_toplevel *toplevel,
		int index) {
	if (index < 0 || index >= AMBER_WORKSPACE_COUNT ||
			index == toplevel->workspace) {
		return;
	}
	if (toplevel->fullscreen) {
		/* The window's node lives in the overlay tree while
		 * fullscreen; drop that first so the reparent is clean. */
		toplevel_set_fullscreen(toplevel, false);
	}
	animation_cancel_for(toplevel); // parent is about to change
	struct amber_output *output = toplevel->output;
	struct amber_workspace *src = toplevel_workspace(toplevel);
	bool was_focused = toplevel->server->focused_toplevel == toplevel;

	wl_list_remove(&toplevel->link);
	wl_list_insert(&output->workspaces[index].toplevels,
		&toplevel->link); // rightmost slot
	wlr_scene_node_reparent(&toplevel->scene_tree->node,
		output->workspaces[index].tree);
	toplevel->workspace = index;
	toplevel->placed = false; // old workspace coords are not a visual start

	workspace_arrange(src);
	workspace_arrange(&output->workspaces[index]);
	if (was_focused && index != output->active_workspace) {
		focus_workspace_topmost(toplevel->server, src);
	}
	ipc_broadcast(toplevel->server);
}

/* While the focused window is a fullscreen tile, hide the top layer
 * (bars) like niri does; anything else brings it back. */
static void workspace_update_top_layer(struct amber_output *output) {
	bool hide_top = false;
	struct amber_toplevel *f =
		output->server->focused_toplevel;
	if (f != NULL && f->fullscreen && f->output == output &&
			f->workspace == output->active_workspace) {
		hide_top = true;
	}
	wlr_scene_node_set_enabled(
		&output->layer_trees[ZWLR_LAYER_SHELL_V1_LAYER_TOP]->node,
		!hide_top);
}

static void toplevel_set_fullscreen(struct amber_toplevel *t,
		bool fullscreen) {
	if (t->fullscreen == fullscreen) {
		return;
	}
	struct amber_output *output = t->output;
	if (output == NULL) {
		return;
	}
	animation_cancel_for(t); // node is about to reparent

	if (fullscreen) {
		t->pre_fs_box.width = t->xdg_toplevel->base->geometry.width;
		t->pre_fs_box.height = t->xdg_toplevel->base->geometry.height;
		t->maximized = false;
	}
	t->fullscreen = fullscreen;
	/* Hint the client so it can drop shadows/CSD chrome if it wants. */
	if (toplevel_alive(t)) {
		wlr_xdg_toplevel_set_fullscreen(t->xdg_toplevel, fullscreen);
	}

	/* True fullscreen renders ABOVE the top/overlay layers (bars,
	 * notifs): move the window's tree into the overlay tree. Both
	 * trees share the output origin, so local coords are unchanged. */
	if (fullscreen) {
		wlr_scene_node_reparent(&t->scene_tree->node,
			output->layer_trees[
				ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY]);
		/* Overlay tree shares the workspace trees' origin, so the
		 * window covers the whole output at local (0,0). */
		wlr_scene_node_set_position(&t->scene_tree->node, 0, 0);
		wlr_scene_node_raise_to_top(&t->scene_tree->node);
		t->placed = false; // overlay coords must not seed a glide
		toplevel_configure_size(t,
			output->layout_box.width, output->layout_box.height);
	} 	else {
		/* Back into its workspace tree; arrange restores geometry. */
		wlr_scene_node_reparent(&t->scene_tree->node,
			output->workspaces[t->workspace].tree);
		t->placed = false; // node still sits at the old overlay (0,0)
	}

	if (!t->floating) {
		workspace_arrange(toplevel_workspace(t));
	} else if (fullscreen) {
		/* Fullscreened floats become tiles (niri behavior): keeps
		 * them inside the strip and restore is trivial. */
		toplevel_to_tiled(t);
	}
	workspace_update_top_layer(output);
	toplevel_apply_fx(t);
	ipc_broadcast(t->server);
}

/* Fake fullscreen: cover the usable area without telling the client. */
static void toplevel_toggle_maximize(struct amber_toplevel *t) {
	if (t->fullscreen) {
		return; // fullscreen wins until it is toggled off
	}
	t->maximized = !t->maximized;
	if (t->maximized && t->floating) {
		toplevel_to_tiled(t);
	}
	workspace_arrange(toplevel_workspace(t));
	toplevel_apply_fx(t);
	ipc_broadcast(t->server);
}

/* ========================= SceneFX effects =============================== */

static void fx_corner_cb(struct wlr_scene_buffer *buffer,
		int sx, int sy, void *data) {
	int radius = *(int *)data;
	wlr_scene_buffer_set_corner_radius(buffer, radius);
}

/* Resolve effective effects for a window: global defaults, then the
 * first matching rule wins for each property (later rules override). */
static void rule_resolve(struct amber_server *server,
		struct amber_toplevel *toplevel, bool *blur_out,
		int *radius_out, float *opacity_out) {
	const char *app_id = toplevel->xdg_toplevel->app_id;
	bool blur = server->blur_enabled;
	int radius = server->corner_radius;
	float opacity = 1.0f;

	struct amber_window_rule *rule;
	wl_list_for_each(rule, &server->window_rules, link) {
		if (rule->app_id != NULL && (app_id == NULL ||
				strcmp(rule->app_id, app_id) != 0)) {
			continue;
		}
		if (rule->has_blur) {
			blur = rule->blur;
		}
		if (rule->has_corner) {
			radius = rule->corner_radius;
		}
		if (rule->has_opacity) {
			opacity = rule->opacity;
		}
	}
	*blur_out = blur;
	*radius_out = radius < 0 ? 0 : radius;
	*opacity_out = opacity < 0.0f ? 0.0f :
		(opacity > 1.0f ? 1.0f : opacity);
}

static bool parse_rule_bool(const char *v, bool *out) {
	if (strcmp(v, "yes") == 0 || strcmp(v, "true") == 0) {
		*out = true;
	} else if (strcmp(v, "no") == 0 || strcmp(v, "false") == 0) {
		*out = false;
	} else {
		return false;
	}
	return true;
}

/* Parse "rule=<app-id> opt=val,opt=val" (app-id may be empty). */
static void parse_window_rule(struct amber_server *server,
		const char *value) {
	struct amber_window_rule *rule = calloc(1, sizeof(*rule));
	if (rule == NULL) {
		return;
	}

	while (*value == ' ' || *value == '\t') {
		value++;
	}
	const char *opts = strchr(value, ' ');
	if (opts == NULL) {
		opts = value + strlen(value);
	}
	size_t id_len = (size_t)(opts - value);
	if (id_len > 0) {
		rule->app_id = strndup(value, id_len);
	}

	while (*opts != '\0') {
		while (*opts == ' ' || *opts == '\t' || *opts == ',') {
			opts++;
		}
		const char *eq = strchr(opts, '=');
		if (eq == NULL) {
			break;
		}
		const char *end = eq + 1;
		while (*end != '\0' && *end != ',') {
			end++;
		}
		char key[32], val[64];
		size_t klen = (size_t)(eq - opts);
		size_t vlen = (size_t)(end - eq - 1);
		if (klen < sizeof(key) && vlen < sizeof(val)) {
			memcpy(key, opts, klen);
			key[klen] = '\0';
			memcpy(val, eq + 1, vlen);
			val[vlen] = '\0';
			if (strcmp(key, "blur") == 0) {
				rule->has_blur =
					parse_rule_bool(val, &rule->blur);
			} else if (strcmp(key, "corner-radius") == 0) {
				rule->corner_radius = atoi(val);
				rule->has_corner = true;
				if (rule->corner_radius < 0) {
					rule->corner_radius = 0;
				}
			} else if (strcmp(key, "opacity") == 0) {
				float o = atof(val);
				rule->opacity = o < 0.0f ? 0.0f :
					(o > 1.0f ? 1.0f : o);
				rule->has_opacity = true;
			}
			/* Unknown keys are ignored. */
		}
		opts = end;
	}

	wl_list_insert(server->window_rules.prev, &rule->link);
}

static void toplevel_apply_fx(struct amber_toplevel *toplevel) {
	struct amber_server *server = toplevel->server;
	bool fullscreen = toplevel->fullscreen;

	bool blur;
	int radius;
	float opacity;
	if (fullscreen) {
		blur = false;
		radius = 0;
		opacity = 1.0f;
	} else {
		rule_resolve(server, toplevel, &blur, &radius, &opacity);
	}
	if (!fullscreen) {
		struct amber_animation *wa, *wtmp;
		wl_list_for_each_safe(wa, wtmp, &server->animations,
				link) {
			if (wa->kind == ANIM_WOBBLE &&
					wa->toplevel == toplevel) {
				/* Blur re-samples per cell during wobble
				 * and draws grid seams - park it until
				 * the sheet settles. */
				blur = false;
				break;
			}
		}
	}
	wlr_scene_node_for_each_buffer(&toplevel->scene_tree->node,
		fx_corner_cb, &radius);

	/* --- opacity (fades the whole window content incl. popups) --- */
	animation_tree_set_opacity(toplevel->scene_tree,
		toplevel->fx_opacity >= 0 ? toplevel->fx_opacity : opacity);

	/* --- blur (backdrop) --- */
	if (blur) {
		struct wlr_box geo =
			toplevel->xdg_toplevel->base->geometry;
		if (geo.width >= 1 && geo.height >= 1) {
			if (toplevel->blur_node == NULL) {
				toplevel->blur_node =
					wlr_scene_optimized_blur_create(
						toplevel->scene_tree,
						geo.width, geo.height);
			} else {
				wlr_scene_optimized_blur_set_size(
					toplevel->blur_node,
					geo.width, geo.height);
			}
			wlr_scene_node_set_enabled(
				&toplevel->blur_node->node, true);
		}
	} else if (toplevel->blur_node != NULL) {
		wlr_scene_node_set_enabled(&toplevel->blur_node->node,
			false);
	}

	/* --- drop shadow (floating windows only; a static scene node,
	 * so it costs nothing per frame once placed) --- */
	bool want_shadow = server->shadows_enabled &&
		toplevel->floating && !fullscreen;
	if (want_shadow) {
		struct wlr_box geo =
			toplevel->xdg_toplevel->base->geometry;
		int sigma = 8;
		int margin = sigma * 2;
		if (toplevel->shadow_node == NULL) {
			toplevel->shadow_node = wlr_scene_shadow_create(
				toplevel->scene_tree,
				geo.width + margin * 2,
				geo.height + margin * 2,
				radius + margin / 2, sigma,
				(float[4]){0.0f, 0.0f, 0.0f, 0.55f});
			/* Under everything else in the window tree
			 * (content, popups, blur). */
			if (!wl_list_empty(&toplevel->scene_tree->children)) {
				struct wlr_scene_node *first_child;
				first_child = wl_container_of(
					toplevel->scene_tree->children.next,
					first_child, link);
				wlr_scene_node_place_below(
					&toplevel->shadow_node->node,
					first_child);
			}
		} else {
			wlr_scene_shadow_set_size(toplevel->shadow_node,
				geo.width + margin * 2,
				geo.height + margin * 2);
			wlr_scene_shadow_set_corner_radius(
				toplevel->shadow_node, radius + margin / 2);
		}
		wlr_scene_node_set_position(&toplevel->shadow_node->node,
			geo.x - margin, geo.y - margin);
		wlr_scene_node_set_enabled(&toplevel->shadow_node->node,
			true);
	} else if (toplevel->shadow_node != NULL) {
		wlr_scene_node_set_enabled(&toplevel->shadow_node->node,
			false);
	}
}
static void toggle_focused_fullscreen(struct amber_server *server) {
	struct amber_toplevel *f = server->focused_toplevel;
	if (f != NULL) {
		toplevel_set_fullscreen(f, !f->fullscreen);
		toplevel_apply_fx(f);
	}
}

static void toggle_focused_float(struct amber_server *server) {
	struct amber_toplevel *f = server->focused_toplevel;
	if (f != NULL) {
		toplevel_toggle_float(f);
		ipc_broadcast(server); // occupancy unchanged, float state is
	}
}

enum amber_binding_id {
	AMBER_BINDING_QUIT,
	AMBER_BINDING_CLOSE,
	AMBER_BINDING_TERMINAL,
	AMBER_BINDING_EXEC,
	AMBER_BINDING_CYCLE_FOCUS,
	AMBER_BINDING_WORKSPACE,
	AMBER_BINDING_MOVE_TO_WORKSPACE,
	AMBER_BINDING_MOVE_COLUMN,
	AMBER_BINDING_COLUMN_WIDTH,
	AMBER_BINDING_TOGGLE_FLOAT,
	AMBER_BINDING_TOGGLE_FULLSCREEN,
	AMBER_BINDING_TOGGLE_MAXIMIZE,
	AMBER_BINDING_SWITCHER,
	AMBER_BINDING_OVERVIEW,
	AMBER_BINDING_CYCLE_OPACITY,
};

struct amber_binding {
	uint32_t mods;
	xkb_keysym_t sym;
	enum amber_binding_id id;
	int arg;
	char *cmd; // AMBER_BINDING_EXEC only
};

/*
 * Configuration: ~/.config/amberwm/amberwm.cfg
 *
 *   # comments
 *   terminal=foot
 *   gaps=8
 *   min-column-width=240
 *   default-column-width=0.5
 *   bind=SUPER+SHIFT+Q,close
 *   bind=SUPER+1,workspace 1
 *   bind=SUPER+T,exec foot -e btop
 *
 * Missing file falls back to the built-in defaults below.
 */

/* Defaults written when no config exists; also the built-in fallback. */
struct amber_binding_defaults {
	const char *combo;
	enum amber_binding_id id;
	int arg;
	const char *cmd; // AMBER_BINDING_EXEC only
};

static const struct amber_binding_defaults amber_default_bindings[] = {
	{ "SUPER+RETURN", AMBER_BINDING_TERMINAL, 0 , NULL },
	{ "SUPER+SHIFT+Q", AMBER_BINDING_CLOSE, 0 , NULL },
	{ "SUPER+SHIFT+E", AMBER_BINDING_QUIT, 0 , NULL },
	{ "SUPER+TAB", AMBER_BINDING_CYCLE_FOCUS, 1 , NULL },
	{ "SUPER+SHIFT+TAB", AMBER_BINDING_CYCLE_FOCUS, -1 , NULL },
	{ "SUPER+1", AMBER_BINDING_WORKSPACE, 0 , NULL },
	{ "SUPER+2", AMBER_BINDING_WORKSPACE, 1 , NULL },
	{ "SUPER+3", AMBER_BINDING_WORKSPACE, 2 , NULL },
	{ "SUPER+4", AMBER_BINDING_WORKSPACE, 3 , NULL },
	{ "SUPER+5", AMBER_BINDING_WORKSPACE, 4 , NULL },
	{ "SUPER+6", AMBER_BINDING_WORKSPACE, 5 , NULL },
	{ "SUPER+7", AMBER_BINDING_WORKSPACE, 6 , NULL },
	{ "SUPER+8", AMBER_BINDING_WORKSPACE, 7 , NULL },
	{ "SUPER+9", AMBER_BINDING_WORKSPACE, 8 , NULL },
	{ "SUPER+SHIFT+1", AMBER_BINDING_MOVE_TO_WORKSPACE, 0 , NULL },
	{ "SUPER+SHIFT+2", AMBER_BINDING_MOVE_TO_WORKSPACE, 1 , NULL },
	{ "SUPER+SHIFT+3", AMBER_BINDING_MOVE_TO_WORKSPACE, 2 , NULL },
	{ "SUPER+SHIFT+4", AMBER_BINDING_MOVE_TO_WORKSPACE, 3 , NULL },
	{ "SUPER+SHIFT+5", AMBER_BINDING_MOVE_TO_WORKSPACE, 4 , NULL },
	{ "SUPER+SHIFT+6", AMBER_BINDING_MOVE_TO_WORKSPACE, 5 , NULL },
	{ "SUPER+SHIFT+7", AMBER_BINDING_MOVE_TO_WORKSPACE, 6 , NULL },
	{ "SUPER+SHIFT+8", AMBER_BINDING_MOVE_TO_WORKSPACE, 7 , NULL },
	{ "SUPER+SHIFT+9", AMBER_BINDING_MOVE_TO_WORKSPACE, 8 , NULL },
	/* Scrollable-tiling navigation (niri-style). */
	{ "SUPER+H", AMBER_BINDING_CYCLE_FOCUS, -1 , NULL },
	{ "SUPER+L", AMBER_BINDING_CYCLE_FOCUS, 1 , NULL },
	{ "SUPER+LEFT", AMBER_BINDING_CYCLE_FOCUS, -1 , NULL },
	{ "SUPER+RIGHT", AMBER_BINDING_CYCLE_FOCUS, 1 , NULL },
	{ "SUPER+SHIFT+H", AMBER_BINDING_MOVE_COLUMN, -1 , NULL },
	{ "SUPER+SHIFT+L", AMBER_BINDING_MOVE_COLUMN, 1 , NULL },
	{ "SUPER+SHIFT+LEFT", AMBER_BINDING_MOVE_COLUMN, -1 , NULL },
	{ "SUPER+SHIFT+RIGHT", AMBER_BINDING_MOVE_COLUMN, 1 , NULL },
	{ "SUPER+MINUS", AMBER_BINDING_COLUMN_WIDTH, -1 , NULL },
	{ "SUPER+EQUAL", AMBER_BINDING_COLUMN_WIDTH, 1 , NULL },
	{ "SUPER+V", AMBER_BINDING_TOGGLE_FLOAT, 0, NULL },
	{ "SUPER+F", AMBER_BINDING_TOGGLE_MAXIMIZE, 0, NULL },
	{ "SUPER+SHIFT+F", AMBER_BINDING_TOGGLE_FULLSCREEN, 0, NULL },
	{ "SUPER+S", AMBER_BINDING_CYCLE_OPACITY, 0, NULL },
	{ "SUPER+D", AMBER_BINDING_OVERVIEW, 0, NULL },
	{ "SUPER+T", AMBER_BINDING_TERMINAL, 0, NULL },
	{ "SUPER+SPACE", AMBER_BINDING_EXEC, 0,
			"noctalia msg panel-toggle launcher" },
	/* Noctalia-driven hardware keys (OSDs included). */
	{ "XF86AudioRaiseVolume", AMBER_BINDING_EXEC, 0,
			"noctalia msg volume-up" },
	{ "XF86AudioLowerVolume", AMBER_BINDING_EXEC, 0,
			"noctalia msg volume-down" },
	{ "XF86AudioMute", AMBER_BINDING_EXEC, 0,
			"noctalia msg volume-mute" },
	{ "XF86AudioPlay", AMBER_BINDING_EXEC, 0,
			"noctalia msg media toggle" },
	{ "XF86AudioNext", AMBER_BINDING_EXEC, 0,
			"noctalia msg media next" },
	{ "XF86AudioPrev", AMBER_BINDING_EXEC, 0,
			"noctalia msg media previous" },
	{ "XF86MonBrightnessUp", AMBER_BINDING_EXEC, 0,
			"noctalia msg brightness-up" },
	{ "XF86MonBrightnessDown", AMBER_BINDING_EXEC, 0,
			"noctalia msg brightness-down" },
};

#define AMBER_DEFAULT_BINDING_COUNT \
	(sizeof(amber_default_bindings) / sizeof(amber_default_bindings[0]))

static uint32_t parse_modifier(const char *name) {
	if (strcmp(name, "SUPER") == 0 || strcmp(name, "LOGO") == 0 ||
			strcmp(name, "MOD4") == 0) {
		return WLR_MODIFIER_LOGO;
	}
	if (strcmp(name, "SHIFT") == 0) {
		return WLR_MODIFIER_SHIFT;
	}
	if (strcmp(name, "CTRL") == 0 || strcmp(name, "CONTROL") == 0) {
		return WLR_MODIFIER_CTRL;
	}
	if (strcmp(name, "ALT") == 0) {
		return WLR_MODIFIER_ALT;
	}
	return 0;
}

/* Parse "SUPER+SHIFT+RETURN" into mods + keysym. */
static bool parse_combo(char *combo, uint32_t *mods, xkb_keysym_t *sym) {
	*mods = 0;
	*sym = XKB_KEY_NoSymbol;

	char *saveptr = NULL;
	char *token = strtok_r(combo, "+", &saveptr);
	char *key_name = NULL;
	while (token != NULL) {
		uint32_t mod = parse_modifier(token);
		if (mod != 0) {
			*mods |= mod;
		} else {
			key_name = token; // last non-modifier token is the key
		}
		token = strtok_r(NULL, "+", &saveptr);
	}
	if (key_name == NULL) {
		return false;
	}
	*sym = xkb_keysym_from_name(key_name, XKB_KEYSYM_CASE_INSENSITIVE);
	/* "F" resolves to XKB_KEY_F, which only matches caps-lock or
	 * shifted input; a config key name means the physical key, so
	 * fold letters to their lowercase keysym. */
	if (*sym >= XKB_KEY_A && *sym <= XKB_KEY_Z) {
		*sym += XKB_KEY_a - XKB_KEY_A;
	}
	return *sym != XKB_KEY_NoSymbol;
}

/* Parse an action spec like "workspace 3" or "exec foot -e btop". */
static bool parse_action(struct amber_server *server,
		struct amber_binding *b, const char *spec) {
	while (*spec == ' ') {
		spec++;
	}
	char buf[256];
	snprintf(buf, sizeof(buf), "%s", spec);
	char *sp = strchr(buf, ' ');
	if (sp != NULL) {
		*sp = '\0';
	}

	if (strcmp(buf, "quit") == 0) {
		b->id = AMBER_BINDING_QUIT;
	} else if (strcmp(buf, "close") == 0) {
		b->id = AMBER_BINDING_CLOSE;
	} else if (strcmp(buf, "terminal") == 0) {
		b->id = AMBER_BINDING_TERMINAL;
	} else if (strcmp(buf, "focus-next") == 0) {
		b->id = AMBER_BINDING_CYCLE_FOCUS;
		b->arg = 1;
	} else if (strcmp(buf, "focus-prev") == 0) {
		b->id = AMBER_BINDING_CYCLE_FOCUS;
		b->arg = -1;
	} else if (strcmp(buf, "move-left") == 0) {
		b->id = AMBER_BINDING_MOVE_COLUMN;
		b->arg = -1;
	} else if (strcmp(buf, "move-right") == 0) {
		b->id = AMBER_BINDING_MOVE_COLUMN;
		b->arg = 1;
	} else if (strcmp(buf, "width-dec") == 0) {
		b->id = AMBER_BINDING_COLUMN_WIDTH;
		b->arg = -1;
	} else if (strcmp(buf, "width-inc") == 0) {
		b->id = AMBER_BINDING_COLUMN_WIDTH;
		b->arg = 1;
	} else if (strcmp(buf, "toggle-float") == 0) {
		b->id = AMBER_BINDING_TOGGLE_FLOAT;
	} else if (strcmp(buf, "toggle-fullscreen") == 0) {
		b->id = AMBER_BINDING_TOGGLE_FULLSCREEN;
	} else if (strcmp(buf, "toggle-maximize") == 0) {
		b->id = AMBER_BINDING_TOGGLE_MAXIMIZE;
	} else if (strcmp(buf, "switcher") == 0) {
		b->id = AMBER_BINDING_SWITCHER;
	} else if (strcmp(buf, "overview") == 0) {
		b->id = AMBER_BINDING_OVERVIEW;
	} else if (strcmp(buf, "workspace") == 0 || strcmp(buf,
				"move-to-workspace") == 0) {
		int n = sp ? atoi(sp + 1) : 0;
		if (n < 1 || n > AMBER_WORKSPACE_COUNT) {
			return false;
		}
		b->id = strcmp(buf, "workspace") == 0
			? AMBER_BINDING_WORKSPACE : AMBER_BINDING_MOVE_TO_WORKSPACE;
		b->arg = n - 1;
	} else if (strcmp(buf, "exec") == 0) {
		if (sp == NULL || *(sp + 1) == '\0') {
			return false;
		}
		b->id = AMBER_BINDING_EXEC;
		b->cmd = strdup(sp + 1);
	} else {
		return false;
	}
	(void)server;
	return true;
}

/* Expand a leading ~ to $HOME; returns a malloc'd string. */
static char *expand_path(const char *path) {
	if (path[0] != '~' || (path[1] != '/' && path[1] != '\0')) {
		return strdup(path);
	}
	const char *home = getenv("HOME");
	if (home == NULL) {
		return strdup(path);
	}
	size_t len = strlen(home) + strlen(path); // '~' replaced by home
	char *out = malloc(len);
	snprintf(out, len, "%s%s", home, path + 1);
	return out;
}

enum { AMBER_CONFIG_OK, AMBER_CONFIG_ERROR };

/* Append one parsed "bind=" line to server->bindings. */
static int config_add_binding(struct amber_server *server,
		const char *value) {
	char buf[512];
	snprintf(buf, sizeof(buf), "%s", value);
	char *comma = strchr(buf, ',');
	if (comma == NULL) {
		wlr_log(WLR_ERROR, "config: bad bind '%s'", value);
		return AMBER_CONFIG_ERROR;
	}
	*comma = '\0';

	struct amber_binding b = {0};
	if (!parse_combo(buf, &b.mods, &b.sym)) {
		wlr_log(WLR_ERROR, "config: bad key combo in '%s'", value);
		return AMBER_CONFIG_ERROR;
	}
	if (!parse_action(server, &b, comma + 1)) {
		wlr_log(WLR_ERROR, "config: unknown action in '%s'", value);
		return AMBER_CONFIG_ERROR;
	}

	/* A config "bind=" line overrides a default with the same combo
	 * instead of stacking a second entry. */
	for (int i = 0; i < server->binding_count; i++) {
		struct amber_binding *old = &server->bindings[i];
		if (old->mods == b.mods && old->sym == b.sym) {
			free(old->cmd);
			*old = b;
			return AMBER_CONFIG_OK;
		}
	}

	struct amber_binding *grown = realloc(server->bindings,
		(server->binding_count + 1) * sizeof(b));
	if (grown == NULL) {
		free(b.cmd);
		return AMBER_CONFIG_ERROR;
	}
	server->bindings = grown;
	server->bindings[server->binding_count++] = b;
	return AMBER_CONFIG_OK;
}

static void config_set_defaults(struct amber_server *server) {
	for (size_t i = 0; i < AMBER_DEFAULT_BINDING_COUNT; i++) {
		const struct amber_binding_defaults *d =
			&amber_default_bindings[i];
		char combo[64];
		snprintf(combo, sizeof(combo), "%s", d->combo);
		struct amber_binding b = {0};
		if (!parse_combo(combo, &b.mods, &b.sym)) {
			continue;
		}
		b.id = d->id;
		b.arg = d->arg;
		b.cmd = d->cmd != NULL ? strdup(d->cmd) : NULL;
		struct amber_binding *grown = realloc(server->bindings,
			(server->binding_count + 1) * sizeof(b));
		if (grown == NULL) {
			break;
		}
		server->bindings = grown;
		server->bindings[server->binding_count++] = b;
	}
}

static void config_load(struct amber_server *server) {
	/* Non-binding defaults. */
	server->gaps = AMBER_DEFAULT_GAPS;
	server->min_column_width = AMBER_DEFAULT_MIN_COLUMN_WIDTH;
	server->default_column_fraction = AMBER_DEFAULT_COLUMN_FRACTION;
	server->corner_radius = 8;
	server->blur_enabled = false;
	server->blur_radius = 5;
	server->shadows_enabled = false;
	server->center_focused_column = false;
	server->animations_enabled = true;
	server->animation_duration_ms = 150;
	server->lamp_close_enabled = true;
	server->lamp_close_duration_ms = 350; // KDE's ~350ms feel
	server->ws_slide_enabled = true;
	server->ws_slide_duration_ms = 250;
	server->wobbly_windows = true; // Compiz-style spring grid on drag
	server->idle_throttle_enabled = true;
	server->idle_throttle_seconds = 30;
	server->idle_throttle_hz = 10;
	server->idle_throttled = false;
	server->idle_inhibitors = 0;
	server->dyn_ws = true; // advertise only active/occupied workspaces
	const char *env_theme = getenv("XCURSOR_THEME");
	server->cursor_theme = env_theme != NULL ? strdup(env_theme) : NULL;
	const char *env_size = getenv("XCURSOR_SIZE");
	int cs = env_size != NULL ? atoi(env_size) : 24;
	server->cursor_size = cs >= 8 && cs <= 128 ? cs : 24;
	wl_list_init(&server->animations);
	wl_list_init(&server->window_rules);

	const char *override = getenv("AMBER_CONFIG");
	char path[512];
	FILE *f = NULL;
	if (override != NULL) {
		snprintf(path, sizeof(path), "%s", override);
		f = fopen(override, "r");
	} else {
		const char *xdg = getenv("XDG_CONFIG_HOME");
		const char *home = getenv("HOME");
		if (xdg != NULL) {
			snprintf(path, sizeof(path),
				"%s/amberwm/amberwm.cfg", xdg);
			f = fopen(path, "r");
		}
		if (f == NULL && home != NULL) {
			snprintf(path, sizeof(path),
				"%s/.config/amberwm/amberwm.cfg", home);
			f = fopen(path, "r");
		}
	}
	if (f != NULL) {
		/* Remember where the config lives so the inotify watcher
		 * can pick up edits. */
		free(server->config_path);
		server->config_path = strdup(path);
	}
	if (f == NULL) {
		config_set_defaults(server);
		wlr_log(WLR_INFO, "no config found, using defaults");
		return;
	}

	/* Always load the full default binding set first; a config "bind="
	 * line then overrides any default that shares its key combo. Without
	 * this, a hand-written config silently drops every default shortcut
	 * (terminal, workspaces, exit, overview, etc.). */
	config_set_defaults(server);

	char line[1024];
	while (fgets(line, sizeof(line), f) != NULL) {
		/* Strip comment and trailing newline. */
		char *hash = strchr(line, '#');
		if (hash != NULL) {
			*hash = '\0';
		}
		line[strcspn(line, "\r\n")] = '\0';

		char *eq = strchr(line, '=');
		if (eq == NULL) {
			continue;
		}
		*eq = '\0';
		char *key = line;
		char *value = eq + 1;
		while (*value == ' ') {
			value++;
		}

		if (strcmp(key, "bind") == 0) {
			config_add_binding(server, value);
		} else if (strcmp(key, "terminal") == 0) {
			free(server->terminal_cmd);
			server->terminal_cmd = strdup(value);
		} else if (strcmp(key, "autostart") == 0) {
			if (*value == '\0') {
				continue;
			}
			char **grown = realloc(server->autostart,
				(server->autostart_count + 1) * sizeof(*grown));
			if (grown != NULL) {
				server->autostart = grown;
				server->autostart[server->autostart_count++] =
					strdup(value);
			}
		} else if (strcmp(key, "wallpaper") == 0) {
			free(server->wallpaper_path);
			server->wallpaper_path = *value == '\0'
				? NULL : expand_path(value);
		} else if (strcmp(key, "corner-radius") == 0) {
			server->corner_radius = atoi(value);
			if (server->corner_radius < 0) {
				server->corner_radius = 0;
			}
		} else if (strcmp(key, "blur") == 0) {
			server->blur_enabled = strcmp(value, "yes") == 0;
		} else if (strcmp(key, "shadows") == 0) {
			server->shadows_enabled =
				strcmp(value, "yes") == 0;
		} else if (strcmp(key, "center-focused-column") == 0) {
			server->center_focused_column =
				strcmp(value, "yes") == 0;
		} else if (strcmp(key, "dynamic-workspaces") == 0) {
			server->dyn_ws = strcmp(value, "yes") == 0;
		} else if (strcmp(key, "animations") == 0) {
			server->animations_enabled =
				strcmp(value, "yes") == 0;
		} else if (strcmp(key, "animation-duration") == 0) {
			int v = atoi(value);
			server->animation_duration_ms =
				v > 0 && v <= 1000 ? (uint32_t)v : 150;
		} else if (strcmp(key, "close-animation") == 0) {
			server->lamp_close_enabled =
				strcmp(value, "yes") == 0;
		} else if (strcmp(key, "close-animation-duration") == 0) {
			int v = atoi(value);
			server->lamp_close_duration_ms =
				v > 0 && v <= 1000 ? (uint32_t)v : 350;
		} else if (strcmp(key, "workspace-animation") == 0) {
			server->ws_slide_enabled =
				strcmp(value, "yes") == 0;
		} else if (strcmp(key, "workspace-animation-duration") == 0) {
			int v = atoi(value);
			server->ws_slide_duration_ms =
				v > 0 && v <= 1000 ? (uint32_t)v : 250;
		} else if (strcmp(key, "wobbly-windows") == 0) {
			server->wobbly_windows =
				strcmp(value, "yes") == 0;
		} else if (strcmp(key, "idle-throttle") == 0) {
			server->idle_throttle_enabled =
				strcmp(value, "yes") == 0;
		} else if (strcmp(key, "idle-throttle-seconds") == 0) {
			int v = atoi(value);
			server->idle_throttle_seconds =
				v >= 1 && v <= 3600 ? v : 30;
		} else if (strcmp(key, "idle-throttle-hz") == 0) {
			int v = atoi(value);
			server->idle_throttle_hz =
				v >= 1 && v <= 60 ? v : 10;
		} else if (strcmp(key, "cursor-theme") == 0) {
			free(server->cursor_theme);
			server->cursor_theme = *value == '\0'
				? NULL : strdup(value);
		} else if (strcmp(key, "cursor-size") == 0) {
			int v = atoi(value);
			server->cursor_size =
				v >= 8 && v <= 128 ? v : 24;
		} else if (strcmp(key, "blur-radius") == 0) {
			server->blur_radius = atoi(value);
			if (server->blur_radius < 0) {
				server->blur_radius = 5;
			}
		} else if (strcmp(key, "rule") == 0) {
			parse_window_rule(server, value);
		} else if (strcmp(key, "wallpaper-mode") == 0) {
			if (strcmp(value, "cover") == 0) {
				server->wallpaper_mode = AMBER_WALLPAPER_COVER;
			} else if (strcmp(value, "contain") == 0) {
				server->wallpaper_mode = AMBER_WALLPAPER_CONTAIN;
			} else if (strcmp(value, "center") == 0) {
				server->wallpaper_mode = AMBER_WALLPAPER_CENTER;
			} else if (strcmp(value, "stretch") == 0) {
				server->wallpaper_mode = AMBER_WALLPAPER_STRETCH;
			} else {
				wlr_log(WLR_ERROR,
					"config: unknown wallpaper-mode '%s'",
					value);
			}
		} else if (strcmp(key, "gaps") == 0) {
			server->gaps = atoi(value);
			if (server->gaps < 0) {
				server->gaps = 0;
			}
		} else if (strcmp(key, "min-column-width") == 0) {
			int v = atoi(value);
			server->min_column_width = v > 0 ? v
				: AMBER_DEFAULT_MIN_COLUMN_WIDTH;
		} else if (strcmp(key, "default-column-width") == 0) {
			double v = atof(value);
			if (v > 0.05 && v <= 1.0) {
				server->default_column_fraction = v;
			}
		} else if (strlen(key) > 0) {
			wlr_log(WLR_ERROR, "config: unknown key '%s'", key);
		}
	}
	fclose(f);
}

/* Re-read amberwm.cfg live (IPC "reload" / SIGHUP). Autostart commands
 * are NOT re-run; bindings, rules, layout and effects apply immediately. */
static void config_reload(struct amber_server *server) {
	struct amber_window_rule *rule, *rule_tmp;
	wl_list_for_each_safe(rule, rule_tmp, &server->window_rules, link) {
		wl_list_remove(&rule->link);
		free(rule->app_id);
		free(rule);
	}
	for (size_t i = 0; i < server->autostart_count; i++) {
		free(server->autostart[i]);
	}
	free(server->autostart);
	server->autostart = NULL;
	server->autostart_count = 0;
	free(server->bindings);
	server->bindings = NULL;
	server->binding_count = 0;

	config_load(server);

	/* Cursor theme/size may have changed: rebuild the manager and
	 * reapply the image; future clients inherit the new env. */
	wlr_xcursor_manager_destroy(server->cursor_mgr);
	server->cursor_mgr = wlr_xcursor_manager_create(
		server->cursor_theme, server->cursor_size);
	if (server->cursor != NULL) {
		wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr,
			"default");
	}
	if (server->cursor_theme != NULL) {
		setenv("XCURSOR_THEME", server->cursor_theme, 1);
	}
	char csizebuf[16];
	snprintf(csizebuf, sizeof(csizebuf), "%d", server->cursor_size);
	setenv("XCURSOR_SIZE", csizebuf, 1);

	wlr_scene_set_blur_data(server->scene, 3, server->blur_radius,
		0.02f, 0.90f, 0.90f, 1.10f);

	struct amber_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		for (int i = 0; i < AMBER_WORKSPACE_COUNT; i++) {
			if (i == output->active_workspace) {
				workspace_arrange(&output->workspaces[i]);
			}
			struct amber_toplevel *t;
			wl_list_for_each(t,
					&output->workspaces[i].toplevels,
					link) {
				toplevel_apply_fx(t);
			}
		}
	}
	wlr_log(WLR_INFO, "config reloaded");
}

/* Watch the config's directory with inotify; editors either write in
 * place (IN_CLOSE_WRITE) or replace via rename (IN_MOVED_TO). One idle
 * fd otherwise — no polling cost. */
static int config_file_changed(int fd, uint32_t mask, void *data) {
	struct amber_server *server = data;
	char buf[1024] __attribute__((aligned(__alignof__(
		struct inotify_event))));

	while (true) {
		ssize_t n = read(fd, buf, sizeof(buf));
		if (n <= 0) {
			break;
		}
		const char *base = strrchr(server->config_path, '/');
		base = base != NULL ? base + 1 : server->config_path;
		size_t base_len = strlen(base);

		for (char *p = buf; p < buf + n; ) {
			struct inotify_event *ev =
				(struct inotify_event *)p;
			p += sizeof(*ev) + ev->len;
			if ((mask & WL_EVENT_ERROR) != 0 ||
					ev->len == 0 ||
					strlen(ev->name) != base_len ||
					memcmp(ev->name, base,
						base_len) != 0) {
				continue;
			}
			if (ev->mask & (IN_CLOSE_WRITE | IN_MOVED_TO)) {
				config_reload(server);
			}
		}
	}
	return 0;
}

static void config_watch_init(struct amber_server *server) {
	server->config_inotify_fd = -1;
	if (server->config_path == NULL) {
		return; // no config file: nothing to watch
	}
	int fd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
	if (fd < 0) {
		return;
	}
	char dir[512];
	snprintf(dir, sizeof(dir), "%s", server->config_path);
	char *slash = strrchr(dir, '/');
	if (slash != NULL) {
		*slash = '\0';
	}
	uint32_t mask = IN_CLOSE_WRITE | IN_MOVED_TO;
	if (inotify_add_watch(fd, dir[0] != '\0' ? dir : "/", mask) < 0) {
		close(fd);
		return;
	}
	struct wl_event_loop *loop =
		wl_display_get_event_loop(server->wl_display);
	server->config_watch_source =
		wl_event_loop_add_fd(loop, fd, WL_EVENT_READABLE,
			config_file_changed, server);
	server->config_inotify_fd = fd;
	wlr_log(WLR_INFO, "watching %s for changes", server->config_path);
}

static void ext_ws_dyn_sync(struct amber_output *output);

static void switcher_open(struct amber_server *server);
static void switcher_select(struct amber_server *server, size_t idx);
static int64_t idle_now_usec(void);
static void idle_mark_activity(struct amber_server *server);
static struct amber_output *overview_output(struct amber_server *server);
static void overview_open(struct amber_server *server,
	struct amber_output *output);

static void binding_exec(struct amber_server *server,
		const struct amber_binding *binding) {

	switch (binding->id) {
	case AMBER_BINDING_QUIT:
		server->shutting_down = true;
		wl_display_terminate(server->wl_display);
		break;
	case AMBER_BINDING_CLOSE:
		if (server->focused_toplevel != NULL) {
			wlr_xdg_toplevel_send_close(
				server->focused_toplevel->xdg_toplevel);
		}
		break;
	case AMBER_BINDING_TERMINAL:
		spawn(server->terminal_cmd);
		break;
	case AMBER_BINDING_EXEC:
		spawn(binding->cmd);
		break;
	case AMBER_BINDING_SWITCHER:
		switcher_open(server);
		break;
	case AMBER_BINDING_OVERVIEW: {
		struct amber_output *output = active_output(server);
		if (output != NULL) {
			overview_open(server, output);
		}
		break;
	}
	case AMBER_BINDING_CYCLE_FOCUS:
		cycle_focus(server, binding->arg);
		break;
	case AMBER_BINDING_WORKSPACE: {
		struct amber_output *output = active_output(server);
		if (output != NULL) {
			workspace_switch(output, binding->arg);
		}
		break;
	}
	case AMBER_BINDING_MOVE_TO_WORKSPACE:
		if (server->focused_toplevel != NULL) {
			toplevel_move_to_workspace(server->focused_toplevel,
				binding->arg);
		}
		break;
	case AMBER_BINDING_MOVE_COLUMN:
		move_focused_column(server, binding->arg);
		break;
	case AMBER_BINDING_COLUMN_WIDTH: {
		struct amber_output *output = active_output(server);
		int step = output ? output->usable_area.width / 10 : 100;
		resize_focused_column(server,
			binding->arg >= 0 ? step : -step);
		break;
	}
	case AMBER_BINDING_TOGGLE_FLOAT:
		toggle_focused_float(server);
		break;
	case AMBER_BINDING_TOGGLE_FULLSCREEN:
		toggle_focused_fullscreen(server);
		break;
	case AMBER_BINDING_TOGGLE_MAXIMIZE:
		if (server->focused_toplevel != NULL) {
			toplevel_toggle_maximize(server->focused_toplevel);
		}
		break;
	case AMBER_BINDING_CYCLE_OPACITY: {
		struct amber_toplevel *t = server->focused_toplevel;
		if (t == NULL || !toplevel_alive(t)) {
			break;
		}
		static const float stages[] = {1.0f, 0.85f, 0.7f, 0.5f, 0.3f};
		const int nst = (int)(sizeof(stages) / sizeof(stages[0]));
		float cur = t->fx_opacity >= 0 ? t->fx_opacity : 1.0f;
		int next = 0;
		for (int i = 0; i < nst; i++) {
			if (stages[i] > cur - 0.001f) {
				next = (i + 1) % nst;
				break;
			}
		}
		t->fx_opacity = stages[next];
		toplevel_apply_fx(t);
		break;
	}
	}
}

 /* RGBA pixel memory wrapped as a wlr_buffer; shared by the built-in
 * wallpaper and the window switcher. */
struct amber_wallpaper_buffer {
	struct wlr_buffer base;
	void *data;
	int width, height;
};

static const struct wlr_buffer_impl wallpaper_buffer_impl;

/* Wrap freshly allocated RGBA pixels into a buffer. Pixel ownership moves
 * into the buffer and is released when its last reference dies. */
static struct wlr_buffer *rgba_buffer_take(int width, int height,
		unsigned char *pixels) {
	struct amber_wallpaper_buffer *buf = calloc(1, sizeof(*buf));
	if (buf == NULL) {
		free(pixels);
		return NULL;
	}
	wlr_buffer_init(&buf->base, &wallpaper_buffer_impl, width, height);
	buf->data = pixels;
	buf->width = width;
	buf->height = height;
	return &buf->base;
}

static bool ui_in_rounded(int x, int y, int w, int h, int r) {
	int dx = x < 0 ? -x : (x >= w ? x - (w - 1) : 0);
	int dy = y < 0 ? -y : (y >= h ? y - (h - 1) : 0);
	if (dx <= 0 || dy <= 0) {
		return true; // middle region
	}
	int mx = r - dx, my = r - dy;
	return mx * mx + my * my <= r * r;
}

static struct wlr_buffer *ui_rounded_card(int w, int h, int r,
		float col[4]) {
	unsigned char *px = calloc((size_t)w * h, 4);
	if (px == NULL) {
		return NULL;
	}
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			if (!ui_in_rounded(x, y, w, h, r)) {
				continue;
			}
			unsigned char *d = &px[(y * (size_t)w + x) * 4];
			d[0] = (unsigned char)(col[0] * col[3] * 255);
			d[1] = (unsigned char)(col[1] * col[3] * 255);
			d[2] = (unsigned char)(col[2] * col[3] * 255);
			d[3] = (unsigned char)(col[3] * 255);
		}
	}
	return rgba_buffer_take(w, h, px);
}

static struct wlr_buffer *ui_rounded_ring(int w, int h, int r, int t,
		float col[4]) {
	unsigned char *px = calloc((size_t)w * h, 4);
	if (px == NULL) {
		return NULL;
	}
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			bool outer = ui_in_rounded(x, y, w, h, r);
			bool inner = ui_in_rounded(x - t, y - t,
				w - 2 * t, h - 2 * t,
				r > t ? r - t : 1);
			if (!outer || inner) {
				continue;
			}
			unsigned char *d = &px[(y * (size_t)w + x) * 4];
			d[0] = (unsigned char)(col[0] * col[3] * 255);
			d[1] = (unsigned char)(col[1] * col[3] * 255);
			d[2] = (unsigned char)(col[2] * col[3] * 255);
			d[3] = (unsigned char)(col[3] * 255);
		}
	}
	return rgba_buffer_take(w, h, px);
}

static struct wlr_scene_buffer *ui_attach(struct wlr_scene_tree *tree,
		struct wlr_buffer *buf) {
	return buf != NULL ? wlr_scene_buffer_create(tree, buf) : NULL;
}

/* Render one string; small static cache since overlay texts repeat. */

/* Download arrow (shaft + head + underline), drawn by hand. */







/* Intentionally frees nothing: wlroots runs this whenever the seat
 * swaps selections, possibly while clipboard managers still hold
 * offers from earlier copies - freeing there SIGSEGV'd in the mime
 * loop. Sources are tiny and copies are human-rate; the leak is the
 * safe tradeoff. The payload is owned by clipboard_set, not the
 * source, so nothing else needs releasing either. */



/* Render the given output-local box from the scene graph into fresh RGBA
 * memory (8 bit/channel). The whole output frame is composed offscreen via
 * wlr_scene_output_build_state(), then cropped. */
static unsigned char *screenshot_capture(struct amber_server *server,
		struct amber_output *output, struct wlr_box lbox) {
	struct wlr_output *wlr_output = output->wlr_output;
	int ow = wlr_output->width, oh = wlr_output->height;
	if (ow <= 0 || oh <= 0 || lbox.width <= 0 || lbox.height <= 0 ||
			lbox.x < 0 || lbox.y < 0 ||
			lbox.x + lbox.width > ow || lbox.y + lbox.height > oh) {
		return NULL;
	}
	struct wlr_scene_output *scene_output =
		wlr_scene_get_scene_output(server->scene, wlr_output);
	if (scene_output == NULL) {
		return NULL;
	}

	/* An extra (invisible) full-output node defeats direct scanout so
	 * the scene is guaranteed to be composed into the capture buffer. */
	float transparent[4] = {0, 0, 0, 0};
	struct wlr_scene_rect *guard = wlr_scene_rect_create(
		output->layer_trees[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY],
		ow, oh, transparent);

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);
	bool ok = wlr_scene_output_build_state(scene_output, &state, NULL);
	if (ok) {
		/* Presenting the freshly rendered frame is visually a no-op
		 * and gives us ownership semantics identical to a normal
		 * frame commit. */
		ok = wlr_output_commit_state(wlr_output, &state);
	}
	wlr_scene_node_destroy(&guard->node);
	if (!ok || !(state.committed & WLR_OUTPUT_STATE_BUFFER) ||
			state.buffer == NULL) {
		wlr_output_state_finish(&state);
		return NULL;
	}

	unsigned char *result = NULL;
	struct wlr_texture *texture =
		wlr_texture_from_buffer(server->renderer, state.buffer);
	if (texture != NULL) {
		uint32_t fmt = DRM_FORMAT_ABGR8888;
		uint32_t stride = (uint32_t)texture->width * 4;
		void *raw = malloc(stride * texture->height);
		if (raw != NULL) {
			struct wlr_texture_read_pixels_options ropts = {
				.format = fmt,
				.data = raw,
				.stride = stride,
				.src_box = { .x = 0, .y = 0,
					.width = texture->width,
					.height = texture->height },
			};
			bool ok = wlr_texture_read_pixels(texture, &ropts);
			if (!ok) {
				/* Driver refused ABGR: retry with whatever it
				 * prefers and let the swizzle below cope. */
				fmt = wlr_texture_preferred_read_format(
					texture);
				if (fmt == DRM_FORMAT_INVALID) {
					fmt = DRM_FORMAT_ABGR8888;
				}
				ropts.format = fmt;
				ok = wlr_texture_read_pixels(texture, &ropts);
			}
			if (ok) {
				result = malloc(
					(size_t)lbox.width * lbox.height * 4);
				for (int row = 0; row < lbox.height; row++) {
					unsigned char *src = (unsigned char *)raw +
						(size_t)(lbox.y + row) * stride +
						(size_t)lbox.x * 4;
					unsigned char *dst = result +
						(size_t)row * lbox.width * 4;
					switch (fmt) {
					case DRM_FORMAT_ABGR8888: // bytes R,G,B,A
						memcpy(dst, src,
							(size_t)lbox.width * 4);
						break;
					case DRM_FORMAT_XRGB8888:
					case DRM_FORMAT_ARGB8888:
						for (int x = 0; x < lbox.width; x++) {
							dst[x*4+0] = src[x*4+2];
							dst[x*4+1] = src[x*4+1];
							dst[x*4+2] = src[x*4+0];
							dst[x*4+3] = fmt ==
								DRM_FORMAT_ARGB8888 ?
								src[x*4+3] : 255;
						}
						break;
					default: // XRGB-ish fallbacks
						for (int x = 0; x < lbox.width; x++) {
							dst[x*4+0] = src[x*4+3];
							dst[x*4+1] = src[x*4+2];
							dst[x*4+2] = src[x*4+1];
							dst[x*4+3] = 255;
						}
						break;
					}
				}
			}
			free(raw);
		}
		wlr_texture_destroy(texture);
	}
	wlr_output_state_finish(&state);
	return result;
}


/* Alpha-blend the default arrow cursor into a captured frame so "P" can
 * include the pointer position in the saved image. */

/* (Re)capture and attach the frozen frame. Live UI nodes are hidden for
 * the capture so dim/borders never bake into the image. */



/* ==================== Built-in window switcher ====================
 * Dim + grid of STATIC thumbnails captured at open time (cheap on old
 * GPUs; no live compositing). Thumbnails are the labels: no text. */

static void ext_ws_dyn_sync(struct amber_output *output) {
	struct amber_server *server = output->server;
	if (!server->dyn_ws || server->ext_ws_mgr == NULL ||
			output->ext_group == NULL) {
		return;
	}
	for (int i = 0; i < AMBER_WORKSPACE_COUNT; i++) {
		bool want = i == output->active_workspace ||
			!wl_list_empty(&output->workspaces[i].toplevels);
		if (want && output->ext_ws[i] == NULL) {
			char id[16], name[16];
			snprintf(id, sizeof(id), "amber-%d", i);
			snprintf(name, sizeof(name), "%d", i + 1);
			struct wlr_ext_workspace_handle_v1 *h =
				wlr_ext_workspace_handle_v1_create(
					server->ext_ws_mgr, id,
					EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ACTIVATE);
			if (h == NULL) {
				continue;
			}
			wlr_ext_workspace_handle_v1_set_name(h, name);
			uint32_t coords[2] = {0, (uint32_t)i};
			wlr_ext_workspace_handle_v1_set_coordinates(h,
				coords, 2);
			wlr_ext_workspace_handle_v1_set_group(h,
				output->ext_group);
			output->ext_ws[i] = h;
		} else if (!want && i != output->active_workspace &&
				output->ext_ws[i] != NULL) {
			wlr_ext_workspace_handle_v1_destroy(
				output->ext_ws[i]);
			output->ext_ws[i] = NULL;
		}
	}
}

static void switcher_close(struct amber_server *server) {
	if (!server->sw_active && server->sw_dim == NULL) {
		return;
	}
	for (size_t i = 0; i < server->sw_count; i++) {
		if (server->sw_tiles[i].thumb != NULL) {
			wlr_scene_node_destroy(
				&server->sw_tiles[i].thumb->node);
		}
		if (server->sw_tiles[i].icon_node != NULL) {
			wlr_scene_node_destroy(
				&server->sw_tiles[i].icon_node->node);
		}
		if (server->sw_tiles[i].fallback != NULL) {
			wlr_scene_node_destroy(
				&server->sw_tiles[i].fallback->node);
		}
		if (server->sw_tiles[i].ring != NULL) {
			wlr_scene_node_destroy(
				&server->sw_tiles[i].ring->node);
		}
		server->sw_tiles[i].tl = NULL;
	}
	server->sw_count = 0;
	if (server->sw_dim != NULL) {
		wlr_scene_node_destroy(&server->sw_dim->node);
		server->sw_dim = NULL;
	}
	server->sw_active = false;
	server->sw_output = NULL;
}

static void switcher_select(struct amber_server *server, size_t idx) {
	if (server->sw_count == 0) {
		return;
	}
	idx %= server->sw_count;
	if (server->sw_selected < server->sw_count &&
			server->sw_tiles[server->sw_selected].ring != NULL) {
		wlr_scene_node_set_enabled(
			&server->sw_tiles[server->sw_selected].ring->node,
			false);
	}
	server->sw_selected = idx;
	if (server->sw_tiles[idx].ring != NULL) {
		wlr_scene_node_set_enabled(
			&server->sw_tiles[idx].ring->node, true);
	}
}

static void switcher_activate_selected(struct amber_server *server) {
	struct amber_toplevel *t = server->sw_count > 0 &&
		server->sw_selected < server->sw_count
		? server->sw_tiles[server->sw_selected].tl
		: NULL;
	switcher_close(server);
	if (t == NULL || !toplevel_alive(t)) {
		return;
	}
	if (!t->floating && t->output != NULL &&
			t->workspace != t->output->active_workspace) {
		workspace_switch(t->output, t->workspace);
	}
	focus_toplevel(t);
	if (!t->floating && t->output != NULL) {
		/* Same as click-focus: re-arrange so the camera scrolls the
		 * activated column into view even when it was off-screen. */
		workspace_arrange(toplevel_workspace(t));
	}
}

static void switcher_key(struct amber_server *server,
		const xkb_keysym_t *syms, int nsyms) {
	for (int i = 0; i < nsyms; i++) {
		xkb_keysym_t sym = syms[i];
		size_t n = server->sw_count;
		size_t cols = server->sw_cols > 0 ? server->sw_cols : 1;
		if (sym == XKB_KEY_Escape) {
			switcher_close(server);
			return;
		}
		if (sym == XKB_KEY_Tab || sym == XKB_KEY_Right) {
			switcher_select(server, server->sw_selected + 1);
			return;
		}
		if (sym == XKB_KEY_Left) {
			switcher_select(server, server->sw_selected + n - 1);
			return;
		}
		if (sym == XKB_KEY_Down) {
			switcher_select(server, server->sw_selected + cols);
			return;
		}
		if (sym == XKB_KEY_Up) {
			switcher_select(server, server->sw_selected + n -
				(n > cols ? cols : 1));
			return;
		}
		if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter ||
				sym == XKB_KEY_space) {
			switcher_activate_selected(server);
			return;
		}
		if (sym == XKB_KEY_Home) {
			switcher_select(server, 0);
			return;
		}
		if (sym == XKB_KEY_End) {
			switcher_select(server, n - 1);
			return;
		}
		if (sym == XKB_KEY_Page_Down) {
			switcher_select(server, server->sw_selected + cols);
			return;
		}
		if (sym == XKB_KEY_Page_Up) {
			switcher_select(server, server->sw_selected + n -
				(n > cols ? cols : 1));
			return;
		}
		if (sym >= XKB_KEY_1 && sym <= XKB_KEY_9) {
			size_t d = (size_t)(sym - XKB_KEY_1);
			if (d < n) {
				switcher_select(server, d);
				switcher_activate_selected(server);
			}
			return;
		}
	}
}

static void switcher_button(struct amber_server *server,
		const struct wlr_pointer_button_event *event) {
	if (event->state != WL_POINTER_BUTTON_STATE_PRESSED ||
			server->sw_output == NULL) {
		return;
	}
	double cx = server->cursor->x - server->sw_output->layout_box.x;
	double cy = server->cursor->y - server->sw_output->layout_box.y;
	for (size_t i = 0; i < server->sw_count; i++) {
		struct wlr_box *b = &server->sw_tiles[i].box;
		if (cx >= b->x && cy >= b->y &&
				cx < b->x + b->width && cy < b->y + b->height) {
			switcher_select(server, i);
			switcher_activate_selected(server);
			return;
		}
	}
	switcher_close(server);
}

/* ================= Built-in overview (niri-style) =====================
 * Pans out to show every workspace of the focused output stacked
 * vertically (workspace 0 on top, 8 at the bottom) as scaled live-ish
 * snapshots; click one to jump to that workspace and focus its window.
 * Each row is captured by temporarily making its workspace the visible
 * one, reusing screenshot_capture (the same machinery the switcher and
 * the thumbnail path use). */

static struct amber_output *overview_output(struct amber_server *server) {
	struct amber_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		if (output->ov_active) {
			return output;
		}
	}
	return NULL;
}

/* Temporarily show workspace `idx` and grab a full-output snapshot.
 * Always restores the originally visible workspace/tree enable state. */
static struct wlr_buffer *overview_capture(struct amber_server *server,
		struct amber_output *output, int idx) {
	struct wlr_output *wlr_output = output->wlr_output;
	if (wlr_output->width <= 0 || wlr_output->height <= 0) {
		return NULL;
	}
	int og = output->active_workspace;
	if (og == idx) {
		return NULL; // captured live by the caller instead
	}
	/* Anchor the target tree at the output origin and show it. */
	wlr_scene_node_set_position(&output->workspaces[idx].tree->node,
		output->layout_box.x, output->layout_box.y);
	wlr_scene_node_set_enabled(
		&output->workspaces[idx].tree->node, true);
	wlr_scene_node_set_enabled(
		&output->workspaces[og].tree->node, false);
	struct wlr_buffer *buf = NULL;
	unsigned char *px = screenshot_capture(server, output,
		(struct wlr_box){ .x = 0, .y = 0,
			.width = wlr_output->width,
			.height = wlr_output->height });
	if (px != NULL) {
		buf = rgba_buffer_take(wlr_output->width,
			wlr_output->height, px);
	}
	/* Restore visibility immediately, before anyone repaints. */
	wlr_scene_node_set_enabled(
		&output->workspaces[og].tree->node, true);
	wlr_scene_node_set_enabled(
		&output->workspaces[idx].tree->node, false);
	return buf;
}

static void overview_clear_rows(struct amber_output *output) {
	for (int i = 0; i < AMBER_WORKSPACE_COUNT; i++) {
		struct overview_row *row = output->ov_rows[i];
		if (row == NULL) {
			continue;
		}
		if (row->thumb != NULL) {
			wlr_scene_node_destroy(&row->thumb->node);
		}
		if (row->ring != NULL) {
			wlr_scene_node_destroy(&row->ring->node);
		}
		if (row->fallback != NULL) {
			wlr_scene_node_destroy(&row->fallback->node);
		}
		if (row->box != NULL) {
			wlr_scene_node_destroy(&row->box->node);
		}
		if (row->buf != NULL) {
			wlr_buffer_unlock(row->buf);
		}
		free(row);
		output->ov_rows[i] = NULL;
	}
}

static void overview_close(struct amber_server *server) {
	struct amber_output *output = overview_output(server);
	if (output == NULL) {
		return;
	}
	/* Row nodes are children of ov_tree: destroying the tree frees them
	 * too. Clear them first (freeing the row structs/buffers), then the
	 * now-empty container tree, so nothing is double-destroyed. */
	overview_clear_rows(output);
	if (output->ov_dim != NULL) {
		wlr_scene_node_destroy(&output->ov_dim->node);
		output->ov_dim = NULL;
	}
	if (output->ov_tree != NULL) {
		wlr_scene_node_destroy(&output->ov_tree->node);
		output->ov_tree = NULL;
	}
	output->ov_active = false;
}

static void overview_select(struct amber_server *server, int idx) {
	struct amber_output *output = overview_output(server);
	if (output == NULL) {
		return;
	}
	for (int i = 0; i < AMBER_WORKSPACE_COUNT; i++) {
		struct overview_row *row = output->ov_rows[i];
		if (row != NULL && row->ring != NULL) {
			wlr_scene_node_set_enabled(&row->ring->node,
				i == idx);
		}
	}
}

static void overview_activate(struct amber_server *server, int idx) {
	struct amber_output *output = overview_output(server);
	int target = idx;
	overview_close(server);
	if (output == NULL || target < 0 ||
			target >= AMBER_WORKSPACE_COUNT) {
		return;
	}
	if (target != output->active_workspace) {
		workspace_switch(output, target);
	}
	struct amber_workspace *ws = &output->workspaces[target];
	struct amber_toplevel *t;
	wl_list_for_each(t, &ws->toplevels, link) {
		if (toplevel_alive(t)) {
			focus_toplevel(t);
			break;
		}
	}
	workspace_arrange(ws);
}

static void overview_key(struct amber_server *server,
		const xkb_keysym_t *syms, int nsyms) {
	int cur = -1;
	struct amber_output *output = overview_output(server);
	if (output != NULL) {
		cur = output->active_workspace;
	}
	for (int i = 0; i < nsyms; i++) {
		xkb_keysym_t sym = syms[i];
		if (sym == XKB_KEY_Escape) {
			overview_close(server);
			return;
		}
		if (sym == XKB_KEY_Return || sym == XKB_KEY_space) {
			overview_activate(server, cur);
			return;
		}
		if (sym == XKB_KEY_Down || sym == XKB_KEY_KP_Down ||
				sym == XKB_KEY_j) {
			cur = cur < AMBER_WORKSPACE_COUNT - 1
				? cur + 1 : cur;
			overview_select(server, cur);
			continue;
		}
		if (sym == XKB_KEY_Up || sym == XKB_KEY_KP_Up ||
				sym == XKB_KEY_k) {
			cur = cur > 0 ? cur - 1 : cur;
			overview_select(server, cur);
			continue;
		}
		if (sym >= XKB_KEY_1 && sym <= XKB_KEY_9) {
			int d = (int)(sym - XKB_KEY_1);
			overview_activate(server, d);
			return;
		}
	}
}

static void overview_button(struct amber_server *server,
		const struct wlr_pointer_button_event *event) {
	if (event->state != WL_POINTER_BUTTON_STATE_PRESSED) {
		return;
	}
	struct amber_output *output = overview_output(server);
	if (output == NULL) {
		return;
	}
	double cx = server->cursor->x - output->layout_box.x;
	double cy = server->cursor->y - output->layout_box.y;
	for (int i = 0; i < AMBER_WORKSPACE_COUNT; i++) {
		struct overview_row *row = output->ov_rows[i];
		if (row == NULL) {
			continue;
		}
		struct wlr_box *b = &row->geom;
		if (cx >= b->x && cy >= b->y &&
				cx < b->x + b->width && cy < b->y + b->height) {
			overview_activate(server, i);
			return;
		}
	}
	overview_close(server);
}

static void overview_open(struct amber_server *server,
		struct amber_output *output) {
	if (output->ov_active) {
		overview_close(server);
		return;
	}
	if (server->sw_active) {
		switcher_close(server);
	}
	/* Nothing to show on a disabled/zero-size output. */
	struct wlr_output *wlr_output = output->wlr_output;
	if (wlr_output->width <= 0 || wlr_output->height <= 0) {
		return;
	}
	/* Finish any in-flight workspace slide so tree visibility is
	 * well-defined while we temporarily flip enabled states. */
	struct amber_animation *anim, *tmp;
	wl_list_for_each_safe(anim, tmp, &server->animations, link) {
		if (anim->kind == ANIM_WS_SLIDE &&
				anim->output == output) {
			animation_destroy(server, anim, true);
		}
	}

	/* Capture every workspace BEFORE creating any overlay nodes, so no
	 * dim/row geometry leaks into the snapshots. */
	struct wlr_buffer *snaps[AMBER_WORKSPACE_COUNT] = {0};
	for (int i = 0; i < AMBER_WORKSPACE_COUNT; i++) {
		if (i == output->active_workspace) {
			unsigned char *px = screenshot_capture(server, output,
				(struct wlr_box){ .x = 0, .y = 0,
					.width = wlr_output->width,
					.height = wlr_output->height });
			snaps[i] = px != NULL
				? rgba_buffer_take(wlr_output->width,
					wlr_output->height, px)
				: NULL;
		} else {
			snaps[i] = overview_capture(server, output, i);
		}
	}

	output->ov_active = true;

	/* Dim backdrop + container tree for the stacked rows. */
	float dim_col[4] = {0.0f, 0.0f, 0.0f, 0.6f};
	output->ov_dim = wlr_scene_rect_create(
		output->layer_trees[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY],
		wlr_output->width, wlr_output->height, dim_col);
	output->ov_tree = wlr_scene_tree_create(
		output->layer_trees[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY]);
	if (output->ov_dim == NULL || output->ov_tree == NULL) {
		for (int i = 0; i < AMBER_WORKSPACE_COUNT; i++) {
			if (snaps[i] != NULL) {
				wlr_buffer_unlock(snaps[i]);
			}
		}
		overview_close(server);
		return;
	}
	struct wlr_scene_tree *ov = output->ov_tree;

	/* Layout rows: N workspaces stacked vertically, margin + gaps. */
	int ow = wlr_output->width, oh = wlr_output->height;
	int margin = 40, gap = 40;
	int avail_h = oh - 2 * margin - gap * (AMBER_WORKSPACE_COUNT - 1);
	int row_h = avail_h / AMBER_WORKSPACE_COUNT;
	if (row_h < 40) {
		row_h = 40;
	}
	int row_w = row_h * 16 / 9;
	if (row_w > ow - 2 * margin) {
		row_w = ow - 2 * margin;
	}
	if (row_w < 80) {
		row_w = 80;
	}
	int oy = margin;
	int ox = (ow - row_w) / 2;

	for (int i = 0; i < AMBER_WORKSPACE_COUNT; i++) {
		struct overview_row *row = calloc(1, sizeof(*row));
		if (row == NULL) {
			continue;
		}
		row->output = output;
		row->workspace = i;
		row->geom = (struct wlr_box){
			.x = ox, .y = oy, .width = row_w, .height = row_h };

		int ring_pad = 6;
		if (snaps[i] == NULL) {
			float fb_col[4] = {0.13f, 0.13f, 0.16f, 0.97f};
			row->fallback = ui_attach(ov,
				ui_rounded_card(row_w, row_h, 16, fb_col));
			if (row->fallback != NULL) {
				wlr_scene_node_set_position(
					&row->fallback->node, ox, oy);
			}
		} else {
			row->buf = snaps[i];
			snaps[i] = NULL;
			row->thumb = wlr_scene_buffer_create(ov, row->buf);
			if (row->thumb != NULL) {
				wlr_scene_node_set_position(
					&row->thumb->node, ox, oy);
				wlr_scene_buffer_set_dest_size(
					row->thumb, row_w, row_h);
			}
			wlr_buffer_unlock(row->buf);
			row->buf = NULL;
		}
		struct wlr_buffer *ring = ui_rounded_ring(
			row_w + 2 * ring_pad, row_h + 2 * ring_pad, 16, 4,
			(float[4]){1.0f, 1.0f, 1.0f, 1.0f});
		row->ring = ui_attach(ov, ring);
		if (row->ring != NULL) {
			wlr_scene_node_set_position(
				&row->ring->node, ox - ring_pad, oy - ring_pad);
			wlr_scene_node_set_enabled(&row->ring->node,
				i == output->active_workspace);
		}
		output->ov_rows[i] = row;
		oy += row_h + gap;
	}
	for (int i = 0; i < AMBER_WORKSPACE_COUNT; i++) {
		if (snaps[i] != NULL) {
			wlr_buffer_unlock(snaps[i]);
		}
	}
	overview_select(server, output->active_workspace);
}

/* App-icon badges for the switcher: loaded once per app_id and cached
 * across opens (buffers persist by design). hicolor covers most apps;
 * pixmaps catches the rest. */
static struct {
	char app_id[64];
	struct wlr_buffer *buf;
} sw_icons[32];
static size_t sw_icons_n;

static struct wlr_buffer *switcher_icon_for(const char *app_id) {
	if (app_id == NULL || *app_id == '\0') {
		return NULL;
	}
	for (size_t i = 0; i < sw_icons_n; i++) {
		if (strcmp(sw_icons[i].app_id, app_id) == 0) {
			return sw_icons[i].buf;
		}
	}
	static const char *const dirs[] = {
		"/usr/share/icons/hicolor/64x64/apps",
		"/usr/share/icons/hicolor/48x48/apps",
		"/usr/share/icons/hicolor/32x32/apps",
		"/usr/share/pixmaps",
	};
	char path[512];
	struct wlr_buffer *out = NULL;
	for (size_t d = 0; d < sizeof(dirs) / sizeof(dirs[0]); d++) {
		snprintf(path, sizeof(path), "%s/%s.png", dirs[d], app_id);
		int iw = 0, ih = 0;
		unsigned char *px = stbi_load(path, &iw, &ih, NULL, 4);
		if (px != NULL) {
			out = rgba_buffer_take(iw, ih, px);
			break;
		}
	}
	if (out != NULL && sw_icons_n < sizeof(sw_icons) / sizeof(sw_icons[0])) {
		snprintf(sw_icons[sw_icons_n].app_id,
			sizeof(sw_icons[sw_icons_n].app_id), "%s", app_id);
		sw_icons[sw_icons_n].buf = out;
		sw_icons_n++;
	}
	return out;
}

static void switcher_open(struct amber_server *server) {
	struct amber_output *out = active_output(server);
	if (out == NULL) {
		return;
	}
	switcher_close(server);

	struct amber_workspace *ws =
		&out->workspaces[out->active_workspace];
	struct wlr_box lim = out->layout_box;
	struct amber_toplevel *t;
	size_t n = 0;
	size_t max_tiles = sizeof(server->sw_tiles) /
		sizeof(server->sw_tiles[0]);
	wl_list_for_each(t, &ws->toplevels, link) {
		if (n >= max_tiles) {
			break;
		}
		if (!toplevel_alive(t)) {
			continue;
		}
		server->sw_tiles[n++].tl = t;
	}
	if (n == 0) {
		return;
	}

	int margin = 48, gap = 14, pad = 6;
	size_t cols = n <= 5 ? (n == 0 ? 1 : n) : 5;
	size_t rows = (n + cols - 1) / cols;
	int avail = lim.width - 2 * margin - gap * (int)(cols - 1);
	int cell_w = (int)(avail / (int)cols);
	if (cell_w > 260) {
		cell_w = 260;
	}
	if (cell_w < 120) {
		cell_w = 120;
	}
	int cell_h = cell_w * 10 / 16;
	int total_w = cell_w * (int)cols + gap * (int)(cols - 1);
	int total_h = cell_h * (int)rows + gap * (int)(rows - 1);
	int ox = (lim.width - total_w) / 2;
	int oy = (lim.height - total_h) / 2;

	/* Capture BEFORE creating the dim rect so frames come out clean. */
	for (size_t i = 0; i < n; i++) {
		t = server->sw_tiles[i].tl;
		struct wlr_box g =
			t->xdg_toplevel->base->geometry;
		struct wlr_box region = {
			.x = (int)t->scene_tree->node.x + g.x,
			.y = (int)t->scene_tree->node.y + g.y,
			.width = g.width,
			.height = g.height,
		};
		if (region.x < 0) {
			region.width += region.x;
			region.x = 0;
		}
		if (region.y < 0) {
			region.height += region.y;
			region.y = 0;
		}
		if (region.x + region.width > lim.width) {
			region.width = lim.width - region.x;
		}
		if (region.y + region.height > lim.height) {
			region.height = lim.height - region.y;
		}
		struct wlr_buffer *buf = NULL;
		if (region.width > 24 && region.height > 24) {
			unsigned char *frame = screenshot_capture(server,
				out, region);
			if (frame != NULL) {
				buf = rgba_buffer_take(
					region.width, region.height, frame);
			}
		}
		server->sw_tiles[i].thumb = NULL;
		server->sw_tiles[i].icon_node = NULL;
		server->sw_tiles[i].fallback = NULL;
		server->sw_tiles[i].ring = NULL;
		server->sw_tiles[i].box = (struct wlr_box){0};
		server->sw_tiles[i].buf = buf;
	}

	float dim_col[4] = {0.0f, 0.0f, 0.0f, 0.55f};
	server->sw_dim = wlr_scene_rect_create(
		out->layer_trees[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY],
		lim.width, lim.height, dim_col);
	if (server->sw_dim == NULL) {
		for (size_t i = 0; i < n; i++) {
			if (server->sw_tiles[i].buf != NULL) {
				wlr_buffer_unlock(server->sw_tiles[i].buf);
				server->sw_tiles[i].buf = NULL;
			}
			server->sw_tiles[i].tl = NULL;
		}
		return;
	}

	struct wlr_scene_tree *ov =
		out->layer_trees[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY];
	for (size_t i = 0; i < n; i++) {
		t = server->sw_tiles[i].tl;
		int row = (int)(i / cols), col = (int)(i % cols);
		int x = ox + col * (cell_w + gap);
		int y = oy + row * (cell_h + gap);

		struct wlr_buffer *rb = ui_rounded_ring(
			cell_w + 2 * pad + 8, cell_h + 2 * pad + 8, 16, 3,
			(float[4]){1.0f, 1.0f, 1.0f, 1.0f});
		server->sw_tiles[i].ring = ui_attach(ov, rb);
		if (server->sw_tiles[i].ring != NULL) {
			wlr_scene_node_set_position(
				&server->sw_tiles[i].ring->node,
				x - pad - 4, y - pad - 4);
			wlr_scene_node_set_enabled(
				&server->sw_tiles[i].ring->node, false);
		}

		struct wlr_buffer *buf = server->sw_tiles[i].buf;
		if (buf != NULL) {
			server->sw_tiles[i].thumb = wlr_scene_buffer_create(
				out->layer_trees[
					ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY],
				buf);
			if (server->sw_tiles[i].thumb != NULL) {
				wlr_scene_node_set_position(
					&server->sw_tiles[i].thumb->node, x, y);
				wlr_scene_buffer_set_dest_size(
					server->sw_tiles[i].thumb,
					cell_w, cell_h);
			}
			wlr_buffer_unlock(buf);
			server->sw_tiles[i].buf = NULL;
		} else {
			float fb_col[4] = {0.13f, 0.13f, 0.16f, 0.97f};
			server->sw_tiles[i].fallback = ui_attach(ov,
				ui_rounded_card(cell_w, cell_h, 14,
					fb_col));
			if (server->sw_tiles[i].fallback != NULL) {
				wlr_scene_node_set_position(
					&server->sw_tiles[i].fallback->node,
					x, y);
			}
		}
		server->sw_tiles[i].box = (struct wlr_box){
			.x = x, .y = y, .width = cell_w, .height = cell_h };

		struct wlr_buffer *icon = switcher_icon_for(
			t->xdg_toplevel->app_id);
		if (icon != NULL) {
			server->sw_tiles[i].icon_node =
				wlr_scene_buffer_create(
					out->layer_trees[
						ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY],
					icon);
			if (server->sw_tiles[i].icon_node != NULL) {
				int isz = 64;
				wlr_scene_buffer_set_dest_size(
					server->sw_tiles[i].icon_node,
					isz, isz);
				wlr_scene_node_set_position(
					&server->sw_tiles[i].icon_node->node,
					x + (cell_w - isz) / 2,
					y + (cell_h - isz) / 2);
			}
		}
	}

	server->sw_count = n;
	server->sw_cols = cols;
	server->sw_output = out;
	server->sw_active = true;
	/* Alt-Tab feel: land on the window AFTER the focused one so a single
	 * SUPER+TAB opens AND advances to the next window. If focus isn't
	 * one of the tiles, just start at the first tile. */
	size_t start = 0;
	if (server->focused_toplevel != NULL) {
		for (size_t i = 0; i < n; i++) {
			if (server->sw_tiles[i].tl == server->focused_toplevel) {
				start = (i + 1) % n;
				break;
			}
		}
	}
	switcher_select(server, start);
}











static bool keyboard_held_has(struct amber_keyboard *keyboard,
		uint32_t keycode) {
	for (size_t i = 0; i < keyboard->n_held; i++) {
		if (keyboard->held[i] == keycode) {
			return true;
		}
	}
	return false;
}

static void keyboard_held_mark(struct amber_keyboard *keyboard,
		uint32_t keycode) {
	if (!keyboard_held_has(keyboard, keycode) &&
			keyboard->n_held < sizeof(keyboard->held) /
				sizeof(keyboard->held[0])) {
		keyboard->held[keyboard->n_held++] = keycode;
	}
}

static void keyboard_held_clear(struct amber_keyboard *keyboard,
		uint32_t keycode) {
	for (size_t i = 0; i < keyboard->n_held; i++) {
		if (keyboard->held[i] == keycode) {
			keyboard->held[i] = keyboard->held[--keyboard->n_held];
			return;
		}
	}
}

static bool keyboard_consume_swallowed(struct amber_keyboard *keyboard,
		uint32_t keycode) {
	for (size_t i = 0; i < keyboard->n_swallowed; i++) {
		if (keyboard->swallowed[i] == keycode) {
			keyboard->swallowed[i] =
				keyboard->swallowed[--keyboard->n_swallowed];
			return true;
		}
	}
	return false;
}

static void keyboard_mark_swallowed(struct amber_keyboard *keyboard,
		uint32_t keycode) {
	if (keyboard->n_swallowed <
			sizeof(keyboard->swallowed) /
			sizeof(keyboard->swallowed[0])) {
		keyboard->swallowed[keyboard->n_swallowed++] = keycode;
	}
}

static void keyboard_handle_key(
		struct wl_listener *listener, void *data) {
	/* This event is raised when a key is pressed or released. */
	struct amber_keyboard *keyboard =
		wl_container_of(listener, keyboard, key);
	struct amber_server *server = keyboard->server;
	struct wlr_keyboard_key_event *event = data;
	struct wlr_seat *seat = server->seat;

	idle_mark_activity(server);

	if (event->state == WL_KEYBOARD_KEY_STATE_RELEASED) {
		keyboard_held_clear(keyboard, event->keycode);
		if (keyboard_consume_swallowed(keyboard, event->keycode)) {
			return;
		}
	}

	/* Translate libinput keycode -> xkbcommon */
	uint32_t keycode = event->keycode + 8;
	/* Get a list of keysyms based on the keymap for this keyboard */
	const xkb_keysym_t *syms;
	int nsyms = xkb_state_key_get_syms(
			keyboard->wlr_keyboard->xkb_state, keycode, &syms);

	bool handled = false;
	if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		bool is_repeat = keyboard_held_has(keyboard, event->keycode);
		keyboard_held_mark(keyboard, event->keycode);
		uint32_t modifiers =
			wlr_keyboard_get_modifiers(keyboard->wlr_keyboard) &
			~keyboard->lock_mask;
		if (!is_repeat) {
			struct amber_output *ov_out = overview_output(server);
			if (ov_out != NULL) {
				keyboard_mark_swallowed(keyboard,
					event->keycode);
				overview_key(server, syms, nsyms);
				return;
			}
			if (server->sw_active) {
				keyboard_mark_swallowed(keyboard,
					event->keycode);
				switcher_key(server, syms, nsyms);
				return;
			}
			for (int i = 0; i < nsyms && !handled; i++) {
				/* Ctrl+Alt+Fx hands the VT back to logind/
				 * seatd; without this the combo falls
				 * through to clients and the TTY is
				 * unreachable. */
				if (syms[i] >= XKB_KEY_XF86Switch_VT_1 &&
						syms[i] <=
						XKB_KEY_XF86Switch_VT_12) {
					if (server->session != NULL) {
						wlr_session_change_vt(
							server->session,
							(int)(syms[i] -
								XKB_KEY_XF86Switch_VT_1)
								+ 1);
					}
					keyboard_mark_swallowed(keyboard,
						event->keycode);
					handled = true;
					break;
				}
			}
			for (int i = 0; i < nsyms && !handled; i++) {
				xkb_keysym_t sym = syms[i];
				/* Config key names mean physical keys: fold
				 * shifted/caps letters so SUPER+SHIFT+t
				 * matches a bind stored as lowercase. */
				if (sym >= XKB_KEY_A && sym <= XKB_KEY_Z) {
					sym += XKB_KEY_a - XKB_KEY_A;
				}
				for (size_t j = 0;
						j < server->binding_count;
						j++) {
					const struct amber_binding *binding =
						&server->bindings[j];
					if (binding->mods == modifiers &&
							binding->sym == sym) {
						binding_exec(server, binding);
						keyboard_mark_swallowed(
							keyboard,
							event->keycode);
						handled = true;
						break;
					}
				}
			}
		}
	}

	if (!handled) {
		/* Otherwise, we pass it along to the client. */
		wlr_seat_set_keyboard(seat, keyboard->wlr_keyboard);
		wlr_seat_keyboard_notify_key(seat, event->time_msec,
			event->keycode, event->state);
	}
}

static void keyboard_handle_destroy(struct wl_listener *listener, void *data) {
	/* This event is raised by the keyboard base wlr_input_device to signal
	 * the destruction of the wlr_keyboard. It will no longer receive events
	 * and should be destroyed.
	 */
	struct amber_keyboard *keyboard =
		wl_container_of(listener, keyboard, destroy);
	wl_list_remove(&keyboard->modifiers.link);
	wl_list_remove(&keyboard->key.link);
	wl_list_remove(&keyboard->destroy.link);
	wl_list_remove(&keyboard->link);
	free(keyboard);
}

static void server_new_keyboard(struct amber_server *server,
		struct wlr_input_device *device) {
	struct wlr_keyboard *wlr_keyboard = wlr_keyboard_from_input_device(device);

	struct amber_keyboard *keyboard = calloc(1, sizeof(*keyboard));
	keyboard->server = server;
	keyboard->wlr_keyboard = wlr_keyboard;

	/* We need to prepare an XKB keymap and assign it to the keyboard.
	 * This assumes the defaults (e.g. layout = "us"). The xkb context is
	 * shared across all keyboards (see amber_server). */
	struct xkb_context *context = server->xkb_context;
	struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, NULL,
		XKB_KEYMAP_COMPILE_NO_FLAGS);

	wlr_keyboard_set_keymap(wlr_keyboard, keymap);
	xkb_keymap_unref(keymap);

	/* Remember which modifiers are locks so keybinding matching can
	 * ignore them (NumLock must not break Super+1..9). wlr normalizes
	 * modifier bits to the same indexes xkb uses, so 1 << index works. */
	keyboard->lock_mask = 0;
	xkb_mod_index_t lock_idx =
		xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_CAPS);
	if (lock_idx != XKB_MOD_INVALID) {
		keyboard->lock_mask |= (uint32_t)1 << lock_idx;
	}
	lock_idx = xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_NUM);
	if (lock_idx != XKB_MOD_INVALID) {
		keyboard->lock_mask |= (uint32_t)1 << lock_idx;
	}
	wlr_keyboard_set_repeat_info(wlr_keyboard, 25, 600);

	/* Here we set up listeners for keyboard events. */
	keyboard->modifiers.notify = keyboard_handle_modifiers;
	wl_signal_add(&wlr_keyboard->events.modifiers, &keyboard->modifiers);
	keyboard->key.notify = keyboard_handle_key;
	wl_signal_add(&wlr_keyboard->events.key, &keyboard->key);
	keyboard->destroy.notify = keyboard_handle_destroy;
	wl_signal_add(&device->events.destroy, &keyboard->destroy);

	wlr_seat_set_keyboard(server->seat, keyboard->wlr_keyboard);

	/* And add the keyboard to our list of keyboards */
	wl_list_insert(&server->keyboards, &keyboard->link);
}

static void server_new_pointer(struct amber_server *server,
		struct wlr_input_device *device) {
	/* We don't do anything special with pointers. All of our pointer handling
	 * is proxied through wlr_cursor. On another compositor, you might take this
	 * opportunity to do libinput configuration on the device to set
	 * acceleration, etc. */
	wlr_cursor_attach_input_device(server->cursor, device);
}

static void update_seat_capabilities(struct amber_server *server) {
	/* In AmberWM we always have a cursor, even if there are no pointer
	 * devices, so we always include that capability. */
	uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
	if (!wl_list_empty(&server->keyboards)) {
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	}
	wlr_seat_set_capabilities(server->seat, caps);
}

static void server_new_input(struct wl_listener *listener, void *data) {
	/* This event is raised by the backend when a new input device becomes
	 * available. */
	struct amber_server *server =
		wl_container_of(listener, server, new_input);
	struct wlr_input_device *device = data;
	switch (device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		server_new_keyboard(server, device);
		break;
	case WLR_INPUT_DEVICE_POINTER:
		server_new_pointer(server, device);
		break;
	default:
		break;
	}
	/* We need to let the wlr_seat know what our capabilities are, which is
	 * communicated to the client. */
	update_seat_capabilities(server);
}

static void server_new_virtual_keyboard(struct wl_listener *listener,
		void *data) {
	/* zwp_virtual_keyboard_manager_v1: key remappers (kanata, kmonad,
	 * ydotool) inject keys by creating a virtual keyboard bound to our
	 * seat; route it exactly like a hardware keyboard. */
	struct amber_server *server =
		wl_container_of(listener, server, virtual_keyboard_new);
	struct wlr_virtual_keyboard_v1 *vk = data;
	server_new_keyboard(server, &vk->keyboard.base);
	update_seat_capabilities(server);
}

static void server_new_virtual_pointer(struct wl_listener *listener,
		void *data) {
	/* zwp_virtual_pointer_manager_v1: automation (ydotool, kmonad) moves
	 * the cursor by creating a virtual pointer on the seat; wlr_cursor
	 * forwards it like a real mouse. */
	struct amber_server *server =
		wl_container_of(listener, server, virtual_pointer_new);
	struct wlr_virtual_pointer_v1_new_pointer_event *event = data;
	wlr_cursor_attach_input_device(server->cursor,
		&event->new_pointer->pointer.base);
	update_seat_capabilities(server);
}

/* wp_cursor_shape-v1: clients ask us to show a named cursor shape
 * (resize arrows, text caret...) instead of uploading their own. */
static void handle_cursor_shape_request(struct wl_listener *listener,
		void *data) {
	struct amber_server *server =
		wl_container_of(listener, server, cursor_shape_request);
	struct wlr_cursor_shape_manager_v1_request_set_shape_event *event = data;
	/* Only honor the client that currently has pointer focus, same
	 * vetting as seat_request_cursor. */
	if (server->seat->pointer_state.focused_client == event->seat_client) {
		wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr,
			wlr_cursor_shape_v1_name(event->shape));
	}
}

/* xdg-activation: an app with a valid launcher token asks to be shown.
 * Used by "open link in new window" style flows; without this the window
 * opens but never raises/focuses. */
static void handle_xdg_activation_request(struct wl_listener *listener,
		void *data) {
	struct amber_server *server =
		wl_container_of(listener, server, xdg_activation_request);
	struct wlr_xdg_activation_v1_request_activate_event *event = data;
	if (event->surface == NULL || event->token->seat == NULL) {
		return;
	}
	struct wlr_xdg_toplevel *toplevel =
		wlr_xdg_toplevel_try_from_wlr_surface(event->surface);
	if (toplevel == NULL || toplevel->base->data == NULL ||
			!toplevel->base->initialized) {
		return;
	}
	struct wlr_scene_tree *tree = toplevel->base->data;
	struct amber_toplevel *t = tree->node.data;
	/* Pre-map windows have no output yet; toplevel_workspace() would
	 * deref NULL and hand back a garbage-by-arithmetic pointer that no
	 * NULL check downstream can catch. */
	if (t == NULL || t->output == NULL) {
		return;
	}
	struct amber_workspace *ws = toplevel_workspace(t);
	bool on_active_ws = ws != NULL && ws->output != NULL &&
		(ws - ws->output->workspaces) ==
			ws->output->active_workspace;
	if (on_active_ws) {
		focus_toplevel(t);
	} else {
		/* Attention ping from an off-screen workspace: never steal
		 * focus; light up that workspace's pill instead. */
		ext_workspace_set_urgent_by_toplevel(t, true);
	}
}

static void seat_request_cursor(struct wl_listener *listener, void *data) {
	struct amber_server *server = wl_container_of(
			listener, server, request_cursor);
	/* This event is raised by the seat when a client provides a cursor image */
	struct wlr_seat_pointer_request_set_cursor_event *event = data;
	struct wlr_seat_client *focused_client =
		server->seat->pointer_state.focused_client;
	/* This can be sent by any client, so we check to make sure this one is
	 * actually has pointer focus first. */
	if (focused_client == event->seat_client) {
		/* Once we've vetted the client, we can tell the cursor to use the
		 * provided surface as the cursor image. It will set the hardware cursor
		 * on the output that it's currently on and continue to do so as the
		 * cursor moves between outputs. */
		wlr_cursor_set_surface(server->cursor, event->surface,
				event->hotspot_x, event->hotspot_y);
	}
}

static void constraint_deactivate(struct amber_server *server);

static void seat_pointer_focus_change(struct wl_listener *listener, void *data) {
	struct amber_server *server = wl_container_of(
			listener, server, pointer_focus_change);
	/* This event is raised when the pointer focus is changed, including when the
	 * client is closed. We set the cursor image to its default if target surface
	 * is NULL */
	struct wlr_seat_pointer_focus_change_event *event = data;
	if (event->new_surface == NULL) {
		wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
	}
	/* Leaving the constrained surface releases the lock/confine. */
	if (server->active_constraint != NULL &&
			event->new_surface != server->active_constraint->surface) {
		constraint_deactivate(server);
	}
}

static void handle_pointer_constraint_destroy(struct wl_listener *listener,
		void *data) {
	struct amber_constraint_trk *trk =
		wl_container_of(listener, trk, destroy);
	if (trk->server->active_constraint == trk->constraint) {
		trk->server->active_constraint = NULL;
		if (trk->constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED) {
			wlr_cursor_set_xcursor(trk->server->cursor,
				trk->server->cursor_mgr, "default");
		}
	}
	trk->constraint->data = NULL;
	free(trk);
}

static void constraint_deactivate(struct amber_server *server) {
	struct wlr_pointer_constraint_v1 *c = server->active_constraint;
	if (c == NULL) {
		return;
	}
	/* Clear before send: for transient constraints send_deactivated
	 * destroys the constraint, which fires our destroy listener. */
	bool was_locked = c->type == WLR_POINTER_CONSTRAINT_V1_LOCKED;
	server->active_constraint = NULL;
	wlr_pointer_constraint_v1_send_deactivated(c);
	if (was_locked) {
		wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr,
			"default");
	}
}

static void constraint_set_active(struct amber_server *server,
		struct wlr_pointer_constraint_v1 *constraint) {
	if (server->active_constraint == constraint) {
		return;
	}
	if (server->active_constraint != NULL) {
		constraint_deactivate(server);
	}
	/* A constraint only takes hold while the pointer hovers the surface
	 * that requested it (games lock on the frame of a click). */
	struct wlr_seat_pointer_state *ps = &server->seat->pointer_state;
	if (ps->focused_surface != constraint->surface) {
		return;
	}
	server->active_constraint = constraint;
	server->constraint_anchor_x = server->cursor->x;
	server->constraint_anchor_y = server->cursor->y;
	wlr_pointer_constraint_v1_send_activated(constraint);
	if (constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED) {
		wlr_cursor_unset_image(server->cursor);
	}
}

static void handle_pointer_constraint_new(struct wl_listener *listener,
		void *data) {
	struct amber_server *server =
		wl_container_of(listener, server, pointer_constraints_new);
	struct wlr_pointer_constraint_v1 *constraint = data;

	struct amber_constraint_trk *trk = calloc(1, sizeof(*trk));
	if (trk == NULL) {
		return;
	}
	trk->server = server;
	trk->constraint = constraint;
	trk->destroy.notify = handle_pointer_constraint_destroy;
	wl_signal_add(&constraint->events.destroy, &trk->destroy);
	constraint->data = trk;

	constraint_set_active(server, constraint);
}

/* Enforce the active constraint after the cursor moved. Runs before the
 * hover recompute so the client sees the clamped position. */
static void constraint_enforce(struct amber_server *server) {
	struct wlr_pointer_constraint_v1 *c = server->active_constraint;
	if (c == NULL || server->cursor_mode != AMBER_CURSOR_PASSTHROUGH) {
		return;
	}
	struct wlr_seat_pointer_state *ps = &server->seat->pointer_state;
	if (ps->focused_surface == NULL) {
		return;
	}

	if (c->type == WLR_POINTER_CONSTRAINT_V1_LOCKED) {
		/* Pin the cursor to the spot where the lock was taken; the
		 * deltas still flow to the client for camera control. */
		wlr_cursor_warp(server->cursor, NULL,
			server->constraint_anchor_x, server->constraint_anchor_y);
		return;
	}

	/* CONFINED: keep the pointer inside the client's surface-local
	 * region (its bounding box), feeding it only in-region motion. The
	 * surface coordinates are constants of the rendering surface, so
	 * they compare directly against the constraint region. */
	if (ps->focused_surface != c->surface) {
		return;
	}
	pixman_box32_t ext =
		*pixman_region32_extents(&c->current.region);
	double cx = ps->sx < ext.x1 ? ext.x1
		: (ps->sx > ext.x2 ? ext.x2 : ps->sx);
	double cy = ps->sy < ext.y1 ? ext.y1
		: (ps->sy > ext.y2 ? ext.y2 : ps->sy);
	if (cx == ps->sx && cy == ps->sy) {
		return;
	}
	struct wlr_output *out = wlr_output_layout_output_at(
		server->output_layout, server->cursor->x, server->cursor->y);
	double scale = out != NULL ? out->scale : 1.0;
	/* Move by the (scale-normalized) delta; this also notifies the
	 * client, which sees the corrected in-region position. */
	wlr_cursor_move(server->cursor, NULL,
		(cx - ps->sx) / scale, (cy - ps->sy) / scale);
}

static void seat_request_set_selection(struct wl_listener *listener, void *data) {
	/* This event is raised by the seat when a client wants to set the selection,
	 * usually when the user copies something. wlroots allows compositors to
	 * ignore such requests if they so choose, but in amber we always honor
	 */
	struct amber_server *server = wl_container_of(
			listener, server, request_set_selection);
	struct wlr_seat_request_set_selection_event *event = data;
	wlr_seat_set_selection(server->seat, event->source, event->serial);
}

static struct amber_toplevel *desktop_toplevel_at(
		struct amber_server *server, double lx, double ly,
		struct wlr_surface **surface, double *sx, double *sy) {
	/* This returns the topmost node in the scene at the given layout coords.
	 * We only care about surface nodes as we are specifically looking for a
	 * surface in the surface tree of a amber_toplevel. */
	struct wlr_scene_node *node = wlr_scene_node_at(
		&server->scene->tree.node, lx, ly, sx, sy);
	if (node == NULL || node->type != WLR_SCENE_NODE_BUFFER) {
		return NULL;
	}
	struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
	struct wlr_scene_surface *scene_surface =
		wlr_scene_surface_try_from_buffer(scene_buffer);
	if (!scene_surface) {
		return NULL;
	}

	*surface = scene_surface->surface;
	/* Find the node corresponding to the amber_toplevel at the root of this
	 * surface tree, it is the only one for which we set the data field.
	 * Layer surfaces have no amber_toplevel ancestor; stop at the tree
	 * root instead of walking past it (NULL). */
	struct wlr_scene_tree *tree = node->parent;
	while (tree != NULL && tree->node.data == NULL) {
		tree = tree->node.parent;
	}
	if (tree == NULL) {
		return NULL;
	}
	return tree->node.data;
}

static void reset_cursor_mode(struct amber_server *server) {
	/* Reset the cursor mode to passthrough. */
	if (server->cursor_mode == AMBER_CURSOR_MOVE &&
			server->grabbed_toplevel != NULL) {
		animation_wobble_release(server->grabbed_toplevel);
	}
	server->cursor_mode = AMBER_CURSOR_PASSTHROUGH;
	server->grabbed_toplevel = NULL;
}

static void process_cursor_move(struct amber_server *server) {
	struct amber_toplevel *toplevel = server->grabbed_toplevel;
	if (!toplevel->floating) {
		/* Tile-reorder drag: the window stays tiled; when the
		 * cursor crosses into another column's slot, swap strip
		 * positions and let the reflow glide animate the shift. */
		struct amber_workspace *ws = toplevel_workspace(toplevel);
		if (ws == NULL || toplevel->output != ws->output) {
			return;
		}
		int px = (int)(server->cursor->x -
			ws->output->layout_box.x + ws->view_offset);
		struct amber_toplevel *it, *target = NULL;
		wl_list_for_each(it, &ws->toplevels, link) {
			if (it->floating || it == toplevel) {
				continue;
			}
			if (px >= it->tile_x &&
					px < it->tile_x + it->tile_w) {
				target = it;
				break;
			}
		}
		if (target == NULL) {
			return;
		}
		bool target_first = false, seen_self = false;
		wl_list_for_each(it, &ws->toplevels, link) {
			if (it == target && !seen_self) {
				target_first = true;
				break;
			}
			if (it == toplevel) {
				seen_self = true;
			}
		}
		/* Anchor computed before removal so adjacency survives. */
		struct wl_list *anchor = target_first
			? target->link.prev
			: &target->link;
		wl_list_remove(&toplevel->link);
		wl_list_insert(anchor, &toplevel->link);
		focus_toplevel(toplevel);
		workspace_arrange(ws);
		return;
	}
	/* Move the grabbed toplevel to the new position. Scene node
	 * positions are local to the workspace tree, which sits at the
	 * output's origin, so convert cursor layout coords -> local. */
	int old_x = toplevel->scene_tree->node.x;
	int old_y = toplevel->scene_tree->node.y;
	wlr_scene_node_set_position(&toplevel->scene_tree->node,
		server->cursor->x - toplevel->output->layout_box.x - server->grab_x,
		server->cursor->y - toplevel->output->layout_box.y - server->grab_y);
	animation_wobble_nudge(toplevel,
		toplevel->scene_tree->node.x - old_x,
		toplevel->scene_tree->node.y - old_y);
}

static void process_cursor_resize(struct amber_server *server) {
	/*
	 * Resizing the grabbed toplevel can be a little bit complicated, because we
	 * could be resizing from any corner or edge. This not only resizes the
	 * toplevel on one or two axes, but can also move the toplevel if you resize
	 * from the top or left edges (or top-left corner).
	 *
	 * Note that some shortcuts are taken here. In a more fleshed-out
	 * compositor, you'd wait for the client to prepare a buffer at the new
	 * size, then commit any movement that was prepared.
	 */
	struct amber_toplevel *toplevel = server->grabbed_toplevel;
	double border_x = server->cursor->x - toplevel->output->layout_box.x - server->grab_x;
	double border_y = server->cursor->y - toplevel->output->layout_box.y - server->grab_y;
	int new_left = server->grab_geobox.x;
	int new_right = server->grab_geobox.x + server->grab_geobox.width;
	int new_top = server->grab_geobox.y;
	int new_bottom = server->grab_geobox.y + server->grab_geobox.height;

	if (server->resize_edges & WLR_EDGE_TOP) {
		new_top = border_y;
		if (new_top >= new_bottom) {
			new_top = new_bottom - 1;
		}
	} else if (server->resize_edges & WLR_EDGE_BOTTOM) {
		new_bottom = border_y;
		if (new_bottom <= new_top) {
			new_bottom = new_top + 1;
		}
	}
	if (server->resize_edges & WLR_EDGE_LEFT) {
		new_left = border_x;
		if (new_left >= new_right) {
			new_left = new_right - 1;
		}
	} else if (server->resize_edges & WLR_EDGE_RIGHT) {
		new_right = border_x;
		if (new_right <= new_left) {
			new_right = new_left + 1;
		}
	}

	struct wlr_box *geo_box = &toplevel->xdg_toplevel->base->geometry;
	wlr_scene_node_set_position(&toplevel->scene_tree->node,
		new_left - geo_box->x, new_top - geo_box->y);

	int new_width = new_right - new_left;
	int new_height = new_bottom - new_top;
	if (toplevel_alive(toplevel)) {
		wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel,
			new_width, new_height);
	}
}

/* Begin a mouse-driven resize of the focused window's column. The column
 * width is adjusted by how far the pointer travels horizontally from the
 * grab point (a live drag on the column's right edge). */
static void begin_column_resize(struct amber_server *server,
		struct amber_toplevel *toplevel) {
	if (toplevel == NULL || toplevel->floating) {
		return;
	}
	server->grabbed_toplevel = toplevel;
	server->cursor_mode = AMBER_CURSOR_COLUMN_RESIZE;
	/* Remember pointer position in layout space and the starting width. */
	double lx = server->cursor->x -
		(toplevel->output ? toplevel->output->layout_box.x : 0);
	server->grab_x = lx;
	server->grab_geobox.width = toplevel->col_width;
	wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr,
		"col-resize");
}

static void process_cursor_column_resize(struct amber_server *server) {
	struct amber_toplevel *toplevel = server->grabbed_toplevel;
	if (toplevel == NULL || !toplevel_alive(toplevel) || toplevel->floating) {
		return;
	}
	double lx = server->cursor->x -
		(toplevel->output ? toplevel->output->layout_box.x : 0);
	int cur_width = (int)server->grab_geobox.width + (int)(lx - server->grab_x);
	if (cur_width < 80) {
		cur_width = 80; // enforce a sane minimum column width
	}
	toplevel->col_width = cur_width;
	struct amber_workspace *ws = toplevel_workspace(toplevel);
	if (ws != NULL) {
		workspace_arrange(ws);
	}
}

static void process_cursor_motion(struct amber_server *server, uint32_t time) {
	/* Track which output the cursor is on; new windows, workspace
	 * switches and cycling all target it. Cheap: one layout walk per
	 * motion event, only when the pointer actually moves. */
	struct wlr_output *wlr_output = wlr_output_layout_output_at(
		server->output_layout, server->cursor->x, server->cursor->y);
	if (wlr_output != NULL) {
		struct amber_output *output;
		wl_list_for_each(output, &server->outputs, link) {
			if (output->wlr_output == wlr_output) {
				server->active_output = output;
				break;
			}
		}
	}

	/* If the mode is non-passthrough, delegate to those functions. */
	if (server->cursor_mode == AMBER_CURSOR_MOVE) {
		process_cursor_move(server);
		return;
	} else if (server->cursor_mode == AMBER_CURSOR_RESIZE) {
		process_cursor_resize(server);
		return;
	} else if (server->cursor_mode == AMBER_CURSOR_COLUMN_RESIZE) {
		process_cursor_column_resize(server);
		return;
	}

	/* Otherwise, find the toplevel under the pointer and send the event along. */
	double sx, sy;
	struct wlr_seat *seat = server->seat;
	struct wlr_surface *surface = NULL;
	struct amber_toplevel *toplevel = desktop_toplevel_at(server,
			server->cursor->x, server->cursor->y, &surface, &sx, &sy);
	if (!toplevel) {
		/* If there's no toplevel under the cursor, set the cursor image to a
		 * default. This is what makes the cursor image appear when you move it
		 * around the screen, not over any toplevels. */
		wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
	}
	if (surface) {
		/*
		 * Send pointer enter and motion events.
		 *
		 * The enter event gives the surface "pointer focus", which is distinct
		 * from keyboard focus. You get pointer focus by moving the pointer over
		 * a window.
		 *
		 * Note that wlroots will avoid sending duplicate enter/motion events if
		 * the surface has already has pointer focus or if the client is already
		 * aware of the coordinates passed.
		 */
		wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
		wlr_seat_pointer_notify_motion(seat, time, sx, sy);
	} else {
		/* Clear pointer focus so future button events and such are not sent to
		 * the last client to have the cursor over it. */
		wlr_seat_pointer_clear_focus(seat);
	}
}

static void server_cursor_motion(struct wl_listener *listener, void *data) {
	/* This event is forwarded by the cursor when a pointer emits a _relative_
	 * pointer motion event (i.e. a delta) */
	struct amber_server *server =
		wl_container_of(listener, server, cursor_motion);
	struct wlr_pointer_motion_event *event = data;
	idle_mark_activity(server);
	/* The cursor doesn't move unless we tell it to. The cursor automatically
	 * handles constraining the motion to the output layout, as well as any
	 * special configuration applied for the specific input device which
	 * generated the event. You can pass NULL for the device if you want to move
	 * the cursor around without any input. */
	wlr_cursor_move(server->cursor, &event->pointer->base,
			event->delta_x, event->delta_y);
	if (server->active_constraint != NULL) {
		constraint_enforce(server);
	}
	process_cursor_motion(server, event->time_msec);

	/* Forward raw deltas to relative-pointer clients (games and 3D
	 * viewports use the unaccelerated deltas for camera control). */
	if (server->relative_pointer_mgr != NULL) {
		wlr_relative_pointer_manager_v1_send_relative_motion(
			server->relative_pointer_mgr, server->seat,
			(uint64_t)event->time_msec * 1000,
			event->delta_x, event->delta_y,
			event->unaccel_dx, event->unaccel_dy);
	}
}

static void server_cursor_motion_absolute(
		struct wl_listener *listener, void *data) {
	/* This event is forwarded by the cursor when a pointer emits an _absolute_
	 * motion event, from 0..1 on each axis. This happens, for example, when
	 * wlroots is running under a Wayland window rather than KMS+DRM, and you
	 * move the mouse over the window. You could enter the window from any edge,
	 * so we have to warp the mouse there. There is also some hardware which
	 * emits these events. */
	struct amber_server *server =
		wl_container_of(listener, server, cursor_motion_absolute);
	struct wlr_pointer_motion_absolute_event *event = data;
	idle_mark_activity(server);
	wlr_cursor_warp_absolute(server->cursor, &event->pointer->base, event->x,
		event->y);
	if (server->active_constraint != NULL) {
		constraint_enforce(server);
	}
	process_cursor_motion(server, event->time_msec);
}

static void begin_interactive(struct amber_toplevel *toplevel,
		enum amber_cursor_mode mode, uint32_t edges);
static void begin_column_resize(struct amber_server *server,
		struct amber_toplevel *toplevel);
static void process_cursor_column_resize(struct amber_server *server);

static void server_cursor_button(struct wl_listener *listener, void *data) {
	/* This event is forwarded by the cursor when a pointer emits a button
	 * event. */
	struct amber_server *server =
		wl_container_of(listener, server, cursor_button);
	struct wlr_pointer_button_event *event = data;
	idle_mark_activity(server);
	if (overview_output(server) != NULL) {
		overview_button(server, event);
		return;
	}
	if (server->sw_active) {
		switcher_button(server, event);
		return;
	}

	if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
		if (server->cursor_mode != AMBER_CURSOR_PASSTHROUGH) {
			/* The matching press was swallowed by us; swallow the
			 * release too so clients never see a half-click. */
			reset_cursor_mode(server);
			return;
		}
		wlr_seat_pointer_notify_button(server->seat,
				event->time_msec, event->button, event->state);
		return;
	}

	double sx, sy;
	struct wlr_surface *surface = NULL;
	struct amber_toplevel *toplevel = desktop_toplevel_at(server,
			server->cursor->x, server->cursor->y, &surface, &sx, &sy);

	bool logo = modifier_held(server, WLR_MODIFIER_LOGO);
	if (logo && toplevel != NULL && (event->button == BTN_LEFT ||
			event->button == BTN_RIGHT)) {
		/* Compositor drag: focus, then move/resize ourselves without
		 * the client ever seeing this press. */
		focus_toplevel(toplevel);

		if (!toplevel->floating && event->button == BTN_RIGHT) {
			/* Right-drag resize floats first (Hyprland-style);
			 * left-drag keeps tiles in place and reorders them
			 * (see process_cursor_move). */
			toplevel_to_floating(toplevel);
			struct wlr_box *geo =
				&toplevel->xdg_toplevel->base->geometry;
			wlr_scene_node_set_position(&toplevel->scene_tree->node,
				(int)(server->cursor->x -
					toplevel->output->layout_box.x
					- geo->width / 2.0),
				(int)(server->cursor->y -
					toplevel->output->layout_box.y
					- geo->height / 2.0));
		}

		if (event->button == BTN_LEFT) {
			/* SUPER+SHIFT+LEFT-drag on a TILED window resizes its
			 * column (mirrors the SUPER+SHIFT+arrow binding);
			 * plain SUPER+LEFT stays the move/reorder drag. */
			bool shift =
				modifier_held(server, WLR_MODIFIER_SHIFT);
			if (shift && !toplevel->floating) {
				begin_column_resize(server, toplevel);
				return;
			}
			begin_interactive(toplevel, AMBER_CURSOR_MOVE, 0);
		} else {
			/* Pick resize edges from which quadrant of the window
			 * the drag started in (dwl trick). */
			struct wlr_box *geo =
				&toplevel->xdg_toplevel->base->geometry;
			double lx = server->cursor->x -
				toplevel->output->layout_box.x -
				toplevel->scene_tree->node.x;
			double ly = server->cursor->y -
				toplevel->output->layout_box.y -
				toplevel->scene_tree->node.y;
			uint32_t edges = 0;
			if (lx < geo->width / 2.0) {
				edges |= WLR_EDGE_LEFT;
			} else {
				edges |= WLR_EDGE_RIGHT;
			}
			if (ly < geo->height / 2.0) {
				edges |= WLR_EDGE_TOP;
			} else {
				edges |= WLR_EDGE_BOTTOM;
			}
			begin_interactive(toplevel, AMBER_CURSOR_RESIZE, edges);
		}
		return;
	}

	/* Notify the client with pointer focus that a button press has
	 * occurred, then focus it. */
	struct wlr_surface *prev_focus =
		server->seat->keyboard_state.focused_surface;
	wlr_seat_pointer_notify_button(server->seat,
			event->time_msec, event->button, event->state);
	focus_toplevel(toplevel);
	if (event->state == WL_POINTER_BUTTON_STATE_PRESSED &&
			toplevel != NULL && !toplevel->floating &&
			server->seat->keyboard_state.focused_surface !=
				prev_focus) {
		/* Camera follows focus (niri): re-arranging scrolls the
		 * newly focused column into view / centers it. */
		workspace_arrange(toplevel_workspace(toplevel));
	}
}

static void server_cursor_axis(struct wl_listener *listener, void *data) {
	/* This event is forwarded by the cursor when a pointer emits an axis event,
	 * for example when you move the scroll wheel. */
	struct amber_server *server =
		wl_container_of(listener, server, cursor_axis);
	struct wlr_pointer_axis_event *event = data;
	idle_mark_activity(server);
	/* Switcher/overview: scroll wheel cycles the selection instead of
	 * scrolling a client underneath. */
	if (server->sw_active) {
		if (event->orientation == WL_POINTER_AXIS_VERTICAL_SCROLL &&
				event->source == WL_POINTER_AXIS_SOURCE_WHEEL &&
				event->delta_discrete != 0) {
			int dir = event->delta_discrete > 0 ? 1 : -1;
			int step = (int)server->sw_cols > 0
				? (int)server->sw_cols : 1;
			switcher_select(server,
				server->sw_selected + (size_t)(dir * step));
			return;
		}
		return; // swallow the axis while the switcher is open
	}
	/* Notify the client with pointer focus of the axis event. */
	wlr_seat_pointer_notify_axis(server->seat,
			event->time_msec, event->orientation, event->delta,
			event->delta_discrete, event->source, event->relative_direction);
}

static void server_cursor_frame(struct wl_listener *listener, void *data) {
	/* This event is forwarded by the cursor when a pointer emits an frame
	 * event. Frame events are sent after regular pointer events to group
	 * multiple events together. For instance, two axis events may happen at the
	 * same time, in which case a frame event won't be sent in between. */
	struct amber_server *server =
		wl_container_of(listener, server, cursor_frame);
	/* Notify the client with pointer focus of the frame event. */
	wlr_seat_pointer_notify_frame(server->seat);
}

static void arrange_layers(struct amber_output *output);
static void output_update_geometry(struct amber_output *output);
static void config_reload(struct amber_server *server);

/* ==================== IPC (mango-compatible JSON lines) ===================
 *
 * Line protocol over a Unix socket, wire-compatible with Mango's IPC so
 * noctalia's workspace widget works natively:
 *   - discovery: $AMBERWM_IPC_SOCKET holds the socket path (we
 *     setenv it before spawning anything; ambermsg also globs
 *     amberwm-*.sock)
 *   - "watch all-monitors\n"  → persistent conn; we push one JSON line
 *     per state change: {"monitors":[...]}
 *   - "get all-clients\n"     → {"clients":[...]} (one-shot)
 *   - "dispatch view,N\n" /
 *     "dispatch viewcrossmon,N,<out>\n" /
 *     "dispatch focusid client,<id>\n" → {"success":bool}
 * Plus amber-native extras: reload, workspace/focus/close/quit/status. */

#include <sys/socket.h>
#include <sys/un.h>

static size_t json_escape(char *dst, size_t dstlen, const char *src) {
	size_t o = 0;
	for (const unsigned char *p = (const unsigned char *)src;
			p != NULL && *p != '\0' && o + 8 < dstlen; p++) {
		unsigned char c = *p;
		if (c == '"' || c == '\\') {
			dst[o++] = '\\';
			dst[o++] = c;
		} else if (c == '\n') {
			o += (size_t)snprintf(dst + o, dstlen - o, "\\n");
		} else if (c == '\t') {
			o += (size_t)snprintf(dst + o, dstlen - o, "\\t");
		} else if (c < 0x20) {
			o += (size_t)snprintf(dst + o, dstlen - o,
				"\\u%04x", c);
		} else {
			dst[o++] = c;
		}
	}
	dst[o] = '\0';
	return o;
}

static int ipc_count_windows(struct amber_workspace *ws) {
	int n = 0;
	struct amber_toplevel *t;
	wl_list_for_each(t, &ws->toplevels, link) {
		n++;
	}
	return n;
}

/* {"name":..,"active":bool,"x":..,"y":..,"width":..,"height":..,
 *  "active_client":{...}|absent,"tags":[...],"active_tags":[n]} */
static size_t ipc_monitor_json(struct amber_server *server,
		struct amber_output *output, char *buf, size_t len) {
	struct amber_workspace *active_ws =
		&output->workspaces[output->active_workspace];
	struct amber_toplevel *focused = server->focused_toplevel;

	char title[256], appid[128];
	json_escape(title, sizeof(title),
		focused != NULL &&
		focused->output == output &&
		focused->xdg_toplevel->title != NULL
			? focused->xdg_toplevel->title : "");
	json_escape(appid, sizeof(appid),
		focused != NULL && focused->output == output &&
		focused->xdg_toplevel->app_id != NULL
			? focused->xdg_toplevel->app_id : "");

	size_t o = (size_t)snprintf(buf, len,
		"{\"name\":\"%s\",\"active\":%s,"
		"\"x\":%d,\"y\":%d,\"width\":%d,\"height\":%d,",
		output->wlr_output->name,
		server->active_output == output ? "true" : "false",
		output->layout_box.x, output->layout_box.y,
		output->layout_box.width, output->layout_box.height);
	if (title[0] != '\0' || appid[0] != '\0') {
		o += (size_t)snprintf(buf + o, len - o,
			"\"active_client\":{\"id\":\"%ld\",\"title\":\"%s\","
			"\"appid\":\"%s\"},",
			focused != NULL ? focused->ipc_id : 0,
			title, appid);
	}
	o += (size_t)snprintf(buf + o, len - o, "\"tags\":[");
	for (int i = 0; i < AMBER_WORKSPACE_COUNT; i++) {
		int count = ipc_count_windows(&output->workspaces[i]);
		o += (size_t)snprintf(buf + o, len - o,
			"%s{\"index\":%d,\"is_active\":%s,"
			"\"is_urgent\":false,\"client_count\":%d}",
			i > 0 ? "," : "", i + 1,
			i == output->active_workspace ? "true" : "false",
			count);
	}
	o += (size_t)snprintf(buf + o, len - o,
		"],\"active_tags\":[%d],\"focused_workspace\":%d}",
		output->active_workspace + 1, output->active_workspace + 1);
	return o;
}

/* One JSON line describing every monitor; pushed on state changes. */
static void ipc_broadcast(struct amber_server *server) {
	struct amber_output *dyn_out;
	wl_list_for_each(dyn_out, &server->outputs, link) {
		ext_ws_dyn_sync(dyn_out);
	}
	if (wl_list_empty(&server->ipc_watchers)) {
		return;
	}
	char buf[16384];
	size_t o = (size_t)snprintf(buf, sizeof(buf), "{\"monitors\":[");
	struct amber_output *output;
	bool first = true;
	wl_list_for_each(output, &server->outputs, link) {
		if (!first && o < sizeof(buf)) {
			buf[o++] = ',';
		}
		first = false;
		o += ipc_monitor_json(server, output, buf + o,
			sizeof(buf) - o);
	}
	snprintf(buf + o, sizeof(buf) - o, "]}\n");
	size_t line_len = strlen(buf);

	struct amber_ipc_watcher *watcher, *tmp;
	wl_list_for_each_safe(watcher, tmp, &server->ipc_watchers, link) {
		ssize_t rc = send(watcher->fd, buf, line_len, MSG_NOSIGNAL);
		if (rc < 0 && errno != EAGAIN && errno != EINTR) {
			wl_list_remove(&watcher->link);
			wl_event_source_remove(watcher->source);
			close(watcher->fd);
			free(watcher);
		}
	}
}

static int ipc_watcher_readable(int fd, uint32_t mask, void *data) {
	struct amber_server *server = data;
	(void)fd;
	if ((mask & (WL_EVENT_ERROR | WL_EVENT_HANGUP)) != 0 ||
			(mask & WL_EVENT_READABLE) != 0) {
		/* Clients never say anything after the first watch line;
		 * any read event here means they went away. */
		struct amber_ipc_watcher *watcher, *tmp;
		wl_list_for_each_safe(watcher, tmp, &server->ipc_watchers,
				link) {
			if (watcher->fd == fd) {
				wl_list_remove(&watcher->link);
				wl_event_source_remove(watcher->source);
				close(watcher->fd);
				free(watcher);
				break;
			}
		}
		return -1;
	}
	return 0;
}

static struct amber_output *ipc_output_by_name(
		struct amber_server *server, const char *name) {
	struct amber_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		if (strcmp(output->wlr_output->name, name) == 0) {
			return output;
		}
	}
	return NULL;
}

static bool ipc_parse_long(const char *s, long *out) {
	char *end;
	long v = strtol(s, &end, 10);
	if (end == s || (*end != '\0' && *end != ',')) {
		return false;
	}
	*out = v;
	return true;
}

/* Handle a dispatch sub-command (after "dispatch "). Returns JSON reply
 * written into reply buffer. */
static void ipc_dispatch(struct amber_server *server, const char *args,
		char *reply, size_t reply_len) {
	bool ok = false;
	if (strncmp(args, "view,", 5) == 0) {
		long n;
		if (ipc_parse_long(args + 5, &n) && n >= 1 &&
				n <= AMBER_WORKSPACE_COUNT) {
			struct amber_output *output = active_output(server);
			if (output != NULL) {
				workspace_switch(output, (int)n - 1);
				ok = true;
			}
		}
	} else if (strncmp(args, "viewcrossmon,", 13) == 0) {
		long n;
		const char *comma = strchr(args + 13, ',');
		if (comma != NULL && ipc_parse_long(args + 13, &n) &&
				n >= 1 && n <= AMBER_WORKSPACE_COUNT) {
			struct amber_output *output =
				ipc_output_by_name(server, comma + 1);
			if (output == NULL) {
				output = active_output(server);
			}
			if (output != NULL) {
				workspace_switch(output, (int)n - 1);
				ok = true;
			}
		}
	} else if (strncmp(args, "focusid client,", 15) == 0) {
		long id;
		if (ipc_parse_long(args + 15, &id)) {
			struct amber_output *output;
			wl_list_for_each(output, &server->outputs, link) {
				for (int i = 0; i < AMBER_WORKSPACE_COUNT &&
						!ok; i++) {
					struct amber_toplevel *t;
					wl_list_for_each(t,
							&output->workspaces[i]
								.toplevels,
							link) {
						if (t->ipc_id == id) {
							/* Switching to the
							 * window's
							 * workspace makes
							 * it visible. */
							workspace_switch(
								output, i);
							focus_toplevel(t);
							ok = true;
							break;
						}
					}
				}
			}
		}
	}
	snprintf(reply, reply_len, "{\"success\":%s}",
		ok ? "true" : "false");
}

/* Respond to one-shot requests; `req` has no trailing newline. When the
 * request is a watch, registers the connection and returns true meaning
 * "keep the connection open". */
static bool ipc_handle_request(struct amber_server *server, int fd,
		char *req) {
	char reply[4096];

	if (strncmp(req, "watch ", 6) == 0) {
		struct amber_ipc_watcher *watcher = calloc(1,
			sizeof(*watcher));
		if (watcher == NULL) {
			return false;
		}
		watcher->fd = fd;
		struct wl_event_loop *loop =
			wl_display_get_event_loop(server->wl_display);
		watcher->source = wl_event_loop_add_fd(loop, fd,
			WL_EVENT_READABLE, ipc_watcher_readable, server);
		wl_list_insert(server->ipc_watchers.prev, &watcher->link);
		/* Immediate snapshot so the client starts with state. */
		ipc_broadcast(server);
		return true;
	}

	if (strcmp(req, "get all-clients") == 0) {
		size_t o = (size_t)snprintf(reply, sizeof(reply),
			"{\"clients\":[");
		bool first = true;
		struct amber_output *output;
		wl_list_for_each(output, &server->outputs, link) {
			for (int i = 0; i < AMBER_WORKSPACE_COUNT; i++) {
				struct amber_toplevel *t;
				wl_list_for_each(t,
						&output->workspaces[i]
							.toplevels, link) {
					char title[256], appid[128];
					json_escape(title, sizeof(title),
						t->xdg_toplevel->title ?
						t->xdg_toplevel->title : "");
					json_escape(appid, sizeof(appid),
						t->xdg_toplevel->app_id ?
						t->xdg_toplevel->app_id : "");
					if (!first) {
						o += (size_t)snprintf(reply + o,
							sizeof(reply) - o, ",");
					}
					first = false;
					o += (size_t)snprintf(reply + o,
						sizeof(reply) - o,
						"{\"id\":\"%ld\","
						"\"title\":\"%s\","
						"\"appid\":\"%s\","
						"\"monitor\":\"%s\","
						"\"tags\":[%d],"
						"\"is_focused\":%s,"
						"\"x\":%d,\"y\":%d}",
						t->ipc_id, title, appid,
						output->wlr_output->name,
						i + 1,
						server->focused_toplevel == t
							? "true" : "false",
						(int)t->scene_tree->node.x,
						(int)t->scene_tree->node.y);
				}
			}
		}
		snprintf(reply + o, sizeof(reply) - o, "]}\n");
		send(fd, reply, strlen(reply), MSG_NOSIGNAL);
		return false;
	}

	if (strcmp(req, "get version") == 0) {
		snprintf(reply, sizeof(reply),
			"{\"version\":\"amberwm-0.1\"}\n");
		send(fd, reply, strlen(reply), MSG_NOSIGNAL);
		return false;
	}

	if (strncmp(req, "dispatch ", 9) == 0) {
		ipc_dispatch(server, req + 9, reply, sizeof(reply));
		size_t len = strlen(reply);
		reply[len] = '\n';
		send(fd, reply, len + 1, MSG_NOSIGNAL);
		return false;
	}

	/* ---- amber-native commands ---- */
	if (strcmp(req, "reload") == 0) {
		config_reload(server);
		send(fd, "{\"success\":true}\n", 18, MSG_NOSIGNAL);
		return false;
	}
	if (strncmp(req, "workspace ", 10) == 0) {
		long n;
		struct amber_output *output = active_output(server);
		if (output != NULL && ipc_parse_long(req + 10, &n) &&
				n >= 1 && n <= AMBER_WORKSPACE_COUNT) {
			workspace_switch(output, (int)n - 1);
			send(fd, "{\"success\":true}\n", 18, MSG_NOSIGNAL);
		} else {
			send(fd, "{\"success\":false}\n", 19, MSG_NOSIGNAL);
		}
		return false;
	}
	if (strcmp(req, "focus next") == 0 || strcmp(req, "focus prev") == 0) {
		cycle_focus(server, req[6] == 'n' ? 1 : -1);
		send(fd, "{\"success\":true}\n", 18, MSG_NOSIGNAL);
		return false;
	}
	if (strncmp(req, "enable ", 7) == 0 ||
			strncmp(req, "disable ", 8) == 0) {
		bool enable = req[0] == 'e';
		const char *key = req + (enable ? 7 : 8);
		bool *target = NULL;
		bool needs_fx = false, needs_arrange = false;
		if (strcmp(key, "animations") == 0) {
			target = &server->animations_enabled;
		} else if (strcmp(key, "blur") == 0) {
			target = &server->blur_enabled;
			needs_fx = true;
		} else if (strcmp(key, "shadows") == 0) {
			target = &server->shadows_enabled;
			needs_fx = true;
		} else if (strcmp(key, "center-focused-column") == 0) {
			target = &server->center_focused_column;
			needs_arrange = true;
		} else if (strcmp(key, "ws-slide") == 0) {
			target = &server->ws_slide_enabled;
		}
		if (target == NULL) {
			const char *bad =
				"{\"success\":false,\"error\":\"unknown key\"}\n";
			send(fd, bad, strlen(bad), MSG_NOSIGNAL);
			return false;
		}
		bool changed = *target != enable;
		*target = enable;
		if (needs_fx) {
			struct amber_output *output;
			wl_list_for_each(output, &server->outputs, link) {
				for (int i = 0; i < AMBER_WORKSPACE_COUNT;
						i++) {
					struct amber_toplevel *t;
					wl_list_for_each(t,
							&output->workspaces[i]
								.toplevels,
							link) {
						toplevel_apply_fx(t);
					}
				}
			}
		}
		if (needs_arrange && changed) {
			struct amber_output *output;
			wl_list_for_each(output, &server->outputs, link) {
				workspace_arrange(&output->workspaces
						[output->active_workspace]);
			}
		}
		snprintf(reply, sizeof(reply), "{\"success\":true,\"%s\":%s}\n",
			key, enable ? "true" : "false");
		send(fd, reply, strlen(reply), MSG_NOSIGNAL);
		return false;
	}

	if (strncmp(req, "close ", 6) == 0) {
		long id;
		bool ok = ipc_parse_long(req + 6, &id);
		if (ok) {
			ok = false;
			struct amber_output *output;
			wl_list_for_each(output, &server->outputs, link) {
				for (int i = 0;
						!ok && i < AMBER_WORKSPACE_COUNT;
						i++) {
					struct amber_toplevel *t;
					wl_list_for_each(t,
							&output->workspaces[i]
								.toplevels,
							link) {
						if (t->ipc_id == id &&
								toplevel_alive(t)) {
							wlr_xdg_toplevel_send_close(
								t->xdg_toplevel);
							ok = true;
							break;
						}
					}
				}
			}
		}
		send(fd, ok ? "{\"success\":true}\n" : "{\"success\":false}\n",
			ok ? 18 : 19, MSG_NOSIGNAL);
		return false;
	}

	if (strcmp(req, "close") == 0) {
		if (server->focused_toplevel != NULL) {
			wlr_xdg_toplevel_send_close(
				server->focused_toplevel->xdg_toplevel);
		}
		send(fd, "{\"success\":true}\n", 18, MSG_NOSIGNAL);
		return false;
	}
	if (strcmp(req, "quit") == 0) {
		server->shutting_down = true;
		wl_display_terminate(server->wl_display);
		send(fd, "{\"success\":true}\n", 18, MSG_NOSIGNAL);
		return false;
	}
	if (strcmp(req, "status") == 0) {
		struct amber_output *output = active_output(server);
		snprintf(reply, sizeof(reply),
			"active_workspace=%d outputs=%zu\n",
			output != NULL ? output->active_workspace + 1 : 0,
			wl_list_length(&server->outputs));
		send(fd, reply, strlen(reply), MSG_NOSIGNAL);
		return false;
	}

	send(fd, "{\"error\":\"unknown command\"}\n", 29, MSG_NOSIGNAL);
	return false;
}

static int ipc_connection_ready(int listen_fd, uint32_t mask, void *data) {
	struct amber_server *server = data;
	if ((mask & WL_EVENT_READABLE) == 0) {
		return 0;
	}
	int fd = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC);
	if (fd < 0) {
		return 0;
	}

	/* Read one request line (clients write immediately). */
	char req[1024];
	size_t len = 0;
	while (len < sizeof(req) - 1) {
		char c;
		ssize_t rc = recv(fd, &c, 1, 0);
		if (rc <= 0) {
			break;
		}
		if (c == '\n') {
			break;
		}
		req[len++] = c;
	}
	req[len] = '\0';

	if (len > 0 && !ipc_handle_request(server, fd, req)) {
		shutdown(fd, SHUT_WR);
	}
	/* Watchers keep fd open; ownership moved to the watcher list. */
	return 0;
}

void ipc_init(struct amber_server *server) {
	wl_list_init(&server->ipc_watchers);

	const char *runtime = getenv("XDG_RUNTIME_DIR");
	if (runtime == NULL) {
		runtime = "/tmp";
	}
	const char *display = getenv("WAYLAND_DISPLAY");
	if (display == NULL) {
		display = "wayland-0";
	}
	server->ipc_path = malloc(512);
	snprintf(server->ipc_path, 512, "%s/amberwm-%s.sock",
		runtime, display);
	unlink(server->ipc_path);

	int listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK |
		SOCK_CLOEXEC, 0);
	if (listen_fd < 0) {
		return;
	}
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	strncpy(addr.sun_path, server->ipc_path,
		sizeof(addr.sun_path) - 1);
	if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
			listen(listen_fd, 16) < 0) {
		close(listen_fd);
		return;
	}

	struct wl_event_loop *loop =
		wl_display_get_event_loop(server->wl_display);
	server->ipc_source = wl_event_loop_add_fd(loop, listen_fd,
		WL_EVENT_READABLE, ipc_connection_ready, server);

	/* IPC discovery for our own tools (ambermsg globs amberwm-*.sock
	 * first; this is the explicit fallback). Deliberately NOT exported
	 * as MANGO_INSTANCE_SIGNATURE any more: noctalia keys compositor
	 * detection on that variable, treated amber as mango and queried a
	 * client list we never implemented ("no windows" in the switcher).
	 * Unmatched, it falls back to ext-workspace + foreign-toplevel. */
	setenv("AMBERWM_IPC_SOCKET", server->ipc_path, true);
	wlr_log(WLR_INFO, "IPC listening on %s", server->ipc_path);
}

/* ===================== end IPC =========================================== */

static int ipc_hup_reload(int signal, void *data) {
	(void)signal;
	config_reload(data);
	return 0;
}

/* SIGTERM/SIGINT: run the full teardown path (clients, animations,
 * scene) instead of dying mid-frame — logind sends TERM at session
 * end, and an abrupt death there skips buffer/frame cleanup. */
static int ipc_term_quit(int signal, void *data) {
	struct amber_server *server = data;
	wlr_log(WLR_INFO, "signal %d: shutting down", signal);
	server->shutting_down = true;
	wl_display_terminate(server->wl_display);
	return 0;
}

/* ========================= Open animations =============================== */

static int64_t anim_now_usec(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

struct fx_opacity_ctx {
	float opacity;
};

static void fx_opacity_cb(struct wlr_scene_buffer *buffer,
		int sx, int sy, void *data) {
	struct fx_opacity_ctx *ctx = data;
	wlr_scene_buffer_set_opacity(buffer, ctx->opacity);
}

static void animation_tree_set_opacity(struct wlr_scene_tree *tree,
		float opacity) {
	struct fx_opacity_ctx ctx = { .opacity = opacity };
	wlr_scene_node_for_each_buffer(&tree->node, fx_opacity_cb, &ctx);
}

static void animation_lamp_cleanup(struct amber_animation *anim) {
	if (anim->strips != NULL) {
		for (int i = 0; i < anim->strip_count; i++) {
			if (anim->strips[i] != NULL) {
				wlr_scene_node_destroy(&anim->strips[i]->node);
				anim->strips[i] = NULL;
			}
		}
		free(anim->strips);
		anim->strips = NULL;
		anim->strip_count = 0;
	}
	if (anim->snapshot != NULL) {
		wlr_buffer_unlock(anim->snapshot);
		anim->snapshot = NULL;
	}
	free(anim->strip_rect);
	anim->strip_rect = NULL;
}

static void animation_destroy(struct amber_server *server,
		struct amber_animation *anim, bool restore) {
	switch (anim->kind) {
	case ANIM_OPEN:
		if (restore && anim->tree != NULL) {
			wlr_scene_node_set_position(&anim->tree->node,
				anim->base_x, anim->base_y);
			animation_tree_set_opacity(anim->tree, 1.0f);
		}
		break;
	case ANIM_LAMP_CLOSE:
		animation_lamp_cleanup(anim);
		break;
	case ANIM_WOBBLE:
		/* Visibility is restored regardless of `restore`: the real
		 * node was hidden for the grid, and both teardown paths are
		 * safe while the tree still exists. */
		if (anim->tree != NULL) {
			wlr_scene_node_set_enabled(&anim->tree->node, true);
		}
		animation_lamp_cleanup(anim);
		free(anim->px);
		free(anim->py);
		free(anim->vx);
		free(anim->vy);
		break;
	case ANIM_WS_SLIDE:
		if (anim->ws_from != NULL && anim->ws_from->tree != NULL) {
			wlr_scene_node_set_position(&anim->ws_from->tree->node,
				anim->origin_x, anim->origin_y);
			wlr_scene_node_set_enabled(&anim->ws_from->tree->node,
				false); // end state: old workspace hidden
		}
		if (anim->ws_to != NULL && anim->ws_to->tree != NULL) {
			wlr_scene_node_set_position(&anim->ws_to->tree->node,
				anim->origin_x, anim->origin_y);
			wlr_scene_node_set_enabled(&anim->ws_to->tree->node,
				true);
		}
		break;
	case ANIM_FLOAT_TWEEN:
		/* Landing spot is wherever arrange put the node; snapping
		 * there keeps cancel paths (unmap mid-glide) consistent. */
		if (restore && anim->tree != NULL) {
			wlr_scene_node_set_position(&anim->tree->node,
				anim->base_x, anim->base_y);
		}
		break;
	}
	wl_list_remove(&anim->link);
	free(anim);
}

/* Cancel any animation tracking a window whose node is about to be
 * moved or destroyed (unmap, reparent, fullscreen toggles...). */
void animation_cancel_for(struct amber_toplevel *toplevel) {
	struct amber_server *server = toplevel->server;
	struct amber_animation *anim, *tmp;
	wl_list_for_each_safe(anim, tmp, &server->animations, link) {
		if (anim->toplevel == toplevel) {
			animation_destroy(server, anim, false);
		}
	}
}

/* Cubic bezier scalar component (the user's shader math, on CPU). */
static float lamp_bezier(float p0, float p1, float p2, float p3, float t) {
	float u = 1.0f - t;
	return u*u*u*p0 + 3.0f*u*u*t*p1 + 3.0f*u*t*t*p2 + t*t*t*p3;
}

/* One frame of the magic lamp. Each strip's TOP and BOTTOM edges are
 * driven independently through the same bezier the KDE shader applies
 * per-vertex (bottom rows pull ahead: exponent 1.8 -> 0.6 down the
 * window), so neighboring strips always share an exact edge — no gaps
 * or overlapping "cut blocks" mid-flight. X follows the center-line
 * bezier with an outward bow plus a smoothstep pinch into the target. */
static void animation_lamp_tick(struct amber_animation *anim, float p) {
	const float pi = 3.14159265f;
	/* Per-row helpers: progress exponent and smoothstep squeeze for a
	 * normalized row position fy. */
#define LAMP_T(fy) powf(p, 1.8f - 1.2f * (fy))

	for (int i = 0; i < anim->strip_count; i++) {
		float fy0 = (float)i / anim->strip_count;
		float fy1 = (float)(i + 1) / anim->strip_count;
		float fyc = 0.5f * (fy0 + fy1);

		float t0 = LAMP_T(fy0);
		float t1 = LAMP_T(fy1);
		float tc = LAMP_T(fyc);
		if (t0 > 1.0f) t0 = 1.0f;
		if (t1 > 1.0f) t1 = 1.0f;
		if (tc > 1.0f) tc = 1.0f;
		float s1 = t1 * t1 * (3.0f - 2.0f * t1);
		float sc = tc * tc * (3.0f - 2.0f * tc);

		/* Vertical edges: bezier Y of each row boundary. Strip i's
		 * bottom edge equals strip i+1's top edge by construction. */
		float y0o = anim->win_y + fy0 * anim->win_h;
		float y1o = anim->win_y + fy1 * anim->win_h;
		float ny0 = lamp_bezier(y0o,
			y0o + (anim->tgt_y - y0o) * 0.33f,
			y0o + (anim->tgt_y - y0o) * 0.66f,
			anim->tgt_y, t0);
		float ny1 = lamp_bezier(y1o,
			y1o + (anim->tgt_y - y1o) * 0.33f,
			y1o + (anim->tgt_y - y1o) * 0.66f,
			anim->tgt_y, t1);
		int top = (int)ny0;
		int h = (int)ny1 - top;
		if (h < 1) {
			h = 1;
		}

		/* Horizontal: per-row squeeze (the funnel feel — bottom rows
		 * pinch first). Bow amplitude 0.25: the sideways whip stays
		 * readable while its per-row drift keeps silhouette steps
		 * ~3px at 128 strips (24 strips + 0.35 bow produced the
		 * ~15px "connected cubes" this replaced). */
		float bow = sinf(tc * pi) * 0.25f * anim->win_w;
		float xc = anim->win_x + anim->win_w * 0.5f;
		float bxc = lamp_bezier(xc, xc + bow,
			anim->tgt_x + bow * 0.5f, anim->tgt_x, tc);
		bxc += (anim->tgt_x - bxc) * sc;
		int w = (int)(anim->win_w * (1.0f - s1));
		if (w < 1) {
			w = 1;
		}

		struct wlr_scene_buffer *s = anim->strips[i];
		int nx = (int)bxc - w / 2;
		int *lr = &anim->strip_rect[i * 4];
		if (lr[0] != nx || lr[1] != top || lr[2] != w ||
				lr[3] != h) {
			wlr_scene_buffer_set_dest_size(s, w, h);
			wlr_scene_node_set_position(&s->node, nx, top);
			lr[0] = nx;
			lr[1] = top;
			lr[2] = w;
			lr[3] = h;
		}
	}
#undef LAMP_T
}

#define WOB_COLS 8
#define WOB_ROWS 6
#define WOB_STIFFNESS 165.0f
#define WOB_DAMPING 14.5f // zeta ~0.55: a couple of visible overshoots
#define WOB_DT (ANIM_TICK_MS / 1000.0f)
#define WOB_SETTLE_DISP 0.8f  // px
#define WOB_SETTLE_VEL 15.0f  // px/s
#define WOB_MAX_VEL 3500.0f   // px/s nudge clamp

/* One frame of the wobble. Every grid POINT springs toward its rest
 * position (which tracks the window's true position each tick), and
 * each cell renders the rect between its top-left and bottom-right
 * points — neighbors share exact corner points, so the sheet can
 * stretch and overshoot but never tear. Returns true once released
 * and settled, or when the 5s failsafe expires. */
static bool animation_wobble_tick(struct amber_animation *anim) {
	struct amber_toplevel *toplevel = anim->toplevel;
	if (toplevel == NULL || toplevel->scene_tree == NULL) {
		return true;
	}
	float k = WOB_STIFFNESS, c = WOB_DAMPING;
	int64_t now = anim_now_usec();
	float dt = anim->last_usec == 0 ? WOB_DT
		: (float)(now - anim->last_usec) / 1000000.0f;
	if (dt < 0.004f) {
		dt = 0.004f;
	}
	if (dt > 0.032f) {
		dt = 0.032f;
	}
	anim->last_usec = now;
	if (anim->wobble_released) {
		/* Straighten onto the real node after release. */
		float decay = expf(-dt * 9.0f);
		anim->drag_x *= decay;
		anim->drag_y *= decay;
	}
	int nx = toplevel->scene_tree->node.x;
	int ny = toplevel->scene_tree->node.y;
	int pts_x = anim->cols + 1, pts_y = anim->rows + 1;
	float cell_w = (float)anim->gw / anim->cols;
	float cell_h = (float)anim->gh / anim->rows;
	float max_disp = 0.0f, max_vel = 0.0f;

	for (int iy = 0; iy < pts_y; iy++) {
		for (int ix = 0; ix < pts_x; ix++) {
			int idx = iy * pts_x + ix;
			float wgt = anim->wf[idx];
			/* Away-from-grip points lag the rigid node:
			 * target = rigid - drag*(1-w) -> shear/bend. */
			float lx = anim->drag_x * (wgt - 1.0f);
			float ly = anim->drag_y * (wgt - 1.0f);
			float ll = sqrtf(lx * lx + ly * ly);
			if (ll > 64.0f) {
				lx *= 64.0f / ll;
				ly *= 64.0f / ll;
			}
			float tx = nx + ix * cell_w + lx;
			float ty = ny + iy * cell_h + ly;
			float ax = k * (tx - anim->px[idx]) - c * anim->vx[idx];
			float ay = k * (ty - anim->py[idx]) - c * anim->vy[idx];
			anim->vx[idx] += ax * dt;
			anim->vy[idx] += ay * dt;
			anim->px[idx] += anim->vx[idx] * dt;
			anim->py[idx] += anim->vy[idx] * dt;
			float dx = fabsf(tx - anim->px[idx]);
			float dy = fabsf(ty - anim->py[idx]);
			if (dx > max_disp) {
				max_disp = dx;
			}
			if (dy > max_disp) {
				max_disp = dy;
			}
			float avx = fabsf(anim->vx[idx]);
			float avy = fabsf(anim->vy[idx]);
			if (avx > max_vel) {
				max_vel = avx;
			}
			if (avy > max_vel) {
				max_vel = avy;
			}
		}
	}

	for (int iy = 0; anim->strip_count > 0 &&
			iy < anim->rows; iy++) {
		for (int ix = 0; ix < anim->cols; ix++) {
			struct wlr_scene_buffer *cell =
				anim->strips[iy * anim->cols + ix];
			int tl = iy * pts_x + ix;
			int br = (iy + 1) * pts_x + ix + 1;
			int w = (int)(anim->px[br] - anim->px[tl]);
			int h = (int)(anim->py[br] - anim->py[tl]);
			if (w < 1) {
				w = 1;
			}
			if (h < 1) {
				h = 1;
			}
			int nx = (int)anim->px[tl];
			int ny = (int)anim->py[tl];
			int *lr = &anim->strip_rect[(iy * anim->cols + ix) * 4];
			/* 2px right/bottom bleed hides hairline seams between
			 * neighboring cells caused by int truncation. */
			if (lr[0] != nx || lr[1] != ny || lr[2] != w ||
					lr[3] != h) {
				wlr_scene_buffer_set_dest_size(cell,
					w + 2, h + 2);
				wlr_scene_node_set_position(&cell->node, nx, ny);
				lr[0] = nx;
				lr[1] = ny;
				lr[2] = w;
				lr[3] = h;
			}
		}
	}

	bool settled = anim->wobble_released &&
		max_disp < WOB_SETTLE_DISP &&
		max_vel < WOB_SETTLE_VEL;
	if (settled && anim->toplevel != NULL) {
		toplevel_apply_fx(anim->toplevel);
	}
	bool expired = anim_now_usec() - anim->start_usec > 5000000;
	return (anim->wobble_released && settled) || expired;
}

/* One pass over all active animations; true while any remain. Driven
 * by output frame events (vblank-aligned updates) with the 16ms timer
 * as fallback for frameless setups. Safe to run at any cadence: lamp
 * strips are a pure function of wall-time progress, wobble springs use
 * adaptive dt, and unchanged rects skip scene writes entirely. */
static bool animations_run(struct amber_server *server) {
	int64_t now = anim_now_usec();
	bool active = false;

	struct amber_animation *anim, *tmp;
	wl_list_for_each_safe(anim, tmp, &server->animations, link) {
		if (anim->kind == ANIM_WOBBLE) {
			/* Springs decide completion, not wall time. */
			if (animation_wobble_tick(anim)) {
				animation_destroy(server, anim, true);
			}
			active = true;
			continue;
		}
		float p = (float)(now - anim->start_usec) /
			(float)(anim->duration_ms * 1000);
		if (p >= 1.0f) {
			animation_destroy(server, anim, true);
			continue;
		}
		switch (anim->kind) {
		case ANIM_OPEN: {
			/* Cubic ease-out: fast start, gentle landing. */
			float eased = 1.0f - (1.0f - p) * (1.0f - p)
				* (1.0f - p);
			float inv = 1.0f - eased;
			wlr_scene_node_set_position(&anim->tree->node,
				anim->base_x + (int)(anim->from_dx * inv),
				anim->base_y);
			animation_tree_set_opacity(anim->tree,
				0.25f + 0.75f * eased);
		} break;
		case ANIM_LAMP_CLOSE:
			animation_lamp_tick(anim, p);
			break;
		case ANIM_WS_SLIDE: {
			/* Cubic ease-in-out: smooth departure AND arrival. */
			float e = p < 0.5f ? 4.0f * p * p * p
				: 1.0f - powf(-2.0f * p + 2.0f, 3.0f) / 2.0f;
			int width = anim->output != NULL
				? anim->output->layout_box.width : 0;
			wlr_scene_node_set_position(
				&anim->ws_from->tree->node,
				anim->origin_x - (int)(anim->slide_dir *
					width * e),
				anim->origin_y);
			wlr_scene_node_set_position(&anim->ws_to->tree->node,
				anim->origin_x + (int)(anim->slide_dir *
					width * (1.0f - e)),
				anim->origin_y);
		} break;
		case ANIM_FLOAT_TWEEN: {
			/* Cubic ease-out, same feel as window-open. */
			float eased = 1.0f - (1.0f - p) * (1.0f - p)
				* (1.0f - p);
			float inv = 1.0f - eased;
			wlr_scene_node_set_position(&anim->tree->node,
				anim->base_x + (int)((anim->from_x
					- anim->base_x) * inv),
				anim->base_y + (int)((anim->from_y
					- anim->base_y) * inv));
		} break;
		}
		active = true;
	}
	return active;
}

static int animation_tick(void *data) {
	struct amber_server *server = data;
	bool active = animations_run(server);
	server->anim_timer_armed = active;
	/* libwayland 1.26 ignores the callback's positive return value
	 * (rearm-by-return regressed), so rearm explicitly instead. */
	if (active && server->anim_source != NULL) {
		wl_event_source_timer_update(server->anim_source,
			ANIM_TICK_MS);
	}
	return 0; // 0 would normally disarm; handled above
}

static void animations_kick(struct amber_server *server) {
	if (server->anim_timer_armed || server->anim_source == NULL) {
		return;
	}
	server->anim_timer_armed = true;
	wl_event_source_timer_update(server->anim_source, 1);
}

static void animation_start_open(struct amber_toplevel *toplevel) {
	struct amber_server *server = toplevel->server;
	if (!server->animations_enabled ||
			server->animation_duration_ms == 0 ||
			toplevel->fullscreen || toplevel->output == NULL) {
		return;
	}

	struct amber_animation *anim = calloc(1, sizeof(*anim));
	if (anim == NULL) {
		return;
	}
	/* workspace_arrange has already placed the node at its final
	 * spot; capture it and slide in from the usable area's right. */
	anim->kind = ANIM_OPEN;
	anim->toplevel = toplevel;
	anim->tree = toplevel->scene_tree;
	anim->base_x = toplevel->scene_tree->node.x;
	anim->base_y = toplevel->scene_tree->node.y;
	int edge_x = toplevel->output->usable_area.width +
		toplevel->output->usable_area.x;
	anim->from_dx = edge_x - anim->base_x;
	if (anim->from_dx < 32) {
		anim->from_dx = 32; // always at least a nudge
	}
	anim->start_usec = anim_now_usec();
	anim->duration_ms = server->animation_duration_ms;

	wl_list_insert(server->animations.prev, &anim->link);
	animations_kick(server);
}

/* Glide a live window from (from_x, from_y) to wherever its scene node
 * sits right now (the caller arranges first). Skipped when disabled or
 * when the move is too small to see. */
static void animation_start_float_glide(struct amber_toplevel *toplevel,
		int from_x, int from_y) {
	struct amber_server *server = toplevel->server;
	if (!server->animations_enabled ||
			server->animation_duration_ms == 0 ||
			toplevel->scene_tree == NULL) {
		return;
	}
	int to_x = toplevel->scene_tree->node.x;
	int to_y = toplevel->scene_tree->node.y;
	int dx = to_x - from_x, dy = to_y - from_y;
	if (dx * dx + dy * dy < 64) { // under ~8px: not worth a tween
		return;
	}

	/* A newer tween supersedes an in-flight one so rapid rearranges
	 * chain smoothly instead of two animations fighting over the node. */
	struct amber_animation *prev_glide, *glide_tmp;
	wl_list_for_each_safe(prev_glide, glide_tmp, &server->animations,
			link) {
		if (prev_glide->toplevel == toplevel &&
				prev_glide->kind == ANIM_FLOAT_TWEEN) {
			wl_list_remove(&prev_glide->link);
			free(prev_glide);
		}
	}

	struct amber_animation *anim = calloc(1, sizeof(*anim));
	if (anim == NULL) {
		return;
	}
	anim->kind = ANIM_FLOAT_TWEEN;
	anim->toplevel = toplevel;
	anim->tree = toplevel->scene_tree;
	anim->base_x = to_x;
	anim->base_y = to_y;
	anim->from_x = from_x;
	anim->from_y = from_y;
	anim->start_usec = anim_now_usec();
	anim->duration_ms = server->animation_duration_ms;

	wl_list_insert(server->animations.prev, &anim->link);
	animations_kick(server);
}

/* KDE magic-lamp close effect. wlroots emits surface::unmap BEFORE
 * swapping in the empty commit, so the last displayed buffer + logical
 * size are still valid right here: lock the buffer (keeps pixels alive
 * no matter how the client dies), slice it into horizontal strips as
 * scene buffers above every layer, and let animation_lamp_tick drive
 * each strip along the bezier path to the bottom-center target. */
#define LAMP_STRIP_COUNT 128 // dense: per-row side steps stay ~3px

static void animation_start_lamp_close(struct amber_toplevel *toplevel) {
	struct amber_server *server = toplevel->server;
	struct amber_output *output = toplevel->output;
	if (!server->animations_enabled || !server->lamp_close_enabled ||
			server->lamp_close_duration_ms == 0 ||
			output == NULL || output->fx_tree == NULL ||
			toplevel->workspace != output->active_workspace) {
		return;
	}
	if (toplevel->scene_tree == NULL) {
		return;
	}
	/* The window's node x/y are output-local: every parent tree sits
	 * at the output's layout origin. */
	int win_x = toplevel->scene_tree->node.x;
	int win_y = toplevel->scene_tree->node.y;

	/* The scene graph holds its OWN lock on the last displayed buffer
	 * and only clears it on the commit that follows this unmap — so
	 * reading scene_buffer->buffer here is race-free, unlike the
	 * surface's own buffer pointers which the client teardown may
	 * have already freed. */
	struct wlr_buffer *snap_buf = toplevel->scene_buffer != NULL
		? toplevel->scene_buffer->buffer : NULL;
	if (snap_buf == NULL && toplevel->xdg_toplevel->base->surface != NULL) {
		struct wlr_surface *ws =
			toplevel->xdg_toplevel->base->surface;
		snap_buf = ws->buffer != NULL ? &ws->buffer->base
			: ws->current.buffer;
	}
	struct wlr_surface *surface = toplevel->xdg_toplevel->base->surface;
	int surf_w = surface ? surface->current.width : 0;
	int surf_h = surface ? surface->current.height : 0;
	if (snap_buf == NULL || surf_w < 1 || surf_h < 1) {
		return; // nothing worth animating (empty/dead buffer)
	}

	struct amber_animation *anim = calloc(1, sizeof(*anim));
	if (anim == NULL) {
		return;
	}
	anim->kind = ANIM_LAMP_CLOSE;
	anim->output = output;
	anim->win_x = win_x;
	anim->win_y = win_y;
	anim->win_w = surface->current.width;
	anim->win_h = surface->current.height;
	anim->tgt_x = output->usable_area.x +
		output->usable_area.width / 2;
	anim->tgt_y = output->usable_area.y +
		output->usable_area.height;
	anim->start_usec = anim_now_usec();
	anim->duration_ms = server->lamp_close_duration_ms;
	anim->snapshot = snap_buf;
	wlr_buffer_lock(anim->snapshot);

	anim->strip_count = LAMP_STRIP_COUNT;
	anim->strips = calloc(LAMP_STRIP_COUNT,
		sizeof(*anim->strips));
	anim->strip_rect = calloc(LAMP_STRIP_COUNT * 4,
		sizeof(*anim->strip_rect));
	if (anim->strips == NULL || anim->strip_rect == NULL) {
		animation_destroy(server, anim, false);
		return;
	}
	float buf_w = anim->snapshot->width;
	float buf_h = anim->snapshot->height;
	for (int i = 0; i < LAMP_STRIP_COUNT; i++) {
		struct wlr_scene_buffer *s =
			wlr_scene_buffer_create(output->fx_tree,
				anim->snapshot);
		if (s == NULL) {
			anim->strip_count = i; // only destroy what exists
			animation_destroy(server, anim, false);
			return;
		}
		struct wlr_fbox src = {
			.x = 0,
			.y = (double)i * buf_h / LAMP_STRIP_COUNT,
			.width = buf_w,
			.height = buf_h / LAMP_STRIP_COUNT,
		};
		wlr_scene_buffer_set_source_box(s, &src);
		wlr_scene_buffer_set_dest_size(s,
			anim->win_w, anim->win_h / LAMP_STRIP_COUNT);
		wlr_scene_node_set_position(&s->node, win_x,
			win_y + (int)((double)i * anim->win_h /
				LAMP_STRIP_COUNT));
		anim->strips[i] = s;
	}

	wl_list_insert(server->animations.prev, &anim->link);
	animations_kick(server);
}

/* Compiz-style wobble for interactive moves. The window's live buffer
 * is locked (same scene-graph lock trick as the lamp), sliced into a
 * cols x rows grid of scene buffers, and the real node is hidden until
 * the springs settle after button release. */
void animation_start_wobble(struct amber_toplevel *toplevel) {
	struct amber_server *server = toplevel->server;
	struct amber_output *output = toplevel->output;
	if (!server->animations_enabled || !server->wobbly_windows ||
			output == NULL || output->fx_tree == NULL ||
			toplevel->fullscreen ||
			toplevel->workspace != output->active_workspace ||
			toplevel->scene_tree == NULL) {
		return;
	}
	if (false) {
		wlr_log(WLR_INFO, "wobble: bail flags (anim=%d wob=%d out=%p ws_match=%d)",
			server->animations_enabled, server->wobbly_windows,
			(void *)output,
			toplevel->output != NULL &&
				toplevel->workspace == output->active_workspace);
		return;
	}

	/* Re-grabbing during a settle retires the old sheet first. */
	struct amber_animation *old, *old_tmp;
	wl_list_for_each_safe(old, old_tmp, &server->animations, link) {
		if (old->kind == ANIM_WOBBLE && old->toplevel == toplevel) {
			animation_destroy(server, old, true);
		}
	}

	/* Same race-free read as the lamp: the scene graph holds its own
	 * lock on the last displayed buffer. */
	struct wlr_buffer *snap_buf = toplevel->scene_buffer != NULL
		? toplevel->scene_buffer->buffer : NULL;
	struct wlr_surface *surface = toplevel->xdg_toplevel->base->surface;
	int surf_w = surface ? surface->current.width : 0;
	int surf_h = surface ? surface->current.height : 0;
	if (snap_buf == NULL || surf_w < 1 || surf_h < 1) {
		wlr_log(WLR_INFO, "wobble: bail snapshot (buf=%p %dx%d)",
			(void *)snap_buf, surf_w, surf_h);
		return;
	}

	struct amber_animation *anim = calloc(1, sizeof(*anim));
	if (anim == NULL) {
		return;
	}
	anim->kind = ANIM_WOBBLE;
	anim->toplevel = toplevel;
	anim->tree = toplevel->scene_tree;
	anim->output = output;
	anim->gx = toplevel->scene_tree->node.x;
	anim->gy = toplevel->scene_tree->node.y;
	anim->gw = surf_w;
	anim->gh = surf_h;
	anim->cols = WOB_COLS;
	anim->rows = WOB_ROWS;
	anim->start_usec = anim_now_usec();

	int pts_x = anim->cols + 1, pts_y = anim->rows + 1;
	int point_count = pts_x * pts_y;
	anim->px = calloc(point_count, sizeof(float));
	anim->py = calloc(point_count, sizeof(float));
	anim->vx = calloc(point_count, sizeof(float));
	anim->vy = calloc(point_count, sizeof(float));
	anim->wf = calloc(point_count, sizeof(float));
	anim->ax = server->grab_x;
	anim->ay = server->grab_y;
	anim->drag_x = 0.0f;
	anim->drag_y = 0.0f;
	anim->strips = calloc(anim->cols * anim->rows,
		sizeof(*anim->strips));
	anim->strip_rect = calloc(anim->cols * anim->rows * 4,
		sizeof(*anim->strip_rect));
	if (anim->px == NULL || anim->py == NULL || anim->vx == NULL ||
			anim->vy == NULL || anim->wf == NULL ||
			anim->strips == NULL || anim->strip_rect == NULL) {
		animation_destroy(server, anim, false);
		return;
	}
	anim->last_usec = anim_now_usec();

	float cell_w0 = (float)anim->gw / anim->cols;
	float cell_h0 = (float)anim->gh / anim->rows;
	for (int iy = 0; iy < pts_y; iy++) {
		for (int ix = 0; ix < pts_x; ix++) {
			float dxp = ix * cell_w0 - anim->ax;
			float dyp = iy * cell_h0 - anim->ay;
			float dist = sqrtf(dxp * dxp + dyp * dyp);
			anim->wf[iy * pts_x + ix] =
				1.0f / (1.0f + dist / 240.0f);
		}
	}
	toplevel_apply_fx(toplevel);
	anim->snapshot = snap_buf;
	wlr_buffer_lock(anim->snapshot);

	float buf_w = anim->snapshot->width;
	float buf_h = anim->snapshot->height;
	for (int iy = 0; iy < anim->rows; iy++) {
		for (int ix = 0; ix < anim->cols; ix++) {
			struct wlr_scene_buffer *cell =
				wlr_scene_buffer_create(output->fx_tree,
					anim->snapshot);
			if (cell == NULL) {
				anim->strip_count = iy * anim->cols + ix;
				animation_destroy(server, anim, false);
				return;
			}
			struct wlr_fbox src = {
				.x = (double)ix * buf_w / anim->cols,
				.y = (double)iy * buf_h / anim->rows,
				.width = buf_w / anim->cols,
				.height = buf_h / anim->rows,
			};
			wlr_scene_buffer_set_source_box(cell, &src);
			wlr_scene_buffer_set_dest_size(cell,
				anim->gw / anim->cols, anim->gh / anim->rows);
			wlr_scene_node_set_position(&cell->node,
				anim->gx + (int)(ix * anim->gw /
					(float)anim->cols),
				anim->gy + (int)(iy * anim->gh /
					(float)anim->rows));
			anim->strips[iy * anim->cols + ix] = cell;
		}
	}
	anim->strip_count = anim->cols * anim->rows;

	for (int iy = 0; iy < pts_y; iy++) {
		for (int ix = 0; ix < pts_x; ix++) {
			int idx = iy * pts_x + ix;
			anim->px[idx] = anim->gx +
				ix * (float)anim->gw / anim->cols;
			anim->py[idx] = anim->gy +
				iy * (float)anim->gh / anim->rows;
		}
	}

	wlr_scene_node_set_enabled(&anim->tree->node, false);

	wl_list_insert(server->animations.prev, &anim->link);
	animations_kick(server);
}

/* Inject pointer motion into every point so fast drags visibly throw
 * the sheet around (velocity clamp keeps flings sane). */
void animation_wobble_nudge(struct amber_toplevel *toplevel,
		int dx, int dy) {
	struct amber_server *server = toplevel->server;
	struct amber_animation *anim, *tmp;
	wl_list_for_each_safe(anim, tmp, &server->animations, link) {
		if (anim->kind != ANIM_WOBBLE || anim->toplevel != toplevel) {
			continue;
		}
		anim->drag_x += (float)dx;
		anim->drag_y += (float)dy;
		int count = (anim->cols + 1) * (anim->rows + 1);
		for (int i = 0; i < count; i++) {
			float kick = 10.0f * (anim->wf != NULL
					? anim->wf[i] : 1.0f);
			float nvx = anim->vx[i] + dx * kick;
			float nvy = anim->vy[i] + dy * kick;
			if (nvx > WOB_MAX_VEL) nvx = WOB_MAX_VEL;
			if (nvx < -WOB_MAX_VEL) nvx = -WOB_MAX_VEL;
			if (nvy > WOB_MAX_VEL) nvy = WOB_MAX_VEL;
			if (nvy < -WOB_MAX_VEL) nvy = -WOB_MAX_VEL;
			anim->vx[i] = nvx;
			anim->vy[i] = nvy;
		}
	}
}

/* Button-up: let the springs carry the sheet home before swap-back. */
void animation_wobble_release(struct amber_toplevel *toplevel) {
	struct amber_server *server = toplevel->server;
	struct amber_animation *anim, *tmp;
	wl_list_for_each_safe(anim, tmp, &server->animations, link) {
		if (anim->kind == ANIM_WOBBLE && anim->toplevel == toplevel) {
			anim->wobble_released = true;
		}
	}
}

/*
 * Built-in wallpaper.
 *
 * The image is decoded once at startup and wrapped in a wlr_buffer; every
 * output gets a static scene buffer node on its background layer. After
 * the initial upload the per-frame cost is zero: damage tracking only
 * touches it when something actually changes, and nothing renders below.
 */

static bool wallpaper_buffer_begin_data_ptr_access(struct wlr_buffer *wlr_buf,
		uint32_t flags, void **data, uint32_t *format, size_t *stride) {
	struct amber_wallpaper_buffer *buf =
		wl_container_of(wlr_buf, buf, base);
	*data = buf->data;
	*format = DRM_FORMAT_ABGR8888;
	*stride = (size_t)buf->width * 4;
	return true;
}

static void wallpaper_buffer_end_data_ptr_access(struct wlr_buffer *wlr_buf) {
	/* Pixels are plain heap memory; nothing to unmap. */
}

static void wallpaper_buffer_destroy(struct wlr_buffer *wlr_buf) {
	struct amber_wallpaper_buffer *buf =
		wl_container_of(wlr_buf, buf, base);
	free(buf->data);
	free(buf);
}

static const struct wlr_buffer_impl wallpaper_buffer_impl = {
	.destroy = wallpaper_buffer_destroy,
	.begin_data_ptr_access = wallpaper_buffer_begin_data_ptr_access,
	.end_data_ptr_access = wallpaper_buffer_end_data_ptr_access,
};

/* Decode an image file into a wlr_buffer (once, at startup). */
static struct wlr_buffer *wallpaper_load(const char *path) {
	int iw, ih;
	stbi_uc *pixels = stbi_load(path, &iw, &ih, NULL, 4);
	if (pixels == NULL) {
		wlr_log(WLR_ERROR, "wallpaper: failed to load '%s'", path);
		return NULL;
	}
	if (iw <= 0 || ih <= 0 || iw > 16384 || ih > 16384) {
		wlr_log(WLR_ERROR, "wallpaper: bad dimensions %dx%d", iw, ih);
		stbi_image_free(pixels);
		return NULL;
	}
	wlr_log(WLR_INFO, "wallpaper: loaded %s (%dx%d)", path, iw, ih);
	struct amber_wallpaper_buffer *buf = calloc(1, sizeof(*buf));
	if (buf == NULL) {
		stbi_image_free(pixels);
		return NULL;
	}
	wlr_buffer_init(&buf->base, &wallpaper_buffer_impl, iw, ih);
	buf->data = pixels;
	buf->width = iw;
	buf->height = ih;
	return &buf->base;
}

/* (Re)create this output's background node for the current mode. */
static void output_attach_wallpaper(struct amber_output *output) {
	struct amber_server *server = output->server;

	if (output->wallpaper_node != NULL) {
		wlr_scene_node_destroy(&output->wallpaper_node->node);
		output->wallpaper_node = NULL;
	}
	struct wlr_buffer *img = server->wallpaper_buffer;
	if (img == NULL || !output->wlr_output->enabled) {
		return;
	}
	const int W = output->layout_box.width;
	const int H = output->layout_box.height;
	if (W <= 0 || H <= 0) {
		return;
	}
	const int iw = img->width;
	const int ih = img->height;

	struct wlr_scene_buffer *sb =
		wlr_scene_buffer_create(output->
			layer_trees[ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND],
			img);

	switch (server->wallpaper_mode) {
	case AMBER_WALLPAPER_STRETCH:
		wlr_scene_buffer_set_dest_size(sb, W, H);
		wlr_scene_node_set_position(&sb->node, 0, 0);
		break;
	case AMBER_WALLPAPER_COVER: {
		double scale = fmax((double)W / iw, (double)H / ih);
		double src_w = W / scale, src_h = H / scale;
		struct wlr_fbox crop = {
			.x = (iw - src_w) / 2.0,
			.y = (ih - src_h) / 2.0,
			.width = src_w,
			.height = src_h,
		};
		wlr_scene_buffer_set_source_box(sb, &crop);
		wlr_scene_buffer_set_dest_size(sb, W, H);
		wlr_scene_node_set_position(&sb->node, 0, 0);
		break;
	}
	case AMBER_WALLPAPER_CONTAIN:
	case AMBER_WALLPAPER_CENTER: {
		double scale = fmin((double)W / iw, (double)H / ih);
		if (server->wallpaper_mode == AMBER_WALLPAPER_CENTER &&
				scale > 1.0) {
			scale = 1.0; // never upscale in center mode
		}
		int dw = (int)(iw * scale);
		int dh = (int)(ih * scale);
		wlr_scene_buffer_set_dest_size(sb, dw, dh);
		wlr_scene_node_set_position(&sb->node, (W - dw) / 2,
			(H - dh) / 2);
		break;
	}
	}
	output->wallpaper_node = sb;
}

static void output_frame(struct wl_listener *listener, void *data) {
	/* This function is called every time an output is ready to display a frame,
	 * generally at the output's refresh rate (e.g. 60Hz). */
	struct amber_output *output = wl_container_of(listener, output, frame);
	struct wlr_scene *scene = output->server->scene;

	/* Advance animations just before commit so every displayed frame
	 * carries the freshest state — vblank-aligned, no beat stutter
	 * against the 16ms fallback timer. */
	if (output->server->anim_timer_armed &&
			!animations_run(output->server)) {
		output->server->anim_timer_armed = false;
	}

	struct wlr_scene_output *scene_output = wlr_scene_get_scene_output(
		scene, output->wlr_output);

	wlr_scene_output_commit(scene_output, NULL);

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(scene_output, &now);
}

static void output_request_state(struct wl_listener *listener, void *data) {
	/* This function is called when the backend requests a new state for
	 * the output. For example, Wayland and X11 backends request a new mode
	 * when the output window is resized. */
	struct amber_output *output = wl_container_of(listener, output, request_state);
	const struct wlr_output_event_request_state *event = data;
	wlr_output_commit_state(output->wlr_output, event->state);
	output_update_geometry(output);
}

/* ---- Idle CPU/GPU throttle (niri-style refresh drop while idle) ----
 * When no input has arrived for idle-throttle-seconds AND nothing is
 * playing video (no active idle-inhibitor), drop each DRM output to a
 * low refresh rate (idle-throttle-hz, default 10Hz) to cut draw cost.
 * Any input or media activity restores the normal refresh. */

static void idle_throttle_apply(struct amber_server *server, bool throttle) {
	if (server->idle_throttled == throttle) {
		return;
	}
	server->idle_throttled = throttle;
	struct amber_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		struct wlr_output *wlr_output = output->wlr_output;
		if (!wlr_backend_is_drm(wlr_output->backend)) {
			continue;
		}
		struct wlr_output_mode *mode = wlr_output->current_mode;
		struct wlr_output_state state;
		wlr_output_state_init(&state);
		wlr_output_state_set_enabled(&state, true);
		if (throttle) {
			/* Low-refresh custom modes only work on outputs with a
			 * real mode menu. A fixed panel that exposes a single
			 * mode (e.g. LVDS/edp laptops) cannot display an
			 * arbitrary 10Hz rate; modesetting it retries and
			 * flickers. Only throttle when >= 2 modes exist. */
			int modes = 0;
			struct wlr_output_mode *m;
			wl_list_for_each(m, &wlr_output->modes, link) {
				modes++;
			}
			if (modes >= 2 && mode != NULL) {
				int mhz = server->idle_throttle_hz * 1000;
				wlr_output_state_set_custom_mode(&state,
					mode->width, mode->height, mhz);
			}
		} else if (mode != NULL) {
			wlr_output_state_set_mode(&state, mode);
		}
		wlr_output_commit_state(wlr_output, &state);
		wlr_output_state_finish(&state);
	}
	if (throttle) {
		wlr_log(WLR_INFO, "idle-throttle: low refresh requested "
			"(skipped on fixed-mode outputs)");
	} else {
		wlr_log(WLR_INFO, "idle-throttle: normal refresh");
	}
}

/* Called periodically (once a second). Depending on idle time, media
 * inhibitors, and live animations, drop or restore the refresh rate. */
static int idle_throttle_tick(void *data) {
	struct amber_server *server = data;
	bool busy = server->idle_inhibitors > 0 ||
		!wl_list_empty(&server->animations);
	bool active = server->idle_throttle_enabled;
	if (server->idle_throttled && (busy || !active)) {
		idle_throttle_apply(server, false);
		return 0;
	}
	if (!active) {
		return 0;
	}
	int64_t now = idle_now_usec();
	int64_t idle_us = now - server->last_input_usec;
	if (busy) {
		/* Remember idle age so a pause of a video still throttles
		 * after the configured delay. */
		server->last_input_usec = now;
		return 0;
	}
	if (idle_us >= (int64_t)server->idle_throttle_seconds * 1000000) {
		idle_throttle_apply(server, true);
	}
	return 0;
}

#define IDLE_TICK_MS 1000

static int64_t idle_now_usec(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* Stamped by every keyboard/pointer event: user is awake. */
static void idle_mark_activity(struct amber_server *server) {
	server->last_input_usec = idle_now_usec();
	if (server->idle_throttled) {
		idle_throttle_apply(server, false);
	}
}

/* One of these is tracked per live idle-inhibitor (a video playing). */
struct idle_inhibitor_track {
	struct amber_server *server;
	struct wl_listener destroy;
};

static void idle_inhibitor_track_destroy(struct wl_listener *listener,
		void *data) {
	struct idle_inhibitor_track *track =
		wl_container_of(listener, track, destroy);
	struct amber_server *server = track->server;
	(void)data;
	if (server->idle_inhibitors > 0) {
		server->idle_inhibitors--;
	}
	wl_list_remove(&listener->link);
	free(track);
	/* Media stopped, so an idle pause may now legitimately throttle. */
	if (server->idle_inhibitors == 0) {
		idle_mark_activity(server);
	}
}

static void idle_inhibit_new(struct wl_listener *listener, void *data) {
	struct amber_server *server =
		wl_container_of(listener, server, idle_inhibit_new);
	struct wlr_idle_inhibitor_v1 *inhibitor = data;
	/* Register a per-inhibitor destroy listener so we can track how
	 * many media sources are currently inhibiting idle. */
	struct idle_inhibitor_track *track =
		calloc(1, sizeof(*track));
	if (track == NULL) {
		return;
	}
	track->server = server;
	track->destroy.notify = idle_inhibitor_track_destroy;
	wl_signal_add(&inhibitor->events.destroy, &track->destroy);
	server->idle_inhibitors++;
	/* Media started playing: restore full refresh immediately. */
	if (server->idle_throttled) {
		idle_throttle_apply(server, false);
	}
	server->last_input_usec = idle_now_usec();
}

static struct amber_output *find_output(struct amber_server *server,
		struct wlr_output *wlr_output) {
	struct amber_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		if (output->wlr_output == wlr_output) {
			return output;
		}
	}
	return NULL;
}

/* wayland-output-management-v1: kanshi-style tools reconfigure outputs at
 * runtime (mode, scale, transform, position, enable). wlroots maintains the
 * head list; we apply the requested heads and reply. */
static void output_manager_send_configuration(struct amber_server *server) {
	if (server->output_manager == NULL) {
		return;
	}
	struct wlr_output_configuration_v1 *config =
		wlr_output_configuration_v1_create();
	struct amber_output *amber_output;
	wl_list_for_each(amber_output, &server->outputs, link) {
		struct wlr_output_configuration_head_v1 *head =
			wlr_output_configuration_head_v1_create(config,
				amber_output->wlr_output);
		struct wlr_output_layout_output *layout_out =
			wlr_output_layout_get(server->output_layout,
				amber_output->wlr_output);
		if (layout_out != NULL) {
			head->state.x = layout_out->x;
			head->state.y = layout_out->y;
		}
	}
	wlr_output_manager_v1_set_configuration(server->output_manager, config);
}

static void output_manager_apply(struct wl_listener *listener, void *data) {
	struct amber_server *server =
		wl_container_of(listener, server, output_manager_apply);
	struct wlr_output_configuration_v1 *config = data;

	bool ok = true;
	struct wlr_output_configuration_head_v1 *head;
	wl_list_for_each(head, &config->heads, link) {
		struct wlr_output *wlr_output = head->state.output;
		struct amber_output *output = find_output(server, wlr_output);
		/* The head position maps to output-layout coordinates; apply it
		 * before committing so the geometry is consistent. */
		wlr_output_layout_add(server->output_layout, wlr_output,
			head->state.x, head->state.y);

		struct wlr_output_state state;
		wlr_output_state_init(&state);
		wlr_output_head_v1_state_apply(&head->state, &state);
		if (!wlr_output_commit_state(wlr_output, &state)) {
			ok = false;
		}
		wlr_output_state_finish(&state);

		if (output != NULL) {
			output_update_geometry(output);
		}
	}

	if (ok) {
		wlr_output_configuration_v1_send_succeeded(config);
	} else {
		wlr_output_configuration_v1_send_failed(config);
	}
	wlr_output_configuration_v1_destroy(config);

	output_manager_send_configuration(server);
}

static void output_manager_test(struct wl_listener *listener, void *data) {
	struct amber_server *server =
		wl_container_of(listener, server, output_manager_test);
	struct wlr_output_configuration_v1 *config = data;

	bool ok = true;
	struct wlr_output_configuration_head_v1 *head;
	wl_list_for_each(head, &config->heads, link) {
		struct wlr_output_state state;
		wlr_output_state_init(&state);
		wlr_output_head_v1_state_apply(&head->state, &state);
		if (!wlr_output_test_state(head->state.output, &state)) {
			ok = false;
		}
		wlr_output_state_finish(&state);
	}

	if (ok) {
		wlr_output_configuration_v1_send_succeeded(config);
	} else {
		wlr_output_configuration_v1_send_failed(config);
	}
	wlr_output_configuration_v1_destroy(config);
}

static void output_power_set_mode(struct wl_listener *listener, void *data) {
	/* zwlr_output_power_manager_v1: wlopm turns a monitor off/on by
	 * committing an output with the enabled flag toggled (DPMS off on
	 * the DRM backend is exactly that). */
	struct amber_server *server =
		wl_container_of(listener, server, output_power_set_mode);
	struct wlr_output_power_v1_set_mode_event *event = data;

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state,
		event->mode == ZWLR_OUTPUT_POWER_V1_MODE_ON);
	wlr_output_commit_state(event->output, &state);
	wlr_output_state_finish(&state);
	(void)server;
}

/* ext-workspace-v1 requires all group/workspace handles destroyed
 * before the display tears down; also needed for outputs that never
 * pass through output_destroy (direct wl_display_destroy at exit). */
static void ext_workspace_release_output(struct amber_output *output) {
	if (output->server->ext_ws_mgr == NULL) {
		return;
	}
	for (int i = 0; i < AMBER_WORKSPACE_COUNT; i++) {
		if (output->ext_ws[i] != NULL) {
			wlr_ext_workspace_handle_v1_destroy(
				output->ext_ws[i]);
			output->ext_ws[i] = NULL;
		}
	}
	if (output->ext_group != NULL) {
		wlr_ext_workspace_group_handle_v1_destroy(
			output->ext_group);
		output->ext_group = NULL;
	}
}

static void output_destroy(struct wl_listener *listener, void *data) {
	struct amber_output *output = wl_container_of(listener, output, destroy);

	/* Lamp/slide animations hold this output pointer; finish them
	 * before it is freed. */
	struct amber_server *server = output->server;
	struct amber_animation *anim, *tmp;
	wl_list_for_each_safe(anim, tmp, &server->animations, link) {
		if ((anim->kind == ANIM_LAMP_CLOSE ||
					anim->kind == ANIM_WS_SLIDE ||
					anim->kind == ANIM_WOBBLE) &&
				anim->output == output) {
			animation_destroy(server, anim, false);
		}
	}

	/* Release overview UI before the layers/buffers are torn down. */
	if (output->ov_active) {
		overview_close(server);
	}

	wl_list_remove(&output->frame.link);
	wl_list_remove(&output->request_state.link);
	wl_list_remove(&output->destroy.link);
	wl_list_remove(&output->link);
	ext_workspace_release_output(output);
	free(output);
}

/*
 * Layer shell.
 *
 * Bars, launchers and wallpapers live here. Each surface is a child of its
 * output's per-layer scene tree; arrangement is delegated to
 * wlr_scene_layer_surface_v1_configure(), which also folds exclusive zones
 * into the usable area we will later tile windows into.
 */

static void arrange_layers(struct amber_output *output) {
	struct wlr_box full = {0};
	wlr_output_effective_resolution(output->wlr_output,
		&full.width, &full.height);

	/* Pass 1: exclusive surfaces shrink the usable area. */
	struct wlr_box usable = full;
	for (int i = 0; i < 4; i++) {
		struct amber_layer_surface *layer;
		wl_list_for_each(layer, &output->layers[i], link) {
			if (layer->layer_surface->current.exclusive_zone > 0) {
				wlr_scene_layer_surface_v1_configure(
					layer->scene_layer, &full, &usable);
			}
		}
	}
	output->usable_area = usable;

	/* Pass 2: everything else positions within the final usable area. */
	for (int i = 0; i < 4; i++) {
		struct amber_layer_surface *layer;
		wl_list_for_each(layer, &output->layers[i], link) {
			if (layer->layer_surface->current.exclusive_zone > 0) {
				continue;
			}
			struct wlr_box bounds = usable;
			struct wlr_box unused = usable;
			wlr_scene_layer_surface_v1_configure(
				layer->scene_layer, &bounds, &unused);
		}
	}

	/* Keep the strip in sync with reserved space: when a bar maps,
	 * unmaps or resizes, tiles must respect the new usable area. */
	workspace_arrange(&output->workspaces[output->active_workspace]);
	workspace_update_top_layer(output);
}

static void focus_layer_surface(struct amber_server *server,
		struct amber_layer_surface *layer) {
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server->seat);
	server->focused_toplevel = NULL;
	if (keyboard != NULL) {
		wlr_seat_keyboard_notify_enter(server->seat,
			layer->layer_surface->surface,
			keyboard->keycodes, keyboard->num_keycodes,
			&keyboard->modifiers);
	}
}

static void layer_surface_handle_map(struct wl_listener *listener, void *data) {
	struct amber_layer_surface *layer =
		wl_container_of(listener, layer, map);
	layer->mapped = true;
	arrange_layers(layer->output);

	/* Launchers/lock screens ask for keyboard interactivity; hand them
	 * focus on map. Regular bars never set this, so they can't steal it. */
	if (layer->layer_surface->current.keyboard_interactive !=
			ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE) {
		focus_layer_surface(layer->server, layer);
	}
}

static void layer_surface_handle_unmap(struct wl_listener *listener,
		void *data) {
	struct amber_layer_surface *layer =
		wl_container_of(listener, layer, unmap);
	layer->mapped = false;
	if (layer->server->focused_toplevel == NULL) {
		struct amber_output *output = active_output(layer->server);
		if (output != NULL) {
			focus_workspace_topmost(layer->server,
				&output->workspaces[output->active_workspace]);
		}
	}
}

static void layer_surface_handle_commit(struct wl_listener *listener,
		void *data) {
	struct amber_layer_surface *layer =
		wl_container_of(listener, layer, commit);
	struct wlr_layer_surface_v1 *layer_surface = layer->layer_surface;

	if (layer_surface->initial_commit) {
		/* The compositor must configure the surface before it can
		 * map; arrange_layers() does this for every layer surface. */
		arrange_layers(layer->output);
		return;
	}

	/* `current.committed` is non-zero while the client has un-acked
	 * state, which prevents configure<->commit ping-pong loops. */
	if (!layer_surface->current.committed) {
		return;
	}

	arrange_layers(layer->output);
}

static void layer_surface_handle_destroy(struct wl_listener *listener,
		void *data) {
	struct amber_layer_surface *layer =
		wl_container_of(listener, layer, destroy);

	wl_list_remove(&layer->link);
	wl_list_remove(&layer->map.link);
	wl_list_remove(&layer->unmap.link);
	wl_list_remove(&layer->commit.link);
	wl_list_remove(&layer->destroy.link);
	free(layer);
}

static void server_new_layer_surface(struct wl_listener *listener,
		void *data) {
	struct amber_server *server =
		wl_container_of(listener, server, new_layer_surface);
	struct wlr_layer_surface_v1 *layer_surface = data;

	/* Clients may not know which output they want (e.g. launchers);
	 * default to the active one. */
	if (layer_surface->output == NULL) {
		struct amber_output *output = active_output(server);
		if (output == NULL && !wl_list_empty(&server->outputs)) {
			output = wl_container_of(server->outputs.next,
				output, link);
		}
		if (output == NULL) {
			wlr_layer_surface_v1_destroy(layer_surface);
			return;
		}
		layer_surface->output = output->wlr_output;
	}
	struct amber_output *output = NULL;
	struct amber_output *candidate;
	wl_list_for_each(candidate, &server->outputs, link) {
		if (candidate->wlr_output == layer_surface->output) {
			output = candidate;
			break;
		}
	}
	if (output == NULL) {
		wlr_layer_surface_v1_destroy(layer_surface);
		return;
	}

	struct amber_layer_surface *layer = calloc(1, sizeof(*layer));
	layer->server = server;
	layer->output = output;
	layer->layer_surface = layer_surface;

	/* Parent into this output's tree for the requested layer. Layer is
	 * read from pending: clients set it before their first commit. */
	layer->scene_layer = wlr_scene_layer_surface_v1_create(
		output->layer_trees[layer_surface->pending.layer],
		layer_surface);

	layer->map.notify = layer_surface_handle_map;
	wl_signal_add(&layer_surface->surface->events.map, &layer->map);
	layer->unmap.notify = layer_surface_handle_unmap;
	wl_signal_add(&layer_surface->surface->events.unmap, &layer->unmap);
	layer->commit.notify = layer_surface_handle_commit;
	wl_signal_add(&layer_surface->surface->events.commit, &layer->commit);
	layer->destroy.notify = layer_surface_handle_destroy;
	wl_signal_add(&layer_surface->events.destroy, &layer->destroy);

	wl_list_insert(&output->layers[layer_surface->pending.layer],
		&layer->link);
}

static void server_new_output(struct wl_listener *listener, void *data) {
	/* This event is raised by the backend when a new output (aka a display or
	 * monitor) becomes available. */
	struct amber_server *server =
		wl_container_of(listener, server, new_output);
	struct wlr_output *wlr_output = data;

	/* Configures the output created by the backend to use our allocator
	 * and our renderer. Must be done once, before committing the output */
	wlr_output_init_render(wlr_output, server->allocator, server->renderer);

	/* The output may be disabled, switch it on. */
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);

	/* Some backends don't have modes. DRM+KMS does, and we need to set a mode
	 * before we can use the output. The mode is a tuple of (width, height,
	 * refresh rate), and each monitor supports only a specific set of modes. We
	 * just pick the monitor's preferred mode, a more sophisticated compositor
	 * would let the user configure it. */
	struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
	if (mode != NULL) {
		wlr_output_state_set_mode(&state, mode);
	}

	/* Atomically applies the new output state. */
	wlr_output_commit_state(wlr_output, &state);
	wlr_output_state_finish(&state);

	/* Allocates and configures our state for this output */
	struct amber_output *output = calloc(1, sizeof(*output));
	output->wlr_output = wlr_output;
	output->server = server;
	output->active_workspace = 0;
	for (int i = 0; i < 4; i++) {
		wl_list_init(&output->layers[i]);
	}
	for (int i = 0; i < AMBER_WORKSPACE_COUNT; i++) {
		output->workspaces[i].tree = NULL;
		output->workspaces[i].output = output;
		wl_list_init(&output->workspaces[i].toplevels);
	}

	/* Publish the 9 workspaces over ext-workspace-v1 so bars can
	 * render and switch them (caps advertise ACTIVATE only — we do
	 * not support client-driven create/remove). */
	if (server->ext_ws_mgr != NULL) {
		uint32_t ws_caps =
			EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ACTIVATE;
		output->ext_group = wlr_ext_workspace_group_handle_v1_create(
			server->ext_ws_mgr, 0);
		for (int i = 0; i < AMBER_WORKSPACE_COUNT; i++) {
			if (server->dyn_ws && i != output->active_workspace) {
				output->ext_ws[i] = NULL;
				continue;
			}
			char id[16], name[16];
			snprintf(id, sizeof(id), "amber-%d", i);
			snprintf(name, sizeof(name), "%d", i + 1);
			struct wlr_ext_workspace_handle_v1 *h =
				wlr_ext_workspace_handle_v1_create(
					server->ext_ws_mgr, id, ws_caps);
			wlr_ext_workspace_handle_v1_set_name(h, name);
			uint32_t coords[2] = {0, (uint32_t)i};
			wlr_ext_workspace_handle_v1_set_coordinates(h,
				coords, 2);
			wlr_ext_workspace_handle_v1_set_group(h,
				output->ext_group);
			output->ext_ws[i] = h;
		}
		wlr_ext_workspace_group_handle_v1_output_enter(
			output->ext_group, wlr_output);
		ext_workspace_sync_active(output, -1,
			output->active_workspace);
	}

	/* Sets up a listener for the frame event. */
	output->frame.notify = output_frame;
	wl_signal_add(&wlr_output->events.frame, &output->frame);

	/* Sets up a listener for the state request event. */
	output->request_state.notify = output_request_state;
	wl_signal_add(&wlr_output->events.request_state, &output->request_state);

	/* Sets up a listener for the destroy event. */
	output->destroy.notify = output_destroy;
	wl_signal_add(&wlr_output->events.destroy, &output->destroy);

	wl_list_insert(&server->outputs, &output->link);

	/* Adds this to the output layout. The add_auto function arranges outputs
	 * from left-to-right in the order they appear. A more sophisticated
	 * compositor would let the user configure the arrangement of outputs in the
	 * layout.
	 *
	 * The output layout utility automatically adds a wl_output global to the
	 * display, which Wayland clients can see to find out information about the
	 * output (such as DPI, scale factor, manufacturer, etc).
	 */
	struct wlr_output_layout_output *l_output = wlr_output_layout_add_auto(server->output_layout,
		wlr_output);
	struct wlr_scene_output *scene_output = wlr_scene_output_create(server->scene, wlr_output);
	wlr_scene_output_layout_add_output(server->scene_layout, l_output, scene_output);

	/* Per-output subtrees live directly under the scene root and are
	 * kept positioned at the output's layout origin (see
	 * output_update_geometry). Creation order defines stacking:
	 * background < bottom < workspaces < top(bar/notifs) < overlay < fx.
	 * Windows sit BELOW the top layer so bar and notification popups
	 * always render above them (tiles keep off the bar via its
	 * exclusive zone); fullscreen windows move into overlay above all. */
	output->layer_trees[ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND] =
		wlr_scene_tree_create(&server->scene->tree);
	output->layer_trees[ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM] =
		wlr_scene_tree_create(&server->scene->tree);
	for (int i = 0; i < AMBER_WORKSPACE_COUNT; i++) {
		output->workspaces[i].tree =
			wlr_scene_tree_create(&server->scene->tree);
		wlr_scene_node_set_enabled(&output->workspaces[i].tree->node,
			i == output->active_workspace);
	}
	output->layer_trees[ZWLR_LAYER_SHELL_V1_LAYER_TOP] =
		wlr_scene_tree_create(&server->scene->tree);
	output->layer_trees[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY] =
		wlr_scene_tree_create(&server->scene->tree);
	output->fx_tree = wlr_scene_tree_create(&server->scene->tree);

	output_update_geometry(output);

	/* Publish the new monitor to output-management clients (kanshi). */
	output_manager_send_configuration(server);
}

static void output_update_geometry(struct amber_output *output) {
	/* Cache where this output sits in layout coordinates; every
	 * conversion between global cursor coords and window-local node
	 * coords goes through this. Also re-anchor all of this output's
	 * subtree roots at the output origin, since they live directly
	 * under the scene root. */
	wlr_output_layout_get_box(output->server->output_layout,
		output->wlr_output, &output->layout_box);

	struct wlr_scene_node *nodes[14] = {
		&output->layer_trees[ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND]->node,
		&output->layer_trees[ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM]->node,
		&output->layer_trees[ZWLR_LAYER_SHELL_V1_LAYER_TOP]->node,
	};
	for (int i = 0; i < AMBER_WORKSPACE_COUNT; i++) {
		nodes[3 + i] = &output->workspaces[i].tree->node;
	}
	nodes[12] = &output->layer_trees[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY]->node;
	nodes[13] = output->fx_tree != NULL ? &output->fx_tree->node : NULL;
	for (size_t i = 0; i < sizeof(nodes) / sizeof(nodes[0]); i++) {
		if (nodes[i] != NULL) {
			wlr_scene_node_set_position(nodes[i],
				output->layout_box.x, output->layout_box.y);
		}
	}

	arrange_layers(output);
	output_attach_wallpaper(output);
}

/*
 * Foreign toplevel management: bars/docks enumerate windows and can
 * focus or close them (noctalia's taskbar/dock needs this).
 */

static void foreign_handle_activate(struct wl_listener *listener, void *data) {
	struct amber_toplevel *toplevel =
		wl_container_of(listener, toplevel, foreign_activate);
	if (!toplevel_alive(toplevel)) {
		return;
	}
	/* Taskbar click on a window parked on another workspace must
	 * take you there first - focusing a hidden window otherwise. */
	if (!toplevel->floating && toplevel->output != NULL &&
			toplevel->workspace !=
				toplevel->output->active_workspace) {
		workspace_switch(toplevel->output, toplevel->workspace);
	}
	focus_toplevel(toplevel);
}

static void foreign_handle_close(struct wl_listener *listener, void *data) {
	struct amber_toplevel *toplevel =
		wl_container_of(listener, toplevel, foreign_close);
	if (toplevel_alive(toplevel)) {
		wlr_xdg_toplevel_send_close(toplevel->xdg_toplevel);
	}
}

static void toplevel_foreign_init(struct amber_toplevel *toplevel) {
	struct amber_server *server = toplevel->server;
	const char *title = toplevel->xdg_toplevel->title;

	if (server->foreign_toplevel_mgr != NULL) {
		toplevel->foreign_handle =
			wlr_foreign_toplevel_handle_v1_create(
				server->foreign_toplevel_mgr);
		if (toplevel->xdg_toplevel->app_id != NULL) {
			wlr_foreign_toplevel_handle_v1_set_app_id(
				toplevel->foreign_handle,
				toplevel->xdg_toplevel->app_id);
		}
		if (title != NULL) {
			wlr_foreign_toplevel_handle_v1_set_title(
				toplevel->foreign_handle, title);
		}
		toplevel->foreign_activate.notify = foreign_handle_activate;
		wl_signal_add(&toplevel->foreign_handle->events.request_activate,
			&toplevel->foreign_activate);
		toplevel->foreign_close.notify = foreign_handle_close;
		wl_signal_add(&toplevel->foreign_handle->events.request_close,
			&toplevel->foreign_close);
	}

	/* Second enumeration protocol: shells like Noctalia build their
	 * switcher model from this even when zwlr management exists. */
	if (server->ext_toplevel_list != NULL) {
		const struct wlr_ext_foreign_toplevel_handle_v1_state st = {
			.title = title,
			.app_id = toplevel->xdg_toplevel->app_id,
		};
		toplevel->ext_handle = wlr_ext_foreign_toplevel_handle_v1_create(
			server->ext_toplevel_list, &st);
	}

	if (title != NULL) {
		toplevel->foreign_title = strdup(title);
	}
}

static void toplevel_foreign_destroy(struct amber_toplevel *toplevel) {
	if (toplevel->ext_handle != NULL) {
		wlr_ext_foreign_toplevel_handle_v1_destroy(
			toplevel->ext_handle);
		toplevel->ext_handle = NULL;
	}
	if (toplevel->foreign_handle == NULL) {
		return;
	}
	wl_list_remove(&toplevel->foreign_activate.link);
	wl_list_remove(&toplevel->foreign_close.link);
	free(toplevel->foreign_title);
	toplevel->foreign_title = NULL;
	wlr_foreign_toplevel_handle_v1_destroy(toplevel->foreign_handle);
	toplevel->foreign_handle = NULL;
}

/* Push title changes to taskbars, deduplicated: terminals commit every
 * frame and set_title is a broadcast to every bar client. */
static void toplevel_foreign_update_title(struct amber_toplevel *toplevel) {
	if (toplevel->foreign_handle == NULL &&
			toplevel->ext_handle == NULL) {
		return;
	}
	const char *title = toplevel->xdg_toplevel->title;
	if (title == NULL || (toplevel->foreign_title != NULL &&
			strcmp(toplevel->foreign_title, title) == 0)) {
		return;
	}
	if (toplevel->foreign_handle != NULL) {
		wlr_foreign_toplevel_handle_v1_set_title(
			toplevel->foreign_handle, title);
	}
	if (toplevel->ext_handle != NULL) {
		const struct wlr_ext_foreign_toplevel_handle_v1_state st = {
			.title = title,
			.app_id = toplevel->xdg_toplevel->app_id,
		};
		wlr_ext_foreign_toplevel_handle_v1_update_state(
			toplevel->ext_handle, &st);
	}
	free(toplevel->foreign_title);
	toplevel->foreign_title = strdup(title);
}

/*
 * Session lock: lock screens render above everything on their output.
 */

static void session_lock_remove_surfaces(struct amber_server *server) {
	struct amber_lock_surface *surface, *tmp;
	wl_list_for_each_safe(surface, tmp, &server->lock_surfaces, link) {
		wl_list_remove(&surface->destroy.link);
		wl_list_remove(&surface->link);
		wlr_scene_node_destroy(&surface->scene_tree->node);
		free(surface);
	}
}

static void lock_surface_destroy(struct wl_listener *listener, void *data) {
	struct amber_lock_surface *surface =
		wl_container_of(listener, surface, destroy);
	wl_list_remove(&surface->destroy.link);
	wl_list_remove(&surface->link);
	wlr_scene_node_destroy(&surface->scene_tree->node);
	free(surface);
}

static void session_lock_new_surface(struct wl_listener *listener,
		void *data) {
	struct amber_server *server =
		wl_container_of(listener, server, session_lock_new_surface);
	struct wlr_session_lock_surface_v1 *lock_surface = data;

	struct amber_output *output = NULL;
	struct amber_output *candidate;
	wl_list_for_each(candidate, &server->outputs, link) {
		if (candidate->wlr_output == lock_surface->output) {
			output = candidate;
			break;
		}
	}
	if (output == NULL) {
		return;
	}

	wlr_session_lock_surface_v1_configure(lock_surface,
		output->layout_box.width, output->layout_box.height);

	struct amber_lock_surface *surface = calloc(1, sizeof(*surface));
	surface->output = output;
	surface->lock_surface = lock_surface;
	surface->scene_tree = wlr_scene_tree_create(
		output->layer_trees[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY]);
	wlr_scene_subsurface_tree_create(surface->scene_tree,
		lock_surface->surface);

	surface->destroy.notify = lock_surface_destroy;
	wl_signal_add(&lock_surface->events.destroy, &surface->destroy);
	wl_list_insert(&server->lock_surfaces, &surface->link);
}

static void session_lock_unlocked(struct wl_listener *listener, void *data) {
	struct amber_server *server =
		wl_container_of(listener, server, session_lock_unlock);
	session_lock_remove_surfaces(server);
	server->active_lock = NULL;
}

static void session_lock_destroyed(struct wl_listener *listener, void *data) {
	struct amber_server *server =
		wl_container_of(listener, server, session_lock_dead);
	session_lock_remove_surfaces(server);
	server->active_lock = NULL;
}

static void session_lock_new_lock(struct wl_listener *listener, void *data) {
	struct amber_server *server =
		wl_container_of(listener, server, session_lock_new_lock);
	struct wlr_session_lock_v1 *lock = data;

	server->active_lock = lock;
	session_lock_remove_surfaces(server); // stale surfaces, just in case
	server->session_lock_new_surface.notify = session_lock_new_surface;
	wl_signal_add(&lock->events.new_surface,
		&server->session_lock_new_surface);
	server->session_lock_unlock.notify = session_lock_unlocked;
	wl_signal_add(&lock->events.unlock, &server->session_lock_unlock);
	server->session_lock_dead.notify = session_lock_destroyed;
	wl_signal_add(&lock->events.destroy, &server->session_lock_dead);

	wlr_session_lock_v1_send_locked(lock);
}

/* Cache the node that displays the window's main surface buffer (a
 * direct buffer child of the toplevel tree; popups live in their own
 * subtrees and blur/shadow are not buffer nodes). */
static struct wlr_scene_buffer *toplevel_find_buffer(
	struct wlr_scene_tree *tree) {
	struct wlr_scene_node *node;
	wl_list_for_each(node, &tree->children, link) {
		if (node->type == WLR_SCENE_NODE_BUFFER) {
			return wlr_scene_buffer_from_node(node);
		}
		if (node->type == WLR_SCENE_NODE_TREE) {
			struct wlr_scene_buffer *nested =
				toplevel_find_buffer(
					wlr_scene_tree_from_node(node));
			if (nested != NULL) {
				return nested;
			}
		}
	}
	return NULL;
}

static void toplevel_cache_scene_buffer(struct amber_toplevel *toplevel) {
	/* The buffer lives inside the XDG surface's own subtree, not as a
	 * direct child - a shallow scan always returned NULL and silently
	 * killed wobble before its first frame. */
	toplevel->scene_buffer =
		toplevel_find_buffer(toplevel->scene_tree);
}

static void xdg_toplevel_map(struct wl_listener *listener, void *data) {
	/* Called when the surface is mapped, or ready to display on-screen. */
	struct amber_toplevel *toplevel = wl_container_of(listener, toplevel, map);
	struct amber_server *server = toplevel->server;

	/* Windows land on the active workspace of the active output. */
	struct amber_output *output = active_output(server);
	if (output == NULL && !wl_list_empty(&server->outputs)) {
		output = wl_container_of(server->outputs.next, output, link);
	}
	if (output != NULL) {
		toplevel->output = output;
		toplevel->workspace = output->active_workspace;
		struct amber_workspace *workspace =
			&output->workspaces[toplevel->workspace];
		wlr_scene_node_reparent(&toplevel->scene_tree->node,
			workspace->tree);

		/* niri rule: insert right of the focused window; existing
		 * windows never move or resize. */
		struct wl_list *anchor = workspace->toplevels.prev;
		struct amber_toplevel *f = server->focused_toplevel;
		if (f != NULL && f != toplevel && f->output == output &&
				f->workspace == toplevel->workspace) {
			anchor = &f->link;
		}
		wl_list_insert(anchor, &toplevel->link);

		toplevel->floating = false;
		toplevel->col_width = (int)(output->usable_area.width *
			server->default_column_fraction);
	}

	focus_toplevel(toplevel);
	if (toplevel->output != NULL) {
		/* Arrange after focusing: the scroll follows the new focus. */
		workspace_arrange(toplevel_workspace(toplevel));
	}
	toplevel_apply_fx(toplevel);
	toplevel_cache_scene_buffer(toplevel);
	animation_start_open(toplevel);
	toplevel_foreign_init(toplevel);
	ipc_broadcast(server); // occupancy changed
}

static void xdg_toplevel_unmap(struct wl_listener *listener, void *data) {
	/* Called when the surface is unmapped, and should no longer be shown. */
	struct amber_toplevel *toplevel = wl_container_of(listener, toplevel, unmap);

	/* Reset the cursor mode if the grabbed toplevel was unmapped. */
	if (toplevel == toplevel->server->grabbed_toplevel) {
		reset_cursor_mode(toplevel->server);
	}
	/* Capture BEFORE clearing the pointer below — after that this
	 * comparison is always false and vacuum never fires. */
	bool was_focused = toplevel->server->focused_toplevel == toplevel;
	if (toplevel->server->focused_toplevel == toplevel) {
		toplevel->server->focused_toplevel = NULL;
	}
	if (toplevel->fullscreen) {
		toplevel->fullscreen = false;
		if (toplevel->output != NULL) {
			workspace_update_top_layer(toplevel->output);
		}
	}

	struct amber_workspace *ws = toplevel_workspace(toplevel);
	animation_start_lamp_close(toplevel); // snapshot before teardown
	animation_cancel_for(toplevel); // node is about to be destroyed
	toplevel_foreign_destroy(toplevel);
	/* Neighbors captured pre-remove (the removed node's own links go
	 * stale but the list is already rewired). */
	struct wl_list *nb_next = toplevel->link.next;
	struct wl_list *nb_prev = toplevel->link.prev;
	wl_list_remove(&toplevel->link);
	workspace_arrange(ws);
	ipc_broadcast(toplevel->server); // occupancy changed

	if (was_focused && !toplevel->server->shutting_down) {
		/* Closing the focused window must not leave the seat
		 * focusless: first hand focus to the neighbor that took
		 * the slot (right one first, else left). If the workspace
		 * emptied out entirely, hop to the nearest occupied
		 * workspace like niri does instead of staring at an empty
		 * desktop with no focused window. */
		struct wl_list *pick = nb_next != &ws->toplevels
			? nb_next : nb_prev;
		if (pick != &ws->toplevels) {
			struct amber_toplevel *n;
			n = wl_container_of(pick, n, link);
			focus_toplevel(n);
		} else {
			struct amber_output *o = toplevel->output;
			int idx = -1;
			for (int d = 1; idx < 0 &&
					d < AMBER_WORKSPACE_COUNT; d++) {
				int lo = toplevel->workspace - d;
				int hi = toplevel->workspace + d;
				if (lo >= 0 &&
						!wl_list_empty(&o->workspaces[lo].toplevels)) {
					idx = lo;
				} else if (hi < AMBER_WORKSPACE_COUNT &&
						!wl_list_empty(&o->workspaces[hi].toplevels)) {
					idx = hi;
				}
			}
			if (idx >= 0) {
				workspace_switch(o, idx);
			}
		}
	}
}

static void xdg_toplevel_commit(struct wl_listener *listener, void *data) {
	/* Called when a new surface state is committed. */
	struct amber_toplevel *toplevel = wl_container_of(listener, toplevel, commit);

	if (toplevel->xdg_toplevel->base->initial_commit) {
		/* When an xdg_surface performs an initial commit, the compositor must
		 * reply with a configure so the client can map the surface. amber
		 * configures the xdg_toplevel with 0,0 size to let the client pick the
		 * dimensions itself. */
		wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, 0, 0);
		apply_server_decoration_mode(toplevel);
	} else {
		toplevel_foreign_update_title(toplevel);
		/* Window may have resized: keep corners + blur in sync. */
		if (toplevel->scene_tree != NULL &&
				toplevel->xdg_toplevel->base->surface->mapped) {
			toplevel_apply_fx(toplevel);
		}
		/* Push focused-window title updates to IPC watchers (the bar
		 * shows it), but only when it actually changed. */
		if (toplevel == toplevel->server->focused_toplevel) {
			const char *title = toplevel->xdg_toplevel->title;
			if ((title == NULL) !=
					(toplevel->server->last_focused_title
						== NULL) ||
					(title != NULL &&
						strcmp(title,
						toplevel->server->
							last_focused_title) !=
							0)) {
				free(toplevel->server->last_focused_title);
				toplevel->server->last_focused_title =
					title != NULL ? strdup(title) : NULL;
				ipc_broadcast(toplevel->server);
			}
		}
	}
}

static void xdg_toplevel_destroy(struct wl_listener *listener, void *data) {
	/* Called when the xdg_toplevel is destroyed. */
	struct amber_toplevel *toplevel = wl_container_of(listener, toplevel, destroy);

	wl_list_remove(&toplevel->map.link);
	wl_list_remove(&toplevel->unmap.link);
	wl_list_remove(&toplevel->commit.link);
	wl_list_remove(&toplevel->destroy.link);
	wl_list_remove(&toplevel->request_move.link);
	wl_list_remove(&toplevel->request_resize.link);
	wl_list_remove(&toplevel->request_maximize.link);
	wl_list_remove(&toplevel->request_fullscreen.link);

	/* Destroy the container tree (and any popup subtrees with it);
	 * tinywl leaked this per window open/close cycle. */
	wlr_scene_node_destroy(&toplevel->scene_tree->node);
	/* Invalidate the xdg_surface -> scene_tree backref NOW: the node
	 * (and its node.data amber_toplevel) is freed above, and late
	 * xdg-activation requests must not walk into freed memory. */
	toplevel->xdg_toplevel->base->data = NULL;

	free(toplevel);
}

static void begin_interactive(struct amber_toplevel *toplevel,
		enum amber_cursor_mode mode, uint32_t edges) {
	/* This function sets up an interactive move or resize operation, where the
	 * compositor stops propagating pointer events to clients and instead
	 * consumes them itself, to move or resize windows. */
	struct amber_server *server = toplevel->server;

	server->grabbed_toplevel = toplevel;
	server->cursor_mode = mode;

	if (mode == AMBER_CURSOR_MOVE) {
		animation_start_wobble(toplevel);
		server->grab_x = server->cursor->x -
			toplevel->output->layout_box.x - toplevel->scene_tree->node.x;
		server->grab_y = server->cursor->y -
			toplevel->output->layout_box.y - toplevel->scene_tree->node.y;
	} else {
		struct wlr_box *geo_box = &toplevel->xdg_toplevel->base->geometry;

		double border_x = (toplevel->scene_tree->node.x + geo_box->x) +
			((edges & WLR_EDGE_RIGHT) ? geo_box->width : 0);
		double border_y = (toplevel->scene_tree->node.y + geo_box->y) +
			((edges & WLR_EDGE_BOTTOM) ? geo_box->height : 0);
		server->grab_x = server->cursor->x - border_x;
		server->grab_y = server->cursor->y - border_y;

		server->grab_geobox = *geo_box;
		server->grab_geobox.x += toplevel->scene_tree->node.x;
		server->grab_geobox.y += toplevel->scene_tree->node.y;

		server->resize_edges = edges;
	}
}

static void xdg_toplevel_request_move(
		struct wl_listener *listener, void *data) {
	/* This event is raised when a client would like to begin an interactive
	 * move, typically because the user clicked on their client-side
	 * decorations. Note that a more sophisticated compositor should check the
	 * provided serial against a list of button press serials sent to this
	 * client, to prevent the client from requesting this whenever they want. */
	struct amber_toplevel *toplevel = wl_container_of(listener, toplevel, request_move);
	/* Tiles are moved by SUPER+drag (column reorder); honoring CSD
	 * move requests here would yank tiles around on title-bar grabs. */
	if (!toplevel->floating) {
		return;
	}
	begin_interactive(toplevel, AMBER_CURSOR_MOVE, 0);
}

static void xdg_toplevel_request_resize(
		struct wl_listener *listener, void *data) {
	/* This event is raised when a client would like to begin an interactive
	 * resize, typically because the user clicked on their client-side
	 * decorations. Note that a more sophisticated compositor should check the
	 * provided serial against a list of button press serials sent to this
	 * client, to prevent the client from requesting this whenever they want. */
	struct wlr_xdg_toplevel_resize_event *event = data;
	struct amber_toplevel *toplevel = wl_container_of(listener, toplevel, request_resize);
	begin_interactive(toplevel, AMBER_CURSOR_RESIZE, event->edges);
}

static void xdg_toplevel_request_maximize(
		struct wl_listener *listener, void *data) {
	/* This event is raised when a client would like to maximize itself,
	 * typically because the user clicked on the maximize button on client-side
	 * decorations. amber doesn't support maximization, but to conform to
	 * xdg-shell protocol we still must send a configure.
	 * wlr_xdg_surface_schedule_configure() is used to send an empty reply.
	 * However, if the request was sent before an initial commit, we don't do
	 * anything and let the client finish the initial surface setup. */
	struct amber_toplevel *toplevel =
		wl_container_of(listener, toplevel, request_maximize);
	if (toplevel->xdg_toplevel->base->initialized) {
		wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
	}
}

static void xdg_toplevel_request_fullscreen(
		struct wl_listener *listener, void *data) {
	/* Honor what the client asked for instead of ignoring it: a
	 * fullscreen tile is just a normal tile with bigger width. */
	struct amber_toplevel *toplevel =
		wl_container_of(listener, toplevel, request_fullscreen);
	if (!toplevel->xdg_toplevel->base->initialized) {
		return;
	}
	toplevel_set_fullscreen(toplevel,
		toplevel->xdg_toplevel->requested.fullscreen);
}

static void server_new_xdg_toplevel(struct wl_listener *listener, void *data) {
	/* This event is raised when a client creates a new toplevel (application window). */
	struct amber_server *server = wl_container_of(listener, server, new_xdg_toplevel);
	struct wlr_xdg_toplevel *xdg_toplevel = data;

	/* Allocate a amber_toplevel for this surface */
	struct amber_toplevel *toplevel = calloc(1, sizeof(*toplevel));
	toplevel->server = server;
	toplevel->xdg_toplevel = xdg_toplevel;
	toplevel->sent_w = -1; // force the first real configure through
	toplevel->sent_h = -1;
	toplevel->fx_opacity = -1; // follow rules (no manual override)
	toplevel->ipc_id = ++server->ipc_next_id;
	toplevel->scene_tree =
		wlr_scene_xdg_surface_create(&toplevel->server->scene->tree, xdg_toplevel->base);
	toplevel->scene_tree->node.data = toplevel;
	xdg_toplevel->base->data = toplevel->scene_tree;

	/* Listen to the various events it can emit */
	toplevel->map.notify = xdg_toplevel_map;
	wl_signal_add(&xdg_toplevel->base->surface->events.map, &toplevel->map);
	toplevel->unmap.notify = xdg_toplevel_unmap;
	wl_signal_add(&xdg_toplevel->base->surface->events.unmap, &toplevel->unmap);
	toplevel->commit.notify = xdg_toplevel_commit;
	wl_signal_add(&xdg_toplevel->base->surface->events.commit, &toplevel->commit);

	toplevel->destroy.notify = xdg_toplevel_destroy;
	wl_signal_add(&xdg_toplevel->events.destroy, &toplevel->destroy);

	/* cotd */
	toplevel->request_move.notify = xdg_toplevel_request_move;
	wl_signal_add(&xdg_toplevel->events.request_move, &toplevel->request_move);
	toplevel->request_resize.notify = xdg_toplevel_request_resize;
	wl_signal_add(&xdg_toplevel->events.request_resize, &toplevel->request_resize);
	toplevel->request_maximize.notify = xdg_toplevel_request_maximize;
	wl_signal_add(&xdg_toplevel->events.request_maximize, &toplevel->request_maximize);
	toplevel->request_fullscreen.notify = xdg_toplevel_request_fullscreen;
	wl_signal_add(&xdg_toplevel->events.request_fullscreen, &toplevel->request_fullscreen);
}

static void xdg_popup_commit(struct wl_listener *listener, void *data) {
	/* Called when a new surface state is committed. */
	struct amber_popup *popup = wl_container_of(listener, popup, commit);

	if (popup->xdg_popup->base->initial_commit) {
		/* When an xdg_surface performs an initial commit, the compositor must
		 * reply with a configure so the client can map the surface.
		 * amber sends an empty configure. A more sophisticated compositor
		 * might change an xdg_popup's geometry to ensure it's not positioned
		 * off-screen, for example. */
		wlr_xdg_surface_schedule_configure(popup->xdg_popup->base);
	}
}

static void xdg_popup_destroy(struct wl_listener *listener, void *data) {
	/* Called when the xdg_popup is destroyed. */
	struct amber_popup *popup = wl_container_of(listener, popup, destroy);

	wl_list_remove(&popup->commit.link);
	wl_list_remove(&popup->destroy.link);

	free(popup);
}

/* Find the tracked layer surface's output so popups anchored through
 * zwlr_layer_shell_v1.get_popup can render in that output's overlay
 * tree (above windows); NULL when untracked. */
static struct amber_output *amber_layer_output(
		struct amber_server *server, struct wlr_layer_surface_v1 *ls) {
	struct amber_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		struct amber_layer_surface *layer;
		wl_list_for_each(layer, &output->layers[ls->current.layer], link) {
			if (layer->layer_surface == ls) {
				return output;
			}
		}
	}
	return NULL;
}

static void server_new_xdg_popup(struct wl_listener *listener, void *data) {
	/* This event is raised when a client creates a new popup. */
	struct wlr_xdg_popup *xdg_popup = data;
	struct amber_server *server =
		wl_container_of(listener, server, new_xdg_popup);

	struct amber_popup *popup = calloc(1, sizeof(*popup));
	popup->xdg_popup = xdg_popup;

	/* We must add xdg popups to the scene graph so they get rendered. The
	 * wlroots scene graph provides a helper for this, but to use it we must
	 * provide the proper parent scene node of the xdg popup. To enable this,
	 * we always set the user data field of xdg_surfaces to the corresponding
	 * scene node.
	 *
	 * Popups parented through zwlr_layer_shell_v1.get_popup (panel tooltips
	 * and menus) have no xdg parent; on wlroots 0.20 their ->parent is even
	 * NULL. try_from(NULL) segfaults the whole session (seen with noctalia),
	 * so resolve an anchor defensively and only dismiss the popup when no
	 * sane placement target exists. */
	struct wlr_xdg_surface *parent = xdg_popup->parent ?
		wlr_xdg_surface_try_from_wlr_surface(xdg_popup->parent) : NULL;

	struct wlr_scene_tree *parent_tree = NULL;
	if (parent != NULL) {
		parent_tree = parent->data;
	} else {
		/* Layer popups float ABOVE windows: park them in the
		 * overlay tree of the popup's own output when tracked,
		 * else the active one. Trees share the output origin, so
		 * panel-relative configure coords land unchanged. */
		struct wlr_layer_surface_v1 *ls = xdg_popup->parent ?
			wlr_layer_surface_v1_try_from_wlr_surface(
				xdg_popup->parent) : NULL;
		struct amber_output *output = ls != NULL ?
			amber_layer_output(server, ls) : active_output(server);
		if (output != NULL) {
			parent_tree =
				output->layer_trees[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY];
		}
	}
	if (parent_tree == NULL) {
		wlr_xdg_popup_destroy(xdg_popup);
		free(popup);
		return;
	}
	xdg_popup->base->data = wlr_scene_xdg_surface_create(parent_tree, xdg_popup->base);

	popup->commit.notify = xdg_popup_commit;
	wl_signal_add(&xdg_popup->base->surface->events.commit, &popup->commit);

	popup->destroy.notify = xdg_popup_destroy;
	wl_signal_add(&xdg_popup->events.destroy, &popup->destroy);
}

static void apply_server_decoration_mode(struct amber_toplevel *toplevel);

static void server_new_toplevel_decoration(struct wl_listener *listener,
		void *data) {
	/* Every client that asks about decorations is told "the compositor
	 * decorates" — and we then draw nothing. Result: borderless windows
	 * with no CSD titlebars wherever the toolkit cooperates.
	 *
	 * Clients may create the decoration object before their first
	 * commit, where sending a configure would trip wlroots' assertion;
	 * defer those to the initial-commit hook. */
	struct wlr_xdg_toplevel_decoration_v1 *decoration = data;
	if (!decoration->toplevel->base->initialized) {
		return;
	}
	wlr_xdg_toplevel_decoration_v1_set_mode(decoration,
		WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

int main(int argc, char *argv[]) {
	/* Debug logging in hot paths is measurable overhead; default to
	 * errors only, opt back in with AMBERWM_DEBUG=1. */
	wlr_log_init(getenv("AMBERWM_DEBUG") != NULL ? WLR_DEBUG : WLR_INFO,
		NULL);

	/* Prefer server-side decoration in toolkits that only honor env
	 * (GTK apps spawned by us inherit this). */
	setenv("GTK_CSD", "0", true);

	char *startup_cmd = NULL;

	int c;
	while ((c = getopt(argc, argv, "s:h")) != -1) {
		switch (c) {
		case 's':
			startup_cmd = optarg;
			break;
		default:
			printf("Usage: %s [-s startup command]\n", argv[0]);
			return 0;
		}
	}
	if (optind < argc) {
		printf("Usage: %s [-s startup command]\n", argv[0]);
		return 0;
	}

	struct amber_server server = {0};
	/* The Wayland display is managed by libwayland. It handles accepting
	 * clients from the Unix socket, managing Wayland globals, and so on. */
	server.wl_display = wl_display_create();
	/* The backend is a wlroots feature which abstracts the underlying input and
	 * output hardware. The autocreate option will choose the most suitable
	 * backend based on the current environment, such as opening an X11 window
	 * if an X11 server is running. */
	server.backend = wlr_backend_autocreate(wl_display_get_event_loop(server.wl_display), &server.session);
	if (server.backend == NULL) {
		wlr_log(WLR_ERROR, "failed to create wlr_backend");
		return 1;
	}

	/* Autocreates a renderer, either Pixman, GLES2 or Vulkan for us. The user
	 * can also specify a renderer using the WLR_RENDERER env var.
	 * The renderer is responsible for defining the various pixel formats it
	 * supports for shared memory, this configures that for clients. */
	/* SceneFX replaces the GLES2 renderer with its fx_renderer; using
	 * the stock renderer here trips scenefx's render-pass assertions. */
	server.renderer = fx_renderer_create(server.backend);
	if (server.renderer == NULL) {
		wlr_log(WLR_ERROR, "failed to create wlr_renderer");
		return 1;
	}
	/* crocus/i965 (HD 3000) advertises EGL_ANDROID_native_fence_sync but
	 * eglDupNativeFenceFDANDROID then fails on every call, leaking one
	 * fd per frame until "Too many open files" wedges the compositor.
	 * Disabling the timeline feature keeps wlroots on implicit sync,
	 * which is what these drivers actually implement. */
	server.renderer->features.timeline = false;

	wlr_renderer_init_wl_display(server.renderer, server.wl_display);

	/* Autocreates an allocator for us.
	 * The allocator is the bridge between the renderer and the backend. It
	 * handles the buffer creation, allowing wlroots to render onto the
	 * screen */
	server.allocator = wlr_allocator_autocreate(server.backend,
		server.renderer);
	if (server.allocator == NULL) {
		wlr_log(WLR_ERROR, "failed to create wlr_allocator");
		return 1;
	}

	/* This creates some hands-off wlroots interfaces. The compositor is
	 * necessary for clients to allocate surfaces, the subcompositor allows to
	 * assign the role of subsurfaces to surfaces and the data device manager
	 * handles the clipboard. Each of these wlroots interfaces has room for you
	 * to dig your fingers in and play with their behavior if you want. Note that
	 * the clients cannot set the selection directly without compositor approval,
	 * see the handling of the request_set_selection event below.*/
	wlr_compositor_create(server.wl_display, 5, server.renderer);
	wlr_subcompositor_create(server.wl_display);
	wlr_data_device_manager_create(server.wl_display);

	/* Creates an output layout, which a wlroots utility for working with an
	 * arrangement of screens in a physical layout. */
	server.output_layout = wlr_output_layout_create(server.wl_display);

	/* Configure a listener to be notified when new outputs are available on the
	 * backend. */
	wl_list_init(&server.outputs);
	server.new_output.notify = server_new_output;
	wl_signal_add(&server.backend->events.new_output, &server.new_output);

	/* Create a scene graph. This is a wlroots abstraction that handles all
	 * rendering and damage tracking. All the compositor author needs to do
	 * is add things that should be rendered to the scene graph at the proper
	 * positions and then call wlr_scene_output_commit() to render a frame if
	 * necessary.
	 */
	server.scene = wlr_scene_create();
	server.scene_layout = wlr_scene_attach_output_layout(server.scene, server.output_layout);

	/* Set up xdg-shell version 3. The xdg-shell is a Wayland protocol which is
	 * used for application windows. For more detail on shells, refer to
	 * https://drewdevault.com/2018/07/29/Wayland-shells.html.
	 */
	server.xdg_shell = wlr_xdg_shell_create(server.wl_display, 3);
	server.new_xdg_toplevel.notify = server_new_xdg_toplevel;
	wl_signal_add(&server.xdg_shell->events.new_toplevel, &server.new_xdg_toplevel);
	server.new_xdg_popup.notify = server_new_xdg_popup;
	wl_signal_add(&server.xdg_shell->events.new_popup, &server.new_xdg_popup);

	/* Layer shell: bars, launchers, wallpapers (wlr-layer-shell). */
	server.layer_shell = wlr_layer_shell_v1_create(server.wl_display, 4);
	server.new_layer_surface.notify = server_new_layer_surface;
	wl_signal_add(&server.layer_shell->events.new_surface,
		&server.new_layer_surface);

	/* ext-background-effect-v1: lets client panels (e.g. dms) blur the
	 * backdrop behind a surface. Hand-rolled (see structs above). */
	extbe_manager_init(&server);

	/* Decoration negotiation: answer both protocols with "server-side",
	 * then decorate with nothing (no titlebars, no CSD). */
	struct wlr_server_decoration_manager *legacy_decor =
		wlr_server_decoration_manager_create(server.wl_display);
	wlr_server_decoration_manager_set_default_mode(legacy_decor,
		WLR_SERVER_DECORATION_MANAGER_MODE_SERVER);
	server.xdg_decoration_mgr = wlr_xdg_decoration_manager_v1_create(
		server.wl_display);
	server.new_toplevel_decoration.notify = server_new_toplevel_decoration;
	wl_signal_add(
		&server.xdg_decoration_mgr->events.new_toplevel_decoration,
		&server.new_toplevel_decoration);

	/* Cheap protocol globals clients expect from a modern compositor:
	 * primary selection (middle-click paste), viewporter, gamma,
	 * screencopy + export-dmabuf (screenshots/recording), fractional
	 * scaling advertisement, and xdg-output (bars read output info). */
	wlr_primary_selection_v1_device_manager_create(server.wl_display);
	wlr_viewporter_create(server.wl_display);
	wlr_gamma_control_manager_v1_create(server.wl_display);
	wlr_screencopy_manager_v1_create(server.wl_display);
	wlr_export_dmabuf_manager_v1_create(server.wl_display);
	wlr_fractional_scale_manager_v1_create(server.wl_display, 1);
	wlr_xdg_output_manager_v1_create(server.wl_display,
		server.output_layout);

	/* Output management (kanshi) and power (wlopm): runtime monitor
	 * reconfiguration and power switching. */
	server.output_manager = wlr_output_manager_v1_create(server.wl_display);
	server.output_manager_apply.notify = output_manager_apply;
	wl_signal_add(&server.output_manager->events.apply,
		&server.output_manager_apply);
	server.output_manager_test.notify = output_manager_test;
	wl_signal_add(&server.output_manager->events.test,
		&server.output_manager_test);
	server.output_power_mgr =
		wlr_output_power_manager_v1_create(server.wl_display);
	server.output_power_set_mode.notify = output_power_set_mode;
	wl_signal_add(&server.output_power_mgr->events.set_mode,
		&server.output_power_set_mode);

	/* Virtual input devices (kanata/kmonad/ydotool remappers) and
	 * relative pointer motion (games, 3D viewports). */
	server.relative_pointer_mgr =
		wlr_relative_pointer_manager_v1_create(server.wl_display);
	struct wlr_virtual_keyboard_manager_v1 *virtual_kbd_mgr =
		wlr_virtual_keyboard_manager_v1_create(server.wl_display);
	server.virtual_keyboard_new.notify = server_new_virtual_keyboard;
	wl_signal_add(&virtual_kbd_mgr->events.new_virtual_keyboard,
		&server.virtual_keyboard_new);
	struct wlr_virtual_pointer_manager_v1 *virtual_ptr_mgr =
		wlr_virtual_pointer_manager_v1_create(server.wl_display);
	server.virtual_pointer_new.notify = server_new_virtual_pointer;
	wl_signal_add(&virtual_ptr_mgr->events.new_virtual_pointer,
		&server.virtual_pointer_new);

	struct wlr_cursor_shape_manager_v1 *shape_mgr =
		wlr_cursor_shape_manager_v1_create(server.wl_display, 1);
	server.cursor_shape_request.notify = handle_cursor_shape_request;
	wl_signal_add(&shape_mgr->events.request_set_shape,
		&server.cursor_shape_request);

	struct wlr_xdg_activation_v1 *activation =
		wlr_xdg_activation_v1_create(server.wl_display);
	server.xdg_activation_request.notify = handle_xdg_activation_request;
	wl_signal_add(&activation->events.request_activate,
		&server.xdg_activation_request);

	/* Idle reporting (idle behaviors in bars) + inhibition (video
	 * players keep the screen awake). */
	wlr_idle_notifier_v1_create(server.wl_display);
	server.idle_inhibit_mgr = wlr_idle_inhibit_v1_create(server.wl_display);
	server.idle_inhibit_new.notify = idle_inhibit_new;
	wl_signal_add(&server.idle_inhibit_mgr->events.new_inhibitor,
		&server.idle_inhibit_new);

	/* Pointer constraints (games): pointer lock + confine. Enforcement
	 * lives in the constraint_* helpers on the cursor motion path. */
	server.pointer_constraints_mgr =
		wlr_pointer_constraints_v1_create(server.wl_display);
	server.pointer_constraints_new.notify = handle_pointer_constraint_new;
	wl_signal_add(&server.pointer_constraints_mgr->events.new_constraint,
		&server.pointer_constraints_new);

	/* Taskbar/dock window listing and control. */
	server.foreign_toplevel_mgr = wlr_foreign_toplevel_manager_v1_create(
		server.wl_display);
	wlr_xdg_toplevel_icon_manager_v1_create(server.wl_display, 1);

	/* Session lock: lock screens render above all outputs. */
	wl_list_init(&server.lock_surfaces);
	struct wlr_session_lock_manager_v1 *lock_mgr =
		wlr_session_lock_manager_v1_create(server.wl_display);
	server.session_lock_new_lock.notify = session_lock_new_lock;
	wl_signal_add(&lock_mgr->events.new_lock,
		&server.session_lock_new_lock);

	/* ext-workspace-v1 for bars (Noctalia pills + click-to-switch). */
	server.ext_ws_mgr = wlr_ext_workspace_manager_v1_create(
		server.wl_display, 1);
	if (server.ext_ws_mgr != NULL) {
		server.ext_ws_commit.notify = server_ext_ws_commit;
		wl_signal_add(&server.ext_ws_mgr->events.commit,
			&server.ext_ws_commit);
	}

	server.ext_toplevel_list = wlr_ext_foreign_toplevel_list_v1_create(
		server.wl_display, 1);

	/* Screen capture for portals and modern tools (ext-image-copy-capture
	 * with its output-source manager); classic screencopy/export-dmabuf
	 * are created in the cheap-globals block above. */
	wlr_ext_output_image_capture_source_manager_v1_create(
		server.wl_display, 1);
	wlr_ext_image_copy_capture_manager_v1_create(server.wl_display, 1);

	/* Clipboard access for unfocused clients: clipboard managers
	 * (noctalia history), wl-copy/wl-paste, cliphist. Without these
	 * the seat selection is unreachable outside keyboard focus. */
	wlr_data_control_manager_v1_create(server.wl_display);
	wlr_ext_data_control_manager_v1_create(server.wl_display, 1);
	wlr_primary_selection_v1_device_manager_create(server.wl_display);

	/* One xkb context shared by every keyboard that ever connects. */
	server.xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);

	/* Reap spawned children automatically (no zombies). */
	signal(SIGCHLD, SIG_IGN);

	/* User config: ~/.config/amberwm/amberwm.cfg (or $AMBER_CONFIG). */
	server.terminal_cmd = NULL;
	config_load(&server);

	/* Clients inherit the cursor theme via the environment. */
	if (server.cursor_theme != NULL) {
		setenv("XCURSOR_THEME", server.cursor_theme, 1);
	}
	char csizebuf[16];
	snprintf(csizebuf, sizeof(csizebuf), "%d", server.cursor_size);
	setenv("XCURSOR_SIZE", csizebuf, 1);
	wlr_scene_set_blur_data(server.scene, 3, server.blur_radius,
		0.02f, 0.90f, 0.90f, 1.10f);
	if (server.terminal_cmd == NULL) {
		const char *env = getenv("AMBER_TERMINAL");
		if (env == NULL) {
			env = getenv("TERMINAL");
		}
		server.terminal_cmd = strdup(env != NULL ? env : "foot");
	}

	/* Decode the wallpaper before the backend starts; the scene graph
	 * uploads it to a texture lazily on first render. */
	if (server.wallpaper_path != NULL) {
		server.wallpaper_buffer = wallpaper_load(server.wallpaper_path);
	}

	/*
	 * Creates a cursor, which is a wlroots utility for tracking the cursor
	 * image shown on screen.
	 */
	server.cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(server.cursor, server.output_layout);

	/* Creates an xcursor manager, another wlroots utility which loads up
	 * Xcursor themes to source cursor images from and makes sure that cursor
	 * images are available at all scale factors on the screen (necessary for
	 * HiDPI support). */
	server.cursor_mgr = wlr_xcursor_manager_create(
		server.cursor_theme, server.cursor_size);

	/*
	 * wlr_cursor *only* displays an image on screen. It does not move around
	 * when the pointer moves. However, we can attach input devices to it, and
	 * it will generate aggregate events for all of them. In these events, we
	 * can choose how we want to process them, forwarding them to clients and
	 * moving the cursor around. More detail on this process is described in
	 * https://drewdevault.com/2018/07/17/Input-handling-in-wlroots.html.
	 *
	 * And more comments are sprinkled throughout the notify functions above.
	 */
	server.cursor_mode = AMBER_CURSOR_PASSTHROUGH;
	server.cursor_motion.notify = server_cursor_motion;
	wl_signal_add(&server.cursor->events.motion, &server.cursor_motion);
	server.cursor_motion_absolute.notify = server_cursor_motion_absolute;
	wl_signal_add(&server.cursor->events.motion_absolute,
			&server.cursor_motion_absolute);
	server.cursor_button.notify = server_cursor_button;
	wl_signal_add(&server.cursor->events.button, &server.cursor_button);
	server.cursor_axis.notify = server_cursor_axis;
	wl_signal_add(&server.cursor->events.axis, &server.cursor_axis);
	server.cursor_frame.notify = server_cursor_frame;
	wl_signal_add(&server.cursor->events.frame, &server.cursor_frame);

	/*
	 * Configures a seat, which is a single "seat" at which a user sits and
	 * operates the computer. This conceptually includes up to one keyboard,
	 * pointer, touch, and drawing tablet device. We also rig up a listener to
	 * let us know when new input devices are available on the backend.
	 */
	wl_list_init(&server.keyboards);
	server.new_input.notify = server_new_input;
	wl_signal_add(&server.backend->events.new_input, &server.new_input);
	server.seat = wlr_seat_create(server.wl_display, "seat0");
	server.request_cursor.notify = seat_request_cursor;
	wl_signal_add(&server.seat->events.request_set_cursor,
			&server.request_cursor);
	server.pointer_focus_change.notify = seat_pointer_focus_change;
	wl_signal_add(&server.seat->pointer_state.events.focus_change,
			&server.pointer_focus_change);
	server.request_set_selection.notify = seat_request_set_selection;
	wl_signal_add(&server.seat->events.request_set_selection,
			&server.request_set_selection);

	/* Add a Unix socket to the Wayland display. */
	const char *socket = wl_display_add_socket_auto(server.wl_display);
	if (!socket) {
		wlr_backend_destroy(server.backend);
		return 1;
	}

	/* Start the backend. This will enumerate outputs and inputs, become the DRM
	 * master, etc */
	if (!wlr_backend_start(server.backend)) {
		wlr_backend_destroy(server.backend);
		/* Do NOT wl_display_destroy() here: if the backend partially
		 * registered globals before failing, wlroots 0.20 asserts inside
		 * wl_display_destroy() and masks the real error. Just log and
		 * pull the plug so the failed backend's reason hits the console. */
		wlr_log(WLR_ERROR, "failed to start backend; aborting");
		exit(1);
	}

	/* Tell output-management clients about the initial monitor layout. */
	output_manager_send_configuration(&server);

	/* Set the WAYLAND_DISPLAY environment variable to our socket and run
	 * autostart commands (in config order), then the -s command. */
	setenv("WAYLAND_DISPLAY", socket, true);
	ipc_init(&server);
	config_watch_init(&server);

	/* Animation driver: one shared timer, armed only while animating. */
	wl_list_init(&server.animations);
	struct wl_event_loop *anim_loop =
		wl_display_get_event_loop(server.wl_display);
	server.anim_source =
		wl_event_loop_add_timer(anim_loop, animation_tick, &server);

	/* Idle refresh throttle: periodic check (once a second) that drops
	 * output refresh while the desktop sits idle with no media. */
	server.last_input_usec = anim_now_usec();
	server.idle_source =
		wl_event_loop_add_timer(anim_loop, idle_throttle_tick, &server);
	if (server.idle_source != NULL) {
		wl_event_source_timer_update(server.idle_source, IDLE_TICK_MS);
	}

	/* Reload config on SIGHUP (wlroots event loop makes this safe). */
	wl_event_loop_add_signal(anim_loop, SIGHUP,
		ipc_hup_reload, &server);
	wl_event_loop_add_signal(anim_loop, SIGTERM,
		ipc_term_quit, &server);
	wl_event_loop_add_signal(anim_loop, SIGINT,
		ipc_term_quit, &server);

	for (size_t i = 0; i < server.autostart_count; i++) {
		spawn(server.autostart[i]);
	}
	if (startup_cmd) {
		spawn(startup_cmd);
	}
	/* Run the Wayland event loop. This does not return until you exit the
	 * compositor. Starting the backend rigged up all of the necessary event
	 * loop configuration to listen to libinput events, DRM events, generate
	 * frame events at the refresh rate, and so on. */
	wlr_log(WLR_INFO, "Running Wayland compositor on WAYLAND_DISPLAY=%s",
			socket);
	wl_display_run(server.wl_display);


	/* Once wl_display_run returns, we destroy all clients then shut down the
	 * server. */
	wl_display_destroy_clients(server.wl_display);

	if (server.ipc_path != NULL) {
		unlink(server.ipc_path);
		free(server.ipc_path);
	}

	wl_list_remove(&server.new_xdg_toplevel.link);
	wl_list_remove(&server.new_xdg_popup.link);
	wl_list_remove(&server.new_layer_surface.link);
	wl_list_remove(&server.new_toplevel_decoration.link);
	wl_list_remove(&server.cursor_shape_request.link);
	wl_list_remove(&server.xdg_activation_request.link);
	wl_list_remove(&server.session_lock_new_lock.link);
	wl_list_remove(&server.virtual_keyboard_new.link);
	wl_list_remove(&server.virtual_pointer_new.link);
	wl_list_remove(&server.idle_inhibit_new.link);

	wl_list_remove(&server.cursor_motion.link);
	wl_list_remove(&server.cursor_motion_absolute.link);
	wl_list_remove(&server.cursor_button.link);
	wl_list_remove(&server.cursor_axis.link);
	wl_list_remove(&server.cursor_frame.link);

	wl_list_remove(&server.new_input.link);
	wl_list_remove(&server.request_cursor.link);
	wl_list_remove(&server.pointer_focus_change.link);
	wl_list_remove(&server.pointer_constraints_new.link);
	wl_list_remove(&server.request_set_selection.link);

	wl_list_remove(&server.new_output.link);

	/* Animations hold scene nodes; drain them before the scene goes
	 * away so nothing can touch freed nodes during teardown. */
	struct amber_animation *anim, *anim_tmp;
	wl_list_for_each_safe(anim, anim_tmp, &server.animations, link) {
		animation_destroy(&server, anim, false);
	}

	wlr_scene_node_destroy(&server.scene->tree.node);
	wlr_xcursor_manager_destroy(server.cursor_mgr);
	wlr_cursor_destroy(server.cursor);
	wlr_allocator_destroy(server.allocator);
	wlr_renderer_destroy(server.renderer);
	wlr_backend_destroy(server.backend);
	xkb_context_unref(server.xkb_context);
	for (size_t i = 0; i < server.binding_count; i++) {
		free(server.bindings[i].cmd);
	}
	free(server.bindings);
	for (size_t i = 0; i < server.autostart_count; i++) {
		free(server.autostart[i]);
	}
	free(server.autostart);
	free(server.terminal_cmd);
	free(server.wallpaper_path);
	if (server.wallpaper_buffer != NULL) {
		wlr_buffer_drop(server.wallpaper_buffer);
	}
	/* Outputs may still be alive here (backend not explicitly torn
	 * down); ext-workspace handles must die before the display. */
	if (server.ext_ws_mgr != NULL) {
		/* wlroots asserts the commit listener is gone at teardown. */
		wl_list_remove(&server.ext_ws_commit.link);
	}
	/* Output-management listens on the manager; without removing them,
	 * wlroots 0.20 asserts in manager_handle_display_destroy. */
	if (server.output_manager != NULL) {
		wl_list_remove(&server.output_manager_apply.link);
		wl_list_remove(&server.output_manager_test.link);
	}
	/* Same for the output-power manager: its set_mode listener must be
	 * unregistered before wl_display_destroy or wlroots asserts in
	 * output_power_management_v1 handle_display_destroy. */
	if (server.output_power_mgr != NULL) {
		wl_list_remove(&server.output_power_set_mode.link);
	}
	struct amber_output *out, *out_tmp;
	wl_list_for_each_safe(out, out_tmp, &server.outputs, link) {
		ext_workspace_release_output(out);
	}
	wl_display_destroy(server.wl_display);
	return 0;
}
