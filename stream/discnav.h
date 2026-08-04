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

#endif // MP_STREAM_DISCNAV_H
