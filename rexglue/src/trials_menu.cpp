// In-game settings menu (F1). An ImGui overlay drawn by the SDK's overlay system (like the F3/F4
// overlays). Edits are kept in the menu until "Save", which applies them to the running game
// (rex::cvar) and writes them into trialshd.toml, replacing only the affected "key = value" lines
// so the file's comments stay as they are.
#include "trials_menu.h"

#include "http_client.h"

#include <imgui.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/input/input.h>
#include <rex/input/input_system.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/window.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

REXCVAR_DEFINE_INT32(trialshd_menu_size, 0, "TrialsHD",
                     "Size of the F1 menu: 0 = automatic (from screen size), 1 = small, "
                     "2 = medium, 3 = large, 4 = extra large");

namespace {

constexpr const char* kDefaultServer = "https://trials.bytrev.org";

std::string GetStr(const char* name) {
  return rex::cvar::GetFlagInfo(name) ? rex::cvar::Query<std::string>(name) : std::string();
}
bool Has(const char* name) { return rex::cvar::GetFlagInfo(name) != nullptr; }
bool GetBool(const char* name, bool def) {
  return Has(name) ? rex::cvar::Query<bool>(name) : def;
}
int GetInt(const char* name, int def) {
  return Has(name) ? rex::cvar::Query<int32_t>(name) : def;
}
double GetDouble(const char* name, double def) {
  return Has(name) ? rex::cvar::Query<double>(name) : def;
}
// Bloom/DoF as they were when the game started (what the shaders were built with), in percent.
int BootPct(const char* name) {
  static const int bloom = int(GetDouble("trialshd_bloom", 1.0) * 100 + 0.5);
  static const int dof = int(GetDouble("trialshd_dof", 1.0) * 100 + 0.5);
  static const int bright = int(GetDouble("trialshd_brightness", 1.0) * 100 + 0.5);
  if (std::strcmp(name, "trialshd_bloom") == 0) return bloom;
  if (std::strcmp(name, "trialshd_brightness") == 0) return bright;
  return dof;
}
bool NeedsRestart(const char* name) {
  auto* info = rex::cvar::GetFlagInfo(name);
  return info && info->lifecycle != rex::cvar::Lifecycle::kHotReload;
}

bool ValidName(const std::string& n) {
  if (n.empty() || n.size() > 15) return false;
  return std::all_of(n.begin(), n.end(), [](unsigned char c) {
    return std::isalnum(c) || c == ' ' || c == '_' || c == '-';
  });
}

std::string TomlQuote(const std::string& s) {
  std::string out = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  return out + "\"";
}

// Replace "key = ..." lines in the toml file (keeping everything else), appending keys that are
// not there yet. values: key -> already-formatted toml value.
bool WriteTomlKeys(const std::filesystem::path& path,
                   const std::vector<std::pair<std::string, std::string>>& values,
                   std::string* error) {
  std::vector<std::string> lines;
  {
    std::ifstream in(path, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      lines.push_back(line);
    }
  }
  std::vector<bool> done(values.size(), false);
  for (auto& line : lines) {
    size_t i = line.find_first_not_of(" \t");
    if (i == std::string::npos || line[i] == '#') continue;
    size_t eq = line.find('=', i);
    if (eq == std::string::npos) continue;
    std::string key = line.substr(i, eq - i);
    while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
    for (size_t k = 0; k < values.size(); ++k) {
      if (values[k].first == key) {
        line = key + " = " + values[k].second;
        done[k] = true;
      }
    }
  }
  bool header = false;
  for (size_t k = 0; k < values.size(); ++k) {
    if (done[k]) continue;
    if (!header) {
      lines.push_back("");
      lines.push_back("# --- Set from the in-game menu (F1) ---");
      header = true;
    }
    lines.push_back(values[k].first + " = " + values[k].second);
  }
  std::filesystem::path tmp = path;
  tmp += ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
      *error = "could not write " + tmp.string();
      return false;
    }
    for (auto& l : lines) out << l << "\n";
    if (!out.good()) {
      *error = "could not write " + tmp.string();
      return false;
    }
  }
  std::error_code ec;
  std::filesystem::rename(tmp, path, ec);
  if (ec) {
    *error = "could not replace " + path.string() + ": " + ec.message();
    return false;
  }
  return true;
}

// Result of "Test connection", filled in by a background thread.
struct TestState {
  std::mutex mu;
  bool running = false;
  bool ok = false;
  std::string message;
};

class TrialsMenu;
TrialsMenu* g_menu = nullptr;

// --- Controller ------------------------------------------------------------------------------
// The menu reads the pads itself (on the UI thread). While the menu is open, or while both
// bumpers are held (the open/close shortcut LB+RB+X), the game is shown an untouched
// controller, so the shortcut and menu navigation don't also steer the bike.
constexpr uint16_t kBtnUp = 0x0001, kBtnDown = 0x0002, kBtnLeft = 0x0004, kBtnRight = 0x0008,
                   kBtnStart = 0x0010, kBtnBack = 0x0020, kBtnLB = 0x0100, kBtnRB = 0x0200,
                   kBtnA = 0x1000, kBtnB = 0x2000, kBtnX = 0x4000, kBtnY = 0x8000;

struct Pad {
  uint16_t buttons = 0;
  float lx = 0, ly = 0;  // -1..1, y up
};

rex::input::InputSystem* g_input = nullptr;
rex::ui::ImGuiDrawer* g_drawer = nullptr;
rex::ui::Window* g_window = nullptr;
std::filesystem::path g_config_path;
thread_local bool t_menu_reading = false;  // our own reads see the real controller
std::atomic<bool> g_block_game{false};
Pad g_pad;  // this frame's combined state of all pads (UI thread only)

float Axis(int16_t v) {
  float f = v / 32767.0f;
  return std::fabs(f) < 0.25f ? 0.0f : std::clamp(f, -1.0f, 1.0f);
}

Pad ReadPads() {
  Pad p;
  if (!g_input) return p;
  t_menu_reading = true;
  for (uint32_t user = 0; user < 4; ++user) {
    rex::input::X_INPUT_STATE st{};
    if (g_input->GetState(user, &st) != 0) continue;
    p.buttons |= uint16_t(st.gamepad.buttons);
    float lx = Axis(int16_t(st.gamepad.thumb_lx)), ly = Axis(int16_t(st.gamepad.thumb_ly));
    if (std::fabs(lx) > std::fabs(p.lx)) p.lx = lx;
    if (std::fabs(ly) > std::fabs(p.ly)) p.ly = ly;
  }
  t_menu_reading = false;
  return p;
}

void ToggleMenuInternal();

class TrialsMenu final : public rex::ui::ImGuiDialog {
 public:
  TrialsMenu(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window,
             std::filesystem::path config_path)
      : ImGuiDialog(drawer), window_(window), config_path_(std::move(config_path)) {
    Load();
  }

  void RequestClose() { Close(); }

  float S(float v) const { return v * scale_; }

  // Called on close so no gamepad key stays "held" inside ImGui.
  static void ReleaseGamepad(ImGuiIO& io) {
    for (ImGuiKey k = ImGuiKey_GamepadStart; k <= ImGuiKey_GamepadRStickDown;
         k = ImGuiKey(k + 1))
      io.AddKeyAnalogEvent(k, false, 0.0f);
  }

 protected:
  void OnClose() override {
    ReleaseGamepad(GetIO());
    if (g_menu == this) g_menu = nullptr;
  }

  void FeedGamepad(ImGuiIO& io) {
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    const Pad p = io.WantTextInput ? Pad{} : g_pad;
    auto key = [&](ImGuiKey k, bool down) { io.AddKeyEvent(k, down); };
    key(ImGuiKey_GamepadFaceDown, p.buttons & kBtnA);
    key(ImGuiKey_GamepadFaceRight, p.buttons & kBtnB);
    key(ImGuiKey_GamepadFaceLeft, p.buttons & kBtnX);
    key(ImGuiKey_GamepadFaceUp, p.buttons & kBtnY);
    key(ImGuiKey_GamepadDpadUp, p.buttons & kBtnUp);
    key(ImGuiKey_GamepadDpadDown, p.buttons & kBtnDown);
    key(ImGuiKey_GamepadDpadLeft, p.buttons & kBtnLeft);
    key(ImGuiKey_GamepadDpadRight, p.buttons & kBtnRight);
    key(ImGuiKey_GamepadStart, p.buttons & kBtnStart);
    key(ImGuiKey_GamepadBack, p.buttons & kBtnBack);
    // Bumpers drive tab switching only when pressed alone (LB+RB is the shortcut).
    bool both = (p.buttons & kBtnLB) && (p.buttons & kBtnRB);
    key(ImGuiKey_GamepadL1, !both && (p.buttons & kBtnLB));
    key(ImGuiKey_GamepadR1, !both && (p.buttons & kBtnRB));
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickLeft, p.lx < 0, std::max(0.0f, -p.lx));
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickRight, p.lx > 0, std::max(0.0f, p.lx));
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickUp, p.ly > 0, std::max(0.0f, p.ly));
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickDown, p.ly < 0, std::max(0.0f, -p.ly));
  }

  void OnDraw(ImGuiIO& io) override {
    FeedGamepad(io);
    // Menu size: automatic from the screen height (bigger on big screens), or the user's pick.
    static const float kSizes[] = {0, 1.0f, 1.35f, 1.7f, 2.1f};
    if (menu_size_ >= 1 && menu_size_ <= 4) {
      scale_ = kSizes[menu_size_];
    } else {
      scale_ = std::clamp(io.DisplaySize.y / 820.0f, 1.0f, 2.4f);
    }
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * scale_);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(8), S(5)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(10), S(7)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(16), S(14)));
    DrawWindow(io);
    ImGui::PopStyleVar(3);
    ImGui::PopFont();
  }

  void DrawWindow(ImGuiIO& io) {
    const float w = std::min(S(640), io.DisplaySize.x - 40.0f);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w, 0), ImGuiCond_Always);
    bool open = true;
    if (!ImGui::Begin("Trials HD Settings", &open,
                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
      ImGui::End();
      if (!open) Close();
      return;
    }

    if (ImGui::IsWindowAppearing()) ImGui::SetWindowFocus();
    // Controller: LB/RB switch tabs, B closes (unless editing a text box).
    int select_tab = -1;
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false)) select_tab = (tab_ + 2) % 3;
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false)) select_tab = (tab_ + 1) % 3;
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) && !ImGui::IsAnyItemActive())
      open = false;

    if (ImGui::BeginTabBar("tabs")) {
      if (ImGui::BeginTabItem("Player & Online", nullptr,
                              select_tab == 0 ? ImGuiTabItemFlags_SetSelected : 0)) {
        tab_ = 0;
        DrawPlayerTab();
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Graphics & Sound", nullptr,
                              select_tab == 1 ? ImGuiTabItemFlags_SetSelected : 0)) {
        tab_ = 1;
        DrawGraphicsTab();
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Controls", nullptr,
                              select_tab == 2 ? ImGuiTabItemFlags_SetSelected : 0)) {
        tab_ = 2;
        DrawControlsTab();
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
    }

    ImGui::Separator();
    if (ImGui::Button("Save", ImVec2(S(120), 0))) Save();
    ImGui::SameLine();
    if (ImGui::Button("Close", ImVec2(S(120), 0))) open = false;
    ImGui::SameLine();
    if (!status_.empty()) {
      ImGui::TextColored(status_ok_ ? ImVec4(0.4f, 0.9f, 0.4f, 1) : ImVec4(1, 0.45f, 0.4f, 1),
                         "%s", status_.c_str());
    } else {
      ImGui::TextDisabled("F1 or LB+RB+X opens/closes this menu");
    }
    ImGui::End();
    if (!open) Close();
  }

 private:
  void Load() {
    std::snprintf(name_, sizeof(name_), "%s", GetStr("trialshd_player_name").c_str());
    std::snprintf(key_, sizeof(key_), "%s", GetStr("trialshd_player_key").c_str());
    std::string url = GetStr("trialshd_server_url");
    online_ = !url.empty();
    std::snprintf(server_, sizeof(server_), "%s", url.empty() ? kDefaultServer : url.c_str());
    res_scale_ = std::clamp(GetInt("resolution_scale", 1), 1, 3);
    vsync_ = GetBool("vsync", true);
    letterbox_ = GetBool("present_letterbox", true);
    aniso_ = std::clamp(GetInt("anisotropic_override", 3), -1, 5);
    fullscreen_ = window_ ? window_->IsFullscreen() : GetBool("fullscreen", false);
    mute_ = GetBool("audio_mute", false);
    menu_size_ = std::clamp(GetInt("trialshd_menu_size", 0), 0, 4);
    keyboard_ = GetBool("trialshd_keyboard", true);
    fps_cap_ = GetInt("trialshd_fps_cap", 0) > 0;
    BootPct("trialshd_bloom");
    bloom_pct_ = std::clamp(int(GetDouble("trialshd_bloom", 1.0) * 100 + 0.5), 0, 200);
    dof_pct_ = std::clamp(int(GetDouble("trialshd_dof", 1.0) * 100 + 0.5), 0, 100);
    bright_pct_ = std::clamp(int(GetDouble("trialshd_brightness", 1.0) * 100 + 0.5), 50, 150);
  }

  void Help(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
      ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
      ImGui::TextUnformatted(text);
      ImGui::PopTextWrapPos();
      ImGui::EndTooltip();
    }
  }

  // A 5%-step slider that also works with a controller / arrow keys (one press = one step).
  void PctSlider(const char* flag, const char* label, int* pct, int lo, int hi, const char* help) {
    if (!Has(flag)) return;
    int step = std::clamp((*pct - lo + 2) / 5, 0, (hi - lo) / 5);
    char shown[16];
    std::snprintf(shown, sizeof(shown), "%d%%%%", lo + step * 5);
    ImGui::SetNextItemWidth(S(220));
    if (ImGui::SliderInt(label, &step, 0, (hi - lo) / 5, shown)) *pct = lo + step * 5;
    Help((std::string(help) + " Applies after a restart.").c_str());
    if (*pct != BootPct(flag)) {
      ImGui::SameLine();
      ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "(after restart)");
    }
  }

  void RestartNote(const char* flag) {
    if (NeedsRestart(flag)) {
      ImGui::SameLine();
      ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "(after restart)");
    }
  }

  void DrawPlayerTab() {
    ImGui::SeparatorText("Player");
    ImGui::SetNextItemWidth(S(260));
    ImGui::InputText("Name", name_, sizeof(name_));
    Help("Your name in the game and on the leaderboards: 1-15 letters, digits, spaces, _ or -.");
    if (name_[0] && !ValidName(name_)) {
      ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1), "Use 1-15 letters, digits, spaces, _ or -");
    }

    ImGui::SetNextItemWidth(S(260));
    ImGui::InputText("Password", key_, sizeof(key_),
                     show_key_ ? 0 : ImGuiInputTextFlags_Password);
    ImGui::SameLine();
    ImGui::Checkbox("Show", &show_key_);
    Help("Make one up (8 or more characters). The first time you connect it is saved for your "
         "name; after that only this password can post times as you. Don't reuse an important "
         "password.");
    if (key_[0] && std::strlen(key_) < 8) {
      ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1), "At least 8 characters");
    }

    ImGui::SeparatorText("Online leaderboards");
    ImGui::Checkbox("Play online", &online_);
    Help("Uploads your times and shows everyone's times and friends in the game's leaderboards. "
         "Needs a name and a password.");
    ImGui::BeginDisabled(!online_);
    ImGui::SetNextItemWidth(S(360));
    ImGui::InputText("Server", server_, sizeof(server_));
    Help("The community server is https://trials.bytrev.org. Change it only to use a different "
         "server.");
    if (ImGui::Button("Test connection")) StartTest();
    ImGui::SameLine();
    {
      std::lock_guard<std::mutex> lock(test_->mu);
      if (test_->running) {
        ImGui::TextDisabled("Connecting...");
      } else if (!test_->message.empty()) {
        ImGui::TextColored(test_->ok ? ImVec4(0.4f, 0.9f, 0.4f, 1) : ImVec4(1, 0.45f, 0.4f, 1),
                           "%s", test_->message.c_str());
      }
    }
    ImGui::EndDisabled();
    ImGui::Spacing();
    ImGui::TextDisabled("Name and online changes take effect after restarting the game.");
    ImGui::TextDisabled("Steam Deck: Steam+X opens the on-screen keyboard.");
  }

  void DrawGraphicsTab() {
    ImGui::SeparatorText("Display");
    if (Has("resolution_scale")) {
      // Integer multiples of the game's 1280x720 (the engine can't do in-between sizes like
      // 1080p). Suggest the one that matches the screen height.
      const char* scales[] = {"1x  (1280x720, original)", "2x  (2560x1440)", "3x  (3840x2160)"};
      int idx = res_scale_ - 1;
      ImGui::SetNextItemWidth(S(240));
      if (ImGui::Combo("Resolution scale", &idx, scales, 3)) res_scale_ = idx + 1;
      Help("How sharp the game is drawn: a multiple of the original 1280x720. Pick the one "
           "closest to your screen's height (1440p screen: 2x, 4K: 3x). A higher setting than "
           "your screen still looks smoother (like anti-aliasing) if your graphics card keeps "
           "60 fps. This sets sharpness only; the picture's shape (16:9) stays the same.");
      RestartNote("resolution_scale");
      float h = ImGui::GetIO().DisplaySize.y;
      if (h > 0) {
        int best = std::clamp(int(h / 720.0f + 0.5f), 1, 3);
        ImGui::TextDisabled("Suggested for your screen (%d pixels tall): %dx", int(h), best);
      }
    }
    ImGui::Checkbox("Fullscreen", &fullscreen_);
    Help("Applies straight away when you press Save.");
    if (Has("vsync")) {
      ImGui::Checkbox("Vsync", &vsync_);
      Help("Keeps the game in step with your screen. Leave on unless you know you want it off.");
      RestartNote("vsync");
    }
    if (Has("trialshd_fps_cap")) {
      ImGui::Checkbox("Frame limiter (even 60 fps)", &fps_cap_);
      Help("Holds frames at an exact 60 per second. Try this if the game stutters on a "
           "120/144/165 Hz screen or with an NVIDIA card. Leave off if it's already smooth. "
           "Applies straight away when you press Save.");
    }
    if (Has("present_letterbox")) {
      ImGui::Checkbox("Black bars on wide screens", &letterbox_);
      Help("Trials HD is a 16:9 game. On wider screens (21:9 ultrawide, 16:10) it shows black "
           "bars at the sides to keep the right shape. Turn this off to stretch the picture to "
           "fill the screen instead (everything looks wider).");
      RestartNote("present_letterbox");
    }
    if (Has("anisotropic_override")) {
      const char* aniso[] = {"Game default", "Off", "1x", "2x", "4x", "8x", "16x"};
      int idx = aniso_ + 1;
      ImGui::SetNextItemWidth(S(220));
      if (ImGui::Combo("Texture filtering", &idx, aniso, 7)) aniso_ = idx - 1;
      Help("Anisotropic filtering: keeps the track sharp at a distance. 16x looks best and costs "
           "little on most graphics cards.");
      RestartNote("anisotropic_override");
    }

    if (Has("trialshd_bloom") || Has("trialshd_brightness")) {
      ImGui::SeparatorText("Effects (after restart)");
      PctSlider("trialshd_brightness", "Brightness", &bright_pct_, 50, 150,
                "How bright the picture is while riding. 100% is the original game; turn it down "
                "if bright scenes look washed out.");
      PctSlider("trialshd_bloom", "Bloom (glow)", &bloom_pct_, 0, 200,
                "The glow around bright lights and windows. 100% is the original game, 0% turns "
                "it off.");
      PctSlider("trialshd_dof", "Depth of field (blur)", &dof_pct_, 0, 100,
                "How blurry things near the camera and far in the background get. 100% is the "
                "original game, 0% keeps everything sharp.");
    }

    if (Has("audio_mute")) {
      ImGui::SeparatorText("Sound");
      ImGui::Checkbox("Mute", &mute_);
      RestartNote("audio_mute");
    }

    ImGui::SeparatorText("This menu");
    {
      const char* sizes[] = {"Automatic", "Small", "Medium", "Large", "Extra large"};
      ImGui::SetNextItemWidth(S(220));
      ImGui::Combo("Menu size", &menu_size_, sizes, 5);
      Help("Changes straight away. Press Save to keep it.");
    }

    ImGui::Spacing();
    ImGui::TextDisabled("F3 shows the FPS counter. F4 has every advanced engine setting.");
  }

  void Row(const char* what, const char* keys) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted(what);
    ImGui::TableSetColumnIndex(1);
    ImGui::TextUnformatted(keys);
  }

  void DrawControlsTab() {
    ImGui::SeparatorText("Keyboard");
    ImGui::Checkbox("Play with the keyboard", &keyboard_);
    Help("The keyboard works like a controller (alongside a real one). Turn off if it gets in "
         "the way.");
    if (ImGui::BeginTable("keys", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
      Row("Accelerate", "Up arrow / W");
      Row("Brake", "Down arrow / S");
      Row("Lean back / forward", "Left / Right arrows, A / D");
      Row("Select (A)", "Enter / Space");
      Row("Back (B)", "Backspace");
      Row("Pause (Start)", "Escape / P");
      Row("Restart (Back)", "Tab / R");
      Row("X / Y", "X / Y");
      Row("LB / RB", "Q / E");
      Row("D-pad", "Numpad 8 / 2 / 4 / 6");
      ImGui::EndTable();
    }

    ImGui::SeparatorText("Controller");
    if (ImGui::BeginTable("pad", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
      Row("Open / close this menu", "LB + RB + X  (L1 + R1 + Square)");
      Row("Move", "D-pad / left stick");
      Row("Select / tick", "A  (Cross)");
      Row("Close", "B  (Circle)");
      Row("Switch tab", "LB / RB");
      ImGui::EndTable();
    }
    ImGui::Spacing();
    ImGui::TextDisabled("Keyboard: F1 or Q + E + X opens/closes this menu.");
  }

  void StartTest() {
    {
      std::lock_guard<std::mutex> lock(test_->mu);
      if (test_->running) return;
      test_->running = true;
      test_->message.clear();
    }
    std::string url = server_, name = name_, key = key_;
    while (!url.empty() && url.back() == '/') url.pop_back();
    std::vector<std::pair<std::string, std::string>> headers = {{"X-Player-Name", name},
                                                                {"X-Player-Key", key}};
    std::string cf_id = GetStr("trialshd_cf_client_id"), cf_secret =
                                                             GetStr("trialshd_cf_client_secret");
    if (!cf_id.empty()) {
      headers.push_back({"CF-Access-Client-Id", cf_id});
      headers.push_back({"CF-Access-Client-Secret", cf_secret});
    }
    std::shared_ptr<TestState> state = test_;
    std::thread([state, url, name, headers] {
      bool ok = false;
      std::string msg;
      if (url.empty()) {
        msg = "Enter a server address";
      } else {
        HttpResponse r = HttpRequest("GET", url + "/api/v1/hello", headers, "", 8000);
        if (r.status == 200) {
          ok = true;
          msg = "Connected as " + name;
        } else if (r.status == 0) {
          msg = "Can't reach the server (" + r.error + ")";
        } else {
          // Server errors are JSON: {"error": "..."}
          std::string e = r.body;
          size_t p = e.find("\"error\"");
          if (p != std::string::npos) {
            size_t a = e.find('"', e.find(':', p) + 1);
            size_t b = a == std::string::npos ? a : e.find('"', a + 1);
            if (b != std::string::npos) e = e.substr(a + 1, b - a - 1);
          }
          msg = e.empty() ? "Server said HTTP " + std::to_string(r.status) : e;
        }
      }
      std::lock_guard<std::mutex> lock(state->mu);
      state->running = false;
      state->ok = ok;
      state->message = msg;
    }).detach();
  }

  void Save() {
    std::string name = name_, key = key_, server = server_;
    while (!server.empty() && server.back() == '/') server.pop_back();
    // Graphics/sound always save; the player/online part only when it's valid.
    std::string online_problem;
    if (!name.empty() && !ValidName(name))
      online_problem = "name not saved - use 1-15 letters, digits, spaces, _ or -";
    else if (online_ && (name.empty() || key.size() < 8))
      online_problem = "online not saved - needs a name and an 8+ character password";

    std::vector<std::pair<std::string, std::string>> toml;
    if (online_problem.empty()) {
      std::string url = online_ ? server : std::string();
      toml = {
          {"trialshd_player_name", TomlQuote(name)},
          {"trialshd_player_key", TomlQuote(key)},
          {"trialshd_server_url", TomlQuote(url)},
      };
      rex::cvar::SetFlagByName("trialshd_player_name", name);
      rex::cvar::SetFlagByName("trialshd_player_key", key);
      rex::cvar::SetFlagByName("trialshd_server_url", url);
    }

    auto set_bool = [&](const char* flag, bool v) {
      if (!Has(flag)) return;
      rex::cvar::SetFlagByName(flag, v ? "true" : "false");
      toml.push_back({flag, v ? "true" : "false"});
    };
    auto set_int = [&](const char* flag, int v) {
      if (!Has(flag)) return;
      rex::cvar::SetFlagByName(flag, std::to_string(v));
      toml.push_back({flag, std::to_string(v)});
    };
    set_int("resolution_scale", res_scale_);
    set_bool("vsync", vsync_);
    set_bool("present_letterbox", letterbox_);
    set_int("anisotropic_override", aniso_);
    set_bool("audio_mute", mute_);
    set_bool("fullscreen", fullscreen_);
    set_int("trialshd_menu_size", menu_size_);
    set_bool("trialshd_keyboard", keyboard_);
    set_int("trialshd_fps_cap", fps_cap_ ? 60 : 0);
    auto set_pct = [&](const char* flag, int pct) {
      if (!Has(flag)) return;
      char v[16];
      std::snprintf(v, sizeof(v), "%.2f", pct / 100.0);
      rex::cvar::SetFlagByName(flag, v);  // shaders already built keep the startup value
      toml.push_back({flag, v});
    };
    set_pct("trialshd_bloom", bloom_pct_);
    set_pct("trialshd_dof", dof_pct_);
    set_pct("trialshd_brightness", bright_pct_);
    if (window_ && window_->IsFullscreen() != fullscreen_) window_->SetFullscreen(fullscreen_);

    std::string error;
    if (!WriteTomlKeys(config_path_, toml, &error)) {
      REXLOG_ERROR("[menu] save failed: {}", error);
      SetStatus(false, "Couldn't save: " + error);
      return;
    }
    REXLOG_INFO("[menu] settings saved to {}", config_path_.string());
    if (online_problem.empty())
      SetStatus(true, "Saved");
    else
      SetStatus(false, "Graphics saved; " + online_problem);
  }

  void SetStatus(bool ok, std::string s) {
    status_ok_ = ok;
    status_ = std::move(s);
  }

  rex::ui::Window* window_;
  std::filesystem::path config_path_;
  char name_[32] = {};
  char key_[128] = {};
  char server_[256] = {};
  bool show_key_ = false;
  bool online_ = false;
  int res_scale_ = 1;
  bool vsync_ = true;
  bool letterbox_ = true;
  int aniso_ = 3;
  bool fullscreen_ = false;
  bool mute_ = false;
  int menu_size_ = 0;
  int tab_ = 0;
  bool keyboard_ = true;
  bool fps_cap_ = false;
  int bloom_pct_ = 100;
  int dof_pct_ = 100;
  int bright_pct_ = 100;
  float scale_ = 1.0f;
  std::string status_;
  bool status_ok_ = true;
  std::shared_ptr<TestState> test_ = std::make_shared<TestState>();
};

// Always-present invisible dialog: runs every frame on the UI thread, reads the controllers,
// opens/closes the menu on LB+RB+X and decides when the game must not see the controller.
class PadWatcher final : public rex::ui::ImGuiDialog {
 public:
  explicit PadWatcher(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO&) override {
    g_pad = ReadPads();
    const uint16_t b = g_pad.buttons;
    const bool bumpers = (b & kBtnLB) && (b & kBtnRB);
    const bool x_down = b & kBtnX;
    if (bumpers && x_down && !prev_x_) ToggleMenuInternal();
    prev_x_ = x_down;

    // After the menu closes, keep the game blind until every button is let go, so the button
    // that closed it (or the shortcut) doesn't reach the game.
    const bool open = g_menu != nullptr;
    if (was_open_ && !open) wait_release_ = true;
    if (wait_release_ && b == 0) wait_release_ = false;
    was_open_ = open;
    g_block_game = open || bumpers || wait_release_;
  }

 private:
  bool prev_x_ = false;
  bool was_open_ = false;
  bool wait_release_ = false;
};

void ToggleMenuInternal() {
  if (g_menu) {
    g_menu->RequestClose();  // deleted after its next draw
    return;
  }
  if (!g_drawer) return;
  g_menu = new TrialsMenu(g_drawer, g_window, g_config_path);
}

}  // namespace

void InitTrialsMenu(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window,
                    const std::filesystem::path& config_path) {
  g_drawer = drawer;
  g_window = window;
  g_config_path = config_path;
  if (drawer) new PadWatcher(drawer);  // lives for the whole session
}

void ConnectTrialsMenuInput(rex::input::InputSystem* input) {
  if (!input) return;
  g_input = input;
  ImGuiIO* io = g_drawer ? &g_drawer->GetIO() : nullptr;
  // Same rule as the SDK's default (game ignores the pad while the mouse is over an overlay),
  // plus: blind while our menu is open / the shortcut is held. Our own reads always see it.
  input->SetActiveCallback([io] {
    if (t_menu_reading) return true;
    if (g_block_game) return false;
    return !(io && io->WantCaptureMouse);
  });
}

void ToggleTrialsMenu() { ToggleMenuInternal(); }

bool TrialsMenuReadingPads() { return t_menu_reading; }

bool TrialsMenuOpen() { return g_menu != nullptr; }
