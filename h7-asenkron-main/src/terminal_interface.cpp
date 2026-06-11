#include "terminal_interface.h"

void TerminalInterface::begin(unsigned long baud) {
    TERMINAL_UART.begin(baud);
    resetBuffer();
}

bool TerminalInterface::update() {
    if (_lineReady) {
        return true;
    }

    while (TERMINAL_UART.available()) {
        char c = (char)TERMINAL_UART.read();

        if (c == '\r') continue;

        if (c == '\n') {
            if (_linePos == 0) continue;
            _lineBuffer[_linePos] = '\0';
            _lineReady = true;
            if (_overflowed) {
                _overflowed = false;
                TERMINAL_UART.println("WARN: terminal line was too long; truncated.");
            }
            return true;
        }

        if (c == '\b' || c == 127) {
            if (_linePos > 0) _linePos--;
            continue;
        }

        if (_linePos < (TERMINAL_LINE_MAX - 1)) {
            _lineBuffer[_linePos++] = c;
        } else {
            _overflowed = true;
        }
    }

    return false;
}

const char* TerminalInterface::readLine() {
    static char copy[TERMINAL_LINE_MAX];
    memcpy(copy, _lineBuffer, _linePos + 1);
    resetBuffer();
    return copy;
}

void TerminalInterface::resetBuffer() {
    _linePos = 0;
    _lineReady = false;
    _overflowed = false;
    _lineBuffer[0] = '\0';
}

void TerminalInterface::println(const char* msg) {
    TERMINAL_UART.println(msg);
}

void TerminalInterface::println(const String& msg) {
    TERMINAL_UART.println(msg);
}

void TerminalInterface::print(const char* msg) {
    TERMINAL_UART.print(msg);
}

void TerminalInterface::print(const String& msg) {
    TERMINAL_UART.print(msg);
}

void TerminalInterface::showPrompt() {
    TERMINAL_UART.print(TERMINAL_PROMPT);
}
