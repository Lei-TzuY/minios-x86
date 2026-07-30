# CLAUDE.md — miniOS 工作指引

32-bit x86 教學型作業系統。核心 C/ASM 約 9,100 行（不含產生檔）、`user/` ring-3
程式約 3,500 行、`tests/` 原生單元測試約 4,200 行。

**先讀 `PROJECT_STATE.md`**：那裡有架構、已完成工作、設計決策、測試基準、已知問題
與下一輪候選。本檔只講「怎麼動手」。

---

## 建置與測試（WSL）

工具鏈在 WSL distro `Ubuntu-26.04`（gcc 15.2 `-m32`、GNU as、qemu-system-i386、
python3）。Windows 端沒有編譯器。

| 指令 | 用途 | 時間 |
|---|---|---|
| `make all -j4` | 建置核心（**不是** `make`，見下） | ~30s |
| `make unit` | 16 套原生單元測試 | <1s |
| `make test` | 完整回歸：`unit` + 4 個 QEMU 目標 | ~8-10 分鐘 |
| `make bench` | 效能量測（資訊性，不在 `make test` 內） | ~5s |

- 裸 `make` 只會建第一個目標（一個單元測試執行檔），**不是**核心。用 `make all`。
- `make test` 的 QEMU 目標：`test-ata-absent`、`test-boot`、`test-iso`、`test-shell`。

### ⚠️ 離開碼陷阱（會造成假綠燈）

```
wsl.exe -d Ubuntu-26.04 -- bash -lc "cd ... && make test; echo EXIT=\$?"
```
**恆回報 0**，即使 make 實際失敗。離開碼在 Git Bash → wsl.exe → bash -lc 的多層
跳脫間遺失。Session 1 曾因此把一整輪「測試通過」的結論建立在假象上。

**正確做法**：把邏輯寫進 `.sh`，讓 `$?` 在單一 WSL bash 程序內被擷取：

```bash
MSYS_NO_PATHCONV=1 wsl.exe -d Ubuntu-26.04 -- bash /mnt/c/<path>/verify.sh all
```
腳本內 `make ...; rc=$?; echo "EXIT=$rc"`。`MSYS_NO_PATHCONV=1` 防止 Git Bash
竄改 `/mnt/c/...` 路徑。

長時間執行請用背景任務，並**保留完整輸出**（別用 `| tail -N`）——失敗時需要
完整 log 才能診斷。

---

## 修改程式碼的標準流程

1. **先審查、再動手**：本專案多數 P0/P1 是「讀懂機制後發現」的，不是靠跑測試撞到。
2. **修完要有測試**：單元測試（`tests/`）＋ 必要時端對端（`user/` 新程式）。
3. **用突變測試證明新測試有牙齒**——這是本專案的標準做法，不是選配。
4. **每階段跑 `make test` 並確認真實離開碼 0**。
5. 更新 `findings.md`（問題與修法）、`progress.md`（本輪流水帳）、`task_plan.md`（階段）。

### 突變測試的操作紀律

突變會**改動真實原始檔**，兩個實際踩過的坑：

- **中斷的執行會把突變留在樹裡**。曾有一個 `/* MUTANT M1 */` 的 stub 留在工作樹中
  ——正好是該輪剛修好的那個 bug。還原必須放在 `trap ... EXIT` 裡、每輪執行、
  並驗證位元組相同，**不可延到最後**。
- **可能有另一個 session 在改同一棵樹**。開始時釘住基準，每輪比對，
  **不同就中止而非覆寫**，並把該輪結果視為無效。

工具面：**這棵樹在此主機是 CRLF**。用 `\n` 寫的 pattern 對不上原始位元組。
Python 字面替換 + 「讀入正規化成 LF → 替換 → 依原行尾寫回」最可靠；
sed/perl 經 bash 傳多行 pattern 太脆弱。

### 誠實準則（本專案的核心價值）

- **不得捏造測試結果**。失敗就報失敗，附上輸出。
- 存活的突變要**追查原因**：是測試缺口（補測試），還是等價突變（誠實記錄，
  不硬寫假測試去「殺」它）。
- 無法確認的需求採**最保守、相容現有設計**的方案，並記錄假設。
- 區分「量過效能」與「驗證過正確性」——F22（P0）就是因為混淆這兩者而潛伏了 23 輪。

---

## 新增 user 程式的完整接線

漏一處就編不起來或測試飄移。以 `bigseek` 為例，需要改 **6 個地方**：

1. `user/<name>.c`
2. `user/Makefile` — 加入 `.elf` 清單
3. `Makefile` — `OBJS` 加 `<name>_embed.o`
4. `Makefile` — `user/<name>.elf:` 規則 + `<name>_embed.c/.o` 規則
5. `Makefile` — `clean` 目標加 `<name>_embed.c`
6. `kernel.c` — `extern` 宣告 + `ramfs_create_static_file(...)` + 載入訊息

再加上 `test-shell` 的送鍵與斷言、**`RAMFS nodes=N` 加一**、README 程式數加一。

### test-shell 的兩個時間/計數陷阱

- **`RAMFS nodes=N` 是精確斷言**。新增內嵌程式會改變 N。**測試程式忘記刪掉的
  臨時檔也會**——最常見原因是 `sys_create()` 本身就回傳一個**已開啟**的 fd，
  再呼叫 `sys_open()` 會拿到第二個參照，結尾的 `unlink` 就被（正確地）拒絕，
  節點留下。測試程式應**斷言清理成功**而非假設成功。
- **QEMU timeout 對時間預算敏感**：新增 `send_keys`/`sleep` 會把送鍵總時間推過
  `timeout Ns`，導致執行被腰斬、後面全部斷言失敗。加指令時一併調高逾時。

---

## 專案慣例

- flat layout：核心原始碼全在根目錄；`user/` 是 ring-3；`tests/` 是原生單元測試。
- `Makefile` 是唯一建置系統（無 CMake/Meson）。`gen_*.py` 產生內嵌資源。
- 註解說明**為什麼**（尤其是不明顯的取捨與曾經的 bug），不覆述程式碼在做什麼。
- 程式碼風格：4 空格縮排、K&R 大括號、`snake_case`。沒有 `goto`。
- 單元測試可 `#include "../<module>.c"` 取得 static 函式；高耦合模組用
  `-ffunction-sections -fdata-sections -Wl,--gc-sections` 讓連結器丟掉未觸及的
  函式，把 stub 面壓到最小（見 `test_process_env`、`test_syscall_valid`、`test_elf`）。
- 特權指令用 `HOSTED_TEST` 巨集守護，集中在 **`irq.h`**（`cli`/`sti`）與
  **`io.h`**（port I/O）兩處；`pipe.c`/`sem.c`/`timer.c`/`task.c` 等透過 include
  它們間接受惠，所以能在 host 上原生測試。
  **改動這類守護後要驗證核心 codegen 不變**：重建並用 `cmp` 逐一比對受影響的
  `.o`（REFACTOR1 與 CAP6 都這樣做過，各 7 個 `.o` 位元組完全相同）。
