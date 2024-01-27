#pragma once
#include <string>
class Parser {
public:
    const char* skipWhitespace(const char* json);
    const char* parseStr(const char* json, std::string& value);
    int parseValue(const char* json, bool* mode, int* speed, int* pressure);


private:
    std::string currentKey;
    int returnValue = 0;
};