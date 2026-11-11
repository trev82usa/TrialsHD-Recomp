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
// Shaders are compiled when the game loads them, so changes apply after a restart.
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

REXCVAR_DEFINE_DOUBLE(trialshd_bloom, 1.0, "TrialsHD",
                      "Bloom (glow) strength: 1.0 = original, 0.5 = half, 0 = off. Applies after "
                      "a restart")
    .range(0.0, 2.0);
REXCVAR_DEFINE_DOUBLE(trialshd_dof, 1.0, "TrialsHD",
                      "Depth of field (background/foreground blur) strength: 1.0 = original, "
                      "0 = always sharp. Applies after a restart")
    .range(0.0, 1.0);
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

void PatchShaderText(char* buf, size_t len) {
  // Cheap filter: only HLSL post-process sources mention these names.
  std::string_view view(buf, len);
  const bool post = view.find("sampledColorACC") != std::string_view::npos;
  const bool mb = view.find("texColorBlur") != std::string_view::npos;
  if (!post && !mb) return;

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
