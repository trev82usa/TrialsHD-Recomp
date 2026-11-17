// Game version the build targets: the original release, or Title Update 4 (TRIALSHD_TU4,
// set by CMake). Recompiled functions are named after their address, which moved in TU4;
// these names keep the hooks readable (G_<original address>) and pick the right one.
// The TU4 addresses come from tools/match_functions.py (original vs TU4 code).
#pragma once

#define TRIALSHD_IMP_(n) __imp__##n
#define TRIALSHD_IMP(n) TRIALSHD_IMP_(n)

#if defined(TRIALSHD_TU4)
#define G_82124A38 sub_82124F08
#define G_82125078 sub_82125548
#define G_821251D8 sub_821256A8
#define G_82125270 sub_82125740
#define G_82125348 sub_82125818
#define G_821253A8 sub_82125878
#define G_821254A8 sub_82125978
#define G_82143E10 sub_821442E0
#define G_82174280 sub_82174460
#define G_821BD0F0 sub_821BD6F8
#define G_821BE150 sub_821BE758
#define G_821BE348 sub_821BE950
#define G_821BE3F0 sub_821BE9F8
#define G_82333DB8 sub_82332790
#define G_82553918 sub_82553E28
#define G_825F0278 sub_825F4B98
// Offset of the post-process effect pointer in the post-process object (r18 in G_825F0278).
#define TRIALSHD_POST_EFFECT_FIELD 0x1CB7C
#else
#define G_82124A38 sub_82124A38
#define G_82125078 sub_82125078
#define G_821251D8 sub_821251D8
#define G_82125270 sub_82125270
#define G_82125348 sub_82125348
#define G_821253A8 sub_821253A8
#define G_821254A8 sub_821254A8
#define G_82143E10 sub_82143E10
#define G_82174280 sub_82174280
#define G_821BD0F0 sub_821BD0F0
#define G_821BE150 sub_821BE150
#define G_821BE348 sub_821BE348
#define G_821BE3F0 sub_821BE3F0
#define G_82333DB8 sub_82333DB8
#define G_82553918 sub_82553918
#define G_825F0278 sub_825F0278
#define TRIALSHD_POST_EFFECT_FIELD 0x1C37C
#endif
