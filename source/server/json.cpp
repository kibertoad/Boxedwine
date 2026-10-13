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
#include "json.h"
#include <stdlib.h>

// Bodies are small; this stops a deeply nested one from exhausting the stack.
static const U32 MAX_DEPTH = 64;

namespace {
    class Parser {
    public:
        Parser(const std::string& text) : text(text) {}

        bool parseDocument(JsonValue& result, std::string& error) {
            skipSpace();
            if (!parseValue(result, 0)) {
                error = this->error;
                return false;
            }
            skipSpace();
            if (pos != text.size()) {
                fail("unexpected text after the value");
                error = this->error;
                return false;
            }
            return true;
        }

    private:
        const std::string& text;
        size_t pos = 0;
        std::string error;

        bool fail(const std::string& message) {
            if (error.empty()) {
                error = message + " at offset " + std::to_string(pos);
            }
            return false;
        }

        void skipSpace() {
            while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\n' || text[pos] == '\r')) {
                pos++;
            }
        }

        bool literal(const char* word) {
            size_t len = strlen(word);
            if (text.compare(pos, len, word) != 0) {
                return fail("invalid literal");
            }
            pos += len;
            return true;
        }

        bool parseValue(JsonValue& value, U32 depth) {
            if (depth > MAX_DEPTH) {
                return fail("nesting too deep");
            }
            if (pos >= text.size()) {
                return fail("unexpected end of input");
            }
            char c = text[pos];
            if (c == '{') {
                return parseObject(value, depth);
            }
            if (c == '[') {
                return parseArray(value, depth);
            }
            if (c == '"') {
                value.type = JsonValue::String;
                return parseString(value.string);
            }
            if (c == 't') {
                value.type = JsonValue::Bool;
                value.boolValue = true;
                return literal("true");
            }
            if (c == 'f') {
                value.type = JsonValue::Bool;
                value.boolValue = false;
                return literal("false");
            }
            if (c == 'n') {
                value.type = JsonValue::Null;
                return literal("null");
            }
            if (c == '-' || (c >= '0' && c <= '9')) {
                return parseNumber(value);
            }
            return fail(std::string("unexpected character '") + c + "'");
        }

        bool parseObject(JsonValue& value, U32 depth) {
            value.type = JsonValue::Object;
            pos++;
            skipSpace();
            if (pos < text.size() && text[pos] == '}') {
                pos++;
                return true;
            }
            while (true) {
                skipSpace();
                if (pos >= text.size() || text[pos] != '"') {
                    return fail("expected a member name");
                }
                std::string name;
                if (!parseString(name)) {
                    return false;
                }
                if (value.get(name)) {
                    return fail("duplicate member \"" + name + "\"");
                }
                skipSpace();
                if (pos >= text.size() || text[pos] != ':') {
                    return fail("expected ':'");
                }
                pos++;
                skipSpace();
                value.members.emplace_back(name, JsonValue());
                if (!parseValue(value.members.back().second, depth + 1)) {
                    return false;
                }
                skipSpace();
                if (pos < text.size() && text[pos] == ',') {
                    pos++;
                    continue;
                }
                if (pos < text.size() && text[pos] == '}') {
                    pos++;
                    return true;
                }
                return fail("expected ',' or '}'");
            }
        }

        bool parseArray(JsonValue& value, U32 depth) {
            value.type = JsonValue::Array;
            pos++;
            skipSpace();
            if (pos < text.size() && text[pos] == ']') {
                pos++;
                return true;
            }
            while (true) {
                skipSpace();
                value.items.emplace_back();
                if (!parseValue(value.items.back(), depth + 1)) {
                    return false;
                }
                skipSpace();
                if (pos < text.size() && text[pos] == ',') {
                    pos++;
                    continue;
                }
                if (pos < text.size() && text[pos] == ']') {
                    pos++;
                    return true;
                }
                return fail("expected ',' or ']'");
            }
        }

        bool parseHex4(U32& result) {
            if (pos + 4 > text.size()) {
                return fail("truncated \\u escape");
            }
            result = 0;
            for (int i = 0; i < 4; i++) {
                char c = text[pos++];
                result <<= 4;
                if (c >= '0' && c <= '9') {
                    result |= (U32)(c - '0');
                } else if (c >= 'a' && c <= 'f') {
                    result |= (U32)(c - 'a' + 10);
                } else if (c >= 'A' && c <= 'F') {
                    result |= (U32)(c - 'A' + 10);
                } else {
                    return fail("invalid \\u escape");
                }
            }
            return true;
        }

        static void appendUtf8(std::string& out, U32 cp) {
            if (cp < 0x80) {
                out += (char)cp;
            } else if (cp < 0x800) {
                out += (char)(0xC0 | (cp >> 6));
                out += (char)(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                out += (char)(0xE0 | (cp >> 12));
                out += (char)(0x80 | ((cp >> 6) & 0x3F));
                out += (char)(0x80 | (cp & 0x3F));
            } else {
                out += (char)(0xF0 | (cp >> 18));
                out += (char)(0x80 | ((cp >> 12) & 0x3F));
                out += (char)(0x80 | ((cp >> 6) & 0x3F));
                out += (char)(0x80 | (cp & 0x3F));
            }
        }

        bool parseString(std::string& out) {
            pos++; // opening quote
            while (true) {
                if (pos >= text.size()) {
                    return fail("unterminated string");
                }
                unsigned char c = (unsigned char)text[pos++];
                if (c == '"') {
                    return true;
                }
                if (c < 0x20) {
                    return fail("control character in string");
                }
                if (c != '\\') {
                    out += (char)c;
                    continue;
                }
                if (pos >= text.size()) {
                    return fail("unterminated string");
                }
                char e = text[pos++];
                switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    U32 cp = 0;
                    if (!parseHex4(cp)) {
                        return false;
                    }
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        U32 low = 0;
                        if (text.compare(pos, 2, "\\u") != 0) {
                            return fail("unpaired surrogate");
                        }
                        pos += 2;
                        if (!parseHex4(low) || low < 0xDC00 || low > 0xDFFF) {
                            return fail("unpaired surrogate");
                        }
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return fail("unpaired surrogate");
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default:
                    return fail("invalid escape");
                }
            }
        }

        bool parseNumber(JsonValue& value) {
            size_t start = pos;
            if (text[pos] == '-') {
                pos++;
            }
            if (pos >= text.size() || !isdigit((unsigned char)text[pos])) {
                return fail("invalid number");
            }
            if (text[pos] == '0') {
                pos++;
            } else {
                while (pos < text.size() && isdigit((unsigned char)text[pos])) {
                    pos++;
                }
            }
            if (pos < text.size() && text[pos] == '.') {
                pos++;
                if (pos >= text.size() || !isdigit((unsigned char)text[pos])) {
                    return fail("invalid number");
                }
                while (pos < text.size() && isdigit((unsigned char)text[pos])) {
                    pos++;
                }
            }
            if (pos < text.size() && (text[pos] == 'e' || text[pos] == 'E')) {
                pos++;
                if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) {
                    pos++;
                }
                if (pos >= text.size() || !isdigit((unsigned char)text[pos])) {
                    return fail("invalid number");
                }
                while (pos < text.size() && isdigit((unsigned char)text[pos])) {
                    pos++;
                }
            }
            value.type = JsonValue::Number;
            value.number = strtod(text.substr(start, pos - start).c_str(), nullptr);
            return true;
        }
    };
}

const JsonValue* JsonValue::get(const std::string& name) const {
    if (type != Object) {
        return nullptr;
    }
    for (auto& member : members) {
        if (member.first == name) {
            return &member.second;
        }
    }
    return nullptr;
}

const char* JsonValue::typeName(Type type) {
    switch (type) {
    case Null: return "null";
    case Bool: return "a boolean";
    case Number: return "a number";
    case String: return "a string";
    case Array: return "an array";
    case Object: return "an object";
    }
    return "unknown";
}

bool JsonValue::parse(const std::string& text, JsonValue& result, std::string& error) {
    result = JsonValue();
    Parser parser(text);
    return parser.parseDocument(result, error);
}
