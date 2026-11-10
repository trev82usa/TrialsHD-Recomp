// In-game settings menu (F1 or LB+RB+X on a controller): player name/password, online server,
// graphics and sound, saved to trialshd.toml next to the game.
#pragma once

#include <filesystem>

namespace rex::ui {
class ImGuiDrawer;
class Window;
}  // namespace rex::ui
namespace rex::input {
class InputSystem;
}

// Call from OnCreateDialogs (UI thread): remembers where to draw and starts the controller
// watcher.
void InitTrialsMenu(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window,
                    const std::filesystem::path& config_path);
// Call once the runtime exists (OnPostSetup): lets the menu read controllers and hide them
// from the game while it's open.
void ConnectTrialsMenuInput(rex::input::InputSystem* input);
// Opens the menu if closed, closes it if open (UI thread).
void ToggleTrialsMenu();
bool TrialsMenuOpen();
// True while the menu itself is reading the controllers (on this thread).
bool TrialsMenuReadingPads();
