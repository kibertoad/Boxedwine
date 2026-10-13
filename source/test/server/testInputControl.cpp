/*
 *  Copyright (C) 2012-2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#include "boxedwine.h"

#ifdef __TEST

#include "../../server/localserver.h"

#ifdef BOXEDWINE_LOCAL_SERVER

#include "../../server/json.h"
#include "../../server/inputcontrol.h"
#include "../cpu/testCPU.h"
#include <thread>

#ifdef WIN32
#undef BOOL
#include <winsock2.h>
#include <ws2tcpip.h>
#define BOOL unsigned int
#undef min
#undef max
typedef SOCKET InputTestSocket;
static void closeInputTestSocket(InputTestSocket s) { closesocket(s); }
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
typedef int InputTestSocket;
static void closeInputTestSocket(InputTestSocket s) { close(s); }
#endif

// After the socket headers: X11 names clash with the Windows ones.
#include "../../x11/x11.h"

namespace {
    typedef InputControl::Event Event;

    std::string describe(const std::vector<Event>& events) {
        std::string result;
        for (const Event& e : events) {
            if (!result.empty()) {
                result += " ";
            }
            switch (e.kind) {
            case Event::KeyDown: result += "kd" + std::to_string(e.scancode); break;
            case Event::KeyUp: result += "ku" + std::to_string(e.scancode); break;
            case Event::MouseMove: result += "mv" + std::to_string(e.x) + "," + std::to_string(e.y); break;
            case Event::MouseMoveBy: result += "by" + std::to_string(e.x) + "," + std::to_string(e.y); break;
            case Event::MouseDown: result += "md" + std::to_string(e.button) + (e.hasPosition ? "@" + std::to_string(e.x) + "," + std::to_string(e.y) : ""); break;
            case Event::MouseUp: result += "mu" + std::to_string(e.button) + (e.hasPosition ? "@" + std::to_string(e.x) + "," + std::to_string(e.y) : ""); break;
            case Event::Wait: result += "w" + std::to_string(e.ms); break;
            }
        }
        return result;
    }

    void expectEvents(const std::string& kind, const std::string& body, const std::string& expected) {
        std::vector<Event> events;
        std::string error;
        if (!InputControl::parse(kind, body, events, error)) {
            testFail("%s %s was refused: %s", kind.c_str(), body.c_str(), error.c_str());
            return;
        }
        std::string got = describe(events);
        if (got != expected) {
            testFail("%s %s gave \"%s\", expected \"%s\"", kind.c_str(), body.c_str(), got.c_str(), expected.c_str());
        }
    }

    void expectRefused(const std::string& kind, const std::string& body, const std::string& errorPart) {
        std::vector<Event> events;
        std::string error;
        if (InputControl::parse(kind, body, events, error)) {
            testFail("%s %s was accepted", kind.c_str(), body.c_str());
            return;
        }
        if (!events.empty()) {
            testFail("%s %s was refused but left events", kind.c_str(), body.c_str());
        }
        if (error.find(errorPart) == std::string::npos) {
            testFail("%s %s: error \"%s\" lacks \"%s\"", kind.c_str(), body.c_str(), error.c_str(), errorPart.c_str());
        }
    }

    InputTestSocket connectInput(U32 port) {
        InputTestSocket s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons((U16)port);
        if (connect(s, (sockaddr*)&addr, sizeof(addr)) != 0) {
            closeInputTestSocket(s);
            return (InputTestSocket)-1;
        }
#ifdef WIN32
        DWORD timeout = 5000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
#else
        timeval timeout = { 5, 0 };
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif
        return s;
    }

    std::string httpRequest(U32 port, const std::string& method, const std::string& path, const std::string& body = std::string()) {
        InputTestSocket s = connectInput(port);
        if (s == (InputTestSocket)-1) {
            return std::string();
        }
        std::string request = method + " " + path + " HTTP/1.1\r\nHost: localhost\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
        send(s, request.data(), (int)request.size(), 0);
        std::string response;
        char buffer[4096];
        while (true) {
            int received = recv(s, buffer, sizeof(buffer), 0);
            if (received <= 0) {
                break;
            }
            response.append(buffer, received);
        }
        closeInputTestSocket(s);
        return response;
    }
}

void testServerJsonParsing() {
    JsonValue v;
    std::string error;
    if (!JsonValue::parse(" {\"a\":[1,-2.5e2,true,false,null],\"b\":{\"c\":\"x\\\"\\u00e9\\ud83d\\ude00\"}} ", v, error)) {
        testFail("valid JSON was refused: %s", error.c_str());
        return;
    }
    const JsonValue* a = v.get("a");
    if (!a || a->type != JsonValue::Array || a->items.size() != 5 || a->items[1].number != -250 || !a->items[2].boolValue || a->items[4].type != JsonValue::Null) {
        testFail("array members were wrong");
    }
    const JsonValue* b = v.get("b");
    const JsonValue* c = b ? b->get("c") : nullptr;
    if (!c || c->string != "x\"\xC3\xA9\xF0\x9F\x98\x80") {
        testFail("string escapes were decoded wrong");
    }
    const char* bad[] = { "", "{", "[1,]", "{\"a\":1,}", "{\"a\":1,\"a\":2}", "01", "1.", "\"\\x\"", "\"\\ud800\"", "[1] 2", "tru", "{a:1}", "\"a\nb\"" };
    for (const char* text : bad) {
        if (JsonValue::parse(text, v, error)) {
            testFail("invalid JSON %s was accepted", text);
        }
    }
    std::string deep(100, '[');
    deep += std::string(100, ']');
    if (JsonValue::parse(deep, v, error) || error.find("deep") == std::string::npos) {
        testFail("JSON nested 100 deep was accepted");
    }
}

void testInputControlParsing() {
    // a=4, Return=40, LShift=225, LCtrl=224, LAlt=226, Escape=41
    expectEvents("key", "{\"key\":\"enter\"}", "kd40 w50 ku40");
    expectEvents("key", "{\"key\":\"A\",\"mods\":[\"ctrl\",\"shift\"],\"hold_ms\":0}", "kd224 kd225 kd4 ku4 ku225 ku224");
    expectEvents("key", "{\"key\":\"esc\",\"action\":\"down\",\"mods\":[\"alt\"]}", "kd226 kd41");
    expectEvents("key", "{\"key\":\"esc\",\"action\":\"up\",\"mods\":[\"alt\"]}", "ku41 ku226");
    expectEvents("key", "{\"key\":\"F12\"}", "kd69 w50 ku69");
    expectEvents("key", "{\"key\":\";\"}", "kd51 w50 ku51");
    expectEvents("key", "{\"scancode\":30,\"hold_ms\":5}", "kd30 w5 ku30");
    expectEvents("text", "{\"text\":\"aB!\\n\",\"delay_ms\":0}", "kd4 w30 ku4 kd225 kd5 w30 ku5 ku225 kd225 kd30 w30 ku30 ku225 kd40 w30 ku40");
    expectEvents("mouse", "{\"x\":10,\"y\":20}", "mv10,20");
    expectEvents("mouse", "{\"dx\":-3,\"dy\":4}", "by-3,4");
    expectEvents("mouse", "{\"action\":\"click\",\"x\":5,\"y\":6}", "md1@5,6 w50 mu1");
    expectEvents("mouse", "{\"action\":\"click\",\"button\":\"right\",\"hold_ms\":0}", "md3 mu3");
    expectEvents("mouse", "{\"action\":\"down\",\"button\":\"middle\",\"x\":1,\"y\":2}", "md2@1,2");
    expectEvents("mouse", "{\"action\":\"wheel\",\"amount\":-2}", "md5 mu5 md5 mu5");
    expectEvents("wait", "{\"ms\":250}", "w250");
    expectEvents("batch", "[{\"type\":\"key\",\"key\":\"esc\",\"hold_ms\":0},{\"type\":\"wait\",\"ms\":200},{\"type\":\"mouse\",\"action\":\"click\",\"x\":1,\"y\":1,\"hold_ms\":0}]", "kd41 ku41 w200 md1@1,1 mu1");
    expectEvents("batch", "{\"events\":[{\"type\":\"type\",\"text\":\"z\",\"delay_ms\":0}]}", "kd29 w30 ku29");

    expectRefused("key", "{\"key\":\"o\",\"modifiers\":[\"alt\"]}", "did you mean `mods`?");
    expectRefused("key", "{\"key\":\"nosuchkey\"}", "Keys: a-z");
    expectRefused("key", "{}", "either `key` or `scancode`");
    expectRefused("key", "{\"key\":\"a\",\"scancode\":4}", "either `key` or `scancode`");
    expectRefused("key", "{\"key\":\"a\",\"mods\":[\"hyper\"]}", "unknown modifier");
    expectRefused("key", "{\"key\":\"a\",\"action\":\"tap\"}", "press, down or up");
    expectRefused("key", "{\"key\":\"a\",\"hold_ms\":1.5}", "whole number");
    expectRefused("key", "{\"key\":\"a\",\"hold_ms\":-1}", "from 0 to");
    expectRefused("text", "{\"text\":\"caf\xC3\xA9\"}", "character 3");
    expectRefused("text", "{}", "`text` is required");
    expectRefused("mouse", "{\"x\":1}", "both `x` and `y`");
    expectRefused("mouse", "{\"x\":1,\"y\":1,\"dx\":1}", "not both");
    expectRefused("mouse", "{\"action\":\"click\",\"dx\":1}", "only go with move");
    expectRefused("mouse", "{\"action\":\"move\"}", "move needs");
    expectRefused("mouse", "{\"action\":\"drag\",\"x\":1,\"y\":1}", "move, down, up, click or wheel");
    expectRefused("mouse", "{\"button\":\"side\",\"x\":1,\"y\":1}", "left, middle or right");
    expectRefused("wait", "{}", "`ms` is required");
    expectRefused("batch", "[{\"type\":\"key\",\"key\":\"a\"},{\"type\":\"key\",\"kye\":\"b\"}]", "events[1]: unknown field `kye`");
    expectRefused("batch", "[{\"key\":\"a\"}]", "`type` is required");
    expectRefused("batch", "{\"events\":[],\"extra\":1}", "only `events`");
    expectRefused("batch", "[1]", "must be an object");
    expectRefused("key", "not json", "not JSON");
    expectRefused("key", "[{\"key\":\"a\"}]", "must be an object");

    // Every named key reaches the X server as some key.
    XKeyboard::init();
    std::string names = InputControl::keyNames();
    size_t start = names.find("kp_0-kp_9, ");
    names = names.substr(start + strlen("kp_0-kp_9, "));
    names = names.substr(0, names.find(", or one character"));
    std::vector<std::string> keys = { "a", "z", "0", "9", "f1", "f12", "kp_0", "kp_9" };
    size_t pos = 0;
    while (pos < names.size()) {
        size_t comma = names.find(", ", pos);
        keys.push_back(names.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos));
        pos = comma == std::string::npos ? names.size() : comma + 2;
    }
    for (const std::string& key : keys) {
        std::vector<Event> events;
        std::string error;
        if (!InputControl::parse("key", "{\"key\":\"" + key + "\",\"hold_ms\":0}", events, error) || events.empty()) {
            testFail("key %s was refused: %s", key.c_str(), error.c_str());
            continue;
        }
        if (!XKeyboard::sdl2x11(events[0].scancode)) {
            testFail("key %s (scancode %u) has no X key", key.c_str(), events[0].scancode);
        }
    }
}

void testInputControlEndToEnd() {
    std::mutex mutex;
    std::vector<Event> received;
    bool failNext = false;
    InputControl::setSink([&](const Event& e) -> std::string {
        std::lock_guard<std::mutex> lock(mutex);
        if (failNext) {
            failNext = false;
            return "no display";
        }
        received.push_back(e);
        return std::string();
    });
    startInputControl();
    std::string error;
    if (!LocalServer::start(0, error)) {
        testFail("server did not start: %s", error.c_str());
        InputControl::setSink(nullptr);
        return;
    }
    U32 port = LocalServer::port();

    std::string response = httpRequest(port, "POST", "/input/key", "{\"key\":\"enter\",\"hold_ms\":0}");
    if (response.find("HTTP/1.1 200") != 0 || response.find("{\"delivered\":2}") == std::string::npos) {
        testFail("key press was not answered with 2 delivered: %s", response.c_str());
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (describe(received) != "kd40 ku40") {
            testFail("sink got \"%s\"", describe(received).c_str());
        }
        received.clear();
    }

    response = httpRequest(port, "POST", "/input/batch", "[{\"type\":\"key\",\"key\":\"a\"},{\"type\":\"key\",\"kye\":\"b\"}]");
    if (response.find("HTTP/1.1 400") != 0 || response.find("events[1]") == std::string::npos) {
        testFail("bad batch was not a 400: %s", response.c_str());
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!received.empty()) {
            testFail("a refused batch delivered events");
        }
        failNext = true;
    }

    response = httpRequest(port, "POST", "/input/mouse", "{\"action\":\"click\",\"x\":3,\"y\":4,\"hold_ms\":0}");
    if (response.find("HTTP/1.1 503") != 0 || response.find("\"delivered\":0") == std::string::npos || response.find("no display") == std::string::npos) {
        testFail("a failed delivery was not a 503: %s", response.c_str());
    }

    response = httpRequest(port, "POST", "/input/wait?wait=false", "{\"ms\":10}");
    if (response.find("HTTP/1.1 202") != 0 || response.find("\"queued\":1") == std::string::npos) {
        testFail("wait=false was not a 202: %s", response.c_str());
    }

    response = httpRequest(port, "GET", "/input/key");
    if (response.find("HTTP/1.1 404") != 0 && response.find("HTTP/1.1 405") != 0) {
        testFail("GET of an input route was served: %s", response.substr(0, 40).c_str());
    }

    response = httpRequest(port, "GET", "/status");
    if (response.find("HTTP/1.1 200") != 0 || response.find("\"processes\":[") == std::string::npos || response.find("\"uptime_ms\":") == std::string::npos) {
        testFail("status was wrong: %s", response.c_str());
    }

    // Let the background wait finish before the sink goes away.
    std::string ignored;
    InputControl::run({}, ignored);
    LocalServer::stop();
    InputControl::setSink(nullptr);
    InputControl::reset();
}

#endif

#endif
