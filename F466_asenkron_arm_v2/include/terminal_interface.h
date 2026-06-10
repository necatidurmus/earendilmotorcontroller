#ifndef TERMINAL_INTERFACE_H
#define TERMINAL_INTERFACE_H

#include <Arduino.h>
#include "config.h"

class TerminalInterface {
public:
    void begin(uint32_t baud);
    bool update();
    const char* readLine();
    void showPrompt();

    template<typename T> void print(const T& value) { TERMINAL_UART.print(value); }
    template<typename T> void println(const T& value) { TERMINAL_UART.println(value); }
    void println() { TERMINAL_UART.println(); }

private:
    char line_[TERMINAL_LINE_MAX] = {0};
    size_t pos_ = 0;
    bool lineReady_ = false;
    bool overflowed_ = false;
};

#endif // TERMINAL_INTERFACE_H
