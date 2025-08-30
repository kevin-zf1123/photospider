#include "cli/terminal_input.hpp"
#include <iostream>

#ifdef _WIN32
// Windows implementation
#include <conio.h>

namespace ps {

void TerminalInput::SetRaw() {
    if (h_in_ == INVALID_HANDLE_VALUE) return;
    DWORD new_mode = original_mode_;
    new_mode &= ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT);
    SetConsoleMode(h_in_, new_mode);
}

void TerminalInput::Restore() {
    if (h_in_ != INVALID_HANDLE_VALUE) {
        SetConsoleMode(h_in_, original_mode_);
    }
}

TerminalInput::TerminalInput() {
    h_in_ = GetStdHandle(STD_INPUT_HANDLE);
    if (h_in_ != INVALID_HANDLE_VALUE) {
        GetConsoleMode(h_in_, &original_mode_);
        SetRaw();
    }
}

TerminalInput::~TerminalInput() {
    Restore();
}

int TerminalInput::GetChar() {
    int ch = _getch();
    if (ch == 3) { // Ctrl+C
        return CTRL_C;
    }
    if (ch == 0 || ch == 224) { // Special key
        ch = _getch();
        switch (ch) {
            case 72: return UP;
            case 80: return DOWN;
            case 75: return LEFT;
            case 77: return RIGHT;
            case 83: return DEL;
            default: return UNKNOWN;
        }
    }
    switch (ch) {
        case 8:  return BACKSPACE;
        case 9:  return TAB;
        case 13: return ENTER;
        case 27: return ESC;
        default: return ch; // Printable char
    }
}

} // namespace ps

#else
// Unix (Linux/macOS) implementation
#include <unistd.h>

namespace ps {

void TerminalInput::SetRaw() {
    struct termios raw = original_termios_;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

void TerminalInput::Restore() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_termios_);
}

TerminalInput::TerminalInput() {
    if (tcgetattr(STDIN_FILENO, &original_termios_) != -1) {
        SetRaw();
    }
}

TerminalInput::~TerminalInput() {
    Restore();
}

int TerminalInput::GetChar() {
    char c;
    if (read(STDIN_FILENO, &c, 1) != 1) return UNKNOWN;

    if (c == 3) {
        return CTRL_C;
    } else if (c == '\x1b') {
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return ESC;
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return UNKNOWN;
        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) return UNKNOWN;
                if (seq[2] == '~') {
                    switch (seq[1]) {
                        case '3': return DEL;
                        default: return UNKNOWN;
                    }
                }
            } else {
                switch (seq[1]) {
                    case 'A': return UP;
                    case 'B': return DOWN;
                    case 'C': return RIGHT;
                    case 'D': return LEFT;
                }
            }
        }
        return UNKNOWN;
    } else if (c == 127 || c == 8) { // Backspace on Mac/Linux
        return BACKSPACE;
    } else if (c == '\n' || c == '\r') {
        return ENTER;
    } else if (c == '\t') {
        return TAB;
    }

    return c;
}

} // namespace ps

#endif
