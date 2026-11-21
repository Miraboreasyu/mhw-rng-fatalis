# `rng_fatalis` 官方插件技术文档（黑龙一阶段二足纯 RNG 出招控制）

- **插件标识**：`fatalis`（编译产物 `rng_fatalis.dll`，版本 `1.0.0`）
- **源码位置**：[fatalis_plugin.cpp](fatalis_plugin.cpp)
- **宿主依赖**：`mhw_rng.dll`（Plugin SDK `kMhwRngApiVersion == 2`，头文件 [mhw_rng_api.h](include/mhw_rng_api.h)）
- **接管调用点**：`RVA 0x1339C8C`（`cThinkMgr::updateThk` @ `0x141339640` 内唯一的 `CALL MtRandom::nrand`）

本文档说明如何仅通过 `mhw_rng` 的同步抽签回调实时识别黑龙（`em013`，`MonsterID == 101`）的 THK 行为树执行上下文，并在一阶段二足状态下以纯 RNG 返回值精确锁定各距离/方位区间招式，全程不安装额外游戏 Hook、不修改 `.thk` 文件、不写入游戏 AI 内存。

---

## 一、设计约束与核心原理

1. **零额外 Hook / 零脚本改动**：插件仅向微内核注册一处 Pull 模式同步回调 `RegisterSlotHook(selfId, 0x1339C8CU, &OnThkRandomDraw, nullptr)`，所有干预均通过返回精确计算的 `uint32_t` 抽签结果完成。
2. **仅作用于黑龙一阶段二足**：
   - 先通过调用栈与 `cThinkMgr::vft` 验证上下文合法性，再校验 `*(uint32_t*)(monster + 0x12280) == 101`。
   - 仅当当前 THK 节点索引属于黑龙一阶段二足对应节点时返回求解值；其它怪物、黑龙二/三阶段节点及四足节点全部返回 `kMhwRngActionUseCondition(0)` 保持正常执行。

---

## 二、THK 虚拟机抽签栈帧与内存读取链

对《怪物猎人：世界 冰原》Steam 15.23 版本的静态与二进制分析确认，`cThinkMgr::updateThk`（`0x141339640`）在调用 `MtRandom::nrand(&MtRandom::sInstance)`（返回地址 `RVA 0x1339C8C`）之前，已经将当前待选 `RANDOM` 分支表填充完毕，因此回调函数可直接从栈帧与对象指针只读读取完整候选集：

1. **调用方栈帧解析（`slotPtr = ctx->returnAddrSlot`）**：
   - `updateThk` 序言压栈 3 个非易失寄存器并开辟 `0x80` 字节栈帧，因此调用方返回地址固定位于 `slotPtr[20]`。
   - 游戏内 `cThinkMgr::updateThk` 仅有两个调用方：
     - **`0x133B6F0`（`FUN_14133b640`，主 AI 帧更新）**：`cThinkEm* = slotPtr[19]`（保存的 `RDI`），`cThinkMgr* = thinkEm + 0x20`，`Monster* = slotPtr[21]`（保存的 `RBX`）。
     - **`0x133A876`（`FUN_14133a850`，预取周期）**：`cThinkMgr* = slotPtr[21]`，`Monster* = *(cThinkMgr + 0x508)`（兜底从 `thinkEm + 0x08` 读取）。
2. **`cThinkMgr` 实时字段与候选表布局**：
   - `mgr + 0x00`：`cThinkMgr::vft` 虚表指针（固定 `exeBase + 0x324F438`），用于指针合法性校验。
   - `mgr + 0x40`：最多 30 项 `ThkCandidate` 数组，每项 8 字节（`int32_t segIdx` + `float weight`，未用项以 `-1` 终止）。
   - `mgr + 0x4F8`：调用栈深度 `depth`（`0` 为 `em013_00.thk`，`1` 为 `em013_55.thk`）。
   - `frameBase = mgr + depth * 0x180`：
     - `frameBase + 0x350`：`nodeBase`（当前节点的 `THKSegment[]` 基址，步长 `0x80` 字节）。
     - `frameBase + 0x35C`：`thkFileIdx`（`0` 或 `55`）。
   - `mgr + 0x364`（当 `depth == 1` 时有效）：`callerNodeArrIdx`，即进入 `em013_55` 的外层 `em013_00` 节点数组下标（用于精确区分是否从一阶段调用 `55:297` 孕吐）。
3. **节点数组下标定位与原版浮点反算**：
   - 从 `nodeBase` 起按 `0x80` 步长向前扫描至 `seg[0] & 1` 的结束标记段，其 `+0x24` 偏移存储 `10000 + nodeArrIdx`。
   - 原版在 `0x141339C8C` 处按 `roll = (float)u * (100.0f / 2^32) * (totalWeight * 0.01f)` 命中首个满足 `acc <= roll && roll < acc + weight[k]` 的候选分枝。
   - 插件选取目标候选分枝 `k` 后，计算区间中点比例 `frac = (cumBefore + 0.5 * weight[k]) / totalWeight`，并映射至 `[0x00000001U, 0xFFFFFF00U]`，保证对非零权重分支的命中率为 100%。

---

## 三、一阶段二足控招映射表（基于原版 `em013_00.thk` / `em013_55.thk`）

| 触发条件（仅限 `MonsterID == 101` 一阶段） | THK 定位特征 | 锁定目标分枝 | 原版权重 | 实际效果 |
| :--- | :--- | :--- | :---: | :--- |
| `< 900` 近距离 | `depth=0, file=0, idx in {75, 76, 129, 130}` | `55:297`（`55[idx=87]`） | `10 / 100` | 只出孕吐 |
| 孕吐派生判定 | `depth=1, file=55, idx=87`，且 `caller in {75, 76, 129, 130}` | `ENDR`（`seg=10`） | `65 / 100` | 禁止 35% 派生扇形火（二/三阶段不拦截） |
| `900 ~ 2000` 正面（`315°~45°`） | `depth=0, file=0, idx in {77, 131}, firstSeg==1` | `55:184`（`55[idx=84]`） | `25~30 / 100` | 只出不派生扇火的正面地毯火 |
| `900 ~ 2000` 侧面（`45°~135°` / `225°~315°`） | `depth=0, file=0, idx in {77, 131}, firstSeg >= 25` | `55:188`（`55[idx=89]`） | `20~35 / 100` | 只出侧地毯火 |
| `900 ~ 2000` 背面（`135°~225°`） | `depth=0, file=0, idx in {77, 131}, firstSeg==13/12` | `55:175`（`55[idx=78]`） | `25~30 / 100` | 只出 2 足甩尾 |
| `2000 ~ 4500` 超远程 | `depth=0, file=0, idx in {78, 132}` | 正背 `55:184` / 侧面 `55:188` | `25~35 / 100` | 只出不派生扇火的地毯火 |
| `> 4500` 极远距离 | `depth=0, file=0, idx in {79, 133}` | 默认 `55:179`（可选 `55:220`） | `35~60 / 100` | 只出单发火球（可选三连火球） |
| 辅助守卫（`guards on`，默认开） | `idx=18`、`idx=14/15`、`idx=46 (firstSeg>=53)` | 对应 `ENDR` / `RANDOM` / 地毯火 | `10~90 / 100` | 跳过 P1 15% 额外龙车、保持二足态及遮挡补偿 |

---

## 四、聊天命令参考（`$rng fatalis`）

| 命令格式 | 说明 |
| :--- | :--- |
| `$rng fatalis` 或 `$rng fatalis status` | 查看当前开关、`>4500` 火球模式、P1 守卫开关及最近一次命中记录 |
| `$rng fatalis on` / `off` | 开启或关闭黑龙一阶段二足纯 RNG 控招（`on` 时若总接管未开启会自动开启） |
| `$rng fatalis history` | 显示最近最多 8 条受控抽签日志（距离方位段、节点下标、分枝权重与注入 `u32`） |
| `$rng fatalis far single` / `far multi` | 切换 `>4500` 极远距离目标：单发火球（`55:179`）或三连火球（`55:220`） |
| `$rng fatalis guards on` / `off` | 开关辅助守卫（倒地派生 `129..133`、遮挡 `46`、额外龙车 `18`、二/四足切换 `14/15`） |
| `$rng fatalis help` | 在游戏聊天框查看简要参数帮助 |

---

## 五、独立仓库目录与构建部署

本目录自带本地 SDK 头文件副本与独立构建脚本，可作为独立 Git 仓库维护：

- `fatalis_plugin.cpp`：插件主源码
- `include/mhw_rng_api.h`：`mhw_rng` Plugin SDK v2 头文件本地副本
- `sync_sdk.bat`：从主框架仓库更新 `include\mhw_rng_api.h`
- `build.bat`：编译产出 `build\rng_fatalis.dll`（支持 `--deploy` 直接复制至游戏插件目录）

常用构建命令：
```bat
:: 1. 同步 SDK 头文件（可选传入主框架仓库路径）
sync_sdk.bat D:\path\to\mhw-rng

:: 2. 独立编译至 build\rng_fatalis.dll
build.bat

:: 3. 编译并部署至游戏 nativePC\plugins\mhw_rng\rng_fatalis.dll
build.bat --deploy
```

部署后在游戏中输入 `$rng load fatalis`（首次加载）或 `$rng reload fatalis`（热更新），随后输入 `$rng fatalis on` 即可启用。
