#include "keyboard_input.h"

#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/input/input_driver.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/system/xtypes.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

#include <algorithm>
#include <array>
#include <vector>
#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>

REXCVAR_DEFINE_BOOL(trialshd_keyboard, true, "TrialsHD",
                    "Play with the keyboard: arrows/WASD ride, Enter/Space = A, Backspace = B, "
                    "Escape/P = Start (pause), Tab/R = Back (restart)");

namespace {

using rex::X_RESULT;
using rex::X_STATUS;
using rex::input::X_INPUT_CAPABILITIES;
using rex::input::X_INPUT_KEYSTROKE;
using rex::input::X_INPUT_STATE;
using rex::input::X_INPUT_VIBRATION;

constexpr uint16_t kUp = 0x0001, kDown = 0x0002, kLeft = 0x0004, kRight = 0x0008,
                   kStart = 0x0010, kBack = 0x0020, kLB = 0x0100, kRB = 0x0200, kA = 0x1000,
                   kB = 0x2000, kX = 0x4000, kY = 0x8000;

// Virtual key codes (Windows VK values, same on every platform in the SDK).
enum : uint8_t {
  kVkBACK = 0x08, kVkTAB = 0x09, kVkRETURN = 0x0D, kVkESCAPE = 0x1B, kVkSPACE = 0x20,
  kVkLEFT = 0x25, kVkUP = 0x26, kVkRIGHT = 0x27, kVkDOWN = 0x28,
  kVkNUMPAD2 = 0x62, kVkNUMPAD4 = 0x64, kVkNUMPAD6 = 0x66, kVkNUMPAD8 = 0x68,
};

class KeyboardDriver final : public rex::input::InputDriver,
                             public rex::ui::WindowInputListener,
                             public rex::ui::WindowListener {
 public:
  explicit KeyboardDriver(rex::ui::Window* window) : InputDriver(window, 0) {
    if (window) {
      window->AddInputListener(this, 0);
      window->AddListener(this);
    }
  }
  // Lives as long as the game. Not detaching here: at shutdown the window may already be gone.
  ~KeyboardDriver() override = default;

  X_STATUS Setup() override { return X_STATUS_SUCCESS; }



  X_RESULT GetCapabilities(uint32_t user, uint32_t, X_INPUT_CAPABILITIES* caps) override {
    if (!Enabled() || user != 0) return X_ERROR_DEVICE_NOT_CONNECTED;
    std::memset(caps, 0, sizeof(*caps));
    caps->type = 1;      // XINPUT_DEVTYPE_GAMEPAD
    caps->sub_type = 1;  // XINPUT_DEVSUBTYPE_GAMEPAD
    caps->gamepad.buttons = 0xF3FF;
    caps->gamepad.left_trigger = 0xFF;
    caps->gamepad.right_trigger = 0xFF;
    caps->gamepad.thumb_lx = int16_t(0x7FFF);
    caps->gamepad.thumb_ly = int16_t(0x7FFF);
    return X_ERROR_SUCCESS;
  }

  X_RESULT GetState(uint32_t user, X_INPUT_STATE* out) override {
    if (!Enabled() || user != 0) return X_ERROR_DEVICE_NOT_CONNECTED;
    std::memset(out, 0, sizeof(*out));
    if (!is_active()) {
      out->packet_number = packet_.load();
      return X_ERROR_SUCCESS;
    }
    auto k = [this](uint8_t vk) { return keys_[vk].load(std::memory_order_relaxed); };
    auto any = [&](uint8_t a, uint8_t b) { return k(a) || k(b); };

    const bool up = any(kVkUP, 'W'), down = any(kVkDOWN, 'S');
    const bool left = any(kVkLEFT, 'A'), right = any(kVkRIGHT, 'D');
    uint16_t buttons = 0;
    if (any(kVkRETURN, kVkSPACE)) buttons |= kA;
    if (k(kVkBACK)) buttons |= kB;
    if (k('X')) buttons |= kX;
    if (k('Y')) buttons |= kY;
    if (k('Q')) buttons |= kLB;
    if (k('E')) buttons |= kRB;
    if (any(kVkESCAPE, 'P')) buttons |= kStart;
    if (any(kVkTAB, 'R')) buttons |= kBack;
    if (k(kVkNUMPAD8)) buttons |= kUp;
    if (k(kVkNUMPAD2)) buttons |= kDown;
    if (k(kVkNUMPAD4)) buttons |= kLeft;
    if (k(kVkNUMPAD6)) buttons |= kRight;

    auto& g = out->gamepad;
    g.buttons = buttons;
    // Up/Down: accelerate/brake while riding, and the stick's up/down for menus (Trials only
    // uses left/right on the stick while riding).
    g.right_trigger = up ? 0xFF : 0;
    g.left_trigger = down ? 0xFF : 0;
    g.thumb_lx = int16_t(left == right ? 0 : (right ? 32767 : -32768));
    g.thumb_ly = int16_t(up == down ? 0 : (up ? 32767 : -32768));

    // Count a new packet whenever the state changes (games use it to spot input).
    uint64_t sig = uint64_t(buttons) | (uint64_t(up) << 16) | (uint64_t(down) << 17) |
                   (uint64_t(left) << 18) | (uint64_t(right) << 19);
    if (last_sig_.exchange(sig) != sig) {
      ++packet_;
    }
    out->packet_number = packet_.load();
    return X_ERROR_SUCCESS;
  }

  X_RESULT SetState(uint32_t user, X_INPUT_VIBRATION*) override {
    return Enabled() && user == 0 ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
  }

  // Button-press events (XInputGetKeystroke). Games use these to see which controller
  // pressed Start/A on the title screen and in menus, so the keyboard must produce them too.
  X_RESULT GetKeystroke(uint32_t user, uint32_t, X_INPUT_KEYSTROKE* out) override {
    if (!Enabled() || user != 0) return X_ERROR_DEVICE_NOT_CONNECTED;
    std::lock_guard<std::mutex> lock(queue_mu_);
    // Nothing queued: answer "not connected" so the input system goes on to ask the real
    // controller (it stops at the first driver that says "empty").
    if (queue_.empty()) return X_ERROR_DEVICE_NOT_CONNECTED;
    const Stroke s = queue_.front();
    queue_.pop_front();
    std::memset(out, 0, sizeof(*out));
    out->virtual_key = s.pad_vk;
    out->flags = s.flags;
    out->user_index = 0;
    if (logged_strokes_ < 4) {
      ++logged_strokes_;
      REXLOG_WARN("[input] keyboard -> game: button event {:04X} flags {}", s.pad_vk, s.flags);
    }
    return X_ERROR_SUCCESS;
  }

  // WindowInputListener (UI thread)
  void OnKeyDown(rex::ui::KeyEvent& e) override { Set(e, true); }
  void OnKeyUp(rex::ui::KeyEvent& e) override { Set(e, false); }

  // WindowListener: drop held keys when the window loses focus (no stuck throttle).
  void OnLostFocus(rex::ui::UISetupEvent&) override {
    for (auto& key : keys_) key.store(false, std::memory_order_relaxed);
  }

 private:
  static bool Enabled() { return rex::cvar::Query<bool>("trialshd_keyboard"); }

  void Set(rex::ui::KeyEvent& e, bool down) {
    uint16_t vk = static_cast<uint16_t>(e.virtual_key());
    if (vk >= 256) return;
    const bool was_down = keys_[vk].exchange(down, std::memory_order_relaxed);
    if (!down && !was_down) return;
    // Not while our overlay menu has the controls (is_active false) or keyboard is off.
    if (!Enabled() || (down && !is_active())) return;
    const uint16_t flags = !down ? 0x0002 : (was_down ? 0x0005 : 0x0001);  // UP / DOWN|REPEAT
    for (uint16_t pad : PadKeysFor(uint8_t(vk))) {
      if (!pad) break;
      std::lock_guard<std::mutex> lock(queue_mu_);
      if (queue_.size() < 64) queue_.push_back({pad, flags});
    }
  }

  // Xbox VK_PAD_* codes for a keyboard key (up to 2).
  static std::array<uint16_t, 2> PadKeysFor(uint8_t vk) {
    constexpr uint16_t A = 0x5800, B = 0x5801, X = 0x5802, Y = 0x5803, RB = 0x5804,
                       LB = 0x5805, LT = 0x5806, RT = 0x5807, DUp = 0x5810, DDown = 0x5811,
                       DLeft = 0x5812, DRight = 0x5813, Start = 0x5814, Back = 0x5815,
                       LsUp = 0x5820, LsDown = 0x5821, LsRight = 0x5822, LsLeft = 0x5823;
    switch (vk) {
      case kVkRETURN: case kVkSPACE: return {A, 0};
      case kVkBACK: return {B, 0};
      case 'X': return {X, 0};
      case 'Y': return {Y, 0};
      case 'E': return {RB, 0};
      case 'Q': return {LB, 0};
      case kVkUP: case 'W': return {LsUp, RT};
      case kVkDOWN: case 'S': return {LsDown, LT};
      case kVkLEFT: case 'A': return {LsLeft, 0};
      case kVkRIGHT: case 'D': return {LsRight, 0};
      case kVkNUMPAD8: return {DUp, 0};
      case kVkNUMPAD2: return {DDown, 0};
      case kVkNUMPAD4: return {DLeft, 0};
      case kVkNUMPAD6: return {DRight, 0};
      case kVkESCAPE: case 'P': return {Start, 0};
      case kVkTAB: case 'R': return {Back, 0};
      default: return {0, 0};
    }
  }

  struct Stroke {
    uint16_t pad_vk;
    uint16_t flags;
  };
  std::mutex queue_mu_;
  std::deque<Stroke> queue_;
  std::atomic<int> logged_strokes_{0};

  std::atomic<bool> keys_[256] = {};
  std::atomic<uint32_t> packet_{0};
  std::atomic<int> logged_keys_{0}, logged_states_{0};
  std::atomic<uint64_t> last_sig_{0};
};

}  // namespace

// The SDK's InputSystem asks its drivers in order and, for capabilities and button events,
// stops at the first one that answers; its built-in "nop" driver always answers, so a driver
// added at the end is never asked. Move ours to the front. InputSystem (SDK 0.9) is
// { vtable, Window* window_, std::vector<std::unique_ptr<InputDriver>> drivers_ }; the window
// pointer is checked before touching anything.
bool MoveKeyboardDriverFirst(rex::input::InputSystem* input, rex::input::InputDriver* ours) {
  struct Layout {
    void* vtable;
    rex::ui::Window* window;
    std::vector<std::unique_ptr<rex::input::InputDriver>> drivers;
  };
  auto* l = reinterpret_cast<Layout*>(input);
  if (l->window != input->window() || l->drivers.empty() || l->drivers.size() > 16 ||
      l->drivers.back().get() != ours) {
    REXLOG_WARN("[input] keyboard: could not move driver first (unexpected SDK layout)");
    return false;
  }
  std::rotate(l->drivers.begin(), l->drivers.end() - 1, l->drivers.end());
  REXLOG_INFO("[input] keyboard driver moved first ({} drivers)", l->drivers.size());
  return true;
}

std::unique_ptr<rex::input::InputDriver> CreateKeyboardDriver(rex::ui::Window* window) {
  REXLOG_INFO("[input] keyboard controls ready");
  return std::make_unique<KeyboardDriver>(window);
}
