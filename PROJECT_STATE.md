# PROJECT_STATE — miniOS 專案狀態（截至 Session 39 / 2026-09-01）

新 session 接手請依序讀：本檔 → `CLAUDE.md`（操作方式）→ 需要細節時查
`findings.md`（每個問題的完整分析）與 `progress.md`（每輪流水帳）。

---

## 1. 這是什麼

32-bit x86 教學型作業系統，從零以 C 與組合語言寫成，Multiboot/GRUB 開機，
在 ring 3 執行使用者程式。

| 項目 | 數量 |
|---|---|
| 核心 C/ASM（不含 `*_embed.c` 產生檔） | ~9,400 行 |
| `user/` ring-3 程式與 syscall wrapper | ~4,300 行 |
| `tests/` 原生單元測試 | ~12,700 行 |
| 系統呼叫 | 52 |
| 使用者程式 | 53 |
| 單元測試套件 | 25 |

### 子系統

| 層 | 檔案 | 說明 |
|---|---|---|
| 開機 | `boot.s`, `multiboot.h`, `linker.ld`, `grub.cfg` | Multiboot 進入點 |
| 記憶體 | `pmm.c`, `paging.c`, `paging_s.s`, `heap.c` | 位元圖 frame 配置、分頁 + COW、核心堆積 |
| 中斷 | `idt.c`, `isr.c`, `interrupt.s`, `irq.h`, `io.h` | IDT、ISR、IRQ save/restore |
| 排程 | `task.c`, `switch_s.s`, `timer.c` | ready 環 + blocked 串列、context switch、PIT |
| 行程 | `process.c`, `syscall.c`, `usermode_s.s`, `elf_loader.c` | fork/exec/signal/thread、int 0x80、ELF 載入 |
| IPC | `pipe.c`, `sem.c` | 環狀緩衝 pipe、計數號誌 |
| 檔案系統 | `fs.c`(VFS), `ramfs.c`, `diskfs.c`, `fat16.c`, `procfs.c` | 三個後端 + 合成 `/proc` |
| 驅動 | `ata.c`, `kb.c`, `rtc.c`, `vga.c` | PIO ATA、鍵盤、CMOS RTC、文字模式 |
| 其他 | `gdt.c`, `gdt_s.s`, `utils.c`, `kernel.c` | GDT/TSS、字串與記憶體、開機流程 + 核心 shell |

---

## 2. 不可破壞的行為（改動前務必確認）

這些是既有測試守護的性質，違反了就是回歸。

### 併發模型

- **`int $0x80` 的 IDT 閘門是 `0xEE`（interrupt gate）**，CPU 進入時自動清 IF，
  因此**系統呼叫全程關中斷**。這是本專案「全域 cli」併發模型的基礎——多數地方
  「沒上鎖」是設計選擇而非 bug。
- 推論：**核心裡任何無界迴圈都會凍結整台機器**（F22 就是這樣的 P0）。
- 阻塞點一律寫成 `while (cond) task_block_killable(ch);`——偽喚醒必須無害。

### 參照與生命週期

- **檔案開啟中不得 unlink**：三個檔案系統都必須維持。F11（dup2/redirect）、
  F12（FAT16 開啟計數）、F20（ELF 載入器）三個修復都依賴它。破壞它 =
  核心 use-after-free（從已釋放記憶體讀出函式指標並呼叫）。
- **RAMFS 的 `impl` 一個欄位身兼兩用**：bit 0 是 `RAMFS_DYNAMIC` 旗標，其上是
  開啟參照計數。open/close 不得動到 bit 0。
- **參照必須成對**：取得後所有離開路徑都要釋放。`elf_load_image` 為此把主體拆成
  `elf_load_from_node()`，讓外層只剩單一出口。
- 位址空間在最後一個 task 結束前不得銷毀（F1/F7 的教訓，`thread_count` 延後機制）。

### 使用者輸入的驗證

- **每個 raw 使用者指標都要過 `user_buffer_valid` / `user_string_valid`**；
  界限比較要寫成不會溢位的形式（先確立下界，再用減法）。
- **ELF 的每個欄位都是攻擊者可控**（可寫任意位元組到檔案再執行）。
  `paging_map_user_page()` **不做任何範圍檢查**，載入器的檢查是唯一防線。
- program header 讀兩次（驗證、使用），**兩次都要驗**（F21 的 TOCTOU）。

### 排程

- blocked 串列是 **FIFO**（尾插頭取）。改回 LIFO 會讓 starvation 回歸，
  也會讓依賴身分喚醒的地方失去正確性（F10 的教訓）。
- 需要喚醒**特定** task 時用 `task_wake_task()`，不可用 `task_wake_one(channel)`
  ——不相關的 task 常共用 channel。
- 被標記 `kill_pending` 的 task **從此不再等待**（`task_block_killable` 前後都檢查）。

### 測試基準（`test-shell` 結尾的精確斷言）

```
User pages: accessible=0 spaces=0
Processes: running=0 zombies=0 peak=4
Tasks: blocked=0
Timers: sleeping=0
RAMFS nodes=60
ATA sectors: available=2048 reads=19 writes=76
DiskFS: mounted=1 generation=9 files=0
```
這些是**洩漏偵測器**：任何未釋放的行程/task/計時器/節點都會讓它們失衡。

---

## 3. 已完成工作

### 已修問題（F1–F26）

分佈：**6 個 P0**（F1、F2、F14、F22、F25、F26；其中四個可讓**整台機器停機／凍結**，
F25 則是**權限提升**——ring 3 自己取得 IOPL）、
4 個 P1、8 個 P2、6 個 P3、2 個 P4。

| ID | 級別 | 摘要 |
|---|---|---|
| F1 | P0 | 多執行緒 exit 的位址空間 UAF（`thread_count` 延後銷毀） |
| F2 | P0 | `signal_deliver` 未驗證使用者 ESP → 核心整台停機 |
| F3 | P3 | procfs 固定緩衝區可溢位 |
| F4 | P2 | FAT16 節點池別名導致資料錯置 |
| F5/F6 | P4 | `make clean` 遺漏、README 數字不一致 |
| F7 | P1 | execv 從多執行緒行程呼叫的 UAF |
| F8 | P2 | `vfs_resolve_path` 過長路徑靜默截斷→解析到祖先目錄 |
| F9 | P3 | FAT16 叢集耗盡時長度記錯 |
| F10 | P1 | 用 channel 喚醒「特定 task」→ 系統死鎖（由 FAIR1 暴露） |
| F11 | P1 | dup2/redirect 未取 VFS 參照 → 核心 UAF |
| F12 | P2 | FAT16 無開啟計數 → 跨檔案資料洩漏 |
| F13 | P2 | fork 未繼承 fd 0/1 |
| F14 | P0 | `SYS_SIGRETURN` 完全未驗證 → 核心整台停機 |
| F15/F16 | P3 | `sys_sbrk` 的 INT32_MIN UB、umalloc 的 int 溢位 |
| F17 | P2 | kill 多執行緒行程只殺得掉一個 task |
| F18 | P3 | `pmm_init_region` 重新保留 frame 0 時計數未補回 |
| F19 | P2 | 停在阻塞等待中的 task 完全殺不到（kill_pending 機制） |
| F20 | P1 | ELF 載入器全程不持有 VFS 參照 → 載入中被 unlink = UAF |
| F21 | P2 | program header 驗證後重讀，中間可被改寫（TOCTOU） |
| F22 | **P0** | RAMFS 容量倍增溢位守衛差一步 → **無窮迴圈凍結整台機器** |
| F23 | P3 | FAT16 寫入 0 位元組時仍把長度推到 seek 位置（F9 修法的殘留邊界） |
| F24 | P2 | ATA 逾時的命令會答覆**下一個**操作 → 讀到別的 sector／假的寫入成功 |
| F25 | **P0** | `sys_sigreturn` 讓 ring 3 自選 EFLAGS → **IOPL=3／NT／VM／關 IF** |
| F26 | **P0** | 未處理的 ring-3 #DE/#UD/#GP iret 回原指令 → **無窮例外凍結整機** |

### 功能與效能

- **FEAT1**：可執行檔改走 VFS 查找，能從任何已掛載檔案系統執行程式。
  （**注意**：這也是 F20/F21/CAP12 之所以必要的原因——威脅模型因此改變。）
- **FEAT2**：新增 `dup()` 系統呼叫，配置最低可用 fd；檔案與 pipe end 都取得獨立
  參照，並保留來源描述子的目前 offset。所有權語意由 hosted fd regression 守護。
- **FAIR1**：blocked 串列改 FIFO 喚醒。
- **PERF1**：memcpy/memset 4-byte 批次（對齊時 3–5x；**來源/目的相對未對齊時
  無改善**，已誠實記錄）。
- **PERF2**：ramfs_write 幾何成長（攤還 O(1) append）。
- **REFACTOR1**：`irq.h` 抽出重複 7 次的 save/restore，**實測 7 個 `.o` 位元組相同**。
- **HARD1**：`resolve_fs` 補上與 `resolve_parent_fs` 一致的「中途組件必須是目錄」
  檢查。**目前不可觸發**（所有後端的檔案節點 finddir 都是 NULL），是縱深防禦而非
  修掉的 bug——補它是因為原本同一個路徑在兩個進入點意義不同，而那正是 F8 的溫床。

### 測試能力（CAP1–CAP23）

- **CAP1** `test-iso`：補上從未驗證過的 GRUB/ISO 開機路徑。
- **CAP2** `tests/` 原生單元測試框架 + 突變測試方法論。
- **CAP3–CAP13**：fat16 / diskfs / pipe / sem / timer / task / rtc / process-env /
  syscall-valid / paging-cow / elf / ramfs 各套件。
- **CAP14** `test_fs`：VFS 核心（兩個嚴格解析器 + 六個 dispatch wrapper），用刻意
  寬鬆的 mock 後端，讓每一次拒絕都能歸因到 fs.c 自己。
- **CAP15** `test_kb`：鍵盤驅動（環狀緩衝、修飾鍵狀態機、Ctrl+C 派送）。用
  `#define IO_H` 換掉 io.h 自備 `inb`，不動任何核心標頭。**沒找到 bug**，產出是
  18 個突變證明過的測試。
- **CAP16** `test_procfs`：**F3 的守衛第一次被實際執行**，精確邊界（473/512、
  12 行）被釘住；兩個完全沒有界限檢查的產生器最壞情況被算出並守住。灌毒 +
  `-fsanitize=bounds` 雙重不變式。
- **CAP17** `test_vga`：文字主控台（捲動、環繞、退格、十進位格式化）。QEMU 那套讀
  的是 putchar **在游標運算之前**就送出的 port 0xE9 位元組流，所以這些錯誤對端對端
  完全不可見。突變 21/21 全由斷言抓到。
- **CONF1** `tests/fs_conformance.h`：三個檔案系統後端共用的一致性契約
  （不可能觸及的寫入必須：會回來、存 0 位元組、檔案完全不變、讀取安全回 EOF）。
  **突變 C3 只有契約抓到，既有 943 個 diskfs 檢查全漏**。
- **HEAP1**：heap.c 稽核無缺陷，但補上四個真實測試缺口（378 → 720 檢查）。
- **CAP18** `test_ata`：ATA PIO 驅動，用**有狀態的假 IDE 裝置**（BSY 依可設定的輪詢
  次數才清、DRQ 在 256 words 後自落、BSY/DRQ 期間的命令寫入一律忽略、讀取失敗時
  ERR 與 DRQ 同時拉起）。irq.h 也換成計數版本，驗證八條 return path 的 save/restore
  配對。找到 **F24**。
- **CAP19** `test_fdtable`：每行程描述子表的**所有權契約**。stub 保有真實參照計數，
  沒有對應 open 的 close 記為 underflow——**只看回傳值的測試對每個突變都會通過**。
  把「每個交給新行程的 slot 必須帶著空的 `open_files[]`」這條跨 `syscall.c`／
  `process.c`、散落七條釋放路徑的假設變成可執行契約。沒找到缺陷。
- **CAP20** `test_process`（369 checks）：行程生命週期狀態機（F1/F7/F17/F19 的發源地）。排程器被
  **模型化**：記錄誰 park、park 在哪個 channel、誰被喚醒，以及「park 了卻沒人叫醒」
  ——後者把「會永遠掛住」變成具名斷言。釘住 `waitpid` 對 SIGCHLD 喚醒的隱性相依。
  follow-up 補上真正的 spurious wake 與 same-channel decoy，並以僅 hosted 的 release
  observer 驗證 auto-reap 不會 double release；`task_exit` 的 retired kernel stack 則由
  `test_task`（82 checks）釘住「下一個 scheduler safe point 才回收」。沒找到缺陷。
- **CAP21** `test_signal`：訊號遞送生命週期。以 `mmap(MAP_FIXED_NOREPLACE)` 真的
  映射使用者堆疊，所以建框／還原走的是**真的指標運算**。找到 **F25**（P0，權限
  提升）。三個原本只被 segfault 或 timeout 抓到的突變，改成具名斷言。
- **CAP22** `test_vm_lifecycle`（36 checks）＋ `test_process`（397 checks）：mmap/sbrk
  的位址空間所有權。審計一度發現 bitmap 與 PTE teardown 間的假想競態，但把 hosted
  model 對齊 `int $0x80` interrupt gate 後證明在現行單核、全域 cli ABI 下不可達；沒有
  修改 production code。sbrk 邊界、mmap free/reuse、fork/exec/slot-reuse metadata 與
  gate-to-iret 排程邊界均已釘住；7/7 meaningful mutants killed。
- **CAP23** `user/stress.c` ＋ `test-stress`：在真實 ring 3／QEMU 中整合施壓 heap、
  真正耗盡並回收 sbrk arena、demand paging、完整 4 MiB mmap 區、PIT/IRQ preemption、
  scheduler/context switch、帶著 heap/mmap/fd/pipe 輪流遭 #PF/#DE/#UD/#GP 的異常退出、
  syscall pointer validation、RAMFS/DiskFS/FAT16、threads/processes、fd/pipe/process/node
  exhaustion 與反覆 fork/exec/create/destroy。monitor-driven harness 在同次開機跑兩輪，
  要求十個具名階段各通過兩次，並逐欄比較 PMM、heap、user spaces、process/task/timer、
  RAMFS 快照；另有 ASan/UBSan、cppcheck 與 7 個具名 QEMU capacity/leak/exception mutants
  的 CI gate。

目前 25 套件，`make unit` <1 秒：

```
utils 50032 / fs-path 36 / fs-vfs 277 / pmm 58 / heap 720 / fat16 37455 /
diskfs 979 / pipe 89 / sem 38 / timer 63 / task 82 / rtc 35 /
process-env 47 / syscall-valid 46 / paging-cow 31 / elf 146 / ramfs 4335 /
kb 1675 / procfs 183 / vga 4769 / ata 8995 / fdtable 489 / process 419 /
signal 103 / vm-lifecycle 36
```

---

## 4. 重要設計決策（不要「順手改掉」）

| 決策 | 理由 |
|---|---|
| 全域 cli 併發模型 | 單核教學系統的簡化選擇。「沒上鎖」多為設計而非 bug。 |
| 執行檔解析**不**相對 cwd | 否則 ush `cd fat` 後跑 `cat` 會壞（`cat` 在 `/cat`）。 |
| kill 不做 EINTR 上拋 | 終止不需回到使用者空間，`task_exit` 即可；EINTR 會動到 ABI。 |
| `timer_sleep` 不用 `task_block_killable` | 它持有 sleep slot，必須先歸還再離開，且要判斷 slot 還是不是自己的。 |
| 界限檢查寫成不會溢位的減法 | 先確立下界再減，避免加法繞回（F22 就是加法/倍增繞回）。 |
| 政策與機制分層 | 「殺哪個行程」留在 `process.c`，「醒來就走」在 `task.c`，避免反向相依。 |
| ATA 全程 cli | 序列化同一組 I/O 埠；代價是磁碟 I/O 期間漏 tick（見已知限制）。 |

---

## 5. 已知限制（刻意不修，附理由）

1. **`ata.c` 的 PIO 輪詢全程 cli**：`ata_read_sector`/`ata_write_sector` 從
   `save_irq_disable()` 到 `restore_irq()` 之間包含等待 BSY/DRQ 的忙等（最多
   `ATA_POLL_LIMIT=100000` 次）全程關中斷。代價是磁碟 I/O 期間漏 timer tick、
   體感卡頓。
   **Session 30 重新評估（見 findings.md 的 ASSESS1）：測試安全網已經足夠**
   （CAP18 的假裝置能編寫任意 BSY/DRQ/ERR/逾時序列，28 個突變證明有牙齒），
   **但阻礙不在驅動**：`ata_read_sector` 目前永不阻塞，而 `diskfs.c` **完全沒有
   序列化**（整個檔案沒有鎖、沒有 cli），它的狀態全靠「syscall 全程關中斷」這個從
   interrupt gate 繼承來的假設。改成阻塞式 = 先替儲存堆疊引入併發模型，與下面第 2
   項同一性質的架構決定。

2. **訊號遞送無法觸及阻塞中的 thread**（終止的部分已由 F19 修好）：
   `process_send_signal` 只喚醒 `process->task`，且訊號只在返回使用者模式時遞送
   （`signal_deliver` 檢查 `regs->cs != 0x1B`），所以停在等待中的 thread 收不到
   可捕捉的訊號。根治需要「可中斷睡眠 + EINTR 上拋」，牽涉所有阻塞點**以及使用者
   空間對「系統呼叫可能被中斷」的預期**，會動到 ABI。

3. **`timer_sleep` 被訊號（非 kill）提早喚醒**時直接回傳 0，且 slot 佔到原到期
   時間才被 `timer_callback` 清掉。提早返回符合 POSIX `sleep()` 語意，slot 滯留
   有界且會自癒（不會被解參照）。修它需改動 `test_timer` 對「睡著」的建模。

4. **`ramfs_write` 重用路徑的 memset 是冗餘防禦**（突變 R6 存活）：程式碼註解
   自承「already zero by the invariant, but make it explicit」。與成長路徑的
   memset 互為備援。誠實記錄，不硬寫測試去殺它。

5. **`phdr_in_user_range` 的檔案範圍檢查無法單獨觸發溢位**（突變 E4 存活）：
   短路求值中 `p_filesz <= p_memsz` 與 `p_memsz <= USER_STACK_BOTTOM - p_vaddr`
   排在它前面，路徑到不了。等價突變。

6. **procfs 的兩個等價突變**（P4、P12，Session 28）：`/proc/processes` 的守衛只看
   `pos`，而略過一個行程不改變 `pos`，所以 `break` 與 `continue` 產生的位元組完全
   相同；`proc_read` 的 `offset >= len` 改成 `> len` 之後，`offset == len` 會被
   下一行的 size 夾成 0、`memcpy` 0 bytes，回傳同樣是 0。兩者都是刻意的縱深防禦，
   誠實記錄不硬寫測試去殺。

7. **`/proc/self/name` 與 `/proc/self/status` 完全沒有界限檢查**：今天安全是因為
   欄位剛好夠窄（status 最壞 75 bytes vs 512 緩衝區），**不是任何程式碼在維護的
   性質**。已由 CAP16 的最壞情況測試守住——欄位一變寬，那個測試就會失敗。

8. **Git-for-Windows 的 CRLF 轉換**使 WSL 內 git 視整棵樹為已修改。

9. **`sys_seek` 允許的 offset 遠超任何後端能支撐的範圍**（最大 0x7FFFFFFF）。
   Session 29 調查後**刻意不改**，理由見 findings.md 的 SEEK1：三個後端目前
   全部正確、沒有一個有依據的全域常數（各後端上限相差四個數量級）、且收窄會讓
   `user/bigseek.c` 這個唯一的 F22 端對端證據失效。殘留風險由 CONF1 的可執行
   後端契約承接。

---

## 6. 下一輪候選工作

依「價值 / 風險」排序，附上為什麼值得做。

### A. 尚未單元測試的模組（延續 CAP 系列）

- ~~**`procfs.c`**~~：**Session 28 完成，見 CAP16**（沒找到 bug；F3 的守衛第一次
  被實際執行，精確邊界已釘住；兩個無守衛產生器的最壞情況已算出並守住）。
- ~~**`kb.c`**~~：**Session 27 完成，見 CAP15**（沒找到 bug；0xE0 延伸掃描碼
  「不處理但後果良性」的推導已轉成測試）。
- ~~**`vga.c`**~~：**Session 29 完成，見 CAP17**（沒找到 bug；順帶修正 test.h 讓
  失敗訊息在崩潰前被 flush 出來）。
- ~~**`fs.c` 的 `vfs_resolve_path`**~~：**Session 26 完成，見 CAP14**——並且做的是
  比原本設想更完整的範圍（兩個嚴格解析器 + dispatch wrapper + 正規化器與解析器的
  一致性），因為缺口其實在「消費正規化輸出的那一層」。
- ~~**`heap.c`**~~：**Session 29 完成，見 HEAP1**（稽核無缺陷；補四個真實測試缺口，
  378 → 720 檢查；stub 改為貼近真實 PMM 的連續配發）。

**A 類已全部完成**，`ata.c`（CAP18）與描述子表（CAP19）也已完成。剩下的候選：

- ~~**CAP20：行程生命週期狀態機**~~：**Session 32 完成**（`test_process` 369 checks；
  額外 lifecycle mutants 14/14 killed、1 equivalent；retired-stack mutants 2/2 killed；
  無缺陷）。ASSESS2 找到的隱性相依已被釘住。
- ~~**CAP21：訊號遞送的生命週期**~~：**Session 33 完成**（88 檢查、突變 22/23、
  找到 **F25**，P0 權限提升）。
- ~~**CAP22：mmap / sbrk 位址空間所有權**~~：**Session 34 完成**（`test_vm_lifecycle`
  36 checks、`test_process` 397 checks、7/7 meaningful mutants killed）。初步的
  `munmap` race 是 hosted model 沒有模型化 syscall interrupt gate；在現行 ABI 下不可達，
  故沒有硬加冗餘 lock。
- ~~**CAP23：跨子系統 QEMU 壓力與 teardown**~~：**Session 35 完成**。同一個 ring-3
  workload 組合記憶體、分頁、timer preemption、threads、processes、syscalls、三個可寫
  filesystem 與精確資源耗盡；同次開機跑兩輪並要求完整資源快照一致。pipe、semaphore、
  timer 與 process teardown 的跨模組缺口已由此直接覆蓋。
- **Session 36 加固**：heap exhaustion 不再以固定 iteration 冒充耗盡；必須實際收到
  `malloc()==NULL`、驗證所有 live chunk、逆序釋放後再成功配置 128 KiB。mutation matrix
  另注入 `munmap` 清 PTE 但漏還實體頁，必須由兩輪 PMM snapshot drift 擊殺。
- **Session 37 加固**：每輪另啟動 24 個故意 page fault 的子行程；每個都帶著 16 頁
  sbrk heap、32 頁 mmap、一個開啟檔案與一對 pipe fd，必須以 status `-1` 被 parent
  精確回收。兩個新增 mutants 分別證明 user-fault status 與異常 fd/pipe teardown gate。
- **Session 38 / F26**：同一 fault workload 輪流執行 #PF、整數除零 #DE、`ud2` #UD、
  ring-3 `cli` #GP。generic ISR 依 `CS.RPL` 分流：CPL3 只終止肇事 task，CPL0 才輸出
  `KERNEL EXCEPTION` 並停機；修正原本印字後 iret 回同一指令造成的整機無窮例外。
- **B 類**：IRQ-driven ATA（見 ASSESS1，阻礙是 diskfs 沒有併發模型）、
  可中斷睡眠 + EINTR（會動 ABI）。兩者都需要人類決策。

### B. 已知限制的攻堅

- **IRQ-driven ATA**（限制 1）：風險高、改動大，但現在有 16 套單元測試 +
  端對端做安全網，比前幾輪可行。可先建 ATA 的單元測試（RAM stub 已在
  `test_diskfs` 用過）再重構——這正是 Session 15 做 `irq.h` 重構的順序。
- **可中斷睡眠 + EINTR**（限制 2）：會動 ABI，需要先想清楚使用者空間契約。

### C. 方法論

- ~~**`sys_seek` 的上界**~~：**Session 29 已調查並決定不改，見 findings.md 的
  SEEK1**。三個實測理由：三個後端目前全部正確；**沒有一個有依據的常數**可放在
  syscall 邊界（各後端上限相差四個數量級，且 syscall 那層看不出 fd 屬於哪個後端）；
  收窄會讓 `user/bigseek.c` 失效——那是唯一證明 ring-3 整條路徑撐得住 F22 的產物。
  殘留風險（新後端要自己重擋）改由 **CONF1 的可執行契約**處理。
- **對其他「量過效能但沒測正確性」的地方做一輪盤點**——F22 潛伏 23 輪的根因。

---

## 7. 檔案地圖（文件）

| 檔案 | 內容 |
|---|---|
| `CLAUDE.md` | 怎麼建置/測試/修改，操作紀律與陷阱 |
| `PROJECT_STATE.md` | 本檔：狀態、決策、基準、已知問題、下一輪候選 |
| `task_plan.md` | 各階段（Phase 0–27）的目標與完成狀態 |
| `findings.md` | 每個 F/CAP/PERF 項目的完整分析與修法 |
| `progress.md` | 每輪 session 的流水帳（含犯過的錯與更正） |
| `README.md` | 對外說明（英/中） |
| `tests/BENCHMARKS.md` | PERF1/PERF2 的實測數據與誠實限制 |
