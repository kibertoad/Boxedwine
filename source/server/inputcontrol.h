/*
 *  Copyright (C) 2012-2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#ifndef __INPUT_CONTROL_H__
#define __INPUT_CONTROL_H__

#include "localserver.h"

#ifdef BOXEDWINE_LOCAL_SERVER

#include <string>
#include <vector>
#include <functional>

// Keyboard and mouse input from the local server's /input routes. A request
// body becomes a list of low-level events, and each event is handed to the X
// server on the main thread, where SDL's own input is handled.
class InputControl {
public:
    struct Event {
        enum Kind { KeyDown, KeyUp, MouseMove, MouseMoveBy, MouseDown, MouseUp, Wait };
        Kind kind = Wait;
        U32 scancode = 0; // SDL scancode, for KeyDown and KeyUp
        S32 x = 0;        // MouseMove: screen position; MouseMoveBy: delta
        S32 y = 0;
        bool hasPosition = false; // MouseDown and MouseUp: move to x, y first
        U32 button = 0;   // X button: 1 left, 2 middle, 3 right, 4 and 5 the wheel
        U32 ms = 0;       // Wait
    };

    // kind is the route's last path segment: key, text, mouse, wait or batch.
    // Returns false with error (and no events) when anything in the body is
    // wrong, so a bad request sends nothing.
    static bool parse(const std::string& kind, const std::string& body, std::vector<Event>& events, std::string& error);

    // Delivers one event and returns an error message, or an empty string.
    typedef std::function<std::string(const Event& event)> Sink;
    // Replaces the X server sink; tests pass their own. nullptr restores it.
    static void setSink(Sink sink);

    // Delivers events in order, waiting for Wait events. Requests run one at a
    // time. Returns how many events were delivered; on failure error says why.
    static U32 run(const std::vector<Event>& events, std::string& error);

    // Every key name /input/key accepts, for its error message.
    static std::string keyNames();

    // The virtual pointer that /input/mouse moves, in screen pixels.
    static void pointer(S32& x, S32& y, bool& set);

    static void reset();
};

// Registers /input/key, /input/text, /input/mouse, /input/wait, /input/batch and /status.
void startInputControl();

#endif

#endif
