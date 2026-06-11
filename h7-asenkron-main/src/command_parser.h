#ifndef COMMAND_PARSER_H
#define COMMAND_PARSER_H

#include "types.h"
#include "config.h"
#include <Arduino.h>

class CommandParser {
public:
    Command parse(const char* line);
    static CommandType identifyCommand(const char* token);
    static void printHelp();

private:
    static bool equalsIgnoreCase(const char* a, const char* b);
    static bool parsePWM(const char* token, int& pwm);
    static bool parseInteger(const char* token, long& value);
    static bool parseMotorName(const char* token, uint8_t& motorIdOut);

    static bool validateArmCommand(const char* payload, const char*& error);
    static bool validateArmSetValue(const char* field, long value, const char*& error);
    static bool isValidArmId(long id);
};

#endif // COMMAND_PARSER_H