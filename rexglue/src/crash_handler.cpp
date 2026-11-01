// Crash reporting for test builds. On an unhandled exception (Windows) or fatal signal (Linux) it
// flushes the log and writes crash.txt next to the executable: the exception, the faulting
// address, and the call stack as module+offset (plus symbol names when a .pdb is present).
// Offsets can be resolved later against the symbols artifact from the same CI build.
#include "crash_handler.h"

#include <rex/logging/api.h>

#include <cstdio>
#include <cstdint>
#include <exception>
#include <string>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#  include <dbghelp.h>
#  pragma comment(lib, "dbghelp.lib")

namespace {

std::wstring CrashFilePath() {
  wchar_t buf[MAX_PATH];
  DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
  std::wstring p(buf, n);
  auto slash = p.find_last_of(L"\\/");
  return (slash == std::wstring::npos ? std::wstring() : p.substr(0, slash + 1)) + L"crash.txt";
}

void DescribeAddress(FILE* f, HANDLE process, DWORD64 addr) {
  HMODULE mod = nullptr;
  char modName[MAX_PATH] = "?";
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCSTR>(addr), &mod) && mod) {
    char full[MAX_PATH];
    if (GetModuleFileNameA(mod, full, MAX_PATH)) {
      const char* base = strrchr(full, '\\');
      snprintf(modName, sizeof(modName), "%s", base ? base + 1 : full);
    }
  }
  uint64_t offset = mod ? addr - reinterpret_cast<uint64_t>(mod) : addr;

  alignas(SYMBOL_INFO) char symBuf[sizeof(SYMBOL_INFO) + 256];
  auto* sym = reinterpret_cast<SYMBOL_INFO*>(symBuf);
  sym->SizeOfStruct = sizeof(SYMBOL_INFO);
  sym->MaxNameLen = 255;
  DWORD64 disp = 0;
  if (SymFromAddr(process, addr, &disp, sym))
    fprintf(f, "  %s+0x%llX  %s+0x%llX\n", modName, (unsigned long long)offset, sym->Name,
            (unsigned long long)disp);
  else
    fprintf(f, "  %s+0x%llX\n", modName, (unsigned long long)offset);
}

LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
  static volatile LONG entered = 0;
  if (InterlockedExchange(&entered, 1)) return EXCEPTION_CONTINUE_SEARCH;

  rex::FlushLogging();

  FILE* f = _wfopen(CrashFilePath().c_str(), L"w");
  if (!f) return EXCEPTION_CONTINUE_SEARCH;

  auto* rec = ep->ExceptionRecord;
  fprintf(f, "Trials HD crashed\n\nException 0x%08lX at ", rec->ExceptionCode);
  HANDLE process = GetCurrentProcess();
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS);
  SymInitialize(process, nullptr, TRUE);
  DescribeAddress(f, process, reinterpret_cast<DWORD64>(rec->ExceptionAddress));
  if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2)
    fprintf(f, "Access violation %s address 0x%llX\n",
            rec->ExceptionInformation[0] == 0 ? "reading" : rec->ExceptionInformation[0] == 1 ? "writing" : "executing",
            (unsigned long long)rec->ExceptionInformation[1]);
  fprintf(f, "Thread id %lu\n\nCall stack:\n", GetCurrentThreadId());

  CONTEXT ctx = *ep->ContextRecord;
  STACKFRAME64 frame{};
  frame.AddrPC.Offset = ctx.Rip;    frame.AddrPC.Mode = AddrModeFlat;
  frame.AddrFrame.Offset = ctx.Rbp; frame.AddrFrame.Mode = AddrModeFlat;
  frame.AddrStack.Offset = ctx.Rsp; frame.AddrStack.Mode = AddrModeFlat;
  for (int i = 0; i < 48; i++) {
    if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(), &frame, &ctx, nullptr,
                     SymFunctionTableAccess64, SymGetModuleBase64, nullptr) || !frame.AddrPC.Offset)
      break;
    DescribeAddress(f, process, frame.AddrPC.Offset);
  }
  fclose(f);
  return EXCEPTION_CONTINUE_SEARCH;
}

void OnTerminate() {
  rex::FlushLogging();
  if (FILE* f = _wfopen(CrashFilePath().c_str(), L"w")) {
    fprintf(f, "Trials HD terminated (std::terminate: uncaught C++ exception or abort)\n");
    fclose(f);
  }
  std::abort();
}

}  // namespace

void InstallCrashHandler() {
  SetUnhandledExceptionFilter(CrashFilter);
  std::set_terminate(OnTerminate);
}

#else

#include <csignal>
#include <execinfo.h>
#include <unistd.h>

namespace {
void OnSignal(int sig) {
  rex::FlushLogging();
  if (FILE* f = fopen("crash.txt", "w")) {
    fprintf(f, "Trials HD crashed with signal %d\n\nCall stack:\n", sig);
    void* frames[64];
    int n = backtrace(frames, 64);
    fflush(f);
    backtrace_symbols_fd(frames, n, fileno(f));
    fclose(f);
  }
  signal(sig, SIG_DFL);
  raise(sig);
}
}  // namespace

void InstallCrashHandler() {
  for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) signal(sig, OnSignal);
}

#endif
