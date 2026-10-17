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

#ifndef __SERVER_JSON_H__
#define __SERVER_JSON_H__

#include <string>
#include <vector>
#include <utility>

// A JSON value for the local server's request bodies (RFC 8259). Objects keep
// their members in order, and a duplicate name is refused while parsing.
class JsonValue {
public:
    enum Type { Null, Bool, Number, String, Array, Object };

    Type type = Null;
    bool boolValue = false;
    double number = 0;
    std::string string;
    std::vector<JsonValue> items;
    std::vector<std::pair<std::string, JsonValue>> members;

    // nullptr when this is not an object or has no member with that name.
    const JsonValue* get(const std::string& name) const;

    static const char* typeName(Type type);

    // Parses text, which must hold exactly one value. On failure returns
    // false and fills error with what was wrong and where.
    static bool parse(const std::string& text, JsonValue& result, std::string& error);
};

#endif
