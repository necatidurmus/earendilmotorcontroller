#ifndef TERMINAL_INTERFACE_H
#define TERMINAL_INTERFACE_H

#include <Arduino.h>
#include "config.h"

class TerminalInterface {
public:
    void begin(unsigned long baud = TERMINAL_BAUD);
    bool update();
    const char* readLine();

    void println(const char* msg);
    void println(const String& msg);
    void print(const char* msg);
    void print(const String& msg);

    template<typename T> void print(const T& value) { TERMINAL_UART.print(value); }
    template<typename T> void println(const T& value) { TERMINAL_UART.println(value); }
    void println() { TERMINAL_UART.println(); }

    void showPrompt();

private:
    char    _lineBuffer[TERMINAL_LINE_MAX];
    char    _lineCopy[TERMINAL_LINE_MAX];
    uint8_t _linePos = 0;
    bool    _lineReady = false;
    bool    _overflowed = false;

    void resetBuffer();
};

extern TerminalInterface terminal;

#endif // TERMINAL_INTERFACE_H
