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

#include "boxedwine.h"
#include "inputcontrol.h"

#ifdef BOXEDWINE_LOCAL_SERVER

#include "json.h"
#include "knativesystem.h"
#include "knativeinput.h"
#include "../x11/x11.h"
#include <set>
#include <thread>

// Default hold for a key press or a click. Games that poll key state once a
// frame miss a key that goes down and up between two polls.
static const U32 DEFAULT_HOLD_MS = 50;
// Default gap after each character of /input/text.
static const U32 DEFAULT_TEXT_DELAY_MS = 30;
static const U32 TEXT_HOLD_MS = 30;
static const U32 MAX_HOLD_MS = 10000;
static const U32 MAX_WAIT_MS = 60000;
static const size_t MAX_EVENTS = 4096;

// SDL scancodes (the USB HID usage IDs), so this file needs no SDL headers.
enum : U32 {
    SC_A = 4, SC_1 = 30, SC_0 = 39, SC_RETURN = 40, SC_ESCAPE = 41, SC_BACKSPACE = 42, SC_TAB = 43,
    SC_SPACE = 44, SC_MINUS = 45, SC_EQUALS = 46, SC_LEFTBRACKET = 47, SC_RIGHTBRACKET = 48,
    SC_BACKSLASH = 49, SC_SEMICOLON = 51, SC_APOSTROPHE = 52, SC_GRAVE = 53, SC_COMMA = 54,
    SC_PERIOD = 55, SC_SLASH = 56, SC_CAPSLOCK = 57, SC_F1 = 58, SC_PRINTSCREEN = 70,
    SC_SCROLLLOCK = 71, SC_PAUSE = 72, SC_INSERT = 73, SC_HOME = 74, SC_PAGEUP = 75, SC_DELETE = 76,
    SC_END = 77, SC_PAGEDOWN = 78, SC_RIGHT = 79, SC_LEFT = 80, SC_DOWN = 81, SC_UP = 82,
    SC_NUMLOCK = 83, SC_KP_DIVIDE = 84, SC_KP_MULTIPLY = 85, SC_KP_MINUS = 86, SC_KP_PLUS = 87,
    SC_KP_ENTER = 88, SC_KP_1 = 89, SC_KP_0 = 98, SC_KP_PERIOD = 99,
    SC_LCTRL = 224, SC_LSHIFT = 225, SC_LALT = 226, SC_LGUI = 227, SC_RCTRL = 228, SC_RSHIFT = 229,
    SC_RALT = 230, SC_RGUI = 231,
};

namespace {
    struct KeyName {
        const char* name;
        U32 scancode;
    };

    const KeyName namedKeys[] = {
        { "enter", SC_RETURN }, { "return", SC_RETURN }, { "esc", SC_ESCAPE }, { "escape", SC_ESCAPE },
        { "backspace", SC_BACKSPACE }, { "tab", SC_TAB }, { "space", SC_SPACE },
        { "minus", SC_MINUS }, { "equals", SC_EQUALS }, { "leftbracket", SC_LEFTBRACKET },
        { "rightbracket", SC_RIGHTBRACKET }, { "backslash", SC_BACKSLASH }, { "semicolon", SC_SEMICOLON },
        { "quote", SC_APOSTROPHE }, { "backquote", SC_GRAVE }, { "comma", SC_COMMA }, { "period", SC_PERIOD },
        { "slash", SC_SLASH }, { "capslock", SC_CAPSLOCK }, { "printscreen", SC_PRINTSCREEN },
        { "scrolllock", SC_SCROLLLOCK }, { "pause", SC_PAUSE }, { "insert", SC_INSERT }, { "home", SC_HOME },
        { "pageup", SC_PAGEUP }, { "delete", SC_DELETE }, { "end", SC_END }, { "pagedown", SC_PAGEDOWN },
        { "right", SC_RIGHT }, { "left", SC_LEFT }, { "down", SC_DOWN }, { "up", SC_UP },
        { "numlock", SC_NUMLOCK }, { "kp_divide", SC_KP_DIVIDE }, { "kp_multiply", SC_KP_MULTIPLY },
        { "kp_minus", SC_KP_MINUS }, { "kp_plus", SC_KP_PLUS }, { "kp_enter", SC_KP_ENTER },
        { "kp_period", SC_KP_PERIOD },
        { "shift", SC_LSHIFT }, { "lshift", SC_LSHIFT }, { "rshift", SC_RSHIFT },
        { "ctrl", SC_LCTRL }, { "lctrl", SC_LCTRL }, { "rctrl", SC_RCTRL },
        { "alt", SC_LALT }, { "lalt", SC_LALT }, { "ralt", SC_RALT },
        { "win", SC_LGUI }, { "lwin", SC_LGUI }, { "rwin", SC_RGUI },
    };

    // Characters of a US keyboard: the key and whether Shift goes with it.
    bool charToKey(char c, U32& scancode, bool& shift) {
        static const char* plain = "-=[]\\;',./`";
        static const char* shifted = "_+{}|:\"<>?~";
        static const U32 symbolKeys[] = { SC_MINUS, SC_EQUALS, SC_LEFTBRACKET, SC_RIGHTBRACKET, SC_BACKSLASH,
            SC_SEMICOLON, SC_APOSTROPHE, SC_COMMA, SC_PERIOD, SC_SLASH, SC_GRAVE };
        static const char* shiftedDigits = ")!@#$%^&*(";
        shift = false;
        if (c >= 'a' && c <= 'z') {
            scancode = SC_A + (U32)(c - 'a');
            return true;
        }
        if (c >= 'A' && c <= 'Z') {
            scancode = SC_A + (U32)(c - 'A');
            shift = true;
            return true;
        }
        if (c >= '1' && c <= '9') {
            scancode = SC_1 + (U32)(c - '1');
            return true;
        }
        if (c == '0') {
            scancode = SC_0;
            return true;
        }
        if (c == ' ') {
            scancode = SC_SPACE;
            return true;
        }
        if (c == '\n') {
            scancode = SC_RETURN;
            return true;
        }
        if (c == '\t') {
            scancode = SC_TAB;
            return true;
        }
        if (c == '\b') {
            scancode = SC_BACKSPACE;
            return true;
        }
        if (const char* p = strchr(plain, c); p && c) {
            scancode = symbolKeys[p - plain];
            return true;
        }
        if (const char* p = strchr(shifted, c); p && c) {
            scancode = symbolKeys[p - shifted];
            shift = true;
            return true;
        }
        if (const char* p = strchr(shiftedDigits, c); p && c) {
            U32 digit = (U32)(p - shiftedDigits);
            scancode = digit == 0 ? SC_0 : SC_1 + digit - 1;
            shift = true;
            return true;
        }
        return false;
    }

    bool keyFromName(const std::string& rawName, U32& scancode) {
        std::string name;
        for (char c : rawName) {
            name += (char)tolower((unsigned char)c);
        }
        for (const KeyName& key : namedKeys) {
            if (name == key.name) {
                scancode = key.scancode;
                return true;
            }
        }
        if (name.size() >= 2 && name[0] == 'f' && isdigit((unsigned char)name[1])) {
            int n = atoi(name.c_str() + 1);
            if (n >= 1 && n <= 12 && name == "f" + std::to_string(n)) {
                scancode = SC_F1 + (U32)(n - 1);
                return true;
            }
        }
        if (name.size() == 4 && name.compare(0, 3, "kp_") == 0 && isdigit((unsigned char)name[3])) {
            int n = name[3] - '0';
            scancode = n == 0 ? SC_KP_0 : SC_KP_1 + (U32)(n - 1);
            return true;
        }
        if (rawName.size() == 1) {
            bool shift = false;
            // A single character names its key; "A" and "a" are the same key.
            return charToKey((char)tolower((unsigned char)rawName[0]), scancode, shift) && !shift;
        }
        return false;
    }

    class BodyError {
    public:
        std::string message;
    };

    // Reads the fields of one event object and refuses names it doesn't use,
    // since dropping a misspelled field would send other input than asked for.
    class Fields {
    public:
        Fields(const JsonValue& value, const std::string& where) : value(value), where(where) {}

        const JsonValue* get(const char* name) {
            allowed.push_back(name);
            return value.get(name);
        }

        std::string string(const char* name, const std::string& defaultValue) {
            const JsonValue* v = get(name);
            if (!v) {
                return defaultValue;
            }
            if (v->type != JsonValue::String) {
                throw BodyError{ where + "`" + name + "` must be a string" };
            }
            return v->string;
        }

        bool hasNumber(const char* name) {
            const JsonValue* v = get(name);
            if (v && v->type != JsonValue::Number) {
                throw BodyError{ where + "`" + name + "` must be a number" };
            }
            return v != nullptr;
        }

        S32 integer(const char* name, S32 defaultValue, S32 min, S32 max) {
            const JsonValue* v = get(name);
            if (!v) {
                return defaultValue;
            }
            if (v->type != JsonValue::Number || v->number != (double)(S64)v->number) {
                throw BodyError{ where + "`" + name + "` must be a whole number" };
            }
            if (v->number < min || v->number > max) {
                throw BodyError{ where + "`" + name + "` must be from " + std::to_string(min) + " to " + std::to_string(max) };
            }
            return (S32)v->number;
        }

        void checkUnknown() {
            for (auto& member : value.members) {
                if (std::find(allowed.begin(), allowed.end(), member.first) != allowed.end()) {
                    continue;
                }
                std::string list;
                for (const char* name : allowed) {
                    list += std::string(list.empty() ? "" : ", ") + "`" + name + "`";
                }
                std::string hint;
                if (member.first == "modifiers" || member.first == "modifier") {
                    hint = " (did you mean `mods`?)";
                }
                throw BodyError{ where + "unknown field `" + member.first + "`, expected one of " + list + hint };
            }
        }

    private:
        const JsonValue& value;
        std::string where;
        std::vector<const char*> allowed;
    };

    U32 modifierKey(const std::string& rawName, const std::string& where) {
        std::string name;
        for (char c : rawName) {
            name += (char)tolower((unsigned char)c);
        }
        if (name == "shift") return SC_LSHIFT;
        if (name == "ctrl" || name == "control") return SC_LCTRL;
        if (name == "alt") return SC_LALT;
        if (name == "win" || name == "meta" || name == "super") return SC_LGUI;
        throw BodyError{ where + "unknown modifier \"" + rawName + "\", expected shift, ctrl, alt or win" };
    }

    void addKey(std::vector<InputControl::Event>& events, InputControl::Event::Kind kind, U32 scancode) {
        InputControl::Event e;
        e.kind = kind;
        e.scancode = scancode;
        events.push_back(e);
    }

    void addWait(std::vector<InputControl::Event>& events, U32 ms) {
        if (!ms) {
            return;
        }
        InputControl::Event e;
        e.kind = InputControl::Event::Wait;
        e.ms = ms;
        events.push_back(e);
    }

    void parseKey(Fields& f, std::vector<InputControl::Event>& events, const std::string& where) {
        std::string name = f.string("key", "");
        bool hasScancode = f.hasNumber("scancode");
        U32 scancode = 0;
        if (hasScancode) {
            scancode = (U32)f.integer("scancode", 0, 1, 511);
        }
        std::string action = f.string("action", "press");
        U32 hold = (U32)f.integer("hold_ms", (S32)DEFAULT_HOLD_MS, 0, (S32)MAX_HOLD_MS);
        std::vector<U32> mods;
        if (const JsonValue* m = f.get("mods")) {
            if (m->type != JsonValue::Array) {
                throw BodyError{ where + "`mods` must be an array of names" };
            }
            for (auto& item : m->items) {
                if (item.type != JsonValue::String) {
                    throw BodyError{ where + "`mods` must be an array of names" };
                }
                mods.push_back(modifierKey(item.string, where));
            }
        }
        f.checkUnknown();
        if (name.empty() == !hasScancode) {
            throw BodyError{ where + "give either `key` or `scancode`" };
        }
        if (!hasScancode && !keyFromName(name, scancode)) {
            throw BodyError{ where + "unknown key \"" + name + "\". Keys: " + InputControl::keyNames() };
        }
        if (action != "press" && action != "down" && action != "up") {
            throw BodyError{ where + "`action` must be press, down or up" };
        }
        if (action == "press" || action == "down") {
            for (U32 mod : mods) {
                addKey(events, InputControl::Event::KeyDown, mod);
            }
            addKey(events, InputControl::Event::KeyDown, scancode);
        }
        if (action == "press") {
            addWait(events, hold);
        }
        if (action == "press" || action == "up") {
            addKey(events, InputControl::Event::KeyUp, scancode);
            for (auto it = mods.rbegin(); it != mods.rend(); ++it) {
                addKey(events, InputControl::Event::KeyUp, *it);
            }
        }
    }

    void parseText(Fields& f, std::vector<InputControl::Event>& events, const std::string& where) {
        std::string text = f.string("text", "");
        U32 delay = (U32)f.integer("delay_ms", (S32)DEFAULT_TEXT_DELAY_MS, 0, (S32)MAX_HOLD_MS);
        f.checkUnknown();
        if (!f.get("text")) {
            throw BodyError{ where + "`text` is required" };
        }
        for (size_t i = 0; i < text.size(); i++) {
            U32 scancode = 0;
            bool shift = false;
            if (!charToKey(text[i], scancode, shift)) {
                throw BodyError{ where + "character " + std::to_string(i) + " of `text` has no key on a US keyboard; "
                    "send it with /input/key or leave it out" };
            }
            if (shift) {
                addKey(events, InputControl::Event::KeyDown, SC_LSHIFT);
            }
            addKey(events, InputControl::Event::KeyDown, scancode);
            addWait(events, TEXT_HOLD_MS);
            addKey(events, InputControl::Event::KeyUp, scancode);
            if (shift) {
                addKey(events, InputControl::Event::KeyUp, SC_LSHIFT);
            }
            addWait(events, delay);
        }
    }

    U32 buttonFromName(const std::string& name, const std::string& where) {
        if (name == "left") return 1;
        if (name == "middle") return 2;
        if (name == "right") return 3;
        throw BodyError{ where + "`button` must be left, middle or right" };
    }

    void parseMouse(Fields& f, std::vector<InputControl::Event>& events, const std::string& where) {
        std::string action = f.string("action", "move");
        bool hasX = f.hasNumber("x");
        bool hasY = f.hasNumber("y");
        S32 x = f.integer("x", 0, -100000, 100000);
        S32 y = f.integer("y", 0, -100000, 100000);
        bool hasDx = f.hasNumber("dx");
        bool hasDy = f.hasNumber("dy");
        S32 dx = f.integer("dx", 0, -100000, 100000);
        S32 dy = f.integer("dy", 0, -100000, 100000);
        std::string buttonName = f.string("button", "left");
        U32 hold = (U32)f.integer("hold_ms", (S32)DEFAULT_HOLD_MS, 0, (S32)MAX_HOLD_MS);
        S32 amount = f.integer("amount", 1, -100, 100);
        f.checkUnknown();
        if (hasX != hasY) {
            throw BodyError{ where + "give both `x` and `y`" };
        }
        if ((hasDx || hasDy) && hasX) {
            throw BodyError{ where + "give `x` and `y`, or `dx` and `dy`, not both" };
        }
        U32 button = buttonFromName(buttonName, where);

        InputControl::Event e;
        if (action == "move") {
            if (hasX) {
                e.kind = InputControl::Event::MouseMove;
                e.x = x;
                e.y = y;
            } else if (hasDx || hasDy) {
                e.kind = InputControl::Event::MouseMoveBy;
                e.x = dx;
                e.y = dy;
            } else {
                throw BodyError{ where + "move needs `x` and `y`, or `dx` and `dy`" };
            }
            events.push_back(e);
            return;
        }
        if (hasDx || hasDy) {
            throw BodyError{ where + "`dx` and `dy` only go with move" };
        }
        e.x = x;
        e.y = y;
        e.hasPosition = hasX;
        if (action == "wheel") {
            if (amount == 0) {
                throw BodyError{ where + "`amount` must not be 0" };
            }
            // X buttons 4 and 5 are one wheel step up and down.
            e.button = amount > 0 ? 4 : 5;
            for (S32 i = 0; i < std::abs(amount); i++) {
                e.kind = InputControl::Event::MouseDown;
                events.push_back(e);
                e.kind = InputControl::Event::MouseUp;
                e.hasPosition = false;
                events.push_back(e);
            }
            return;
        }
        e.button = button;
        if (action == "down" || action == "click") {
            e.kind = InputControl::Event::MouseDown;
            events.push_back(e);
            e.hasPosition = false;
        }
        if (action == "click") {
            addWait(events, hold);
        }
        if (action == "up" || action == "click") {
            e.kind = InputControl::Event::MouseUp;
            events.push_back(e);
        }
        if (action != "down" && action != "up" && action != "click") {
            throw BodyError{ where + "`action` must be move, down, up, click or wheel" };
        }
    }

    void parseEvent(const JsonValue& value, const std::string& defaultType, const std::string& where, std::vector<InputControl::Event>& events) {
        if (value.type != JsonValue::Object) {
            throw BodyError{ where + "an event must be an object" };
        }
        Fields f(value, where);
        std::string type = f.string("type", defaultType);
        if (type.empty()) {
            throw BodyError{ where + "`type` is required: key, text, mouse or wait" };
        }
        if (type == "key") {
            parseKey(f, events, where);
        } else if (type == "text" || type == "type") {
            parseText(f, events, where);
        } else if (type == "mouse") {
            parseMouse(f, events, where);
        } else if (type == "wait") {
            U32 ms = (U32)f.integer("ms", 0, 0, (S32)MAX_WAIT_MS);
            f.checkUnknown();
            if (!value.get("ms")) {
                throw BodyError{ where + "`ms` is required" };
            }
            addWait(events, ms);
        } else {
            throw BodyError{ where + "unknown event type \"" + type + "\", expected key, text, mouse or wait" };
        }
    }

    // Input state the X server sink keeps: the virtual pointer and what is held.
    std::mutex stateMutex;
    S32 pointerX = 0;
    S32 pointerY = 0;
    bool pointerSet = false;
    std::set<U32> heldKeys;
    U32 heldButtons = 0; // bit n-1 for X button n

    U32 nativeModifiers() {
        U32 result = 0;
        if (heldKeys.count(SC_LSHIFT) || heldKeys.count(SC_RSHIFT)) result |= NATIVE_SHIFT_MASK;
        if (heldKeys.count(SC_LCTRL) || heldKeys.count(SC_RCTRL)) result |= NATIVE_CONTROL_MASK;
        if (heldKeys.count(SC_LALT) || heldKeys.count(SC_RALT)) result |= NATIVE_ALT_MASK;
        if (heldButtons & 1) result |= NATIVE_LEFT_BUTTON_MASK;
        if (heldButtons & 2) result |= NATIVE_MIDDLE_BUTTON_MASK;
        if (heldButtons & 4) result |= NATIVE_RIGHT_BUTTON_MASK;
        return result;
    }

    // XServer::mouseMove and mouseButton take coordinates of the window shown
    // full screen when there is one (an OpenGL game), as SDL reports them.
    void screenToInputCoords(XServer* server, S32& x, S32& y) {
        XWindowPtr presented = server->getFakeFullScreenWindow();
        if (presented) {
            presented->screenToWindow(x, y);
        }
    }

    // The server starts before the window, and getCurrentInput() needs the screen.
    KNativeInputPtr currentInput() {
        KNativeScreenPtr screen = KNativeSystem::getScreen();
        return screen ? screen->getInput() : nullptr;
    }

    // Runs on the main thread.
    std::string applyToXServer(const InputControl::Event& e, KNativeInputPtr input) {
        XServer* server = XServer::getServer(true);
        if (!server) {
            return "the guest has not opened a display yet";
        }
        std::lock_guard<std::mutex> lock(stateMutex);
        switch (e.kind) {
        case InputControl::Event::KeyDown:
        case InputControl::Event::KeyUp: {
            U32 x11Key = XKeyboard::sdl2x11(e.scancode);
            if (!x11Key) {
                return "scancode " + std::to_string(e.scancode) + " has no X key";
            }
            bool down = e.kind == InputControl::Event::KeyDown;
            // Update the modifiers first: the event carries them in its state.
            if (down) {
                heldKeys.insert(e.scancode);
            } else {
                heldKeys.erase(e.scancode);
            }
            input->setInjectedModifiers(nativeModifiers());
            server->key(x11Key, down);
            return "";
        }
        case InputControl::Event::MouseMove:
        case InputControl::Event::MouseMoveBy: {
            if (e.kind == InputControl::Event::MouseMoveBy && server->wantsRelativeMouse()) {
                // The game reads raw motion (XInput2), with no pointer position.
                server->mouseMove(e.x, e.y, true);
                return "";
            }
            S32 x = e.x;
            S32 y = e.y;
            if (e.kind == InputControl::Event::MouseMoveBy) {
                x += pointerX;
                y += pointerY;
            }
            S32 width = (S32)input->screenWidth();
            S32 height = (S32)input->screenHeight();
            x = std::max(0, std::min(x, width - 1));
            y = std::max(0, std::min(y, height - 1));
            pointerX = x;
            pointerY = y;
            pointerSet = true;
            input->setInjectedPointer(x, y);
            screenToInputCoords(server, x, y);
            server->mouseMove(x, y, false);
            return "";
        }
        case InputControl::Event::MouseDown:
        case InputControl::Event::MouseUp: {
            if (e.hasPosition) {
                S32 width = (S32)input->screenWidth();
                S32 height = (S32)input->screenHeight();
                pointerX = std::max(0, std::min(e.x, width - 1));
                pointerY = std::max(0, std::min(e.y, height - 1));
                pointerSet = true;
                input->setInjectedPointer(pointerX, pointerY);
                S32 x = pointerX;
                S32 y = pointerY;
                screenToInputCoords(server, x, y);
                server->mouseMove(x, y, false);
            } else if (!pointerSet) {
                return "move the mouse first, or give `x` and `y`";
            }
            bool down = e.kind == InputControl::Event::MouseDown;
            U32 bit = 1u << (e.button - 1);
            if (down) {
                heldButtons |= bit;
            } else {
                heldButtons &= ~bit;
            }
            input->setInjectedModifiers(nativeModifiers());
            S32 x = pointerX;
            S32 y = pointerY;
            screenToInputCoords(server, x, y);
            server->mouseButton(e.button, x, y, down);
            return "";
        }
        case InputControl::Event::Wait:
            return "";
        }
        return "";
    }

    std::string deliverToXServer(const InputControl::Event& e) {
        KNativeInputPtr input = currentInput();
        if (!input) {
            return "Boxedwine has no input device yet";
        }
        std::string error;
        // Blocks until the main thread ran it, so the events reach the X
        // server in order and before the request is answered.
        input->runOnUiThread([&error, &e, input]() {
            error = applyToXServer(e, input);
        });
        return error;
    }

    std::mutex sinkMutex;
    InputControl::Sink sink;
    // One request delivers at a time, so events of two requests don't interleave.
    std::mutex runMutex;
}

bool InputControl::parse(const std::string& kind, const std::string& body, std::vector<Event>& events, std::string& error) {
    events.clear();
    JsonValue value;
    if (!JsonValue::parse(body, value, error)) {
        error = "the body is not JSON: " + error;
        return false;
    }
    try {
        if (kind == "batch") {
            const JsonValue* list = &value;
            if (value.type == JsonValue::Object) {
                list = value.get("events");
                if (!list || value.members.size() != 1) {
                    throw BodyError{ "a batch is an array of events, or an object with only `events`" };
                }
            }
            if (list->type != JsonValue::Array) {
                throw BodyError{ "a batch is an array of events, or an object with only `events`" };
            }
            for (size_t i = 0; i < list->items.size(); i++) {
                parseEvent(list->items[i], "", "events[" + std::to_string(i) + "]: ", events);
            }
        } else if (kind == "key" || kind == "text" || kind == "mouse" || kind == "wait") {
            parseEvent(value, kind, "", events);
        } else {
            throw BodyError{ "unknown input kind \"" + kind + "\"" };
        }
        if (events.size() > MAX_EVENTS) {
            throw BodyError{ "too many events in one request (at most " + std::to_string(MAX_EVENTS) + ")" };
        }
    } catch (BodyError& e) {
        events.clear();
        error = e.message;
        return false;
    }
    return true;
}

void InputControl::setSink(Sink newSink) {
    std::lock_guard<std::mutex> lock(sinkMutex);
    sink = newSink;
}

U32 InputControl::run(const std::vector<Event>& events, std::string& error) {
    std::lock_guard<std::mutex> runLock(runMutex);
    Sink current;
    {
        std::lock_guard<std::mutex> lock(sinkMutex);
        current = sink ? sink : Sink(deliverToXServer);
    }
    U32 delivered = 0;
    for (const Event& e : events) {
        if (LocalServer::isStopping()) {
            error = "the server is stopping";
            return delivered;
        }
        if (e.kind == Event::Wait) {
            std::this_thread::sleep_for(std::chrono::milliseconds(e.ms));
        } else {
            error = current(e);
            if (!error.empty()) {
                return delivered;
            }
        }
        delivered++;
    }
    return delivered;
}

std::string InputControl::keyNames() {
    std::string result = "a-z, 0-9, f1-f12, kp_0-kp_9";
    for (const KeyName& key : namedKeys) {
        result += std::string(", ") + key.name;
    }
    return result + ", or one character such as \";\"";
}

void InputControl::pointer(S32& x, S32& y, bool& set) {
    std::lock_guard<std::mutex> lock(stateMutex);
    x = pointerX;
    y = pointerY;
    set = pointerSet;
}

void InputControl::reset() {
    std::lock_guard<std::mutex> lock(stateMutex);
    pointerX = pointerY = 0;
    pointerSet = false;
    heldKeys.clear();
    heldButtons = 0;
}

static void handleInput(const std::string& kind, const LocalServer::Request& request, LocalServer::Connection& connection) {
    std::vector<InputControl::Event> events;
    std::string error;
    if (!InputControl::parse(kind, request.body, events, error)) {
        connection.sendError(400, error);
        return;
    }
    bool wait = request.param("wait", "true") != "false";
    if (!wait) {
        // Answer now and deliver in the background, still in request order.
        std::thread([events]() {
            std::string ignored;
            InputControl::run(events, ignored);
        }).detach();
        connection.sendJson(202, "{\"queued\":" + std::to_string(events.size()) + "}");
        return;
    }
    U32 delivered = InputControl::run(events, error);
    if (!error.empty()) {
        connection.sendJson(503, "{\"error\":\"" + LocalServer::jsonEscape(error) + "\",\"delivered\":" + std::to_string(delivered) + "}");
        return;
    }
    connection.sendJson(200, "{\"delivered\":" + std::to_string(delivered) + "}");
}

static void handleStatus(const LocalServer::Request& request, LocalServer::Connection& connection) {
    std::string processes;
    KSystem::visitProcesses([&processes](const KProcessPtr& process) {
        if (!processes.empty()) {
            processes += ",";
        }
        const char* state = process->isTerminated() ? "exited" : (process->isStopped() ? "stopped" : "running");
        processes += "{\"pid\":" + std::to_string(process->id)
            + ",\"name\":\"" + LocalServer::jsonEscape(process->name.c_str()) + "\""
            + ",\"command_line\":\"" + LocalServer::jsonEscape(process->commandLine.c_str()) + "\""
            + ",\"state\":\"" + state + "\"}";
    });
    S32 x = 0;
    S32 y = 0;
    bool set = false;
    InputControl::pointer(x, y, set);
    U32 width = 0;
    U32 height = 0;
    if (KNativeInputPtr input = currentInput()) {
        width = input->screenWidth();
        height = input->screenHeight();
    }
    std::string json = "{\"uptime_ms\":" + std::to_string(KSystem::getMilliesSinceStart())
        + ",\"display\":" + (XServer::getServer(true) ? "true" : "false")
        + ",\"screen\":{\"width\":" + std::to_string(width) + ",\"height\":" + std::to_string(height) + "}"
        + ",\"pointer\":" + (set ? "{\"x\":" + std::to_string(x) + ",\"y\":" + std::to_string(y) + "}" : std::string("null"))
        + ",\"processes\":[" + processes + "]}";
    connection.sendJson(200, json);
}

void startInputControl() {
    InputControl::reset();
    static const char* kinds[] = { "key", "text", "mouse", "wait", "batch" };
    static const char* descriptions[] = {
        "Press a key: {\"key\":\"enter\"}, with optional \"mods\":[\"shift\",\"ctrl\",\"alt\"], \"action\":\"press|down|up\", \"hold_ms\" (default 50).",
        "Type text on a US keyboard: {\"text\":\"hello\\n\"}, optional \"delay_ms\" between characters (default 30).",
        "Mouse in screenshot pixels: {\"action\":\"move|down|up|click|wheel\",\"x\":320,\"y\":200}, or \"dx\"/\"dy\" to move by, "
            "\"button\":\"left|middle|right\", \"hold_ms\" for click, \"amount\" for wheel.",
        "Pause between events of a batch: {\"ms\":500}.",
        "Several events in order: [{\"type\":\"key\",\"key\":\"esc\"},{\"type\":\"wait\",\"ms\":200},{\"type\":\"mouse\",\"action\":\"click\",\"x\":10,\"y\":10}].",
    };
    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
        std::string kind = kinds[i];
        LocalServer::addRoute("POST", "/input/" + kind,
            std::string(descriptions[i]) + " Answers once the X server has the events; ?wait=false answers at once.",
            [kind](const LocalServer::Request& request, LocalServer::Connection& connection) {
                handleInput(kind, request, connection);
            });
    }
    LocalServer::addRoute("GET", "/status",
        "JSON with the guest's processes, whether it has a display, the screen size and the virtual pointer.",
        handleStatus);
}

#endif
