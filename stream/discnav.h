/*
 * Minimal Blu-ray/disc navigation contract shared between the bluray stream
 * (producer) and the player (consumer). This is intentionally small: it carries
 * only the read-only menu state a client needs plus the user input commands the
 * stream understands.
 *
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef MP_STREAM_DISCNAV_H
#define MP_STREAM_DISCNAV_H

#include <stdbool.h>
#include <stdint.h>

// Read-only navigation snapshot: stream -> player/client.
// Retrieved via STREAM_CTRL_GET_NAV_STATE.
struct mp_nav_state_info {
    bool menu_active;        // an HDMV interactive menu is on screen
    bool popup_available;    // a popup menu can be toggled (BD_EVENT_POPUP)
    bool mouse_over_button;  // last mouse position was over a menu button
    bool overlay_visible;    // a menu overlay is currently displayed
    int still_seconds;       // 0: none, -1: infinite still, >0: timed still
    uint32_t uo_mask;        // BLURAY_UO_* mask of prohibited operations
    int overlay_w, overlay_h; // authored overlay plane resolution (for scaling)
    int overlay_change_id;   // bumped whenever the overlay bitmaps change
};

// User input actions: player/client -> stream.
// Delivered via STREAM_CTRL_NAV_CMD.
enum mp_nav_action {
    MP_NAV_ACTION_NONE = 0,
    MP_NAV_ACTION_UP,
    MP_NAV_ACTION_DOWN,
    MP_NAV_ACTION_LEFT,
    MP_NAV_ACTION_RIGHT,
    MP_NAV_ACTION_SELECT,      // activate the selected button
    MP_NAV_ACTION_MENU,        // open the disc top/root menu
    MP_NAV_ACTION_POPUP,       // toggle the popup menu
    MP_NAV_ACTION_MOUSE_MOVE,  // move the pointer (x, y in authored coords)
    MP_NAV_ACTION_MOUSE_CLICK, // click at (x, y in authored coords)
    MP_NAV_ACTION_RESUME,      // leave the menu / resume playback if possible
};

struct mp_nav_cmd {
    enum mp_nav_action action;
    int x, y; // authored (overlay-plane) coordinates for mouse actions
};

// Generic prohibited-operation flags exposed in mp_nav_state_info.uo_mask.
// Producers translate the library's native UOP mask into this small subset the
// player may care about; unset means "operation currently allowed".
#define MP_NAV_UO_BUTTON  (1u << 0) // button select/activate prohibited
#define MP_NAV_UO_MENU    (1u << 1) // menu call prohibited
#define MP_NAV_UO_RESUME  (1u << 2) // resume/leave-menu prohibited

// One authored menu sound effect handed from the disc stream to the player.
// libbluray delivers these as short 48 kHz, 16-bit LPCM clips (mono or stereo,
// interleaved) via BD_EVENT_SOUND_EFFECT; the producer copies the PCM out of
// the library on its own thread and transfers ownership of `samples` to the
// consumer. Retrieved via STREAM_CTRL_GET_NAV_SOUND (one clip per call).
struct mp_nav_sound_effect {
    int16_t *samples;    // owned by caller after fetch; talloc-freed by it
    int num_frames;      // frames (not samples) in `samples`
    int num_channels;    // 1 (mono) or 2 (stereo)
    int rate;            // sample rate in Hz (always 48000 for Blu-ray)
};

// Overlay fetch result: bitmaps, their generation id and authored resolution,
// retrieved together via STREAM_CTRL_GET_NAV_OVERLAY so the consumer can never
// pair a bitmap generation with a mismatched change id or scaling size.
struct mp_nav_overlay {
    struct sub_bitmaps *imgs; // owned by caller after fetch; NULL clears/none
    int change_id;            // generation of imgs (and of the current overlay)
    int w, h;                 // authored overlay-plane resolution for scaling
};

#endif // MP_STREAM_DISCNAV_H
