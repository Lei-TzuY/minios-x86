# CLAUDE.md — miniOS 工作指引

32-bit x86 教學型作業系統。核心 C/ASM 約 9,400 行（不含產生檔）、`user/` ring-3
程式約 4,300 行、`tests/` 原生單元測試約 12,700 行。

**先讀 `PROJECT_STATE.md`**：那裡有架構、已完成工作、設計決策、測試基準、已知問題
與下一輪候選。本檔只講「怎麼動手」。

---

## 建置與測試（WSL）

工具鏈在 WSL distro `Ubuntu-26.04`（gcc 15.2 `-m32`、GNU as、qemu-system-i386、
python3）。Windows 端沒有編譯器。

| 指令 | 用途 | 時間 |
|---|---|---|
| `make all -j4` | 建置核心（**不是** `make`，見下） | ~30s |
| `make unit` | 25 套原生單元測試 | <1s |
| `make test` | 完整回歸：`unit` + 5 個 QEMU 目標 | ~8-10 分鐘 |
| `make bench` | 效能量測（資訊性，不在 `make test` 內） | ~5s |

- 裸 `make` 只會建第一個目標（一個單元測試執行檔），**不是**核心。用 `make all`。
- `make test` 的 QEMU 目標：`test-ata-absent`、`test-boot`、`test-iso`、
  `test-stress`、`test-shell`。
- 額外品質 gate：`make sanitize`（hosted ASan + UBSan）、`make static-analysis`
  （Python bytecode + shell syntax + cppcheck）、`make test-stress-mutants`（兩個具名
  capacity、PMM leak、兩個 fault status、abnormal fd/pipe teardown、CPL classification，
  共 7 個 mutants）。

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

### 改文件也要用腳本檔，不要用 `python3 -c "..."`

本專案的 `.md` 文件裡到處是反引號（`` `make test` ``）。把這種文字放進雙引號的
`python3 -c "..."`，**bash 會先把反引號當成命令替換執行掉**。Session 27 就這樣
意外跑掉一次 `make clean`（連帶 `make all`、`make test`），花了十分鐘才確認樹沒事
——原始碼沒受損純粹是運氣好，因為那支 Python 比對不到樣式、一個字都沒寫出去。

**改文件一律寫成 `.py` 腳本檔再執行**，和建置/測試用 `.sh` 是同一個理由：
不要讓內容穿過 bash 的解析。腳本裡的 `patch()` 也要在樣式出現次數不等於 1 時
`sys.exit`，這樣重複執行或樣式漂移會乾淨失敗而不是寫壞檔案。

### 誠實準則（本專案的核心價值）

- **不得捏造測試結果**。失敗就報失敗，附上輸出。
- 存活的突變要**追查原因**：是測試缺口（補測試），還是等價突變（誠實記錄，
  不硬寫假測試去「殺」它）。
- 無法確認的需求採**最保守、相容現有設計**的方案，並記錄假設。
- 區分「量過效能」與「驗證過正確性」——F22（P0）就是因為混淆這兩者而潛伏了 23 輪。
- **crash 與 timeout 只說「有事發生」，不說「斷言起作用了」**：CAP21 有三個突變
  原本只被 `rc=139`（SIGSEGV）或無限迴圈的逾時抓到。裝上 SIGSEGV handler ＋
  三值的 `RUN_MAY_EXIT`（正常返回／`task_exit`／碰到映射外記憶體），再給會 park
  的迴圈一個次數上限，才把它們變成具名斷言。順帶一提，那個「碰到映射外記憶體」
  在真核心裡 CS 仍是 0x08，page fault handler 會判定**核心錯誤而停機**——比
  host 上的 segfault 嚴重得多，值得測試講清楚。
- **均勻的測試資料可能完全碰不到被測的邊界**：CAP16 的截斷測試讓每行都剛好 40 bytes，
  於是 `pos` 只取 40 的倍數，兩個真實的溢位突變（界限放寬一格、少預留七格）**沒有
  改變任何一條斷言**。要讓累積量精準落在守衛的邊界值上，並從兩側夾住。
- **突變被「殺掉」時要查清楚是誰殺的**。Session 29 有三個實例：C1 看似證明了新契約
  有效，實際上是 test_ramfs 既有的看門狗先掛住、契約根本沒跑到；V1/V10 看似只是
  segfault，加上 flush 後才看到崩潰前已有具名斷言失敗。**stdout 在管線裡是區塊
  緩衝的，崩潰會吃掉所有已印出的失敗訊息**——`tests/test.h` 現在逐筆 flush。
- **測試用的 stub 要貼近真實實作的行為特性，不然性質會變得不可測**：heap 的 stub
  原本用 `posix_memalign`，兩次成長的相對位址由 host 決定；真實 `pmm_alloc_blocks`
  是**連續配發 frame** 的。改成單一 arena 依序配發之後，「跨成長邊界的合併」與
  「自由串列位址排序」才變得可觀測（突變 H11 因此才被殺掉）。
- **自己寫的不變式檢查也可能有缺陷**：HEAP1 的第一版「成長只能來自相鄰」只驗了第一個
  鄰居，而連鎖合併時**只有吸收方的 header 會更新**，被吸收的區塊保留舊 size——兩個
  真實突變因此溜過去。改成「成長必須正好等於後續一串實體連續區塊的總和」才正確。
- **故障注入太黏，會讓被測的檢查根本沒被執行到**：CAP18 原本讓 ERR 覆蓋**每一次**
  狀態讀取，於是驅動在命令發出前就先被擋下——**測試在正確程式上也是因為錯的理由通過
  的**，兩個「刪掉錯誤檢查」的突變因此存活。故障要由**對應的事件**觸發（命令完成），
  不是變成永久的環境條件。
- **模擬未定義行為時要選保守的那個解釋**：ATA 規格說「BSY/DRQ 期間寫 command register」
  未定義。假裝置原本讓它中止舊傳輸並接受新命令，於是「只等 BSY 不排空 DRQ」的半套修法
  也能通過。改成「忽略」之後才抓得到。**靠硬體寬容才能運作的驅動是靠運氣運作的**，
  測試模型不該替受測程式挑一個剛好方便的解釋。
- **測參照計數要看所有權，不能看回傳值**：CAP19 的 stub 保有真實計數並把「沒有對應
  open 的 close」記為 underflow。只斷言 `sys_close()` 回傳 0 的測試，對「釋放了但沒
  清空 entry」「fork 複製表卻沒 bump」「兩個 pipe 端搞反」等**每一個**突變都會通過。
- **對稱的狀態會讓「搞反」完全不可觀察**：fork 把 pipe 讀/寫端 bump 反了，在兩端都
  開著時兩邊都是 1→2。先關掉一端製造不對稱，突變才現形。同理，只開兩三個描述子的
  測試蓋不住「迴圈少跑一格」——要把表填滿。
- **數量為一時，兩個不同的條件會退化成同一件事**：CAP20 的「每個 thread 退出都完成
  exit」突變，在只有**一個** thread 時與正確程式完全相同——因為「計數歸零」與
  「有 thread 離開」是同一個事件。要**兩個**才分得開，而那正是 F1 的形狀。
- **交換順序不會改變任何計數**：teardown 的兩步對調，所有 counter 都一樣。要記錄
  每一步的**發生序號**才看得見。
- **「會永遠掛住」要變成具名斷言，不能靠 timeout**：CAP20 的 harness 在連續 64 次
  「park 了卻沒有任何喚醒瞄準自己」之後停下並說明原因。timeout 只說「某處出事」，
  不說「斷言起作用了」——而那正是突變測試要分辨的界線。

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

## 新增單元測試套件的完整接線

三個地方，漏第三個不會壞掉、但會把編出來的執行檔提交進版控：

1. `tests/test_<name>.c`
2. `Makefile` — `UNIT_BINS` 加一項 + 一條建置規則（附註解說明為什麼是這樣連結：
   要連哪些 `.c`、哪些東西在測試裡 stub、有沒有用 `--gc-sections`）
3. **`.gitignore` — 逐一列出每個測試執行檔，新的那個要自己加**

要讓驅動吃得到硬體輸入，**不必改核心標頭**：先 `#define IO_H` 讓 io.h 整個不生效，
再自備 `inb`/`outb`（見 `tests/test_kb.c`）。HOSTED_TEST 下的 `inb` 永遠回 0，
對靠埠讀取輸入的驅動等於不能測；用這個手法就不會動到核心 codegen。
noreturn 的 `task_exit` 用 `setjmp`/`longjmp` 接住，才測得到「被 kill 的 task
從等待迴圈離開」這條路徑。

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
