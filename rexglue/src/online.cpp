// Self-hosted leaderboards for Trials HD.
//
// The game talks to Xbox Live through the XDK's XUser*/XSession* functions, which are compiled
// into the executable as small wrappers around XAM calls. We replace those wrappers (REX_HOOK_RAW
// by guest address) so that:
//   * the signed-in profile gets the player name from trialshd.toml and a stable XUID derived
//     from it, and reports "signed in to Live" when a server is configured;
//   * XSessionWriteStats sends the written leaderboard values to our server (in the background);
//   * XUserCreateStatsEnumeratorByRank/ByXuid + XEnumerate and XUserReadStats return rows from
//     the server, laid out exactly like the Xbox 360 XUSER_STATS_* structures.
//
// Everything is logged with the "[online]" prefix so the layouts and ids can be checked against a
// real session.
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xenumerator.h>
#include <rex/system/xobject.h>

#include "http_client.h"

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

REXCVAR_DEFINE_STRING(trialshd_player_name, "", "TrialsHD",
                      "Your name on leaderboards (1-15 letters/digits). Empty = 'User'");
REXCVAR_DEFINE_STRING(trialshd_server_url, "", "TrialsHD",
                      "Leaderboard server, e.g. https://trials.example.com. Empty = offline");
REXCVAR_DEFINE_STRING(trialshd_player_key, "", "TrialsHD",
                      "Your secret password for the leaderboard server (8+ characters)");
REXCVAR_DEFINE_STRING(trialshd_cf_client_id, "", "TrialsHD",
                      "Cloudflare Access service token Client ID (optional)");
REXCVAR_DEFINE_STRING(trialshd_cf_client_secret, "", "TrialsHD",
                      "Cloudflare Access service token Client Secret (optional)");

namespace {

// ---------------------------------------------------------------------------------------------
// Settings and identity
// ---------------------------------------------------------------------------------------------
std::string Str(const char* name) { return rex::cvar::Query<std::string>(name); }

std::string PlayerName() { return Str("trialshd_player_name").substr(0, 15); }

bool Online() {
  return !Str("trialshd_server_url").empty() && !PlayerName().empty() &&
         Str("trialshd_player_key").size() >= 8;
}

// Must match xuid_for() in server/trials_server.py.
uint64_t XuidFor(const std::string& name) {
  uint64_t h = 0xCBF29CE484222325ull;
  for (char c : name) {
    h ^= uint8_t(std::tolower(uint8_t(c)));
    h *= 0x100000001B3ull;
  }
  return 0x0009000000000000ull | (h & 0x0000FFFFFFFFFFFFull);
}

uint64_t MyXuid() { return XuidFor(PlayerName()); }

std::string Hex64(uint64_t v) {
  char b[17];
  snprintf(b, sizeof(b), "%016llX", (unsigned long long)v);
  return b;
}

// ---------------------------------------------------------------------------------------------
// Guest memory (big-endian, 32-bit addresses)
// ---------------------------------------------------------------------------------------------
struct Mem {
  uint8_t* base;
  uint8_t* p(uint32_t a) const { return base + a; }
  uint8_t u8(uint32_t a) const { return *p(a); }
  uint16_t u16(uint32_t a) const { uint16_t v; memcpy(&v, p(a), 2); return __builtin_bswap16(v); }
  uint32_t u32(uint32_t a) const { uint32_t v; memcpy(&v, p(a), 4); return __builtin_bswap32(v); }
  uint64_t u64(uint32_t a) const { uint64_t v; memcpy(&v, p(a), 8); return __builtin_bswap64(v); }
  void w8(uint32_t a, uint8_t v) const { *p(a) = v; }
  void w16(uint32_t a, uint16_t v) const { v = __builtin_bswap16(v); memcpy(p(a), &v, 2); }
  void w32(uint32_t a, uint32_t v) const { v = __builtin_bswap32(v); memcpy(p(a), &v, 4); }
  void w64(uint32_t a, uint64_t v) const { v = __builtin_bswap64(v); memcpy(p(a), &v, 8); }
};

std::string HexDump(const Mem& m, uint32_t a, uint32_t n) {
  std::string s;
  char b[4];
  for (uint32_t i = 0; i < n; ++i) {
    snprintf(b, sizeof(b), "%02X", m.u8(a + i));
    s += b;
    if (i % 4 == 3) s += ' ';
  }
  return s;
}

// XUSER_DATA types
enum : uint8_t { kContext = 0, kInt32 = 1, kInt64 = 2, kDouble = 3, kUnicode = 4, kFloat = 5,
                 kBinary = 6, kDateTime = 7, kNull = 0xFF };

// One leaderboard value: XUSER_DATA reduced to what leaderboards use.
struct Value {
  uint8_t type = kNull;
  int64_t i = 0;
  double d = 0;
};

// XUSER_DATA: BYTE type @0, union @8 (16 bytes total).
Value ReadData(const Mem& m, uint32_t a) {
  Value v;
  v.type = m.u8(a);
  switch (v.type) {
    case kContext:
    case kInt32: v.i = int32_t(m.u32(a + 8)); break;
    case kInt64:
    case kDateTime: v.i = int64_t(m.u64(a + 8)); break;
    case kDouble: { uint64_t b = m.u64(a + 8); memcpy(&v.d, &b, 8); break; }
    case kFloat: { uint32_t b = m.u32(a + 8); float f; memcpy(&f, &b, 4); v.d = f; break; }
    default: break;
  }
  return v;
}

void WriteData(const Mem& m, uint32_t a, const Value& v) {
  memset(m.p(a), 0, 16);
  m.w8(a, v.type);
  switch (v.type) {
    case kContext:
    case kInt32: m.w32(a + 8, uint32_t(int32_t(v.i))); break;
    case kInt64:
    case kDateTime: m.w64(a + 8, uint64_t(v.i)); break;
    case kDouble: { uint64_t b; memcpy(&b, &v.d, 8); m.w64(a + 8, b); break; }
    case kFloat: { float f = float(v.d); uint32_t b; memcpy(&b, &f, 4); m.w32(a + 8, b); break; }
    default: break;
  }
}

std::string ValueText(const Value& v) {
  char b[64];
  if (v.type == kDouble || v.type == kFloat) snprintf(b, sizeof(b), "%.17g", v.d);
  else snprintf(b, sizeof(b), "%lld", (long long)v.i);
  return b;
}

// ---------------------------------------------------------------------------------------------
// Server access
// ---------------------------------------------------------------------------------------------
HttpResponse Call(const std::string& method, const std::string& path, const std::string& body) {
  std::string url = Str("trialshd_server_url");
  while (!url.empty() && url.back() == '/') url.pop_back();
  std::vector<std::pair<std::string, std::string>> h = {
      {"X-Player-Name", PlayerName()},
      {"X-Player-Key", Str("trialshd_player_key")},
      {"Content-Type", "application/json"},
  };
  if (auto id = Str("trialshd_cf_client_id"); !id.empty()) {
    h.push_back({"CF-Access-Client-Id", id});
    h.push_back({"CF-Access-Client-Secret", Str("trialshd_cf_client_secret")});
  }
  auto r = HttpRequest(method, url + path, h, body, 6000);
  if (r.status != 200)
    REXLOG_WARN("[online] {} {} -> {} {} {}", method, path, r.status, r.error,
                r.body.substr(0, 200));
  return r;
}

// Background queue for score uploads, so finishing a track never waits for the network.
std::mutex g_q_mutex;
std::condition_variable g_q_cv;
std::deque<std::string> g_queue;

void UploadThread() {
  for (;;) {
    std::string body;
    {
      std::unique_lock lock(g_q_mutex);
      g_q_cv.wait(lock, [] { return !g_queue.empty(); });
      body = std::move(g_queue.front());
      g_queue.pop_front();
    }
    auto r = Call("POST", "/api/v1/stats", body);
    if (r.status == 200) REXLOG_WARN("[online] scores uploaded: {}", r.body);
  }
}

void QueueUpload(std::string body) {
  static std::once_flag once;
  std::call_once(once, [] { std::thread(UploadThread).detach(); });
  {
    std::lock_guard lock(g_q_mutex);
    g_queue.push_back(std::move(body));
  }
  g_q_cv.notify_one();
}

// A leaderboard row as returned by the server's text format:
//   total\t<n>
//   <xuid hex>\t<rank>\t<rating>\t<name>\t<id>:<type>:<value>;<id>:<type>:<value>...
struct Row {
  uint64_t xuid = 0;
  uint32_t rank = 0;
  int64_t rating = 0;
  std::string name;
  std::vector<std::pair<uint32_t, Value>> props;  // in write order
};

struct Board {
  bool ok = false;
  uint32_t total = 0;
  std::vector<Row> rows;
};

Board ParseBoard(const HttpResponse& r) {
  Board b;
  if (r.status != 200) return b;
  b.ok = true;
  std::istringstream in(r.body);
  std::string line;
  while (std::getline(in, line)) {
    std::vector<std::string> f;
    size_t s = 0, t;
    while ((t = line.find('\t', s)) != std::string::npos) {
      f.push_back(line.substr(s, t - s));
      s = t + 1;
    }
    f.push_back(line.substr(s));
    if (f.size() == 2 && f[0] == "total") {
      b.total = uint32_t(std::stoul(f[1]));
      continue;
    }
    if (f.size() < 4) continue;
    Row row;
    row.xuid = std::stoull(f[0], nullptr, 16);
    row.rank = uint32_t(std::stoul(f[1]));
    row.rating = std::stoll(f[2]);
    row.name = f[3].substr(0, 15);
    if (f.size() > 4) {
      std::istringstream ps(f[4]);
      std::string item;
      while (std::getline(ps, item, ';')) {
        unsigned id = 0, type = 0;
        char val[64] = {};
        if (sscanf(item.c_str(), "%u:%u:%63s", &id, &type, val) == 3) {
          Value v;
          v.type = uint8_t(type);
          if (v.type == kDouble || v.type == kFloat) v.d = atof(val);
          else v.i = atoll(val);
          row.props.push_back({id, v});
        }
      }
    }
    b.rows.push_back(std::move(row));
  }
  return b;
}

// ---------------------------------------------------------------------------------------------
// XUSER_STATS_* layout helpers
//   XUSER_STATS_SPEC         { DWORD viewId; DWORD numColumnIds; WORD columnIds[64]; }  136 bytes
//   XUSER_STATS_READ_RESULTS { DWORD numViews; PXUSER_STATS_VIEW pViews; }               8
//   XUSER_STATS_VIEW         { DWORD viewId; DWORD totalViewRows; DWORD numRows; PROW }  16
//   XUSER_STATS_ROW          { XUID; DWORD rank; (pad) LONGLONG rating; CHAR tag[16];
//                              DWORD numColumns; PXUSER_STATS_COLUMN pColumns; }         48
//   XUSER_STATS_COLUMN       { WORD columnId; (pad) XUSER_DATA value; }                  24
// ---------------------------------------------------------------------------------------------
struct Spec {
  uint32_t view = 0;
  std::vector<uint16_t> columns;
};

std::vector<Spec> ReadSpecs(const Mem& m, uint32_t count, uint32_t ptr) {
  std::vector<Spec> specs;
  for (uint32_t i = 0; i < count && i < 64; ++i) {
    uint32_t a = ptr + i * 136;
    Spec s;
    s.view = m.u32(a);
    uint32_t n = std::min<uint32_t>(m.u32(a + 4), 64);
    for (uint32_t c = 0; c < n; ++c) s.columns.push_back(m.u16(a + 8 + c * 2));
    specs.push_back(std::move(s));
  }
  return specs;
}

std::string SpecsText(const std::vector<Spec>& specs) {
  std::string s;
  for (auto& sp : specs) {
    s += "view " + std::to_string(sp.view) + " cols[";
    for (auto c : sp.columns) s += std::to_string(c) + " ";
    s += "] ";
  }
  return s;
}

uint32_t ResultsSize(const std::vector<Spec>& specs, uint32_t rows_per_view) {
  uint32_t size = 8;
  for (auto& s : specs) size += 16 + rows_per_view * (48 + 24 * uint32_t(s.columns.size()));
  return size;
}

// Which written property a requested column shows. Trials HD's Live configuration (seen in
// XSessionWriteStats / XUserReadStats calls):
//   column 1      -> 0x10000002  track time in ms (shown as the time)
//   column 2      -> 0x10000001
//   column 3      -> 0x10000003  faults
//   columns 4-16  -> 0x10000009.. checkpoint times
//   columns 31-44 -> 0x20000048.. 64-bit per-checkpoint data
// Rows are ranked by 0x20000006 = -(faults << 32 | time), higher is better.
uint32_t ColumnProperty(uint16_t column) {
  if (column == 1) return 0x10000002;
  if (column == 2) return 0x10000001;
  if (column == 3) return 0x10000003;
  if (column >= 4 && column <= 16) return 0x10000009 + (column - 4);
  if (column >= 31 && column <= 44) return 0x20000048 + (column - 31);
  return 0;
}

const Value* ColumnValue(const Row& row, uint16_t column) {
  uint32_t want = ColumnProperty(column);
  for (auto& [id, v] : row.props)
    if (id == want) return &v;
  return nullptr;
}

// Writes XUSER_STATS_READ_RESULTS at guest address `buf` (capacity `cap`). `boards[i]` holds
// the rows for specs[i]. Returns false if it does not fit.
bool WriteResults(const Mem& m, uint32_t buf, uint32_t cap, const std::vector<Spec>& specs,
                  const std::vector<Board>& boards) {
  uint32_t need = 8;
  for (size_t i = 0; i < specs.size(); ++i)
    need += 16 + uint32_t(boards[i].rows.size()) * (48 + 24 * uint32_t(specs[i].columns.size()));
  if (need > cap) return false;
  memset(m.p(buf), 0, need);
  uint32_t views = buf + 8;
  uint32_t cursor = views + 16 * uint32_t(specs.size());
  m.w32(buf, uint32_t(specs.size()));
  m.w32(buf + 4, views);
  for (size_t i = 0; i < specs.size(); ++i) {
    const Spec& sp = specs[i];
    const Board& b = boards[i];
    uint32_t view = views + 16 * uint32_t(i);
    uint32_t rows = cursor;
    cursor += 48 * uint32_t(b.rows.size());
    m.w32(view, sp.view);
    m.w32(view + 4, b.total);
    m.w32(view + 8, uint32_t(b.rows.size()));
    m.w32(view + 12, b.rows.empty() ? 0 : rows);
    for (size_t r = 0; r < b.rows.size(); ++r) {
      const Row& row = b.rows[r];
      uint32_t ra = rows + 48 * uint32_t(r);
      m.w64(ra, row.xuid);
      m.w32(ra + 8, row.rank);
      m.w64(ra + 16, uint64_t(row.rating));
      memcpy(m.p(ra + 24), row.name.c_str(), std::min<size_t>(row.name.size(), 15));
      m.w32(ra + 40, uint32_t(sp.columns.size()));
      uint32_t cols = cursor;
      cursor += 24 * uint32_t(sp.columns.size());
      m.w32(ra + 44, sp.columns.empty() ? 0 : cols);
      for (size_t c = 0; c < sp.columns.size(); ++c) {
        uint32_t ca = cols + 24 * uint32_t(c);
        m.w16(ca, sp.columns[c]);
        const Value* v = ColumnValue(row, sp.columns[c]);
        WriteData(m, ca + 8, v ? *v : Value{});
      }
    }
  }
  return true;
}

std::string BoardLog(const Board& b) {
  std::string s = "total " + std::to_string(b.total) + ":";
  for (auto& r : b.rows) s += " #" + std::to_string(r.rank) + " " + r.name + "=" +
                              std::to_string(r.rating);
  return s;
}

// ---------------------------------------------------------------------------------------------
// Stats enumerator (XUserCreateStatsEnumeratorByRank / ByXuid)
// ---------------------------------------------------------------------------------------------
class StatsEnumerator : public rex::system::XEnumerator {
 public:
  StatsEnumerator(rex::system::KernelState* ks, bool by_rank, uint32_t rank_start, uint64_t pivot,
                  uint32_t num_rows, std::vector<Spec> specs)
      : XEnumerator(ks, 1, ResultsSize(specs, num_rows)),
        by_rank_(by_rank), rank_start_(rank_start), pivot_(pivot), num_rows_(num_rows),
        specs_(std::move(specs)) {}

  uint32_t WriteItems(uint32_t buffer_ptr, uint8_t* buffer_data,
                      uint32_t* written_count) override {
    *written_count = 0;
    if (done_) return 18;  // ERROR_NO_MORE_FILES
    done_ = true;
    Mem m{buffer_data - buffer_ptr};
    std::vector<Board> boards;
    for (auto& sp : specs_) {
      std::string q = "/api/v1/leaderboard?fmt=text&view=" + std::to_string(sp.view) +
                      "&count=" + std::to_string(num_rows_);
      q += by_rank_ ? "&start=" + std::to_string(rank_start_) : "&around=" + Hex64(pivot_);
      Board b = ParseBoard(Call("GET", q, ""));
      REXLOG_WARN("[online] enumerate view {} ({}): {}", sp.view,
                  by_rank_ ? "by rank " + std::to_string(rank_start_) : "around " + Hex64(pivot_),
                  BoardLog(b));
      boards.push_back(std::move(b));
    }
    if (!WriteResults(m, buffer_ptr, uint32_t(item_size()), specs_, boards)) {
      REXLOG_WARN("[online] enumerate: results do not fit in {} bytes", item_size());
      return 122;  // ERROR_INSUFFICIENT_BUFFER
    }
    *written_count = 1;
    return 0;
  }

 private:
  bool by_rank_;
  uint32_t rank_start_;
  uint64_t pivot_;
  uint32_t num_rows_;
  std::vector<Spec> specs_;
  bool done_ = false;
};

uint32_t CreateStatsEnumerator(const Mem& m, bool by_rank, uint32_t rank_start, uint64_t pivot,
                               uint32_t num_rows, uint32_t num_specs, uint32_t specs_ptr,
                               uint32_t pcb_ptr, uint32_t phandle_ptr) {
  auto specs = ReadSpecs(m, num_specs, specs_ptr);
  num_rows = std::min<uint32_t>(num_rows, 100);
  REXLOG_WARN("[online] create stats enumerator {} start={} pivot={} rows={} {}",
              by_rank ? "ByRank" : "ByXuid", rank_start, Hex64(pivot), num_rows, SpecsText(specs));
  auto* ks = REX_KERNEL_STATE();
  auto e = rex::system::make_object<StatsEnumerator>(ks, by_rank, rank_start, pivot, num_rows,
                                                     specs);
  if (XFAILED(e->Initialize(0, 0xFB, 0, 0, 0))) return 1627;  // ERROR_FUNCTION_FAILED
  if (pcb_ptr) m.w32(pcb_ptr, uint32_t(e->item_size()));
  if (phandle_ptr) m.w32(phandle_ptr, e->handle());
  return 0;
}

// ---------------------------------------------------------------------------------------------
// Friends enumerator (XFriendsCreateEnumerator). Items are XONLINE_FRIEND, 196 bytes:
//   XUID @0, CHAR gamertag[16] @8, DWORD friendState @24, XNKID session @28, DWORD titleId @36,
//   FILETIME userTime @40, XNKID invite @48, FILETIME inviteTime @56, DWORD cchRichPresence @64,
//   WCHAR richPresence[64] @68
// ---------------------------------------------------------------------------------------------
constexpr uint32_t kFriendSize = 196;

class FriendsEnumerator : public rex::system::XEnumerator {
 public:
  FriendsEnumerator(rex::system::KernelState* ks, uint32_t start, uint32_t count)
      : XEnumerator(ks, count, kFriendSize), start_(start), count_(count) {}

  uint32_t WriteItems(uint32_t buffer_ptr, uint8_t* buffer_data,
                      uint32_t* written_count) override {
    *written_count = 0;
    if (done_) return 18;  // ERROR_NO_MORE_FILES
    done_ = true;
    Mem m{buffer_data - buffer_ptr};
    auto r = Call("GET", "/api/v1/friends?fmt=text", "");
    std::vector<std::pair<uint64_t, std::string>> friends;
    std::istringstream in(r.status == 200 ? r.body : "");
    std::string line;
    while (std::getline(in, line)) {
      auto tab = line.find('\t');
      if (tab == std::string::npos) continue;
      friends.push_back({std::stoull(line.substr(0, tab), nullptr, 16),
                         line.substr(tab + 1, 15)});
    }
    std::string names;
    uint32_t n = 0;
    for (size_t i = start_; i < friends.size() && n < count_; ++i, ++n) {
      uint32_t a = buffer_ptr + n * kFriendSize;
      memset(m.p(a), 0, kFriendSize);
      m.w64(a, friends[i].first);
      memcpy(m.p(a + 8), friends[i].second.data(), friends[i].second.size());
      m.w32(a + 36, 0x5841095A);  // Trials HD title id
      names += " " + friends[i].second;
    }
    REXLOG_WARN("[online] friends ({}):{}", n, names);
    *written_count = n;
    return n ? 0 : 18;
  }

 private:
  uint32_t start_, count_;
  bool done_ = false;
};

}  // namespace

// ---------------------------------------------------------------------------------------------
// Hooks (addresses of the XDK wrappers inside default.xex)
// ---------------------------------------------------------------------------------------------

// XUserGetSigninInfo(userIndex, flags, XUSER_SIGNIN_INFO*)
REX_EXTERN(__imp__sub_82125078);
REX_HOOK_RAW(sub_82125078) {
  uint32_t user = ctx.r3.u32, info = ctx.r5.u32;
  __imp__sub_82125078(ctx, base);
  if (ctx.r3.u32 != 0 || user != 0 || !info || PlayerName().empty()) return;
  Mem m{base};
  m.w64(info, MyXuid());
  if (Online()) m.w32(info + 12, 2);  // eXUserSigninState_SignedInToLive
  memset(m.p(info + 24), 0, 16);
  std::string n = PlayerName();
  memcpy(m.p(info + 24), n.data(), n.size());
}

// XUserGetSigninState(userIndex)
REX_EXTERN(__imp__sub_821251D8);
REX_HOOK_RAW(sub_821251D8) {
  uint32_t user = ctx.r3.u32;
  __imp__sub_821251D8(ctx, base);
  if (user == 0 && ctx.r3.u32 == 1 && Online()) ctx.r3.u64 = 2;
}

// XUserGetXUID(userIndex, XUID*)
REX_EXTERN(__imp__sub_821254A8);
REX_HOOK_RAW(sub_821254A8) {
  uint32_t user = ctx.r3.u32, out = ctx.r4.u32;
  __imp__sub_821254A8(ctx, base);
  if (ctx.r3.u32 == 0 && user == 0 && out && !PlayerName().empty()) Mem{base}.w64(out, MyXuid());
}

// XSessionWriteStats(hSession, xuid, numViews, XSESSION_VIEW_PROPERTIES*, XOVERLAPPED*)
//   XSESSION_VIEW_PROPERTIES { DWORD viewId; DWORD numProperties; XUSER_PROPERTY* }  12 bytes
//   XUSER_PROPERTY           { DWORD propertyId; (pad) XUSER_DATA value; }           24 bytes
REX_EXTERN(__imp__sub_821BE3F0);
REX_HOOK_RAW(sub_821BE3F0) {
  Mem m{base};
  uint64_t xuid = ctx.r4.u64;
  uint32_t num_views = ctx.r5.u32, views = ctx.r6.u32;
  std::string json = "{\"views\":[";
  std::string logline;
  for (uint32_t i = 0; i < num_views && i < 32; ++i) {
    uint32_t v = views + 12 * i;
    uint32_t view_id = m.u32(v), n = m.u32(v + 4), props = m.u32(v + 8);
    if (i) json += ",";
    json += "{\"view\":" + std::to_string(view_id) + ",\"props\":[";
    logline += " view " + std::to_string(view_id) + ":";
    for (uint32_t j = 0; j < n && j < 64; ++j) {
      uint32_t p = props + 24 * j;
      uint32_t id = m.u32(p);
      Value val = ReadData(m, p + 8);
      if (j) json += ",";
      json += "{\"id\":" + std::to_string(id) + ",\"type\":" + std::to_string(val.type) +
              ",\"value\":" + ValueText(val) + "}";
      char b[96];
      snprintf(b, sizeof(b), " %08X(t%u)=%s", id, val.type, ValueText(val).c_str());
      logline += b;
    }
    json += "]}";
    if (n) REXLOG_WARN("[online] raw view {}: {}", view_id, HexDump(m, props, std::min(n, 4u) * 24));
  }
  json += "]}";
  REXLOG_WARN("[online] XSessionWriteStats xuid={}{}", Hex64(xuid), logline);
  if (Online()) QueueUpload(json);
  __imp__sub_821BE3F0(ctx, base);
}

// XSessionFlushStats(hSession, overlapped): only logged.
REX_EXTERN(__imp__sub_821BE348);
REX_HOOK_RAW(sub_821BE348) {
  REXLOG_WARN("[online] XSessionFlushStats");
  __imp__sub_821BE348(ctx, base);
}

// XUserCreateStatsEnumeratorByRank(titleId, rankStart, numRows, numSpecs, specs, pcb, phEnum)
REX_EXTERN(__imp__sub_82125348);
REX_HOOK_RAW(sub_82125348) {
  if (!Online()) return __imp__sub_82125348(ctx, base);
  ctx.r3.u64 = CreateStatsEnumerator(Mem{base}, true, ctx.r4.u32, 0, ctx.r5.u32, ctx.r6.u32,
                                     ctx.r7.u32, ctx.r8.u32, ctx.r9.u32);
}

// XUserCreateStatsEnumeratorByXuid(titleId, xuidPivot, numRows, numSpecs, specs, pcb, phEnum)
REX_EXTERN(__imp__sub_821253A8);
REX_HOOK_RAW(sub_821253A8) {
  if (!Online()) return __imp__sub_821253A8(ctx, base);
  ctx.r3.u64 = CreateStatsEnumerator(Mem{base}, false, 0, ctx.r4.u64, ctx.r5.u32, ctx.r6.u32,
                                     ctx.r7.u32, ctx.r8.u32, ctx.r9.u32);
}

// XUserReadStats(titleId, numXuids, xuids, numSpecs, specs, pcbResults, results, overlapped)
REX_EXTERN(__imp__sub_82125270);
REX_HOOK_RAW(sub_82125270) {
  if (!Online()) return __imp__sub_82125270(ctx, base);
  Mem m{base};
  uint32_t num_xuids = std::min<uint32_t>(ctx.r4.u32, 100), xuids_ptr = ctx.r5.u32;
  auto specs = ReadSpecs(m, ctx.r6.u32, ctx.r7.u32);
  uint32_t pcb = ctx.r8.u32, results = ctx.r9.u32, overlapped = ctx.r10.u32;
  std::vector<uint64_t> xuids;
  for (uint32_t i = 0; i < num_xuids; ++i) xuids.push_back(m.u64(xuids_ptr + 8 * i));

  uint32_t need = ResultsSize(specs, num_xuids);
  uint32_t have = pcb ? m.u32(pcb) : 0;
  REXLOG_WARN("[online] XUserReadStats {} xuids, {} buffer {}/{}", num_xuids, SpecsText(specs),
              have, need);
  if (!results || have < need) {
    if (pcb) m.w32(pcb, need);
    ctx.r3.u64 = 122;  // ERROR_INSUFFICIENT_BUFFER
    return;
  }
  std::vector<Board> boards;
  for (auto& sp : specs) {
    std::string body = "{\"view\":" + std::to_string(sp.view) + ",\"xuids\":[";
    for (size_t i = 0; i < xuids.size(); ++i) body += (i ? ",\"" : "\"") + Hex64(xuids[i]) + "\"";
    body += "]}";
    Board found = ParseBoard(Call("POST", "/api/v1/read?fmt=text", body));
    // One row per requested XUID, in the order asked, empty if the player has no time yet.
    Board b;
    b.ok = found.ok;
    b.total = found.total;
    for (uint64_t x : xuids) {
      auto it = std::find_if(found.rows.begin(), found.rows.end(),
                             [x](const Row& r) { return r.xuid == x; });
      if (it != found.rows.end()) {
        b.rows.push_back(*it);
      } else {
        Row r;
        r.xuid = x;
        if (x == MyXuid()) r.name = PlayerName();
        b.rows.push_back(r);
      }
    }
    REXLOG_WARN("[online] read view {}: {}", sp.view, BoardLog(b));
    boards.push_back(std::move(b));
  }
  uint32_t result = WriteResults(m, results, have, specs, boards) ? 0 : 122;
  if (overlapped) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediateEx(overlapped, result, 0, 0);
    ctx.r3.u64 = 997;  // ERROR_IO_PENDING
  } else {
    ctx.r3.u64 = result;
  }
}

// XFriendsCreateEnumerator(userIndex, startingIndex, friendsToReturn, pcbBuffer, phEnum)
REX_EXTERN(__imp__sub_821BD0F0);
REX_HOOK_RAW(sub_821BD0F0) {
  if (!Online()) return __imp__sub_821BD0F0(ctx, base);
  Mem m{base};
  uint32_t start = ctx.r4.u32, count = std::min<uint32_t>(ctx.r5.u32, 100);
  uint32_t pcb = ctx.r6.u32, phandle = ctx.r7.u32;
  REXLOG_WARN("[online] XFriendsCreateEnumerator start={} count={}", start, count);
  auto e = rex::system::make_object<FriendsEnumerator>(REX_KERNEL_STATE(), start, count ? count : 1);
  if (XFAILED(e->Initialize(0, 0xFC, 0, 0, 0))) {
    ctx.r3.u64 = 1627;
    return;
  }
  if (pcb) m.w32(pcb, (count ? count : 1) * kFriendSize);
  if (phandle) m.w32(phandle, e->handle());
  ctx.r3.u64 = 0;
}

// XSessionLeaveRemote/Local(hSession, count, users, overlapped): ReXGlue does not implement the
// message (XGI 0x000B0013), so the game saw a failure after every posted time and showed
// "Please reconnect". Leaving a session needs no work here: report success.
REX_EXTERN(__imp__sub_821BE150);
REX_HOOK_RAW(sub_821BE150) {
  if (!Online()) return __imp__sub_821BE150(ctx, base);
  uint32_t overlapped = ctx.r6.u32;
  if (overlapped) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediateEx(overlapped, 0, 0, 0);
    ctx.r3.u64 = 997;  // ERROR_IO_PENDING
  } else {
    ctx.r3.u64 = 0;
  }
}
