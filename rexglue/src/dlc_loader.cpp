// DLC: drop content packages (track packs etc. - the STFS files from your console's
// Content/0000000000000000/<title id>/00000002/ folder) into a "dlc" folder next to the game.
// On startup each new package is installed into the game's content folder with the SDK's
// ContentManager::InstallContent, which is where the game looks for downloaded content; after
// that the package can be removed from "dlc". Works for any ReXGlue title.
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_manager.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

void InstallDlcPackages() {
  namespace fs = std::filesystem;
  const fs::path dir = rex::filesystem::GetExecutableFolder() / "dlc";
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) {
    fs::create_directories(dir, ec);  // so people can see where DLC goes
    return;
  }
  auto* ks = REX_KERNEL_STATE();
  auto* cm = ks ? ks->content_manager() : nullptr;
  if (!cm) return;

  // installed.txt remembers "<file name> <size>" for packages already installed.
  const fs::path list_path = dir / "installed.txt";
  std::set<std::string> done;
  {
    std::ifstream in(list_path);
    for (std::string line; std::getline(in, line);)
      if (!line.empty()) done.insert(line);
  }
  int installed = 0, failed = 0;
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    if (!e.is_regular_file()) continue;
    const auto name = e.path().filename().string();
    if (name == "installed.txt" || name.rfind("README", 0) == 0) continue;
    char magic[4] = {};
    {
      std::ifstream f(e.path(), std::ios::binary);
      f.read(magic, 4);
    }
    const std::string m(magic, 4);
    if (m != "LIVE" && m != "PIRS" && m != "CON ") {
      REXLOG_WARN("[dlc] {}: not a content package, skipped", name);
      continue;
    }
    const std::string key = name + " " + std::to_string(e.file_size());
    if (done.count(key)) continue;
    const auto r = cm->InstallContent(e.path());
    if (r == 0) {
      ++installed;
      done.insert(key);
      REXLOG_WARN("[dlc] installed {}", name);
    } else {
      ++failed;
      REXLOG_ERROR("[dlc] couldn't install {} (error {:#x})", name, r);
    }
  }
  if (installed) {
    std::ofstream out(list_path, std::ios::trunc);
    for (const auto& k : done) out << k << "\n";
  }
  if (installed || failed)
    REXLOG_WARN("[dlc] {} package(s) installed, {} failed", installed, failed);
}
