// Bloom and depth-of-field strength.
//
// Trials HD ships its post-processing shaders as HLSL source inside data_misc.pak and compiles
// them at load time. Every file comes out of the pak through zlib's inflate (the game links
// zlib 1.2.3 three times: sub_82174280, sub_82333DB8, sub_82553918). We wrap inflate and, when
// the freshly decompressed text is one of those shaders, edit the bloom / depth-of-field
// constants in place - always replacing text with text of the same length, so nothing else in
// the buffer moves. 100% = untouched original shaders.
//
//   postCombine.fx, postCombinePreview.fx, postCombineBlur.fx
//     bloom: "sampledColorACC*sampledColorACC*0.8f"            -> 0.8 * bloom
//     DoF:   "lerp(sampledColor, sampledColorACC * 2.0f, saturate(focusBlur))"
//            -> "...saturate(focusBlur)*.NN)"                  (blend amount * dof)
//   motionBlurDOFBloom.fx
//     bloom: "texColorBlur*texColorBlur*1.2"                   -> 1.2 * bloom
//     DoF:   "if (blurFactor > 0) blurFactor *= 0.5;"
//            -> "blurFactor*=(blurFactor>0)?A:B;"               (0.5*dof far, 1.0*dof near)
//   brightness: "return userInput3.rrrr * (" -> "return userInput3.r*B.BB*("
//
//   brightness: "return userInput3.rrrr * (" -> "return userInput3.r*B.BB*("   (startup only)
//
// LIVE MODE (default): the post-process shaders get a new parameter
//   float4 trialsFx = {bloom, brightness, dof, 0}   (defaults 1,1,1 = original look)
// and the constants above become e.g. "...*0.8f*trialsFx.x". Room for the extra text comes from
// deleting the shader's comments; the file keeps its exact length (padded with spaces). Every
// frame the game sets "dofParameters" on the post-process effect through its own
// SetValue(name, data, bytes) method; we wrap that method (by redirecting its entry in the
// recompiled code's indirect-call table) and set "trialsFx" right after, from the current
// slider values. So the F1 sliders change the picture immediately.
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#include "generated/default/trialshd_init.h"

REXCVAR_DEFINE_DOUBLE(trialshd_bloom, 1.0, "TrialsHD",
                      "Bloom (glow) strength: 1.0 = original, 0.5 = half, 0 = off. Applies after "
                      "a restart")
    .range(0.0, 2.0);
REXCVAR_DEFINE_DOUBLE(trialshd_dof, 1.0, "TrialsHD",
                      "Depth of field (background/foreground blur) strength: 1.0 = original, "
                      "0 = always sharp. Applies after a restart")
    .range(0.0, 1.0);
REXCVAR_DEFINE_BOOL(trialshd_live_effects, true, "TrialsHD",
                    "Bloom / depth of field / brightness change instantly from the F1 menu. Set "
                    "false to bake them into the shaders at startup instead (needs a restart)");
REXCVAR_DEFINE_DOUBLE(trialshd_brightness, 1.0, "TrialsHD",
                      "Picture brightness while riding: 1.0 = original, 0.8 = 20% darker. Applies "
                      "after a restart")
    .range(0.5, 1.5);

namespace {

std::atomic<int> g_patched{0};

// Replace every occurrence of `from` in buf with `to` (same length), returns count.
int ReplaceAll(char* buf, size_t len, std::string_view from, std::string_view to) {
  if (from.size() != to.size() || from.empty() || len < from.size()) return 0;
  int n = 0;
  char* end = buf + len;
  for (char* p = buf; p + from.size() <= end;) {
    char* hit = std::search(p, end, from.begin(), from.end());
    if (hit == end) break;
    std::memcpy(hit, to.data(), to.size());
    ++n;
    p = hit + to.size();
  }
  return n;
}

std::string Fmt(const char* f, double v) {
  char b[32];
  std::snprintf(b, sizeof(b), f, v);
  return b;
}

std::string PadTo(std::string s, size_t n) {
  if (s.size() < n) s.append(n - s.size(), ' ');
  return s;
}

std::atomic<bool> g_live_shaders{false};

// Removes // and /* */ comments.
std::string StripComments(std::string_view in) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size();) {
    if (in.compare(i, 2, "//") == 0) {
      while (i < in.size() && in[i] != '\n') ++i;
    } else if (in.compare(i, 2, "/*") == 0) {
      size_t e = in.find("*/", i + 2);
      i = e == std::string_view::npos ? in.size() : e + 2;
      out += ' ';
    } else {
      out += in[i++];
    }
  }
  return out;
}

int ReplaceAllStr(std::string& s, std::string_view from, std::string_view to) {
  int n = 0;
  for (size_t pos = 0; (pos = s.find(from, pos)) != std::string::npos; pos += to.size(), ++n)
    s.replace(pos, from.size(), to);
  return n;
}

// Live version of the post-process edits; returns false if it doesn't apply / doesn't fit.
bool PatchPostShaderLive(char* buf, size_t len) {
  std::string_view view(buf, len);
  if (view.find("technique") == std::string_view::npos) return false;  // need the whole file
  std::string t = StripComments(view);
  int n = 0;
  n += ReplaceAllStr(t, "sampledColorACC*sampledColorACC*0.8f",
                     "sampledColorACC*sampledColorACC*(0.8f*trialsFx.x)");
  n += ReplaceAllStr(t, "lerp(sampledColor, sampledColorACC * 2.0f, saturate(focusBlur))",
                     "lerp(sampledColor, sampledColorACC * 2.0f, saturate(focusBlur)*trialsFx.z)");
  n += ReplaceAllStr(t, "return userInput3.rrrr * (", "return userInput3.rrrr * trialsFx.y * (");
  if (n == 0) return false;
  t = "float4 trialsFx = {1.0f, 1.0f, 1.0f, 0.0f};\n" + t;
  if (t.size() > len) {
    REXLOG_WARN("[fx] live effects: shader too long after edit ({} > {})", t.size(), len);
    return false;
  }
  t.append(len - t.size(), ' ');
  std::memcpy(buf, t.data(), len);
  g_live_shaders = true;
  if (g_patched.fetch_add(1) < 8) REXLOG_WARN("[fx] post-process shader set up for live sliders ({} edits)", n);
  return true;
}

void PatchShaderText(char* buf, size_t len) {
  // Cheap filter: only HLSL post-process sources mention these names.
  std::string_view view(buf, len);
  const bool post = view.find("sampledColorACC") != std::string_view::npos;
  const bool mb = view.find("texColorBlur") != std::string_view::npos;
  if (!post && !mb) return;
  if (post && rex::cvar::Query<bool>("trialshd_live_effects") && PatchPostShaderLive(buf, len))
    return;

  const double bloom = std::clamp(rex::cvar::Query<double>("trialshd_bloom"), 0.0, 2.0);
  const double dof = std::clamp(rex::cvar::Query<double>("trialshd_dof"), 0.0, 1.0);
  const double bright = std::clamp(rex::cvar::Query<double>("trialshd_brightness"), 0.5, 1.5);
  int n = 0;
  if (bright != 1.0) {
    // Final output multiplier of postCombine / Preview / Blur / Ghost.
    n += ReplaceAll(buf, len, "return userInput3.rrrr * (",
                    "return userInput3.r*" + Fmt("%.2f", bright) + "*(");
  }
  if (bloom != 1.0) {
    n += ReplaceAll(buf, len, "sampledColorACC*sampledColorACC*0.8f",
                    "sampledColorACC*sampledColorACC*" + Fmt("%.2f", 0.8 * bloom));  // "0.40"
    n += ReplaceAll(buf, len, "texColorBlur*texColorBlur*1.2",
                    "texColorBlur*texColorBlur*" + Fmt("%.1f", 1.2 * bloom));  // "0.6"
  }
  if (dof < 1.0) {
    std::string d = Fmt("%.2f", dof);  // "0.50" -> ".50" so the line keeps its length
    if (d[0] == '0') d.erase(0, 1);
    const std::string_view lerp_from =
        "lerp(sampledColor, sampledColorACC * 2.0f, saturate(focusBlur))";
    std::string lerp_to = "lerp(sampledColor,sampledColorACC*2.0f,saturate(focusBlur)*" + d;
    lerp_to = PadTo(lerp_to, lerp_from.size() - 1) + ")";
    n += ReplaceAll(buf, len, lerp_from, lerp_to);

    const std::string_view mb_from = "if (blurFactor > 0) blurFactor *= 0.5;";
    std::string mb_to = "blurFactor*=(blurFactor>0)?" + Fmt("%.2f", 0.5 * dof) + ":" +
                        Fmt("%.2f", 1.0 * dof) + ";";
    n += ReplaceAll(buf, len, mb_from, PadTo(mb_to, mb_from.size()));
  }
  if (n > 0 && g_patched.fetch_add(1) < 8)
    REXLOG_WARN("[fx] adjusted a post-process shader: bloom {:.0f}%, depth of field {:.0f}%, "
                "brightness {:.0f}% ({} edits)", bloom * 100, dof * 100, bright * 100, n);
}

// z_stream (32-bit big-endian guest): next_out is at offset 12.
uint32_t NextOut(uint8_t* base, uint32_t strm) {
  uint32_t v;
  std::memcpy(&v, base + strm + 12, 4);
  return __builtin_bswap32(v);
}

void AfterInflate(uint8_t* base, uint32_t strm, uint32_t out_before) {
  if (!strm || !out_before) return;
  const uint32_t out_after = NextOut(base, strm);
  if (out_after <= out_before || out_after - out_before > (4u << 20)) return;
  PatchShaderText(reinterpret_cast<char*>(base + out_before), out_after - out_before);
}

}  // namespace

#define TRIALSHD_WRAP_INFLATE(name)                         \
  REX_EXTERN(__imp__##name);                                \
  REX_HOOK_RAW(name) {                                      \
    const uint32_t strm = ctx.r3.u32;                       \
    const uint32_t before = strm ? NextOut(base, strm) : 0; \
    __imp__##name(ctx, base);                               \
    AfterInflate(base, strm, before);                       \
  }

TRIALSHD_WRAP_INFLATE(sub_82174280)
TRIALSHD_WRAP_INFLATE(sub_82333DB8)
TRIALSHD_WRAP_INFLATE(sub_82553918)

// ---------------------------------------------------------------------------------------------
// Live values
// ---------------------------------------------------------------------------------------------
namespace {

PPCFunc* g_orig_set_value = nullptr;  // the game's effect SetValue(name, data, bytes)
uint32_t g_guest_name = 0;            // "trialsFx" in guest memory
uint32_t g_guest_data = 0;            // float4 in guest memory
std::atomic<bool> g_hook_installed{false};

bool NameIs(uint8_t* base, uint32_t ptr, const char* want) {
  // Names passed by the post-process code are string literals inside the game image.
  if (ptr < 0x82000000u || ptr >= 0x82830000u) return false;
  return std::strcmp(reinterpret_cast<const char*>(base + ptr), want) == 0;
}

void PutFloatBE(uint8_t* p, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  v = __builtin_bswap32(v);
  std::memcpy(p, &v, 4);
}

// Replaces the game's SetValue: forwards every call, and after "dofParameters" also sets
// "trialsFx" on the same effect.
void SetValueSpy(PPCContext& ctx, uint8_t* base) {
  const uint32_t obj = ctx.r3.u32, name = ctx.r4.u32;
  g_orig_set_value(ctx, base);
  if (!g_live_shaders || !NameIs(base, name, "dofParameters")) return;
  const uint64_t ret = ctx.r3.u64;
  uint8_t* d = base + g_guest_data;
  PutFloatBE(d + 0, float(std::clamp(rex::cvar::Query<double>("trialshd_bloom"), 0.0, 2.0)));
  PutFloatBE(d + 4, float(std::clamp(rex::cvar::Query<double>("trialshd_brightness"), 0.5, 1.5)));
  PutFloatBE(d + 8, float(std::clamp(rex::cvar::Query<double>("trialshd_dof"), 0.0, 1.0)));
  PutFloatBE(d + 12, 0.0f);
  ctx.r3.u64 = obj;
  ctx.r4.u64 = g_guest_name;
  ctx.r5.u64 = g_guest_data;
  ctx.r6.u64 = 16;
  g_orig_set_value(ctx, base);
  ctx.r3.u64 = ret;
}

// Called with the post-process object once per frame (from the hook below): finds its effect's
// SetValue and routes it through SetValueSpy.
void InstallSetValueSpy(uint8_t* base, uint32_t post_obj) {
  if (g_hook_installed || !g_live_shaders || !post_obj) return;
  auto be32 = [base](uint32_t a) {
    uint32_t v;
    std::memcpy(&v, base + a, 4);
    return __builtin_bswap32(v);
  };
  const uint32_t effect = be32(post_obj + 0x1C37C);  // same field the game reads (r18)
  if (!effect) return;
  const uint32_t vtable = be32(effect);
  const uint32_t target = vtable ? be32(vtable + 12) : 0;
  if (target < REX_CODE_BASE || target >= REX_CODE_BASE + REX_CODE_SIZE) return;
  auto** slot = reinterpret_cast<PPCFunc**>(base + REX_IMAGE_BASE + REX_IMAGE_SIZE +
                                            (uint64_t(target - REX_CODE_BASE) * 2));
  PPCFunc* orig = *slot ? *slot : rex::runtime::ResolveIndirectFunction(target);
  if (!orig) return;
  auto* mem = REX_KERNEL_MEMORY();
  g_guest_name = mem->SystemHeapAlloc(16);
  g_guest_data = mem->SystemHeapAlloc(16);
  if (!g_guest_name || !g_guest_data) return;
  std::memcpy(base + g_guest_name, "trialsFx", 9);
  g_orig_set_value = orig;
  *slot = &SetValueSpy;
  g_hook_installed = true;
  REXLOG_WARN("[fx] live effect sliders active (SetValue at {:08X})", target);
}

}  // namespace

// sub_825F0278: the post-process pass (draws postCombine*, sets dofParameters etc.), r3 = the
// post-process object.
REX_EXTERN(__imp__sub_825F0278);
REX_HOOK_RAW(sub_825F0278) {
  const uint32_t obj = ctx.r3.u32;
  if (!g_hook_installed) InstallSetValueSpy(base, obj);
  __imp__sub_825F0278(ctx, base);
}
