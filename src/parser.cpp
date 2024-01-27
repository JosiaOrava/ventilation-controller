#include "parser.h"

const char* Parser::skipWhitespace(const char* json) {
    while (*json && (*json == ' ' || *json == '\t' || *json == '\n' || *json == '\r')) {
        ++json;
    }
    return json;
}

const char* Parser::parseStr(const char* json, std::string& value) {
    json = skipWhitespace(json);
    if (*json != '"') {
        return nullptr; // Not a valid string
    }
    ++json; // Skip the opening double quote

    while (*json && *json != '"') {
        value += *json;
        ++json;
    }
    if (*json == '"') {
        ++json; // Skip the closing double quote
        return json;
    }
    return nullptr;
}

int Parser::parseValue(const char* json, bool* mode, int* speed, int* pressure) {
    // remove whitespace if any
    json = skipWhitespace(json);

    if (*json == '{') {
        ++json; // next char

        while (*json != '}') {
            currentKey = ""; // resetting key value
            json = parseStr(json, currentKey);
            ++json;

            if (currentKey == "auto") {
                if (*json == 't') {
                    *mode = true;
                    json += 4; // Skip "true"
                }
                else if (*json == 'f') {
                    *mode = false;
                    json += 5; // Skip "false"
                }
                else {
                    return returnValue; // Invalid boolean value
                }
            }
            else if (currentKey == "speed" || currentKey == "pressure") {
                int value = std::stoi(json);

                // Change the right value
                if (currentKey == "speed") *speed = value, returnValue = 1;
                else if (currentKey == "pressure") *pressure = value, returnValue = 2;
                // Move forward depending how big number
                if (value < 10) {
                    json = json + 1;
                }
                else if (value < 100) {
                    json = json + 2;
                }
                else {
                    json = json + 3;
                }
            }
            else {
                // Skip unknown keys
                while (*json != ',' && *json != '}') {
                    ++json;
                }
            }
            if (*json == ',') {
                ++json;
            }
        }
        if (*json == '}') {
            return returnValue;
        }
        return returnValue;
    }
    return returnValue;
}