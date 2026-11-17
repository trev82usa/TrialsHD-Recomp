// Checks the game files before the engine starts, so a missing or wrong default.xex gives a
// clear message instead of "No function registered at ..." or a silent crash.
//
// The program is recompiled from one exact build of the game: default.xex version 0.0.1.2 (the
// Xbox Live Arcade release, build the port was made from). An older copy that never got the update has
// different code, so nothing would line up. The installer (install_game.py /
// TrialsHD-Installer.exe) extracts the package and applies the title update.
#pragma once

#include <rex/logging/api.h>
#include <rex/system.h>
#include <SDL3/SDL_messagebox.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace thd {

constexpr uint32_t kTitleId = 0x5841095A;
constexpr uint32_t kRequiredVersion = 258;  // XEX execution info "version" this build was made from (0.0.1.2)
#if defined(TRIALSHD_TU4)
// Title Update 4 build: default.xex 0.0.1.2 plus the update's default.xexp next to it (the
// engine applies it while loading), or a default.xex already patched to 0.0.4.2
// (tools/apply_title_update.py).
constexpr uint32_t kTu4Version = 0x402;
#endif

inline uint32_t Be32(const std::vector<uint8_t>& d, size_t o) {
  return o + 4 <= d.size() ? (uint32_t(d[o]) << 24) | (uint32_t(d[o + 1]) << 16) |
                                 (uint32_t(d[o + 2]) << 8) | d[o + 3]
                           : 0;
}

inline std::string VersionString(uint32_t v) {
  return std::to_string(v >> 28) + "." + std::to_string((v >> 24) & 0xF) + "." +
         std::to_string((v >> 8) & 0xFFFF) + "." + std::to_string(v & 0xFF);
}

// Empty string = files look right; otherwise a message for the player.
inline std::string CheckGameFiles(const std::filesystem::path& dir) {
  const std::string install_hint =
      "\n\nRun the installer next to the game:\n"
      "  Windows: TrialsHD-Installer.exe\n"
      "  Linux: install_game.py\n"
      "It extracts your Trials HD package into the assets folder\n"
      "and applies the title update if your copy needs it.";
  std::error_code ec;
  const auto xex = dir / "default.xex";
  if (!std::filesystem::exists(xex, ec))
    return "Game files not found.\n\nExpected default.xex and the data folder in:\n" +
           dir.string() + install_hint;
  if (!std::filesystem::exists(dir / "data", ec))
    return "The data folder is missing from:\n" + dir.string() +
           "\n\ndefault.xex was found, but the game also needs the data folder from the "
           "same package." + install_hint;

  std::ifstream f(xex, std::ios::binary);
  std::vector<uint8_t> d(0x10000);
  f.read(reinterpret_cast<char*>(d.data()), std::streamsize(d.size()));
  d.resize(size_t(f.gcount()));
  if (d.size() < 24 || d[0] != 'X' || d[1] != 'E' || d[2] != 'X' || d[3] != '2')
    return "default.xex in\n" + dir.string() + "\nis not an Xbox 360 executable." + install_hint;

  uint32_t version = 0, title = 0;
  const uint32_t count = Be32(d, 20);
  for (uint32_t i = 0; i < count && 24 + i * 8 + 8 <= d.size(); ++i) {
    if (Be32(d, 24 + i * 8) == 0x00040006) {  // execution info
      const uint32_t off = Be32(d, 24 + i * 8 + 4);
      version = Be32(d, off + 4);
      title = Be32(d, off + 12);
    }
  }
  if (title != kTitleId)
    return "default.xex is not from Trials HD (title ID 5841095A)." + install_hint;
#if defined(TRIALSHD_TU4)
  if (version == kTu4Version) return {};
  if (version == kRequiredVersion) {
    std::ifstream p(dir / "default.xexp", std::ios::binary);
    std::vector<uint8_t> h(0x2000);
    p.read(reinterpret_cast<char*>(h.data()), std::streamsize(h.size()));
    h.resize(size_t(p.gcount()));
    uint32_t target = 0;
    for (uint32_t i = 0; h.size() >= 24 && i < Be32(h, 20) && 24 + i * 8 + 8 <= h.size(); ++i)
      if (Be32(h, 24 + i * 8) == 0x000005FF) target = Be32(h, Be32(h, 24 + i * 8 + 4) + 4);
    if (target == kTu4Version) return {};
    return "This build of Trials HD needs Title Update 4.\n\nPut default.xexp from the "
           "Title Update 4 package (Media ID 22E417BF) next to default.xex in:\n" + dir.string() +
           "\n\n(tools/stfs_extract.py <update file> <folder> extracts it.)";
  }
#endif
  if (version != kRequiredVersion)
    return "Your default.xex is version " + VersionString(version) +
           ",\nbut this build needs version " + VersionString(kRequiredVersion) +
           " (the updated Xbox Live Arcade release).\n\nYour copy of the game hasn't had "
           "its title update applied." + install_hint +
           "\n\nIn the installer, pick your title update file as well as the game package.";
  return {};
}

// Shows the problem and exits. GH_SKIP_FILE_CHECK=1 skips it (for testing other builds).
inline void RequireGameFiles(const std::filesystem::path& dir) {
  if (std::getenv("GH_SKIP_FILE_CHECK")) return;
  const std::string problem = CheckGameFiles(dir);
  if (problem.empty()) return;
  REXLOG_ERROR("[files] {}", problem);
  rex::FlushLogging();
  // A real popup on every platform (SDL needs no initialisation for this); the runtime's own
  // message box only prints to the terminal on Linux.
  if (!SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Trials HD", problem.c_str(), nullptr))
    rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, "Trials HD\n\n" + problem);
  std::_Exit(1);
}

}  // namespace thd
