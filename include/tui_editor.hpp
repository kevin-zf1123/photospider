#pragma once
#include "ftxui/component/screen_interactive.hpp"

// A base class for our interactive editors to share the screen instance.
class TuiEditor {
public:
    explicit TuiEditor(ftxui::ScreenInteractive& screen) : screen_(screen) {}
    virtual ~TuiEditor() = default;
    virtual void Run() = 0; // Each editor must implement its own Run loop.

protected:
    ftxui::ScreenInteractive& screen_;
};