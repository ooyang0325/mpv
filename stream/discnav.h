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
    bool menu_transport;     // transport is a menu playlist, not feature video
    bool popup_available;    // a popup menu can be toggled (BD_EVENT_POPUP)
    bool mouse_over_button;  // last mouse position was over a menu button
    bool overlay_visible;    // a menu overlay is currently displayed
    bool wait_pending;       // stream parked at a DVDNAV_WAIT sync point
    int reset_id;            // incremented for each hard media boundary
    int still_seconds;       // 0: none, -1: infinite still, >0: timed still
    int overlay_change_id;   // bumped whenever the overlay bitmaps change
};

// Whether the player pipeline has drained to a DVDNAV_WAIT boundary and the
// stream may be told to continue: the nested demux queues are empty and every
// present audio/video output has presented everything it had. Pure/inline so it
// can be unit tested without the player. Returns false while anything is still
// buffered, and false when nothing is playing (never release blindly).
static inline bool mp_nav_wait_drained(bool demux_empty,
                                       bool have_audio, bool audio_drained,
                                       bool have_video, bool video_drained)
{
    if (!demux_empty)
        return false;
    if (!have_audio && !have_video)
        return false;
    return (!have_audio || audio_drained) && (!have_video || video_drained);
}

// DVDNAV_WAIT handshake phase reported by the stream to the nested demuxer
// (STREAM_CTRL_GET_NAV_WAIT), so the demuxer can resume after a transient EOF
// once the player has drained and released it. Pure/inline for testing.
//   0 = no wait outstanding
//   1 = waiting (player still draining to the WAIT boundary)
//   2 = released (player drained; the stream may run dvdnav_wait_skip and resume)
static inline int mp_nav_wait_phase(bool wait_pending, bool wait_release)
{
    if (!wait_pending)
        return 0;
    return wait_release ? 2 : 1;
}

// Thread-independent "the nested demux queues are drained" test for the
// DVDNAV_WAIT release. demux_reader_state.underrun is forced false when the
// demuxer runs unthreaded (--demuxer-thread=no), so an empty forward queue
// (fw_bytes == 0, which is thread-independent) also counts as drained. Pure/
// inline for testing.
static inline bool mp_nav_demux_drained(bool demux_underrun, int64_t fw_bytes)
{
    return demux_underrun || fw_bytes == 0;
}

// Whether the disc is "held" at a menu or still, i.e. intentionally parked
// waiting for the user rather than actively playing content. The player uses
// this to suppress the cache-buffering pause: an authored still/menu produces
// no packets until the user acts, and that must not be mistaken for a cache
// underrun. Pure/inline for testing. still_seconds: 0 none, -1 inf, >0 timed.
static inline bool mp_nav_hold(bool menu_active, int still_seconds)
{
    return menu_active || still_seconds != 0;
}

// Keep a resumable disc-menu EOF from becoming the file's real EOF during the
// brief handoff from a held menu to the first frame of the selected title.
static inline bool mp_nav_eof_hold(bool held, bool nav_hold, bool outputs_eof)
{
    return nav_hold || (held && outputs_eof);
}

// Whether the player should idle (deselect) the audio output during a disc
// hold. A silent hold -- a still, or a WAIT-parked/looping button menu with no
// program (in-band) audio -- otherwise keeps the audio device open rendering
// silence for the whole (often indefinite) hold, pinning a CoreAudio render
// thread near 100% CPU and preventing the DVDNAV_WAIT audio drain. But a motion
// menu (or still) that carries authored background music must keep playing, so
// idle only when there is genuinely no program audio. Pure/inline for testing.
static inline bool mp_nav_audio_idle(bool menu_active, int still_seconds,
                                     bool wait_pending, bool has_program_audio)
{
    bool hold = menu_active || still_seconds != 0 || wait_pending;
    return hold && !has_program_audio;
}

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
    int64_t pts; // last presented title-relative 90 kHz video PTS, or -1
};

// One authored menu sound effect handed from the disc stream to the player.
// libbluray delivers these as short 48 kHz, 16-bit LPCM clips (mono or stereo,
// interleaved) via BD_EVENT_SOUND_EFFECT; the producer copies the PCM out of
// the library on its own thread and transfers ownership of `samples` to the
// consumer. Retrieved via STREAM_CTRL_GET_NAV_SOUND (one clip per call).
struct mp_nav_sound_effect {
    int16_t *samples;    // owned by caller after fetch; talloc-freed by it
    int num_frames;      // frames (not samples) in `samples`
    int num_channels;    // 1 (mono) or 2 (stereo)
};

// Overlay fetch result: bitmaps, their generation id and authored resolution,
// retrieved together via STREAM_CTRL_GET_NAV_OVERLAY so the consumer can never
// pair a bitmap generation with a mismatched change id or scaling size.
struct mp_nav_overlay {
    struct sub_bitmaps *imgs; // owned by caller after fetch; NULL clears/none
    int change_id;            // generation of imgs (and of the current overlay)
    int w, h;                 // authored overlay-plane resolution for scaling
    bool wait_for_video;      // show with the first frame after a playlist reset
};

#endif // MP_STREAM_DISCNAV_H
