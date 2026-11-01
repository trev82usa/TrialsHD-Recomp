// Trials HD performance diagnostics.
//
//  * Frame counter: wraps the game's present function (sub_82143E10, the only caller of VdSwap)
//    to measure the real guest frame rate. Feeds the F3 overlay and writes a one-line summary to
//    the log every 5 seconds ("FPS: ...").
//  * Sampling profiler (Windows, opt-in with trialshd_profile = true in trialshd.toml): about
//    1000 times a second it briefly pauses every thread that is using CPU, records where it is
//    (plus a short call stack) and writes the totals to profile.txt next to the exe every 10 s.
//    Addresses are written as module + offset; they are turned into function names offline
//    with the PDB from the build's "symbols" artifact.
#include "perf_tools.h"

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/rex_app.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

REXCVAR_DEFINE_BOOL(trialshd_profile, false, "TrialsHD",
                    "Write a CPU sampling profile to profile.txt next to the exe");

namespace {

using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------------------------------------
// Frame counter
// ---------------------------------------------------------------------------------------------
std::mutex g_frame_mutex;
uint64_t g_frames = 0;
Clock::time_point g_last_frame{};
double g_last_frame_ms = 0;
// Window for the overlay (~0.5 s) and for the log summary (5 s).
Clock::time_point g_overlay_start{};
uint64_t g_overlay_frames = 0;
double g_overlay_fps = 0, g_overlay_ms = 0;
Clock::time_point g_log_start{};
uint64_t g_log_frames = 0;
double g_log_worst_ms = 0;
int g_log_slow_frames = 0;  // frames longer than 20 ms

void OnGuestFrame() {
  auto now = Clock::now();
  std::lock_guard lock(g_frame_mutex);
  if (g_frames == 0) {
    g_overlay_start = g_log_start = now;
  } else {
    double ms = std::chrono::duration<double, std::milli>(now - g_last_frame).count();
    g_last_frame_ms = ms;
    g_log_worst_ms = std::max(g_log_worst_ms, ms);
    if (ms > 20.0) ++g_log_slow_frames;
  }
  g_last_frame = now;
  ++g_frames;
  ++g_overlay_frames;
  ++g_log_frames;

  double overlay_s = std::chrono::duration<double>(now - g_overlay_start).count();
  if (overlay_s >= 0.5) {
    g_overlay_fps = g_overlay_frames / overlay_s;
    g_overlay_ms = overlay_s * 1000.0 / g_overlay_frames;
    g_overlay_start = now;
    g_overlay_frames = 0;
  }
  double log_s = std::chrono::duration<double>(now - g_log_start).count();
  if (log_s >= 5.0) {
    REXLOG_WARN("FPS: {:.1f} avg over {:.1f}s, worst frame {:.1f} ms, {} frames over 20 ms",
                g_log_frames / log_s, log_s, g_log_worst_ms, g_log_slow_frames);
    g_log_start = now;
    g_log_frames = 0;
    g_log_worst_ms = 0;
    g_log_slow_frames = 0;
  }
}

}  // namespace

REX_EXTERN(__imp__sub_82143E10);
REX_HOOK_RAW(sub_82143E10) {
  OnGuestFrame();
  __imp__sub_82143E10(ctx, base);
}

rex::ui::FrameStats GetGuestFrameStats() {
  std::lock_guard lock(g_frame_mutex);
  rex::ui::FrameStats s;
  s.frame_count = g_frames;
  s.fps = g_overlay_fps;
  s.frame_time_ms = g_overlay_ms;
  return s;
}

// ---------------------------------------------------------------------------------------------
// Sampling profiler (Windows only)
// ---------------------------------------------------------------------------------------------
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <timeapi.h>
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "winmm.lib")

namespace {

constexpr int kMaxFrames = 24;

struct Module {
  std::string name;
  uintptr_t base = 0, end = 0;
  const RUNTIME_FUNCTION* pdata = nullptr;  // sorted by BeginAddress
  size_t pdata_count = 0;
};
std::vector<Module> g_modules;

void LoadModules() {
  HMODULE mods[512];
  DWORD needed = 0;
  HANDLE proc = GetCurrentProcess();
  if (!EnumProcessModules(proc, mods, sizeof(mods), &needed)) return;
  size_t n = std::min<size_t>(needed / sizeof(HMODULE), 512);
  for (size_t i = 0; i < n; ++i) {
    MODULEINFO mi{};
    char name[MAX_PATH] = {};
    if (!GetModuleInformation(proc, mods[i], &mi, sizeof(mi))) continue;
    GetModuleBaseNameA(proc, mods[i], name, MAX_PATH);
    Module m;
    m.name = name;
    m.base = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
    m.end = m.base + mi.SizeOfImage;
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(m.base);
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(m.base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (dir.VirtualAddress && dir.Size) {
      m.pdata = reinterpret_cast<const RUNTIME_FUNCTION*>(m.base + dir.VirtualAddress);
      m.pdata_count = dir.Size / sizeof(RUNTIME_FUNCTION);
    }
    g_modules.push_back(m);
  }
}

const Module* FindModule(uintptr_t pc) {
  for (const auto& m : g_modules)
    if (pc >= m.base && pc < m.end) return &m;
  return nullptr;
}

// Lock-free replacement for RtlLookupFunctionEntry (which can take locks a paused thread holds).
const RUNTIME_FUNCTION* LookupFunction(const Module* m, uintptr_t pc) {
  if (!m || !m->pdata) return nullptr;
  DWORD rva = static_cast<DWORD>(pc - m->base);
  size_t lo = 0, hi = m->pdata_count;
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    const auto& f = m->pdata[mid];
    if (rva < f.BeginAddress) hi = mid;
    else if (rva >= f.EndAddress) lo = mid + 1;
    else return &f;
  }
  return nullptr;
}

// Walk the stack of a paused thread. Reads memory only: no allocation, no locks.
int UnwindUnsafe(CONTEXT& ctx, uintptr_t* out, int& n) {
  for (; n < kMaxFrames; ++n) {
    uintptr_t pc = ctx.Rip;
    if (!pc) break;
    out[n] = pc;
    const Module* m = FindModule(pc);
    if (!m) break;  // JIT/unknown code: stop rather than guess
    const RUNTIME_FUNCTION* f = LookupFunction(m, pc);
    if (!f) {  // leaf function: return address is at [rsp]
      if (!ctx.Rsp) break;
      ctx.Rip = *reinterpret_cast<const DWORD64*>(ctx.Rsp);
      ctx.Rsp += 8;
    } else {
      void* handler_data = nullptr;
      DWORD64 frame = 0;
      RtlVirtualUnwind(UNW_FLAG_NHANDLER, m->base, pc, const_cast<PRUNTIME_FUNCTION>(f), &ctx,
                       &handler_data, &frame, nullptr);
    }
    if (ctx.Rip == pc) break;
  }
  return n;
}

// A bad stack pointer must not take the game down: stop at the first unreadable frame.
int Unwind(const CONTEXT& start, uintptr_t* out) {
  static CONTEXT ctx;  // only the profiler thread uses this
  ctx = start;
  int n = 0;
  __try {
    UnwindUnsafe(ctx, out, n);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return n;
}

struct ThreadState {
  DWORD tid = 0;
  HANDLE handle = nullptr;
  ULONG64 last_cycles = 0;
  uint64_t samples = 0;
  std::string name;
  std::unordered_map<uint64_t, uint32_t> self;  // key: module index << 32 | rva
};

std::unordered_map<uint64_t, uint32_t> g_self, g_incl;
std::unordered_map<DWORD, ThreadState> g_threads;
uint64_t g_total_samples = 0, g_ticks = 0;
Clock::time_point g_profile_start;

uint64_t Key(uintptr_t pc) {
  for (size_t i = 0; i < g_modules.size(); ++i)
    if (pc >= g_modules[i].base && pc < g_modules[i].end)
      return (uint64_t(i) << 32) | uint32_t(pc - g_modules[i].base);
  return (uint64_t(0xFFFF) << 32) | uint32_t(pc);
}

std::string KeyStr(uint64_t k) {
  uint32_t mi = uint32_t(k >> 32);
  char buf[160];
  snprintf(buf, sizeof(buf), "%s+0x%X", mi < g_modules.size() ? g_modules[mi].name.c_str() : "?",
           uint32_t(k));
  return buf;
}

std::string ThreadName(HANDLE h) {
  using Fn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
  static Fn fn = reinterpret_cast<Fn>(
      GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));
  if (!fn) return {};
  PWSTR w = nullptr;
  std::string out;
  if (SUCCEEDED(fn(h, &w)) && w) {
    char buf[256] = {};
    WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof(buf) - 1, nullptr, nullptr);
    out = buf;
    LocalFree(w);
  }
  return out;
}

void RefreshThreads() {
  DWORD self_tid = GetCurrentThreadId(), pid = GetCurrentProcessId();
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE) return;
  THREADENTRY32 te{sizeof(te)};
  for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
    if (te.th32OwnerProcessID != pid || te.th32ThreadID == self_tid) continue;
    auto& t = g_threads[te.th32ThreadID];
    if (!t.handle) {
      t.tid = te.th32ThreadID;
      t.handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                            FALSE, te.th32ThreadID);
      if (t.handle) QueryThreadCycleTime(t.handle, &t.last_cycles);
    }
    if (t.handle) {
      std::string n = ThreadName(t.handle);
      if (!n.empty()) t.name = n;
    }
  }
  CloseHandle(snap);
}

void WriteReport() {
  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring path(exe);
  path = path.substr(0, path.find_last_of(L"\\/") + 1) + L"profile.txt";
  FILE* f = _wfopen(path.c_str(), L"w");
  if (!f) return;
  double secs = std::chrono::duration<double>(Clock::now() - g_profile_start).count();
  fprintf(f, "Trials HD profile: %.1f s, %llu ticks, %llu samples\n", secs,
          (unsigned long long)g_ticks, (unsigned long long)g_total_samples);
  {
    std::lock_guard lock(g_frame_mutex);
    fprintf(f, "frames: %llu, overlay fps %.1f\n", (unsigned long long)g_frames, g_overlay_fps);
  }
  fprintf(f, "\n[modules]\n");
  for (size_t i = 0; i < g_modules.size(); ++i)
    fprintf(f, "%zu %s %llX %llX\n", i, g_modules[i].name.c_str(),
            (unsigned long long)g_modules[i].base,
            (unsigned long long)(g_modules[i].end - g_modules[i].base));

  auto sorted = [](const std::unordered_map<uint64_t, uint32_t>& m, size_t n) {
    std::vector<std::pair<uint64_t, uint32_t>> v(m.begin(), m.end());
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
    if (v.size() > n) v.resize(n);
    return v;
  };

  fprintf(f, "\n[threads] (samples = times found busy; ~1 per ms of CPU)\n");
  std::vector<const ThreadState*> ts;
  for (auto& [id, t] : g_threads)
    if (t.samples) ts.push_back(&t);
  std::sort(ts.begin(), ts.end(), [](auto* a, auto* b) { return a->samples > b->samples; });
  for (auto* t : ts) {
    fprintf(f, "tid %lu \"%s\" samples %llu (%.1f%% of one core)\n", t->tid, t->name.c_str(),
            (unsigned long long)t->samples, g_ticks ? t->samples * 100.0 / g_ticks : 0.0);
    for (auto& [k, c] : sorted(t->self, 12)) fprintf(f, "    %6u %s\n", c, KeyStr(k).c_str());
  }
  fprintf(f, "\n[self] (where the CPU was, all threads)\n");
  for (auto& [k, c] : sorted(g_self, 300)) fprintf(f, "%u %s\n", c, KeyStr(k).c_str());
  fprintf(f, "\n[inclusive] (on the call stack)\n");
  for (auto& [k, c] : sorted(g_incl, 600)) fprintf(f, "%u %s\n", c, KeyStr(k).c_str());
  fclose(f);
}

void ProfilerThread() {
  SetThreadDescription(GetCurrentThread(), L"TrialsHD profiler");
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
  timeBeginPeriod(1);
  LoadModules();
  g_profile_start = Clock::now();
  auto last_refresh = Clock::now() - std::chrono::seconds(10);
  auto last_report = Clock::now();
  uintptr_t frames[kMaxFrames];
  std::vector<uint64_t> seen;
  seen.reserve(kMaxFrames);
  // Roughly one core-millisecond of work between samples counts as busy.
  constexpr ULONG64 kBusyCycles = 300000;
  for (;;) {
    Sleep(1);
    auto now = Clock::now();
    if (now - last_refresh > std::chrono::seconds(2)) {
      RefreshThreads();
      last_refresh = now;
    }
    ++g_ticks;
    for (auto& [id, t] : g_threads) {
      if (!t.handle) continue;
      ULONG64 cycles = 0;
      if (!QueryThreadCycleTime(t.handle, &cycles)) continue;
      ULONG64 delta = cycles - t.last_cycles;
      t.last_cycles = cycles;
      if (delta < kBusyCycles) continue;
      if (SuspendThread(t.handle) == DWORD(-1)) continue;
      CONTEXT c{};
      c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
      int n = 0;
      if (GetThreadContext(t.handle, &c)) n = Unwind(c, frames);
      ResumeThread(t.handle);
      if (!n) continue;
      ++t.samples;
      ++g_total_samples;
      uint64_t k0 = Key(frames[0]);
      ++t.self[k0];
      ++g_self[k0];
      seen.clear();
      for (int i = 0; i < n; ++i) {
        uint64_t k = Key(frames[i]);
        if (std::find(seen.begin(), seen.end(), k) != seen.end()) continue;
        seen.push_back(k);
        ++g_incl[k];
      }
    }
    if (now - last_report > std::chrono::seconds(10)) {
      WriteReport();
      last_report = now;
    }
  }
}

}  // namespace

void SetHighTimerResolution() {
  // Windows rounds sleeps and timed waits up to the system timer tick (15.6 ms by default).
  // The game and the runtime sleep/wait in short steps every frame, so without this the frame
  // rate drops to ~30. Ask for 1 ms, and 0.5 ms through ntdll where available.
  timeBeginPeriod(1);
  using NtSetTimerResolutionFn = LONG(NTAPI*)(ULONG, BOOLEAN, PULONG);
  if (auto fn = reinterpret_cast<NtSetTimerResolutionFn>(
          GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtSetTimerResolution"))) {
    ULONG actual = 0;
    fn(5000, TRUE, &actual);  // 100 ns units
  }
}

void StartProfilerIfEnabled() {
  if (!rex::cvar::Query<bool>("trialshd_profile")) return;
  REXLOG_WARN("trialshd_profile: writing profile.txt every 10 seconds");
  std::thread(ProfilerThread).detach();
}

#else
void SetHighTimerResolution() {}
void StartProfilerIfEnabled() {}
#endif
