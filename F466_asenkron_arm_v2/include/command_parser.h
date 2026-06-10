#ifndef COMMAND_PARSER_H
#define COMMAND_PARSER_H

#include <Arduino.h>
#include "types.h"

class CommandParser {
public:
    Command parse(const char* line);
    void printHelp();

private:
    static bool parseMotorName(const char* token, uint8_t* motorIdOut);
    static bool parsePwm(const char* token, int* pwmOut);
};

#endif // COMMAND_PARSER_H
