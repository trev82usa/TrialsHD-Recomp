// Keyboard controls: the keyboard acts as controller 1 (arrows/WASD ride, Enter/Space select...).
// Our own input driver because the SDK's keyboard mode (mnk_mode) also grabs the mouse, which
// breaks the overlay menus. Works alongside a real controller.
#pragma once

#include <memory>

namespace rex::ui {
class Window;
}
namespace rex::input {
class InputDriver;
class InputSystem;
}

// Creates the driver (add it to the InputSystem) and starts listening to the window's keys.
std::unique_ptr<rex::input::InputDriver> CreateKeyboardDriver(rex::ui::Window* window);

// Call right after input->AddDriver(<the keyboard driver>): puts it first in the SDK's list so
// the game's "controller connected?" / "button pressed?" questions reach it.
bool MoveKeyboardDriverFirst(rex::input::InputSystem* input, rex::input::InputDriver* ours);
