// ============================================================================
//  MHW RNG Official Plugin: Fatalis Phase 1 Bipedal Pure-RNG Controller (`rng_fatalis.dll`)
//  Version : 1.0.0 (SDK v2 Showcase Implementation)
//  Target  : `RVA 0x1339C8C` (`cThinkMgr::updateThk` @ 0x141339640 `CALL MtRandom::nrand`)
//
//  Strict Pure-RNG Architecture:
//    - Interacts ONLY with `mhw_rng.dll` via `MhwRngHostApi::RegisterSlotHook`.
//    - ZERO extra game detours/hooks, ZERO `.thk` file generation or patching,
//      ZERO writes to game AI state or memory.
//    - Reads `cThinkMgr` live candidate array (`mgr + 0x40`), THK call-stack
//      depth (`mgr + 0x4F8`), current node array index (`endSeg + 0x24 - 10000`),
//      caller node index (`mgr + 0x364`), and `Monster*` (`MonsterID == 101`)
//      under SEH protection, then returns an exact `uint32_t` midpoint into
//      `cThinkMgr::updateThk`'s native weighted selector.
//    - Scoped strictly to Fatalis (`em013`, `MonsterID == 101`) Phase 1 Bipedal
//      decision nodes (`em013_00` idx `75..79` + `55[87]` non-cone suppression,
//      plus toggleable Phase 1 bipedal guards `14/15/18/46/129..133`).
//      Non-Fatalis monsters, Phase 2/3, and Quadruped combat nodes are untouched.
// ============================================================================
#include "mhw_rng_api.h"

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace {

// Verified MHW:I 15.23 (Steam 15539686, ImageBase 0x140000000) RVAs & Offsets:
constexpr std::uint32_t  kRvaThkUpdateRandom     = 0x1339C8CU; // `cThinkMgr::updateThk` `CALL MtRandom::nrand`
constexpr std::uintptr_t kRvaCallerEmAiTick      = 0x132B83DU; // `cThinkEm::update` @ 0x14132B7B0 (PRIMARY large monster AI tick)
constexpr std::uintptr_t kRvaCallerAiTick        = 0x133B6F0U; // `FUN_14133b640(cThinkEm*)` (secondary AI tick)
constexpr std::uintptr_t kRvaCallerPrefetchTick  = 0x133A876U; // `FUN_14133a850(cThinkMgr*)` (prefetch AI tick)
constexpr std::uintptr_t kRvaCThinkMgrVft        = 0x324F438U; // `cThinkMgr::vft`
constexpr std::uintptr_t kRvaCThinkEmVft         = 0x324F320U; // `cThinkEm::vft`

constexpr std::uint32_t  kMonsterIdFatalis       = 101U;       // `em013`
constexpr std::uintptr_t kOffMonsterId           = 0x12280U;
constexpr std::uintptr_t kOffThinkEmToMgr        = 0x20U;
constexpr std::uintptr_t kOffThinkEmMonster      = 0x08U;
constexpr std::uintptr_t kOffMgrCandidates       = 0x40U;      // `ThkCandidate[30]` (`{ int32_t segIdx; float weight; }`)
constexpr std::uintptr_t kOffMgrFrame0JumpStack  = 0x200U;     // `20 * 0x10` (`{ uintptr_t nodeBase; uint64_t segIdx; }`)
constexpr std::uintptr_t kOffMgrFrame0JumpCount  = 0x340U;     // `int32_t` return stack count (0..20)
constexpr std::uintptr_t kOffMgrFrameBase        = 0x180U;     // Stack frame stride (`d * 0x180`)
constexpr std::uintptr_t kOffMgrNodeBase         = 0x350U;     // `frameBase + 0x350`: `THKSegment*`
constexpr std::uintptr_t kOffMgrThkFileIdx       = 0x35CU;     // `frameBase + 0x35C`: `int32_t` (0 = `_00`, 55 = `_55`)
constexpr std::uintptr_t kOffMgrCallerNodeIdx    = 0x364U;     // Frame 0 caller node array idx when `depth == 1`
constexpr std::uintptr_t kOffMgrDepth            = 0x4F8U;     // `uint32_t` call stack depth (0 or 1)
constexpr std::uintptr_t kOffMgrMonster          = 0x508U;     // `Monster*` cached on `cThinkMgr`
constexpr std::uintptr_t kThkSegStride           = 0x80U;      // 128 bytes per `THKSegment`
constexpr std::size_t    kMaxThkCandidates       = 30U;
constexpr std::size_t    kMaxSegmentsPerNode     = 256U;
constexpr std::int32_t   kVanillaEm013NodeCount  = 223;

// Open float interval `(0.0f, 1.0f)` bounds for `cThinkMgr::updateThk` (`acc <= roll && roll < acc + weight[k]`):
constexpr std::uint32_t  kMinSafeFloatU32        = 0x00000001U;
constexpr std::uint32_t  kMaxSafeFloatU32        = 0xFFFFFF00U;

// Condition IDs (`1011..1018`) for kernel telemetry & `rng_inspector` breakdown:
enum FatalisCondId : std::uint32_t {
    kCondCloseUnderbreath   = 1011U, // <900 -> 55:297 孕吐
    kCondSuppressCone       = 1012U, // 55[87] ENDR -> 不派生扇火
    kCondMidFrontCarpet     = 1013U, // 900~2000 正面 -> 55:184 不转扇火正面地毯火
    kCondMidSideCarpet      = 1014U, // 900~2000 侧面 -> 55:188 侧地毯火
    kCondMidBackTail        = 1015U, // 900~2000 背面 -> 55:175 2足甩尾
    kCondLongCarpet         = 1016U, // 2000~4500 -> 55:184 / 55:188 不转扇火地毯火
    kCondFarFireball        = 1017U, // >4500 -> 55:179 单发火球 (or 55:220 三连火球)
    kCondP1BipedalGuard     = 1018U  // 一阶段二足稳态守卫 (14/15/18)
};

#pragma pack(push, 1)
struct ThkCandidate {
    std::int32_t segIdx;
    float        weight;
};
#pragma pack(pop)
static_assert(sizeof(ThkCandidate) == 8U, "ThkCandidate must be 8 bytes");

struct SegmentOption {
    std::int32_t segIdx;
    float        weight;
    std::uint32_t ctrlFlags;
    std::int32_t targetThkFile;
    std::int32_t targetNodeId;
};

enum class ReadFailReason : std::uint8_t {
    kNone          = 0,
    kBadCaller     = 1,
    kBadMgrVft     = 2,
    kBadMonsterPtr = 3,
    kBadDepthOrNode = 4,
    kBadCandidates = 5
};

struct LiveThkContext {
    bool           isValid           = false;
    bool           isFatalis         = false;
    ReadFailReason failReason        = ReadFailReason::kNone;
    std::uint32_t  observedCallerRva = 0U;
    std::uint32_t  depth             = 0U;
    std::int32_t   thkFileIdx        = -1;
    std::int32_t   nodeArrIdx        = -1;
    std::int32_t   callerNodeIdx     = -1;
    std::size_t    candCount         = 0U;
    float          totalWeight       = 0.0f;
    SegmentOption  cands[kMaxThkCandidates]{};
};

struct FatalisHistoryEntry {
    bool          valid         = false;
    std::uint32_t condId        = 0U;
    std::int32_t  nodeArrIdx    = -1;
    std::int32_t  chosenSegIdx  = -1;
    std::int32_t  targetNodeId  = -1;
    float         chosenWeight  = 0.0f;
    float         totalWeight   = 0.0f;
    std::uint32_t injectedU32   = 0U;
    char          bandTag[32]{};
    char          moveTag[40]{};
};

struct DiagSnapshot {
    std::uint64_t enabledCalls      = 0ULL;
    std::uint64_t badCallerCount    = 0ULL;
    std::uint64_t badVftOrPtrCount  = 0ULL;
    std::uint64_t nonFatalisCount   = 0ULL;
    std::uint64_t fatalisUnmatched  = 0ULL;
    std::uint32_t lastBadCallerRva  = 0U;
    std::uint32_t lastUnmatchDepth  = 0U;
    std::int32_t  lastUnmatchFile   = -1;
    std::int32_t  lastUnmatchNode   = -1;
    std::int32_t  lastUnmatchSeg0   = -1;
    std::int32_t  lastUnmatchCaller = -1;
    std::int32_t  lastUnmatchTFile  = -1;
    std::int32_t  lastUnmatchTNode  = -1;
};

constexpr std::size_t kHistoryCap = 8U;

const MhwRngHostApi* g_host   = nullptr;
MhwRngPluginId       g_selfId = 0U;

// Strict leaf mutex: NEVER call `g_host->...` while holding `g_stateMx`!
std::mutex           g_stateMx;
bool                 g_enabled          = false; // Safe default off; enable via `$rng fatalis on`
bool                 g_guardsEnabled    = true;  // Phase 1 auxiliary guards (14/15/18/46/129..133)
bool                 g_farMultiFireball = false; // false = 55:179 (单发火球), true = 55:220 (三连火球)
std::uint64_t        g_controlledRolls  = 0ULL;
FatalisHistoryEntry  g_history[kHistoryCap]{};
std::size_t          g_historyCount     = 0U;
DiagSnapshot         g_diag{};

// ============================================================================
//  Helpers & Math
// ============================================================================

std::string LowerAscii(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    }
    return s;
}

bool IsValidUserPtr(std::uintptr_t p, std::uintptr_t alignMask = 0x7ULL) noexcept {
    return p >= 0x100000ULL && p <= 0x00007FFFFFFF0000ULL && (p & alignMask) == 0ULL;
}

std::uint32_t FractionToOpenSafeU32(double frac0To1) noexcept {
    if (!(frac0To1 > 0.0)) return kMinSafeFloatU32;
    if (frac0To1 >= 1.0) return kMaxSafeFloatU32;
    const auto u = static_cast<std::uint64_t>(frac0To1 * static_cast<double>(4294967296.0) + 0.5);
    if (u <= kMinSafeFloatU32) return kMinSafeFloatU32;
    if (u >= kMaxSafeFloatU32) return kMaxSafeFloatU32;
    return static_cast<std::uint32_t>(u);
}

void CopyTrunc(char* dst, std::size_t cap, const char* src) noexcept {
    if (!dst || cap == 0U) return;
    if (!src) { dst[0] = '\0'; return; }
    std::size_t i = 0U;
    while (i + 1U < cap && src[i] != '\0') {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
}

// ============================================================================
//  SEH-Safe Live THK Context Extraction from `cThinkMgr::updateThk`
// ============================================================================

// Locate node END sentinel (`seg[0] & 1`) to recover exact `nodeArrIdx` (`endVal - 10000`)
std::int32_t ResolveNodeArrIdx(std::uintptr_t nodeBase) noexcept {
    if (!IsValidUserPtr(nodeBase)) return -1;
    for (std::size_t s = 0U; s < kMaxSegmentsPerNode; ++s) {
        const std::uintptr_t segAddr = nodeBase + s * kThkSegStride;
        if ((*reinterpret_cast<const std::uint8_t*>(segAddr) & 0x01U) != 0U) {
            return *reinterpret_cast<const std::int32_t*>(segAddr + 0x24U) - 10000;
        }
    }
    return -1;
}

bool ReadLiveThkContextSEH(const MhwRngDrawContext* ctx, LiveThkContext& out) noexcept {
    if (!ctx || !ctx->returnAddrSlot || !ctx->exeBase) {
        out.failReason = ReadFailReason::kBadCaller;
        return false;
    }
    __try {
        const auto* slotPtr = static_cast<const std::uintptr_t*>(ctx->returnAddrSlot);
        const std::uintptr_t callerRet = slotPtr[20];
        if (callerRet >= ctx->exeBase && callerRet < ctx->exeEnd) {
            out.observedCallerRva = static_cast<std::uint32_t>(callerRet - ctx->exeBase);
        }

        std::uintptr_t mgr     = 0U;
        std::uintptr_t thinkEm = 0U;
        std::uintptr_t monster = 0U;

        if (callerRet == (ctx->exeBase + kRvaCallerEmAiTick)) {
            thinkEm = slotPtr[23]; // `updateThk` saved caller `RSI` (`MOV [RSP+0xB0], RSI`)
            mgr     = thinkEm + kOffThinkEmToMgr;
            monster = slotPtr[19]; // `updateThk` pushed caller `RDI` (`PUSH RDI`)
        } else if (callerRet == (ctx->exeBase + kRvaCallerAiTick)) {
            thinkEm = slotPtr[19]; // caller `RDI`
            mgr     = thinkEm + kOffThinkEmToMgr;
            monster = slotPtr[21]; // caller `RBX`
        } else if (callerRet == (ctx->exeBase + kRvaCallerPrefetchTick)) {
            mgr     = slotPtr[21]; // caller `RBX`
            thinkEm = (mgr >= kOffThinkEmToMgr) ? (mgr - kOffThinkEmToMgr) : 0U;
        } else {
            out.failReason = ReadFailReason::kBadCaller;
            return false;
        }

        if (!IsValidUserPtr(mgr) ||
            *reinterpret_cast<const std::uintptr_t*>(mgr) != (ctx->exeBase + kRvaCThinkMgrVft)) {
            out.failReason = ReadFailReason::kBadMgrVft;
            return false;
        }

        if (!IsValidUserPtr(monster)) {
            monster = *reinterpret_cast<const std::uintptr_t*>(mgr + kOffMgrMonster);
        }
        if (!IsValidUserPtr(monster) && IsValidUserPtr(thinkEm) &&
            *reinterpret_cast<const std::uintptr_t*>(thinkEm) == (ctx->exeBase + kRvaCThinkEmVft)) {
            monster = *reinterpret_cast<const std::uintptr_t*>(thinkEm + kOffThinkEmMonster);
        }
        if (!IsValidUserPtr(monster)) {
            out.failReason = ReadFailReason::kBadMonsterPtr;
            return false;
        }

        out.isValid   = true;
        out.isFatalis = (*reinterpret_cast<const std::uint32_t*>(monster + kOffMonsterId) == kMonsterIdFatalis);
        if (!out.isFatalis) return true;

        const std::uint32_t depth = *reinterpret_cast<const std::uint32_t*>(mgr + kOffMgrDepth);
        if (depth > 1U) {
            out.failReason = ReadFailReason::kBadDepthOrNode;
            return false;
        }
        out.depth = depth;

        const std::uintptr_t frameBase = mgr + static_cast<std::uintptr_t>(depth) * kOffMgrFrameBase;
        const std::uintptr_t nodeBase  = *reinterpret_cast<const std::uintptr_t*>(frameBase + kOffMgrNodeBase);
        if (!IsValidUserPtr(nodeBase)) {
            out.failReason = ReadFailReason::kBadDepthOrNode;
            return false;
        }

        out.thkFileIdx = *reinterpret_cast<const std::int32_t*>(frameBase + kOffMgrThkFileIdx);
        if (depth == 1U) {
            std::int32_t callerIdx = *reinterpret_cast<const std::int32_t*>(mgr + kOffMgrCallerNodeIdx);
            if (callerIdx >= kVanillaEm013NodeCount) {
                // Unwind Frame 0 local jump return stack (`mgr + 0x200`) when instrumented THK uses wrapper nodes
                const std::int32_t jCnt = *reinterpret_cast<const std::int32_t*>(mgr + kOffMgrFrame0JumpCount);
                if (jCnt >= 2 && jCnt <= 20) {
                    for (std::int32_t k = jCnt - 2; k >= 0; --k) {
                        const std::uintptr_t parentBase = *reinterpret_cast<const std::uintptr_t*>(
                            mgr + kOffMgrFrame0JumpStack + static_cast<std::uintptr_t>(k) * 0x10U);
                        const std::int32_t pIdx = ResolveNodeArrIdx(parentBase);
                        if (pIdx >= 0 && pIdx < kVanillaEm013NodeCount) {
                            callerIdx = pIdx;
                            break;
                        }
                    }
                }
            }
            out.callerNodeIdx = callerIdx;
        }

        const std::int32_t resolvedArrIdx = ResolveNodeArrIdx(nodeBase);
        if (resolvedArrIdx < 0) {
            out.failReason = ReadFailReason::kBadDepthOrNode;
            return false;
        }
        out.nodeArrIdx = resolvedArrIdx;

        // Read populated candidate table at `mgr + 0x40` (`Candidate[30]`)
        const auto* cands = reinterpret_cast<const ThkCandidate*>(mgr + kOffMgrCandidates);
        float sumW = 0.0f;
        std::size_t n = 0U;
        for (std::size_t i = 0U; i < kMaxThkCandidates; ++i) {
            if (cands[i].segIdx < 0 || cands[i].segIdx >= static_cast<std::int32_t>(kMaxSegmentsPerNode)) {
                break;
            }
            const float w = cands[i].weight;
            if (!std::isfinite(w) || w < 0.0f) {
                out.failReason = ReadFailReason::kBadCandidates;
                return false;
            }

            const std::uintptr_t seg = nodeBase + static_cast<std::uintptr_t>(cands[i].segIdx) * kThkSegStride;
            SegmentOption& dst = out.cands[n++];
            dst.segIdx    = cands[i].segIdx;
            dst.weight    = w;
            dst.ctrlFlags = *reinterpret_cast<const std::uint32_t*>(seg + 0x00U);

            std::int32_t tf = *reinterpret_cast<const std::int32_t*>(seg + 0x28U);
            std::int32_t tn = *reinterpret_cast<const std::int32_t*>(seg + 0x2CU);
            if (tf < 0) {
                // Follow 1-hop local wrapper nodes (`seg + 0x30` -> trampoline node containing `file=55, node=ID`)
                const std::uintptr_t locPtr = *reinterpret_cast<const std::uintptr_t*>(seg + 0x30U);
                if (IsValidUserPtr(locPtr)) {
                    for (std::size_t ws = 0U; ws < 8U; ++ws) {
                        const std::uintptr_t wseg = locPtr + ws * kThkSegStride;
                        if ((*reinterpret_cast<const std::uint8_t*>(wseg) & 0x01U) != 0U) break;
                        const std::int32_t wtf = *reinterpret_cast<const std::int32_t*>(wseg + 0x28U);
                        if (wtf >= 0) {
                            tf = wtf;
                            tn = *reinterpret_cast<const std::int32_t*>(wseg + 0x2CU);
                            break;
                        }
                    }
                }
            }
            dst.targetThkFile = tf;
            dst.targetNodeId  = tn;
            sumW += w;
        }

        if (n == 0U || !(sumW > 0.0f)) {
            out.failReason = ReadFailReason::kBadCandidates;
            return false;
        }
        out.candCount   = n;
        out.totalWeight = sumW;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out.failReason = ReadFailReason::kBadMgrVft;
        return false;
    }
}

// ============================================================================
//  Pure-RNG Fatalis Phase 1 Bipedal Decision Matcher & Solver
// ============================================================================

struct MatchOutcome {
    bool          matched      = false;
    std::size_t   chosenCand   = 0U;
    std::uint32_t condId       = 0U;
    const char*   bandTag      = "";
    const char*   moveTag      = "";
};

std::int32_t FindTargetCand(const LiveThkContext& lc, std::int32_t fileIdx, std::int32_t nodeId) noexcept {
    for (std::size_t i = 0U; i < lc.candCount; ++i) {
        if (lc.cands[i].weight > 0.0f &&
            lc.cands[i].targetThkFile == fileIdx &&
            lc.cands[i].targetNodeId == nodeId) {
            return static_cast<std::int32_t>(i);
        }
    }
    return -1;
}

std::int32_t FindCtrlKindCand(const LiveThkContext& lc, std::uint32_t ctrlKindMask60) noexcept {
    for (std::size_t i = 0U; i < lc.candCount; ++i) {
        if (lc.cands[i].weight > 0.0f &&
            (lc.cands[i].ctrlFlags & 0xC0U) == ctrlKindMask60) {
            return static_cast<std::int32_t>(i);
        }
    }
    return -1;
}

MatchOutcome MatchFatalisPhase1Rule(const LiveThkContext& lc,
                                    bool guardsEnabled,
                                    bool farMultiFireball) noexcept {
    MatchOutcome res{};
    if (!lc.isValid || !lc.isFatalis || lc.candCount == 0U) return res;
    const std::int32_t firstSeg = lc.cands[0].segIdx;

    // ------------------------------------------------------------------------
    //  Depth 1 (`em013_55.thk`): Non-Cone Suppression for Phase 1 Under-Breath
    // ------------------------------------------------------------------------
    if (lc.depth == 1U && lc.thkFileIdx == 55) {
        if (lc.nodeArrIdx == 87 &&
            (lc.callerNodeIdx == 75 || lc.callerNodeIdx == 76 ||
             (guardsEnabled && (lc.callerNodeIdx == 129 || lc.callerNodeIdx == 130)))) {
            const std::int32_t idx = FindCtrlKindCand(lc, 0x80U); // `ENDR` -> skip 35% cone follow-up
            if (idx >= 0) {
                res.matched    = true;
                res.chosenCand = static_cast<std::size_t>(idx);
                res.condId     = kCondSuppressCone;
                res.bandTag    = "P1 孕吐派生判定";
                res.moveTag    = "不派生扇火(ENDR)";
            }
        }
        return res;
    }

    if (lc.depth != 0U || lc.thkFileIdx != 0) return res;

    // ------------------------------------------------------------------------
    //  Depth 0 (`em013_00.thk`): Primary Phase 1 Bipedal Attack Nodes (75..79)
    //  and Optional Phase 1 Knockdown Follow-up Clones (129..133)
    // ------------------------------------------------------------------------

    // 1. Close range (<900): 75 (<=500), 76 (500..900), [129/130 follow-up] -> 55:297 孕吐
    if (lc.nodeArrIdx == 75 || lc.nodeArrIdx == 76 ||
        (guardsEnabled && (lc.nodeArrIdx == 129 || lc.nodeArrIdx == 130))) {
        const std::int32_t idx = FindTargetCand(lc, 55, 297);
        if (idx >= 0) {
            res.matched    = true;
            res.chosenCand = static_cast<std::size_t>(idx);
            res.condId     = kCondCloseUnderbreath;
            res.bandTag    = (lc.nodeArrIdx == 75 || lc.nodeArrIdx == 129) ? "<500 近距离" : "500~900 近距离";
            res.moveTag    = "孕吐(55:297)";
        }
        return res;
    }

    // 2. Mid/Long range (900~2000): 77 [131 follow-up]
    if (lc.nodeArrIdx == 77 || (guardsEnabled && lc.nodeArrIdx == 131)) {
        if (firstSeg == 1) {
            // Front (315..45 deg) -> 55:184 正面地毯火(不转扇火)
            const std::int32_t idx = FindTargetCand(lc, 55, 184);
            if (idx >= 0) {
                res.matched    = true;
                res.chosenCand = static_cast<std::size_t>(idx);
                res.condId     = kCondMidFrontCarpet;
                res.bandTag    = "900~2000 正面";
                res.moveTag    = "地毯火不转扇火(55:184)";
            }
        } else if ((lc.nodeArrIdx == 77 && firstSeg == 13) || (lc.nodeArrIdx == 131 && firstSeg == 12)) {
            // Back (135..225 deg) -> 55:175 2足甩尾
            const std::int32_t idx = FindTargetCand(lc, 55, 175);
            if (idx >= 0) {
                res.matched    = true;
                res.chosenCand = static_cast<std::size_t>(idx);
                res.condId     = kCondMidBackTail;
                res.bandTag    = "900~2000 背面";
                res.moveTag    = "2足甩尾(55:175)";
            }
        } else if (firstSeg >= 25) {
            // Sides (45..135 / 225..315 / fallback) -> 55:188 侧地毯火
            const std::int32_t idx = FindTargetCand(lc, 55, 188);
            if (idx >= 0) {
                res.matched    = true;
                res.chosenCand = static_cast<std::size_t>(idx);
                res.condId     = kCondMidSideCarpet;
                res.bandTag    = "900~2000 侧面";
                res.moveTag    = "侧地毯火(55:188)";
            }
        }
        return res;
    }

    // 3. Extra-Long range (2000~4500): 78 [132 follow-up] -> 55:184 (front/back) or 55:188 (side)
    if (lc.nodeArrIdx == 78 || (guardsEnabled && lc.nodeArrIdx == 132)) {
        std::int32_t idx = FindTargetCand(lc, 55, 184);
        const char* mv   = "地毯火不转扇火(55:184)";
        const char* bd   = (firstSeg == 1) ? "2000~4500 正面" : "2000~4500 背面";
        if (idx < 0) {
            idx = FindTargetCand(lc, 55, 188);
            mv  = "侧地毯火(55:188)";
            bd  = "2000~4500 侧面";
        }
        if (idx >= 0) {
            res.matched    = true;
            res.chosenCand = static_cast<std::size_t>(idx);
            res.condId     = kCondLongCarpet;
            res.bandTag    = bd;
            res.moveTag    = mv;
        }
        return res;
    }

    // 4. Ultra-Long range (>4500): 79 [133 follow-up] -> 55:179 单发火球 (or 55:220 三连火球)
    if (lc.nodeArrIdx == 79 || (guardsEnabled && lc.nodeArrIdx == 133)) {
        const std::int32_t targetId = farMultiFireball ? 220 : 179;
        const std::int32_t idx      = FindTargetCand(lc, 55, targetId);
        if (idx >= 0) {
            res.matched    = true;
            res.chosenCand = static_cast<std::size_t>(idx);
            res.condId     = kCondFarFireball;
            res.bandTag    = ">4500 超远距离";
            res.moveTag    = farMultiFireball ? "三连火球(55:220)" : "单发火球(55:179)";
        }
        return res;
    }

    // ------------------------------------------------------------------------
    //  Optional Phase 1 Bipedal Auxiliary Guards (`g_guardsEnabled`)
    // ------------------------------------------------------------------------
    if (!guardsEnabled) return res;

    // A. Occluded Phase 1 sectors inside node `idx=46` (`firstSeg >= 53` is Phase 1 ONLY)
    if (lc.nodeArrIdx == 46 && firstSeg >= 53) {
        std::int32_t idx = FindTargetCand(lc, 55, 184);
        const char* mv   = "地毯火不转扇火(55:184)";
        if (idx < 0) {
            idx = FindTargetCand(lc, 55, 188);
            mv  = "侧地毯火(55:188)";
        }
        if (idx >= 0) {
            res.matched    = true;
            res.chosenCand = static_cast<std::size_t>(idx);
            res.condId     = kCondLongCarpet;
            res.bandTag    = "P1 遮挡补偿";
            res.moveTag    = mv;
        }
        return res;
    }

    // B. Phase 1 Bipedal 7-Move 15% Extra Charge (`idx=18`, `firstSeg==7`) -> `ENDR` skip
    if (lc.nodeArrIdx == 18 && firstSeg == 7) {
        const std::int32_t idx = FindCtrlKindCand(lc, 0x80U);
        if (idx >= 0) {
            res.matched    = true;
            res.chosenCand = static_cast<std::size_t>(idx);
            res.condId     = kCondP1BipedalGuard;
            res.bandTag    = "P1 额外龙车判定";
            res.moveTag    = "跳过15%额外龙车(ENDR)";
        }
        return res;
    }

    // C. Phase 1 Stance Transition (`idx=14` quest 51612 / `idx=15` quest 51613)
    if ((lc.nodeArrIdx == 14 && firstSeg == 22) || (lc.nodeArrIdx == 15 && firstSeg == 14)) {
        const std::int32_t idx = FindCtrlKindCand(lc, 0x80U); // `ENDR` wt=90 -> stay bipedal
        if (idx >= 0) {
            res.matched    = true;
            res.chosenCand = static_cast<std::size_t>(idx);
            res.condId     = kCondP1BipedalGuard;
            res.bandTag    = "P1 二足保持判定";
            res.moveTag    = "维持二足态(ENDR)";
        }
        return res;
    }
    if ((lc.nodeArrIdx == 14 && firstSeg == 49) || (lc.nodeArrIdx == 15 && firstSeg == 41)) {
        const std::int32_t idx = FindCtrlKindCand(lc, 0x40U); // `RANDOM` wt=10 -> stand back up asap
        if (idx >= 0) {
            res.matched    = true;
            res.chosenCand = static_cast<std::size_t>(idx);
            res.condId     = kCondP1BipedalGuard;
            res.bandTag    = "P1 四足起身判定";
            res.moveTag    = "加快站立回二足(RANDOM)";
        }
        return res;
    }

    return res;
}

std::uint32_t SolveCandidateU32(const LiveThkContext& lc, std::size_t chosenCand) noexcept {
    double cumBefore = 0.0;
    for (std::size_t i = 0U; i < chosenCand; ++i) {
        cumBefore += static_cast<double>(lc.cands[i].weight);
    }
    const double mid  = cumBefore + 0.5 * static_cast<double>(lc.cands[chosenCand].weight);
    const double frac = mid / static_cast<double>(lc.totalWeight);
    return FractionToOpenSafeU32(frac);
}

// ============================================================================
//  Pull-Mode Hook on `RVA 0x1339C8C` (`cThinkMgr::updateThk`)
// ============================================================================

void __fastcall OnThkRandomDraw(const MhwRngDrawContext* ctx,
                                MhwRngHookDecision* ioDecision,
                                void*) {
    if (!ioDecision) return;

    bool enabled = false;
    bool guards  = true;
    bool farMulti = false;
    {
        std::lock_guard<std::mutex> lk(g_stateMx);
        enabled  = g_enabled;
        guards   = g_guardsEnabled;
        farMulti = g_farMultiFireball;
    }

    if (!enabled) {
        ioDecision->action = kMhwRngActionUseCondition;
        ioDecision->condId = 0U;
        return;
    }

    LiveThkContext lc{};
    const bool readOk = ReadLiveThkContextSEH(ctx, lc);
    if (!readOk || !lc.isValid || !lc.isFatalis) {
        ioDecision->action = kMhwRngActionUseCondition;
        ioDecision->condId = 0U;
        std::lock_guard<std::mutex> lk(g_stateMx);
        ++g_diag.enabledCalls;
        if (!readOk) {
            if (lc.failReason == ReadFailReason::kBadCaller) {
                ++g_diag.badCallerCount;
                g_diag.lastBadCallerRva = lc.observedCallerRva;
            } else {
                ++g_diag.badVftOrPtrCount;
            }
        } else if (lc.isValid && !lc.isFatalis) {
            ++g_diag.nonFatalisCount;
        }
        return;
    }

    const MatchOutcome m = MatchFatalisPhase1Rule(lc, guards, farMulti);
    if (!m.matched) {
        ioDecision->action = kMhwRngActionUseCondition;
        ioDecision->condId = 0U;
        std::lock_guard<std::mutex> lk(g_stateMx);
        ++g_diag.enabledCalls;
        ++g_diag.fatalisUnmatched;
        g_diag.lastUnmatchDepth  = lc.depth;
        g_diag.lastUnmatchFile   = lc.thkFileIdx;
        g_diag.lastUnmatchNode   = lc.nodeArrIdx;
        g_diag.lastUnmatchSeg0   = (lc.candCount > 0U) ? lc.cands[0].segIdx : -1;
        g_diag.lastUnmatchCaller = lc.callerNodeIdx;
        g_diag.lastUnmatchTFile  = (lc.candCount > 0U) ? lc.cands[0].targetThkFile : -1;
        g_diag.lastUnmatchTNode  = (lc.candCount > 0U) ? lc.cands[0].targetNodeId : -1;
        return;
    }

    const std::uint32_t solvedU32 = SolveCandidateU32(lc, m.chosenCand);
    ioDecision->action = kMhwRngActionReturnValue;
    ioDecision->condId = m.condId;
    ioDecision->value0 = solvedU32;

    std::lock_guard<std::mutex> lk(g_stateMx);
    ++g_diag.enabledCalls;
    ++g_controlledRolls;
    for (std::size_t i = kHistoryCap - 1U; i > 0U; --i) {
        g_history[i] = g_history[i - 1U];
    }
    if (g_historyCount < kHistoryCap) ++g_historyCount;

    FatalisHistoryEntry& h = g_history[0];
    h.valid        = true;
    h.condId       = m.condId;
    h.nodeArrIdx   = lc.nodeArrIdx;
    h.chosenSegIdx = lc.cands[m.chosenCand].segIdx;
    h.targetNodeId = lc.cands[m.chosenCand].targetNodeId;
    h.chosenWeight = lc.cands[m.chosenCand].weight;
    h.totalWeight  = lc.totalWeight;
    h.injectedU32  = solvedU32;
    CopyTrunc(h.bandTag, sizeof(h.bandTag), m.bandTag);
    CopyTrunc(h.moveTag, sizeof(h.moveTag), m.moveTag);
}

// ============================================================================
//  Telemetry Formatter & `$rng fatalis` Subcommands
// ============================================================================

bool __fastcall FormatFatalisSlotStatus(const MhwRngSlotSnapshot* snap,
                                        char* outBuf,
                                        std::size_t bufCap,
                                        void*) {
    if (!snap || !outBuf || bufCap == 0U) return false;
    switch (snap->condId) {
        case 0U: {
            bool en = false;
            std::uint64_t cnt = 0ULL;
            {
                std::lock_guard<std::mutex> lk(g_stateMx);
                en  = g_enabled;
                cnt = g_controlledRolls;
            }
            if (!en) return false;
            std::snprintf(outBuf, bufCap, u8"黑龙P1控招生效中(已命中%llu次)",
                          static_cast<unsigned long long>(cnt));
            return true;
        }
        case kCondCloseUnderbreath:
            std::snprintf(outBuf, bufCap, u8"黑龙P1 <900 -> 孕吐(55:297)");
            return true;
        case kCondSuppressCone:
            std::snprintf(outBuf, bufCap, u8"黑龙P1 孕吐 -> 不派生扇火");
            return true;
        case kCondMidFrontCarpet:
            std::snprintf(outBuf, bufCap, u8"黑龙P1 900~2000正面 -> 地毯火(55:184)");
            return true;
        case kCondMidSideCarpet:
            std::snprintf(outBuf, bufCap, u8"黑龙P1 900~2000侧面 -> 侧地毯火(55:188)");
            return true;
        case kCondMidBackTail:
            std::snprintf(outBuf, bufCap, u8"黑龙P1 900~2000背面 -> 2足甩尾(55:175)");
            return true;
        case kCondLongCarpet:
            std::snprintf(outBuf, bufCap, u8"黑龙P1 2000~4500 -> 非扇地毯火");
            return true;
        case kCondFarFireball:
            std::snprintf(outBuf, bufCap, u8"黑龙P1 >4500 -> 火球");
            return true;
        case kCondP1BipedalGuard:
            std::snprintf(outBuf, bufCap, u8"黑龙P1 二足稳态守卫(禁额外车/保持二足)");
            return true;
        default:
            return false;
    }
}

void PrintFatalisDiag() {
    DiagSnapshot ds{};
    std::uint64_t rolls = 0ULL;
    {
        std::lock_guard<std::mutex> lk(g_stateMx);
        ds    = g_diag;
        rolls = g_controlledRolls;
    }
    g_host->ChatPrintf(u8"[fatalis] 诊断: 回调=%llu 命中=%llu 非黑龙=%llu 未知caller=%llu(0x%X) 指针/表异常=%llu",
                       static_cast<unsigned long long>(ds.enabledCalls),
                       static_cast<unsigned long long>(rolls),
                       static_cast<unsigned long long>(ds.nonFatalisCount),
                       static_cast<unsigned long long>(ds.badCallerCount),
                       ds.lastBadCallerRva,
                       static_cast<unsigned long long>(ds.badVftOrPtrCount));
    if (ds.fatalisUnmatched > 0ULL) {
        g_host->ChatPrintf(u8"[fatalis] 黑龙未命中=%llu次, 最近(d=%u file=%d node=%d seg0=%d caller=%d tgt0=%d:%d)",
                           static_cast<unsigned long long>(ds.fatalisUnmatched),
                           ds.lastUnmatchDepth, ds.lastUnmatchFile,
                           ds.lastUnmatchNode, ds.lastUnmatchSeg0, ds.lastUnmatchCaller,
                           ds.lastUnmatchTFile, ds.lastUnmatchTNode);
    }
}

void PrintFatalisStatus() {
    bool en = false, guards = true, farMulti = false;
    std::uint64_t rolls = 0ULL;
    std::uint64_t calls = 0ULL;
    FatalisHistoryEntry latest{};
    {
        std::lock_guard<std::mutex> lk(g_stateMx);
        en       = g_enabled;
        guards   = g_guardsEnabled;
        farMulti = g_farMultiFireball;
        rolls    = g_controlledRolls;
        calls    = g_diag.enabledCalls;
        if (g_historyCount > 0U) latest = g_history[0];
    }

    g_host->ChatPrintf(u8"[fatalis] 状态:%s | >4500火球:%s | P1二足守卫:%s | 累计控制:%llu次",
                       en ? u8"已开启(仅P1二足纯RNG)" : u8"已关闭(原版)",
                       farMulti ? u8"三连火球(55:220)" : u8"单发火球(55:179)",
                       guards ? u8"开(14/15/18/46/129~133)" : u8"关(仅75~79+55[87])",
                       static_cast<unsigned long long>(rolls));
    if (en && rolls == 0ULL && calls > 0ULL) {
        PrintFatalisDiag();
    }
    if (latest.valid) {
        g_host->ChatPrintf(u8"[fatalis] 最近一次: [%s] -> %s (node=%d seg=%d w=%.0f/%.0f u32=0x%08X)",
                           latest.bandTag, latest.moveTag,
                           latest.nodeArrIdx, latest.chosenSegIdx,
                           latest.chosenWeight, latest.totalWeight, latest.injectedU32);
    } else {
        g_host->ChatPrint(u8"[fatalis] 用法: `$rng fatalis on|off|status|history|diag|far single|multi|guards on|off`");
    }
}

void PrintFatalisHistory() {
    FatalisHistoryEntry snap[kHistoryCap]{};
    std::size_t count = 0U;
    {
        std::lock_guard<std::mutex> lk(g_stateMx);
        count = g_historyCount;
        for (std::size_t i = 0U; i < count; ++i) snap[i] = g_history[i];
    }
    if (count == 0U) {
        g_host->ChatPrint(u8"[fatalis] 尚无黑龙P1二足控招记录；开启 `$rng fatalis on` 并进入黑龙任务后自动记录");
        return;
    }
    for (std::size_t i = 0U; i < count; ++i) {
        const auto& h = snap[i];
        g_host->ChatPrintf(u8"[fatalis] #%zu [%s] -> %s | node=%d seg=%d (%.0f/%.0f) u32=0x%08X",
                           i + 1U, h.bandTag, h.moveTag,
                           h.nodeArrIdx, h.chosenSegIdx,
                           h.chosenWeight, h.totalWeight, h.injectedU32);
    }
}

void __fastcall Cmd_Fatalis(int argc, const char* const* argv, const char*, void*) {
    if (!g_host) return;
    if (argc == 0) {
        PrintFatalisStatus();
        return;
    }

    const std::string a0 = LowerAscii(argv[0]);
    if (a0 == "status" || a0 == "info") {
        PrintFatalisStatus();
        return;
    }
    if (a0 == "history" || a0 == "recent" || a0 == "log") {
        PrintFatalisHistory();
        return;
    }
    if (a0 == "diag" || a0 == "debug") {
        PrintFatalisDiag();
        return;
    }
    if (a0 == "help" || a0 == "?") {
        g_host->ChatPrint(u8"[fatalis] 开关: `$rng fatalis on` | `off` | `status` | `history` | `diag`");
        g_host->ChatPrint(u8"[fatalis] >4500火球: `$rng fatalis far single`(55:179) | `far multi`(55:220)");
        g_host->ChatPrint(u8"[fatalis] P1守卫: `$rng fatalis guards on|off` (禁额外车/保持二足/起身回弹/遮挡P1)");
        return;
    }
    if (a0 == "on" || a0 == "1" || a0 == "start" || a0 == "enable") {
        if (!g_host->IsTakeoverEnabled()) {
            g_host->SetTakeoverEnabled(true);
        }
        {
            std::lock_guard<std::mutex> lk(g_stateMx);
            g_enabled = true;
        }
        g_host->ChatPrint(u8"[fatalis] 已开启黑龙P1二足纯RNG控招: <900孕吐不派生扇火 | 900~2000正地毯/侧地毯/背甩尾 | 2000~4500地毯 | >4500火球");
        return;
    }
    if (a0 == "off" || a0 == "0" || a0 == "stop" || a0 == "disable" || a0 == "clear" || a0 == "reset") {
        {
            std::lock_guard<std::mutex> lk(g_stateMx);
            g_enabled = false;
        }
        g_host->ClearSlotAllConditions(kRvaThkUpdateRandom);
        g_host->ChatPrint(u8"[fatalis] 已关闭黑龙P1控招 -> 恢复原版THK随机");
        return;
    }
    if (a0 == "far") {
        if (argc < 2) {
            g_host->ChatPrint(u8"[fatalis] 用法: `$rng fatalis far single` (55:179) 或 `$rng fatalis far multi` (55:220)");
            return;
        }
        const std::string a1 = LowerAscii(argv[1]);
        if (a1 == "single" || a1 == "179" || a1 == "one") {
            {
                std::lock_guard<std::mutex> lk(g_stateMx);
                g_farMultiFireball = false;
            }
            g_host->ChatPrint(u8"[fatalis] >4500 超远距离已设为: 单发火球(55:179)");
            return;
        }
        if (a1 == "multi" || a1 == "triple" || a1 == "3" || a1 == "220") {
            {
                std::lock_guard<std::mutex> lk(g_stateMx);
                g_farMultiFireball = true;
            }
            g_host->ChatPrint(u8"[fatalis] >4500 超远距离已设为: 三连火球(55:220)");
            return;
        }
        g_host->ChatPrintf(u8"[fatalis] 未知 far 模式 `%s`，可选 `single` 或 `multi`", argv[1]);
        return;
    }
    if (a0 == "guards" || a0 == "guard") {
        if (argc < 2) {
            g_host->ChatPrint(u8"[fatalis] 用法: `$rng fatalis guards on|off`");
            return;
        }
        const std::string a1 = LowerAscii(argv[1]);
        if (a1 == "on" || a1 == "1" || a1 == "true") {
            {
                std::lock_guard<std::mutex> lk(g_stateMx);
                g_guardsEnabled = true;
            }
            g_host->ChatPrint(u8"[fatalis] P1二足守卫已开启 (14/15/18/46/129~133)");
            return;
        }
        if (a1 == "off" || a1 == "0" || a1 == "false") {
            {
                std::lock_guard<std::mutex> lk(g_stateMx);
                g_guardsEnabled = false;
            }
            g_host->ChatPrint(u8"[fatalis] P1二足守卫已关闭 (仅控制75~79与55[87])");
            return;
        }
        g_host->ChatPrintf(u8"[fatalis] 未知 guards 参数 `%s`，可选 `on` 或 `off`", argv[1]);
        return;
    }

    g_host->ChatPrintf(u8"[fatalis] 未知子命令 `%s`，输 `$rng fatalis help` 查看用法", argv[0]);
}

const MhwRngPluginDescriptor kPluginDesc = {
    kMhwRngApiVersion,
    "fatalis",
    u8"黑龙一阶段二足纯RNG出招控制插件",
    "1.0.2",
    u8"仅通过mhw_rng抽签回调控制黑龙(MonsterID=101)一阶段二足全距离段出招"
};

} // namespace

MHW_RNG_PLUGIN_EXPORT const MhwRngPluginDescriptor* MhwRngPlugin_GetInfo() {
    return &kPluginDesc;
}

MHW_RNG_PLUGIN_EXPORT bool MhwRngPlugin_Init(const MhwRngHostApi* host, MhwRngPluginId selfId) {
    if (!host || host->apiVersion != kMhwRngApiVersion) return false;
    g_host   = host;
    g_selfId = selfId;

    host->RegisterSiteMeta(selfId,
                           kRvaThkUpdateRandom,
                           "ai.thk_rand",
                           u8"怪物THK行为树随机分支抽取 (由 fatalis 插件监控黑龙P1二足)",
                           &FormatFatalisSlotStatus,
                           nullptr);

    host->RegisterSlotHook(selfId, kRvaThkUpdateRandom, &OnThkRandomDraw, nullptr);

    host->RegisterSubcommand(selfId,
                             "fatalis",
                             "$rng fatalis on|off|status|history|diag|far single|multi|guards on|off",
                             u8"黑龙(em013)一阶段二足纯RNG实时控招",
                             &Cmd_Fatalis,
                             nullptr);
    return true;
}

MHW_RNG_PLUGIN_EXPORT void MhwRngPlugin_Shutdown() {
    std::lock_guard<std::mutex> lk(g_stateMx);
    g_enabled          = false;
    g_guardsEnabled    = true;
    g_farMultiFireball = false;
    g_controlledRolls  = 0ULL;
    g_historyCount     = 0U;
    g_diag             = DiagSnapshot{};
    g_host             = nullptr;
    g_selfId           = 0U;
}
