// moved to include/cli/terminal_input.hpp
#pragma once

#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <termios.h>
#endif

namespace ps {

// Special key codes returned by GetChar
enum Key {
    // Printable keys are returned as their char value
    UP = 1000,
    DOWN,
    LEFT,
    RIGHT,
    BACKSPACE,
    ENTER,
    TAB,
    DEL,
    ESC,
    CTRL_C,
    UNKNOWN
};

class TerminalInput {
public:
    TerminalInput();
    ~TerminalInput();
    int GetChar();

    // New methods to manage terminal state
    void Restore();
    void SetRaw();

private:
#ifdef _WIN32
    HANDLE h_in_;
    DWORD original_mode_;
#else
    struct termios original_termios_;
#endif
};

} // namespace ps
