#include "terminal_interface.h"

void TerminalInterface::begin(uint32_t baud) {
    TERMINAL_UART.begin(baud);
}

bool TerminalInterface::update() {
    if (lineReady_) return true;

    while (TERMINAL_UART.available() > 0) {
        char c = static_cast<char>(TERMINAL_UART.read());

        if (c == '\r') continue;

        if (c == '\n') {
            line_[pos_] = '\0';
            pos_ = 0;
            lineReady_ = true;
            if (overflowed_) {
                overflowed_ = false;
                TERMINAL_UART.println("ERROR: terminal line was too long; truncated.");
            }
            return true;
        }

        if (pos_ < (TERMINAL_LINE_MAX - 1)) {
            line_[pos_++] = c;
        } else {
            // Do not allow buffer overrun. Ignore the rest until newline.
            overflowed_ = true;
        }
    }

    return false;
}

const char* TerminalInterface::readLine() {
    lineReady_ = false;
    return line_;
}

void TerminalInterface::showPrompt() {
    TERMINAL_UART.print(TERMINAL_PROMPT);
}
