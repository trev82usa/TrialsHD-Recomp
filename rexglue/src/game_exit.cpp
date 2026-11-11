// Quitting from the game's own menu.
//
// The game's "Exit" asks the console to go back to the dashboard (XamLoaderLaunchTitle /
// XamLoaderTerminateTitle / HalReturnToFirmware). The engine then tries to terminate the title
// from inside, which can hang - the window stays (or the process keeps running invisibly).
// There is no dashboard to return to, so we just save the log and end the process. Scores are
// uploaded the moment they are set, so nothing is lost.
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

namespace {

[[noreturn]] void QuitNow(const char* why) {
  REXLOG_WARN("[exit] game asked to quit ({}); closing", why);
  rex::FlushLogging();
  std::_Exit(0);
}

}  // namespace

// sub_82124A38: the game's XLaunchNewImage wrapper; it always ends in XamLoaderLaunchTitle
// (return to dashboard), so it's only reached when the game wants to leave.
REX_HOOK_RAW(sub_82124A38) { QuitNow("return to dashboard"); }

// Not hooked: sub_82128508 (a periodic check that only calls HalReturnToFirmware when it fails).
// Safety net for any other route (e.g. XamLoaderTerminateTitle from the startup code): once the
// engine starts terminating the title, give it a few seconds, then end the process.
void StartTitleExitWatch() {
  std::thread([] {
    for (;;) {
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
      auto* ks = REX_KERNEL_STATE();
      if (ks && ks->is_terminating_title()) {
        std::this_thread::sleep_for(std::chrono::seconds(3));
        QuitNow("title terminated");
      }
    }
  }).detach();
}

// Linux: the engine keeps the game's 4.5 GB memory in a shared-memory file (/dev/shm/
// xenia_memory_<n>, held in RAM) and only deletes it on a perfectly clean shutdown. Every crash
// or quick exit leaves one behind still holding RAM; after a few dozen runs the machine slows to
// a crawl and the game dies with SIGBUS. Removing the name is always safe: a running game keeps
// its memory through its open handle, and the RAM is freed as soon as that process ends.
// Call before the runtime starts (old leftovers) and right after (our own file).
void RemoveSharedMemoryFiles(const char* when) {
#if defined(__linux__)
  std::error_code ec;
  int removed = 0;
  for (const auto& e : std::filesystem::directory_iterator("/dev/shm", ec)) {
    const std::string name = e.path().filename().string();
    if (name.rfind("xenia_memory_", 0) != 0) continue;
    std::error_code rm_ec;
    if (std::filesystem::remove(e.path(), rm_ec)) ++removed;
  }
  if (removed) REXLOG_INFO("[memory] removed {} shared-memory file(s) {}", removed, when);
#else
  (void)when;
#endif
}
