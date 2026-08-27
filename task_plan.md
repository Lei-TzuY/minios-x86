# Task Plan — miniOS 全面審查與持續改善

## Goal
對 miniOS（32-bit x86 教學型作業系統，~33K LOC C/ASM，位於本專案根目錄）進行全面程式碼審查，
找出未完成項目、Bug、競態條件、記憶體安全問題、效能瓶頸、架構缺陷與技術債，依優先級修復、
重構、補測試、補文件，每階段修改後都要在 WSL Ubuntu-26.04 中 `make` 編譯並執行 `make test`
驗證不回歸。不得捏造測試結果；無法確認的需求採取最保守、相容現有設計的方案並記錄假設。

> **新 session 從這裡開始**：先讀 `PROJECT_STATE.md`（架構、不可破壞的行為、
> 測試基準、已知問題、下一輪候選）與 `CLAUDE.md`（建置/測試/突變測試的操作紀律）。
> 本檔是**階段流水帳**，記錄每一輪做了什麼；細節在 `findings.md`／`progress.md`。

## Environment
- Windows host，工具鏈全在 WSL distro `Ubuntu-26.04`：gcc 15.2.0 (-m32)、GNU as 2.46、
  qemu-system-i386 10.2.1、python3 3.14.4。Windows 端沒有編譯器。
- **建置一律用腳本擷取離開碼**（`wsl.exe -- bash -lc "...\$?"` 恆回報 0，會造成
  假綠燈——見 Phase 5 的 M1）：
  `MSYS_NO_PATHCONV=1 wsl.exe -d Ubuntu-26.04 -- bash /mnt/c/<path>/verify.sh all`
- 建置目標：`make all -j4`（裸 `make` 只建第一個目標，不是核心）、`make unit`（<1s）、
  `make test`（完整，~8-10 分鐘）、`make bench`（資訊性，不在 `make test` 內）。
- flat layout：核心原始碼在根目錄 (*.c/*.h/*.s)，`user/` 是 ring-3 程式，
  `tests/` 是原生單元測試，`gen_*.py` 產生內嵌資源，`Makefile` 為唯一建置系統。

## 目前基準（Session 30 結束時）
- `make clean && make all -j4`：0 warning / 0 error（-Wall -Wextra，實測計數）
- `make test`：**真實離開碼 0**（unit 21 套件 + test-ata-absent/test-boot/
  test-iso/test-shell）
- test-shell 結尾的洩漏偵測斷言：`running=0 zombies=0 peak=4`、`blocked=0`、
  `sleeping=0`、`RAMFS nodes=59`、`spaces=0`

## Phases

### Phase 0: 建立基準（baseline）
Status: complete
- 確認 WSL 工具鏈（完成）
- clean build：0 warning / 0 error
- make test：全數通過
- git status／.gitignore 已確認：建置產物（.o/.elf/.img）正確被排除、未被追蹤

### Phase 1: 通讀原始碼 + 建立問題清單
Status: complete
- 已通讀全部核心 C/組合語言（約 9000 行，不含產生檔與各 demo）+ user/ 全部
  （約 2900 行）：pmm/paging/heap、isr/interrupt.s/timer/task、process/syscall、
  elf_loader、pipe/sem、fs/ramfs/diskfs/fat16/procfs、ata/kb/rtc/vga/utils、
  gdt/idt、各組合語言進入點、kernel.c、user/ush.c、user_syscall.h、
  gen_*.py、Makefile（根目錄與 user/）、linker.ld
- 建立架構筆記：確認「全域 cli」併發模型的正確性，避免把設計選擇誤判為 bug
- 找到並記錄 6 個具體問題（F1-F6，見 findings.md），另記錄 5 項刻意不修的
  已知限制（見 findings.md「已知限制」段落）

### Phase 2: 依優先級修復
Status: complete
- P0 x2（F1 多執行緒 exit 的 use-after-free、F2 signal_deliver 可讓核心整台
  當機的無界限指標運算）：已修
- P2 x1（F4 fat16 節點池別名/資料錯置）：已修
- P3 x1（F3 procfs 緩衝區潛在溢位）：已修
- P4 x2（F5 Makefile clean 遺漏、F6 README 文件不一致）：已順手修
- 效能：memcpy/memset 4-byte 對齊批次化（PERF1）：已實作
- 正確性微調：terminal_write_dec 改走純 unsigned 格式化：已實作
- 每步都在 WSL 重新 build + make test 驗證，記錄於 progress.md

### Phase 3: 補測試與文件
Status: complete
- 新增 user/threadexit.c 迴歸測試，完整接入建置系統與 test-shell 目標，
  在 QEMU 中實際驗證 F1 修復生效（worker thread 在 main exit 後仍正常跑完，
  系統未當機）
- process.h / user_syscall.h 中「必須在 exit 前 join thread」的舊註解已更新，
  反映修復後的實際（更安全的）行為
- README.md 更新 user 程式數量（37→38）與中文版系統呼叫數（47→51，修正
  與英文版不一致的既有錯誤）

### Phase 4: 最終驗證
Status: complete
- 全量 `make clean && make -j4`：0 warning / 0 error
- 全量 `make test`：exit 0，全數通過（含新增的 threadexit 測試）
- findings.md 中未修復項目已全部記錄為「已知限制」並附理由，不是遺漏

## Phase 5: Session 2 — 深化改進 + 修正驗證方法
Status: complete
- **M1（關鍵）**：發現並更正 Session 1 的驗證缺陷——`wsl.exe -- bash -lc
  "...\$?"` 擷取離開碼恆為 0，導致過時的 `RAMFS nodes=47` 斷言失敗卻被誤判為
  通過。改用可靠的腳本內擷取（scratchpad/verify.sh），重建真正的綠燈基準。
- **F7（P1）**：修 execv 從多執行緒行程呼叫的 UAF（process_exec_reset 拒絕
  thread_count>0）。新增 execguard 迴歸測試。
- **PERF2**：ramfs_write 幾何成長（攤還 O(1) append）。新增 ramgrow 驗證測試。
  （原「已知限制」項目，本輪實作。）
- test-shell：逾時 210s→260s、節點數斷言更新至 50、新增 execguard/ramgrow 斷言。
- 全部以可靠方式驗證：clean build 0 warning/0 error、`make test` 真實離開碼 0。

## Phase 6: Session 3 — 路徑解析、FAT16 中繼資料、排程公平性與併發正確性
Status: complete
- **F8（P2）**：vfs_resolve_path 過長路徑靜默截斷→解析到祖先目錄。改為乾淨
  失敗。新增 pathlim 迴歸測試（同時驗證沒有過度拒絕）。
- **F9（P3）**：FAT16 叢集耗盡時檔案長度記成請求結尾而非實際寫入結尾。改用
  offset + written。
- **FAIR1**：blocked list 改 FIFO 喚醒（原「已知限制」）。
- **F10（P1）**：由 FAIR1 暴露——process_send_signal/process_request_kill 用
  task_wake_one(channel) 喚醒「特定 task」，正確性只是巧合依賴 LIFO；共用
  channel 時會喚錯對象導致**系統死鎖**。新增 task_wake_task 依身分喚醒。
- 節點數斷言更新為 51；全部以可靠方式驗證：clean build 0 warning/0 error、
  `make test` 真實離開碼 0。

## Phase 7: Session 4 — 標準 I/O 的 VFS 參照管理
Status: complete
- **F11（P1，記憶體安全）**：dup2/redirect 把檔案節點掛到 stdin/stdout 未取得
  VFS 參照；配合「dup2 後 close」慣用法（ush 就是這樣寫），節點參照歸零後可被
  unlink→kfree，行程仍持懸空指標，下次寫入會呼叫從已釋放記憶體讀出的函式指標
  （核心 UAF，控制流層級）。修法：dup2/process_redirect 取得參照、替換時釋放，
  process_finish_exit 於行程結束釋放，帳目平衡。
- 新增 user/redirref.c：同時驗證「使用中不可 unlink」與「結束後可 unlink」
  （後者證明沒有反向的參照洩漏）。
- 節點數斷言更新為 52；clean build 0 warning/0 error、`make test` 真實離開碼 0。

## Phase 8: Session 5 — FAT16 開啟計數（三個檔案系統的行為一致化）
Status: complete
- **F12（P2）**：fat16 是唯一沒有開啟計數的檔案系統，unlink 會在檔案仍開啟時
  釋放叢集鏈 → 那些叢集可被配置給新檔案，舊描述子讀到別的檔案內容（靜默的
  跨檔案資料洩漏）。補上 refs + open/close callback，unlink 在開啟時拒絕。
  同時讓 fat16_make_node 只挑 refs==0 的 slot，消除 F4 的殘留風險。
- 新增 user/fatref.c（刻意不真的刪檔，避免影響既有 FAT16 測試）。
- 節點數斷言更新為 53；clean build 0 warning/0 error、`make test` 真實離開碼 0。

## Phase 9: Session 6 — 跨檔案系統執行（功能擴充）
Status: complete
- **FEAT1**：`elf_load_image` 由 `ramfs_find_file` 改為 `resolve_fs`，可從任何
  已掛載的檔案系統執行程式。載入器其餘部分本來就是檔案系統無關的。
- 關鍵取捨：**不**改成 cwd 相對解析（否則 ush 的 `cd fat` 後跑 `cat` 會壞）。
- 驗證：`cp hello fat/hello` → `fat/hello`（從 FAT16 執行）→ `rm fat/hello`；
  "Hello from user space!" 次數 2→3、無 exec 錯誤。

## Phase 10: Session 7 — fork 繼承標準串流
Status: complete
- **F13（P2）**：fork 只複製 fd 3+ 的表，沒複製 fd 0/1（stdout_node/stdin_node/
  stdout_pipe/stdin_pipe），導致 `dup2(fd,1); fork()` 後子行程寫到終端機而非
  重導向目標。改為一併繼承並各自取參照，由 process_finish_exit 釋放。
  建立在 F11 的參照管理之上。
- 新增 user/forkredir.c（祖孫三層驗證繼承，並斷言輸出未洩漏到終端機、
  暫存檔可被刪除以證明無參照洩漏）。
- 節點數 54、README 程式數 46；clean build 0 warning/0 error、真實離開碼 0。

## Phase 11: Session 8 — SYS_SIGRETURN 未驗證（P0）+ 整數處理硬化
Status: complete
- **F14（P0，安全性）**：SYS_SIGRETURN 未驗證就解參照使用者 ESP；任何程式可直接
  int $0x80 觸發（不需在訊號處理常式中），讓核心在 ring 0 讀未映射位址 →
  **整台機器停機**。與 F2 同類——F2 只修了寫入側，讀取側是當時的疏漏。
  修法：解參照前驗證整個 sigcontext 框在使用者堆疊範圍內，否則只殺該行程。
  新增 user/sigretguard.c 實際執行攻擊驗證（系統存活 = 通過）。
- **F15（P3）**：sys_sbrk 對 INT32_MIN 取負是 UB，改用無號運算。
- **F16（P3）**：umalloc 向 sbrk 要求記憶體時的 int 溢位可能變成「縮小堆積」。
- 節點數 55、README 程式數 47；clean build 0 warning/0 error、真實離開碼 0。

## Phase 12: Session 9 — 多執行緒行程的 kill
Status: complete
- **F17（P2）**：process_check_kill 殺掉當前 task 後立刻清除 kill 請求，導致
  多執行緒行程只死一個 task、其餘存活且再也殺不到，行程永遠 RUNNING。
  一行移除即可：交給既有的 stale 檢查在行程真正結束後清除。
- 殘留限制（已誠實記錄於程式碼註解與 findings）：長期阻塞在等待迴圈中的 thread
  不會成為 current，仍殺不到；需要可中斷睡眠，屬較大架構改動，本輪不做。
- 新增 user/killthread.c（背景執行，失敗時不 hang；靠結尾 running=0 斷言鑑別）。
- 節點數 56、README 程式數 48；clean build 0 warning/0 error、真實離開碼 0。

## Phase 13: Session 10 — 建立量測與驗證能力（方向轉換）
Status: complete
- 起因：我對「是否已完善」給了否定評估，並指出自己工作的弱點（效能未量測、
  ISO 路徑未驗證、無單元測試、第 8 輪才找到第 2 輪該發現的 P0）。
- **CAP1**：新增 `test-iso`，補上從未被驗證的 GRUB/ISO 開機路徑；斷言
  Multiboot 記憶體映射（GRUB 路徑的實質差異）與三個檔案系統掛載。
  工具鏈缺少時 SKIP 而非失敗。
- **CAP2**：建立 tests/ 原生單元測試框架，4 套件約 50,500 檢查、<1 秒；
  `make test` 先跑 unit 再跑 QEMU。
- **用突變測試驗證測試本身有效**：注入 5 個 bug，4 個被抓到。
- **F18（P3）**：突變測試間接找出 pmm_init_region 重新保留 frame 0 時未補回
  used_blocks，導致可用區塊回報多一個。兩輪人工審查都漏掉。已修＋加測試。
- 記錄未修項：Git-for-Windows 的 CRLF 轉換使 WSL 內 git 視整棵樹為已修改。

## Phase 14: Session 11 — FAT16 單元測試（覆蓋最薄處 + 關閉 F9 驗證缺口）
Status: complete
- **CAP3**：新增 tests/test_fat16.c，連結核心實際內嵌的同一份映像，每個測試
  重新掛載。37,351 檢查。重點涵蓋叢集鏈延伸/走訪、叢集邊界上下的偏移讀取、
  部分覆寫、建立刪除與叢集回收、開啟中不得 unlink、8.3 名稱限制。
- **關閉 F9 驗證缺口**：單元測試可安全灌爆磁碟區（QEMU 裡不行），直接驗證
  叢集耗盡時 `node->length == written`。findings.md 該註記已更新。
- **突變測試**：注入 5 個 bug 全部被抓到，含 F9/F12 的回歸。
- clean build 0 warning/0 error、`make test` 真實離開碼 0。

## Phase 15: Session 12 — DiskFS 單元測試（信任邊界）
Status: complete
- **CAP4**：新增 tests/test_diskfs.c（943 檢查）。以 RAM 陣列 stub ATA，測試
  `diskfs_mount()` 這條「解析不受信任磁碟資料」的信任邊界：16 種竄改案例
  （含父鏈成環、重名、長度超限等）都必須被拒絕。功能面涵蓋跨磁區寫入、
  空洞補零、大小上限、巢狀目錄、開啟中不得移除。
- 突變測試逼出並修正兩個測試設計缺陷（magic 測試其實在測 checksum；補零測試
  打不到目標路徑）。
- 誠實記錄：write_slot 的兩段清零互為備援，無法個別覆蓋；測試守住的是「不得
  洩漏前一個檔案資料」這個性質。
- 一項我的假設錯、程式碼對的案例（mount 拒絕時刻意保留既有掛載）已修正測試。

## Phase 16: Session 13 — 效能量測（補上未量測的宣稱）
Status: complete
- 新增 `make bench`（資訊性，不納入 make test）。
- memcpy/memset：計時對照（連結真 utils.c）。對齊的頁/磁區 3–5x；誠實補述
  memcpy 在來源/目的相對未對齊時無改善（~1.0x）。
- RAMFS 幾何成長：計數式量測（精確、與 host 無關）。重新配置 N→log2(N)、
  成長複製 O(N²)→O(最終大小)（N=1024 少 516 倍）。
- 結果存成 tests/BENCHMARKS.md；findings.md PERF1/PERF2 更新為「已量測」+誠實限制。
- 至此自我檢討的四項弱點處理三項（ISO/單元測試/效能量測）。

## Phase 17: Session 14 — IPC 單元測試（pipe / sem）
Status: complete
- **CAP5**：新增 tests/test_pipe.c（68 檢查）、tests/test_sem.c（32 檢查）。
- 前置：pipe.c/sem.c 的 cli/sti 在 host 會 SIGSEGV，用核心永不定義的 HOSTED_TEST
  巨集守護編成 no-op（核心 codegen 不變）。
- 手法：腳本化 hook 取代 task_block_current，在單執行緒上確定性走過阻塞退出條件。
- 重點涵蓋環狀緩衝 wrap（多次跨 4096 邊界）、EOF/broken-pipe、參照計數、
  三種阻塞轉換；sem 的計數與阻塞於 0→post 釋放。
- 突變測試：pipe 5/6 抓到（1 benign PASS，誠實預期）、sem 4/4 抓到。
- 記錄技術債：save/restore_irq 在 7 檔重複，抽 irq.h 為後續去重機會。
- 單元測試現 8 套件、~88,900 檢查；clean build 0 warning/0 error、真實離開碼 0。

## Phase 18: Session 15 — irq.h 去重（先建驗證能力、再安全重構）
Status: complete
- **REFACTOR1**：save_irq_disable/restore_irq 原本重複在 7 個檔案，抽成共用
  irq.h（相同函式名 static inline，呼叫點零改動，統一 HOSTED_TEST 守護）。
- **codegen 等價性實測證明**：7 個模組的 .o 重構前後用 cmp 比對，位元組完全相同。
- 順序意義：累積 8 模組單元測試 + 端對端測試後，這個一直不敢動的重構風險才夠低。
- 解鎖：timer/task/process/ata/kb 現在也可原生單元測試（記錄為後續機會）。
- clean build 0 warning/0 error、`make test` 真實離開碼 0。

## Phase 19: Session 16 — timer 單元測試（用上 irq.h 解鎖的可測性）
Status: complete
- **CAP6**：新增 tests/test_timer.c（50 檢查）。核心目標是 tick_reached 的
  wrap-around 比較——naive 無號比較會在 2^32 繞回時誤觸發（~497 天才現形）。
- 手法：stub register_interrupt_handler 捕捉 static 的 timer_callback；
  timer_ticks 全域直接設 0xFFFFFFFE 測繞回。
- 前置：io.h 的 port I/O 加 HOSTED_TEST 守護（timer_install 的 outb 會 fault），
  **已實測證明對核心 codegen 中性**（7 個 include io.h 的 .o cmp 位元組相同）。
- 突變測試：6 個注入全部被抓到，含「tick_reached 改 naive 無號」。
- 測試健壯性：reset() 排空迴圈加 64 次上限，避免「不釋放 slot」突變導致 hang。
- 單元測試現 9 套件；clean build 0 warning/0 error、真實離開碼 0。

## Phase 20: Session 17 — task 排程器單元測試
Status: complete
- **CAP7**：新增 tests/test_task.c（51 檢查）。stub 組語 switch_task 後，測 ready
  環與 blocked 串列的純指標邏輯，含 F10 的 task_wake_task 首次直接覆蓋。
- 涵蓋 FIFO 喚醒順序、依身分喚醒（中段/頭/尾）、channel 選擇性、state 轉換。
- 突變測試 5 個全抓到；並補上第一版漏掉的 state 欄位斷言。
- 工具：多行突變改用 Python 字面替換 + LF 正規化（sed/perl 經 bash 太脆弱）。
- 單元測試現 10 套件；clean build 0 warning/0 error、真實離開碼 0。

## Phase 21: Session 18 — rtc 解碼單元測試（+可測性重構）
Status: complete
- **CAP8**：抽出純函式 rtc_decode（行為保持，由 date 端對端測試確認），新增
  tests/test_rtc.c（35 檢查）測 BCD/binary、12h PM、12→0/12→noon、世紀。
- 涵蓋真實硬體會用、QEMU 從不觸發的路徑（QEMU 固定 binary/24h）。
- 突變測試 6 個全抓到。
- 單元測試現 11 套件；clean build 0 warning/0 error、真實離開碼 0。

## Phase 22: Session 19 — process 環境變數單元測試（--gc-sections 攻克高耦合）
Status: complete
- **CAP9**：新增 tests/test_process_env.c（47 檢查）測 setenv/getenv/env_copy。
- 技術關鍵：process.c 相依 30+ 符號，用 #include + -ffunction-sections +
  --gc-sections 讓連結器丟掉沒被觸及的函式，stub 面縮到 3 個。開啟高耦合模組
  純邏輯測試的路徑。
- 涵蓋 bounded copy 的 max-1、ENV_MAX 上限、overwrite、截斷；突變測試 6 個全抓到。
- 單元測試現 12 套件；clean build 0 warning/0 error、真實離開碼 0。

## Phase 23: Session 20 — syscall 使用者指標驗證單元測試（安全前線）
Status: complete
- **CAP10**：新增 tests/test_syscall_valid.c（36 檢查）測 user_buffer_valid /
  user_string_valid / alloc_fd。--gc-sections 讓 stub 面只剩 paging_user_range_mapped。
- 核心是整數溢位繞過（近頂端 + 巨大長度）——shell 幾乎無法觸發。
- 突變教訓：stub 太嚴格獨立遮蔽了溢位 bug；加「強制已映射」模式隔離後 7 個全抓到。
- 單元測試現 13 套件；clean build 0 warning/0 error、真實離開碼 0。

## Phase 24: Session 21 — 可終止的阻塞等待（關掉 F17 誠實記錄的殘留限制）
Status: complete
- **F19（P2）**：停在 `while (cond) task_block_current(ch);` 的 task 完全殺不到
  ——kill 只由計時器中斷裡的 process_check_kill 施行，而它只看當前 task，
  而阻塞中的 task 永遠不會成為當前 task。後果：行程永遠 RUNNING，等它的人
  永遠阻塞。順帶暴露：只喚醒 proc->task（漏掉其他 thread）、SIGSTOP 停住的
  行程也殺不掉。
- 修法刻意**不做 EINTR 上拋**（那是 Session 9 評估為「大改動」的路線，會動到
  系統呼叫 ABI）：改成 task_t 加 kill_pending 旗標 + task_kill_blocked() 標記並
  喚醒整個行程的 task + task_block_killable() 醒來就離開。10 個阻塞點換掉 9 個。
- timer_sleep 是唯一例外（持有 sleep slot，必須先歸還再離開，且要判斷 slot
  還是不是自己的）。
- 驗證：單元 tests/test_task.c +21 檢查（72 total）與 tests/test_timer.c +13
  檢查（63 total，涵蓋 timer_sleep 三條 kill 路徑，用 longjmp 模擬 task_exit）、
  端對端 user/killwait.c（兩個 task 都在睡時由外部行程發 kill）、
  突變測試 9 個注入全抓到（task.c 6 + timer.c 3，見 Session 23）。
- 節點數 57、README 程式數 49、test-shell 逾時 270s。

## Phase 25: Session 22 — paging COW 參照計數 + user_pte 單元測試
Status: complete
- **CAP11**：新增 tests/test_paging_cow.c（31 檢查）測 user_pte（vaddr→頁表項的
  區域選擇與移位 index）與 COW 參照計數 cow_ref_inc/cow_ref_release。--gc-sections
  讓兩者閉包不需任何外部函式，零 stub。
- 突變測試 7 個：6 個功能上抓到；cow_ref_inc 的越界寫入無功能訊號，改用
  **UBSan 陣列邊界陷阱**（trap 模式、-m32 可用）抓到，補足突變測試對「不可觀察
  記憶體越界」的盲點，7 個全抓到。
- 單元測試現 14 套件；clean build 0 warning/0 error、真實離開碼 0。

## Phase 26: Session 24 — ELF 載入器信任邊界（審查 + 兩個修復 + 單元測試）
Status: complete
- 動機：FEAT1（Session 6）讓可執行檔改走 VFS 之後，使用者可寫任意位元組到檔案再
  執行，ELF 的每個欄位都變成攻擊者可控，而 `paging_map_user_page` 不做範圍檢查。
- **F20（P1，記憶體安全）**：`elf_load_image` 全程未取得 VFS 參照 → 載入中被
  `rm` 就是核心 UAF（與 F11 同型）。修：`open_fs`/`close_fs` 包住整個載入，
  主體拆成 `elf_load_from_node()` 以確保單一出口成對釋放。窗口：RAMFS 上只有
  微秒級（撞不到），但 FAT16/DiskFS 走 ATA PIO 時跨多個 tick，是毫秒級。
- **F21（P2，TOCTOU）**：program header 驗證後又重新讀取，中間可被改寫，未驗證的
  p_vaddr 直達 `paging_map_user_page`。修：抽出 `phdr_in_user_range()` 兩處共用，
  使用前重驗。誠實評估影響僅及行程自身（`user_pte` 擋住核心位址），故列 P2。
- **CAP12：tests/test_elf.c**（139 檢查）。第三條「解析不受信任資料」的信任邊界
  測試。每個拒絕都驗「不得建立位址空間」或「建立後必須銷毀」。
- 突變 5 個：E1/E1b/E2 抓到；**E3 一開始存活**（溢位被 entry-point 檢查遮蔽），
  補「第二個 segment 繞回」的案例後抓到；**E4 確認為等價突變**（被 p_memsz 界限
  遮蔽，路徑到不了），誠實記錄不硬寫測試。
- **第二組獨立突變 12 個（12/12）**：本輪有兩個 session 各自對同一個檔案做突變，
  注入集不同、逼出的缺口也不同——單一組突變不等於測夠了。第二組漏網的兩個都是
  「我的案例被另一條子句先擋掉、被測子句沒被隔離」，修正後才有鑑別力。
- **過程教訓**：兩個 session 並行改同一棵樹時，**會改原始碼的突變腳本其還原會
  覆蓋對方的編輯**，第一輪結果因此不可信。腳本已加入 baseline 完整性檢查
  （檔案被動過就中止而非覆蓋）。

## Phase 27: Session 25 — RAMFS（找到一個 P0：使用者可讓整機凍結）
Status: complete
- 動機：RAMFS 的開啟計數是 F11/F20 兩個已修 P1 的依賴基礎，但只被端對端間接測到。
- **F22（P0，DoS）**：PERF2 幾何成長的溢位守衛差一步——`new_cap == 0x80000000` 時
  `> 0x80000000` 不成立，`*= 2` 截斷成 0，之後**無窮迴圈**。`sys_seek` 允許 offset
  到 0x7FFFFFFF，寫 2 bytes 即可觸發；而 `int $0x80` 是 interrupt gate（`0xEE`），
  CPU 進入時清 IF，於是迴圈在**關中斷**下永遠轉 → **整台機器停機**。
  修：守衛改為 `new_cap > 0xFFFFFFFFU / 2`（在乘法之前擋）。
- **教訓**：Session 13 用 `make bench` 量過 PERF2 的效能，卻沒測算術邊界；
  「量過效能」不等於「驗證過正確性」。
- **CAP13：tests/test_ramfs.c**（4300 檢查）。看門狗 `alarm(30)` 讓無窮迴圈回歸
  變成失敗而非掛住；配置器毒化（填 0xAA）讓缺失的清零無法被「malloc 剛好給零」遮蔽。
- **端對端 user/bigseek.c**：在 QEMU 中實際執行該攻擊，跑到 `[bigseek survived]`
  即證明核心存活。
- 突變 7 個抓到 6：R7 一開始存活（兩段清零互為備援），補「稀疏寫入跨越容量」的
  案例後抓到；R6 確認是冗餘防禦（註解自承），誠實記錄不硬殺。
- 節點數 58、README 程式數 50、逾時 280s。

## Phase 28: Session 26 — VFS 核心（fs.c）＋ FAT16 長度記帳的殘留缺口
Status: complete
- 動機：每個帶路徑的系統呼叫都經過 fs.c 的兩個嚴格解析器，而它們一個單元測試都
  沒有（只有正規化器有，34 檢查，全專案最薄）。F8 就住在這裡。
- **CAP14：tests/test_fs.c**（277 檢查）。mock 檔案系統刻意比任何真實後端寬鬆，
  樹裡放進「名字就叫 `.`／`..`／空字串」與「FS_FILE 卻帶著完整目錄操作」的節點，
  用來隔離「是哪一層擋下來的」。斷言派送到哪個節點、帶什麼名字，而不只是成敗
  ——路徑解析器壞掉的樣子是安靜地回答錯的物件。
- **HARD1**：`resolve_fs` 補上與 `resolve_parent_fs` 相同的「中途組件必須是目錄」
  檢查。**目前不可觸發**，是縱深防禦而非修掉的 bug，誠實記錄。
- **F23（P3）**：`fat16_vfs_write` 在 `written == 0` 時仍把長度推到 offset。
  修：`if (written > 0)` 包住長度更新。端對端 user/fatgrow.c。
- 突變：fs.c 22/22（4 個一開始存活，逼出 3 個真實缺口 + 1 個被我誤判為等價的）、
  fat16.c 3/3（各由不同測試抓到）。
- 節點數 59、README 程式數 51、逾時 285s。

## Phase 29: Session 27 — kb.c（鍵盤驅動：環狀緩衝 / 修飾鍵 / Ctrl+C）
Status: complete
- 動機：核心裡唯一一處「中斷處理常式與 task 同碰一個結構」的地方，而 QEMU 那套只走
  最窄的一條路（緩衝區永不滿、索引永不繞回、沒有按鍵被丟掉）。
- **CAP15：tests/test_kb.c**（1675 檢查）。`#define IO_H` 換掉 io.h 自備 `inb`，
  不動任何核心標頭；`setjmp`/`longjmp` 接住 noreturn 的 `task_exit`。
- **kb.c 本身沒有找到 bug**（誠實記錄）。
- 突變 18/18，其中 3 個以逾時被抓到（症狀是掛住而非答錯，腳本層加 `timeout 20s`）。
  **K17 一開始存活**：`count == 0` 的案例被「緩衝區裡剛好有字元」遮蔽，改成對空
  緩衝區呼叫才有鑑別力——本專案第三次踩到同一形狀。
- 0xE0 延伸掃描碼「不處理但後果良性」的推導已轉成測試，並用 K18（看似合理的修法）
  證明右 Ctrl 會因此壞掉。

## Phase 30: Session 28 — procfs（F3 的守衛第一次被執行）
Status: complete
- 動機：F3 是 procfs 的緩衝區溢位，而它的修法（/proc/processes 界限檢查）**從來
  沒有執行過一次**——只有 pid 到十位數或行程名塞滿欄位時才發火，QEMU 執行碰不到。
- **CAP16：tests/test_procfs.c**（183 檢查）。`gen_buf` 灌毒 0x7F + 「回報長度之後
  必須全是毒」的結構化不變式，外加 `-fsanitize=bounds` trap 模式第二道網。
- **procfs.c 沒有找到 bug**（誠實記錄）。產出是 F3 守衛的精確邊界被釘住，以及兩個
  **完全沒有界限檢查**的產生器最壞情況被算出並守住。
- 突變 16 個：14 抓到（P1 即 F3 溢位本身，被 sanitizer trap）、**P2/P3 一開始存活**
  （均勻的 40-byte 行讓 `pos` 只取 40 的倍數，完全碰不到守衛邊界；改成精準落在 473
  之後被抓到，並從 472 那側夾住）、P4/P12 推導確認為等價突變。

## Phase 31: Session 29 — sys_seek 上界調查、後端契約、VGA、heap 稽核
Status: complete
- 先為 Session 26–28 建立 checkpoint commit（未 push）。
- **SEEK1**：實證調查後**決定不改** `sys_seek` 語義（三個後端已正確、沒有有依據的
  全域常數、收窄會讓 user/bigseek.c 這個唯一的 F22 端對端證據失效）。完整推導在
  findings.md。
- **CONF1**：tests/fs_conformance.h——把「下一個後端要自己重擋」變成三個後端都跑的
  可執行契約。突變 3/3，其中 **C3 只有契約抓到**（既有 943 檢查全漏），
  **C1 是既有看門狗抓到的、不是契約**（誠實記錄）。
- **CAP17**：tests/test_vga.c（4769 檢查），突變 21/21 全由斷言抓到。
  附帶修正 tests/test.h：失敗訊息逐筆 flush，否則崩潰會吃掉所有失敗訊息。
- **HEAP1**：heap.c 稽核無缺陷；補四個真實測試缺口（378 → 720 檢查），突變 15/15
  無等價突變。stub 改為單一 arena 依序配發以貼近真實 PMM。

## Phase 32: Session 30 — ATA PIO 驅動（找到一個 P2）＋ IRQ-driven 可行性評估
Status: complete
- 動機：最後一個沒有單元測試的驅動，位於儲存堆疊最底層；QEMU 的模擬 IDE 從不逾時、
  從不報錯，所以所有失敗路徑都是未執行過的程式碼。
- **F24（P2）**：逾時的命令讓磁碟停在命令中途，下一個操作因此讀到**上一個 sector**，
  或**回報寫入成功卻一個位元組都沒寫**。修：`ata_wait_idle()`——等 BSY **並排空滯留的
  DRQ**，所以是復原而非拒絕。QEMU 裡不可觸發（模擬 IDE 立刻回應），真實硬體會。
- **CAP18：tests/test_ata.c**（8967 檢查）。有狀態的假 IDE 裝置保留真實握手時序；
  irq.h 換成計數版本以驗證八條 return path 的 save/restore 配對。
- 突變 28/28，零等價突變；一開始 8 個存活，其中 A4/A5、A8 暴露的是**測試模型自身**
  的問題（故障注入太黏、對驅動太寬容）。
- **ASSESS1**：IRQ-driven ATA 現在不做——阻礙不是測試覆蓋，而是 `diskfs.c` 完全沒有
  序列化，改成阻塞式需要先替儲存堆疊引入併發模型。

## Phase 33: Session 31 — 描述子表的所有權契約（CAP19）
Status: complete
- 動機：Session 30 稽核出的跨檔案不變式——「每個交給新行程的 slot 都必須帶著空的
  `open_files[]`」——散落在七條釋放路徑上，且一個字都沒寫在程式碼裡。
- 稽核：七條 `process_release()` 路徑全部正確，**沒有現存缺陷**。
- **CAP19：tests/test_fdtable.c**（474 檢查）。stub 保有真實參照計數語意，
  沒有對應 open 的 close 記為 underflow——**只看回傳值的測試對每個突變都會通過**。
- 突變 25 個：23 抓到、**D14 與 D21 確認為等價突變**（各有推導；D21 的等價性只在
  全域 cli 模型下成立，模型一改就是真的 UAF）。
- 一開始 6 個存活，逼出的兩個教訓：對稱狀態讓「搞反」不可觀察；只開兩三個描述子
  蓋不住迴圈邊界。

## Phase 34: Session 32 — 行程生命週期狀態機（CAP20）
Status: complete
- 動機：F1/F7/F17/F19 四個 P0/P1 的發源地，而它們的不變式由多個函式交互維持，
  沒有直接測試。
- **CAP20：tests/test_process.c**（351 檢查）。排程器被**模型化**：記錄誰 park、
  park 在哪個 channel、誰被喚醒、以及「park 了卻沒人叫醒」——後者把「會永遠掛住」
  變成斷言。teardown 每一步記錄發生順序。
- **第一優先的隱性相依已釘住**：無 SIGCHLD handler 時 `waitpid` 仍須被喚醒；
  喚醒須瞄準父行程 task；block 與 broadcast 的 channel 確實不同。
- 突變 35 個：33 抓到、**P25/P26 為互為備援的等價突變**。P9（一個 thread 看不出
  差別，要兩個）與 P17（交換順序不改變計數，要記錄序號）是真實缺口。
- 三個原本只被 timeout 抓到的突變已改為具名斷言。
- **沒有找到缺陷**。
- Session 32 follow-up：harness 的 spurious-wake 腳本原本第一輪就讓 child exit，且沒有
  same-channel decoy，因而無法區分 identity wake 與碰巧的 channel wake；這是**測試模型
  缺口**，不是核心 defect。修正後 `test_process` 為 369 checks，新增的 lifecycle mutant
  matrix 為 14/14 killed、`process_release` 不清欄位為等價（下一次 allocate 完整 memset，
  且 UNUSED slot 不可查）；`test_task` 再以 2/2 mutants 證明 retired kernel stack 不會提前
  free、也不會漏掉下一個 scheduler safe point 的回收。
- 驗證完成：production `process.o`／`task.o` 與無 hosted instrumentation 的 baseline
  位元組相同；`make unit` 24 套件、`make clean && make all -j4`、完整 `make test`
  （288.1 s，HOST_RC=0）全綠，leak/state counters 回 baseline。

## 目前結論（Session 32）
F1–F23 全數修復並驗證：4 個 P0（F1、F2、F14、**F22**）、4 個 P1（F7、F10、F11、F20）、
7 個 P2、6 個 P3、2 個 P4。功能擴充 FEAT1、排程公平性 FAIR1、效能 PERF1/PERF2（已實測）、
去重 REFACTOR1（已證明 codegen 不變）、強化 HARD1（誠實標示為不可觸發的縱深防禦）。
測試能力累積到 17 個單元套件 + 4 個 QEMU 端對端目標。

**未完項目與下一輪候選見 `PROJECT_STATE.md` 第 5、6 節**（已知限制 6 項、
候選工作 A/B/C 三類，附價值與風險評估）。

### Session 33 — CAP21 訊號遞送生命週期
找到 **F25**（P0，權限提升）：`sys_sigreturn` 把使用者的 EFLAGS 原封不動交給
`iret`，而 `iret` 在 CPL 0 執行時會**從堆疊映像載入 IOPL**。修法是只放行程式
自己的算術旗標並強制設回 IF。QEMU 以同一支 `user/sigflags.c` 雙向驗證。
新增 `tests/test_signal.c`（88 檢查），突變 22/23（S14 為等價突變，附推導）。
三個原本只被 segfault／timeout 抓到的突變改成具名斷言。

### Session 34 — CAP22 mmap / sbrk 位址空間所有權
- 審計 `ext_map` reservation 與 demand-paged PTE teardown，新增 `tests/test_vm_lifecycle.c`
  （36 檢查），並把 fork/exec/slot-reuse metadata 契約擴進 `test_process`（397 檢查）。
- 一度懷疑 `sys_munmap` 先清 bitmap、再 unmap PTE 的中間會被 sibling 搶到位址；後來
  發現這是 hosted harness 漏模型化 `int $0x80` interrupt gate 的假競態。現行單核 ABI
  下 IF 在 syscall 全程為 0，task 只能在 iret 後執行；模型已改為明確驗證該順序，
  **沒有為正確程式加入冗餘 lock**。
- 釘住 sbrk 上/下界與 `INT32_MIN`、mmap first-fit/reuse/invalid-free 原子性、fork bitmap
  複製但 parent/child 隔離、exec 換 image 清 bitmap、以及 syscall-gate 到 iret 的
  lifecycle 邊界。突變 **7/7 meaningful killed**；「未加 outer lock」在現行 ABI 下
  為 equivalent/unreachable，誠實排除。

### 方法論上最值得記住的三件事
1. **假綠燈**（Phase 5 / M1）：`wsl.exe -- bash -lc "...$?"` 恆回報 0，一整輪的
   「測試通過」結論曾因此無效。離開碼必須在單一 bash 程序內擷取。
2. **突變測試是證明測試有效的手段，不是形式**：它多次逼出真實的測試缺口
   （CAP10 的 stub 遮蔽溢位、CAP12 的 entry-point 檢查遮蔽 E3、CAP13 的兩段
   清零互為備援）。存活的突變要追查是缺口還是等價突變，不可略過。
3. **「量過效能」≠「驗證過正確性」**：PERF2 在 Session 13 被 `make bench` 量過
   效能，但沒人測過它的算術邊界，F22（P0，凍結整台機器）因此潛伏了 23 輪。

## Phase 35: Session 35 — 跨子系統 QEMU 壓力與自動品質 gate（CAP23）
Status: complete
- 新增 `user/stress.c`，從 ring 3 組合施壓記憶體／分頁、PIT IRQ preemption、排程與
  context switch、syscall 指標驗證、三個可寫 filesystem、thread/process lifecycle、
  fd/pipe/process/node exhaustion，以及反覆 fork/exec/create/destroy。
- 新增 monitor-driven `test-stress`：同次 QEMU 開機執行兩輪，不靠固定完成延遲；每輪
  必須出現九個具名成功 marker，結束後 PMM、heap、user space、process/task/timer、
  RAMFS 快照必須逐欄一致且回到精確 baseline。
- 新增 hosted ASan+UBSan、Python bytecode + shell syntax + cppcheck gate，以及三個必須由
  具名容量斷言或 snapshot drift 擊殺的 QEMU mutants；mutation source 由 EXIT trap
  位元組精確還原。
- 更新 GitHub Actions，使完整 native/QEMU、sanitizer、static-analysis、mutation matrix
  在 branch 與 pull request 自動執行。

## Phase 36: Session 36 — heap 真耗盡與 teardown mutation 加固
Status: complete
- 把固定次數的 heap 壓力改為 bounded-until-NULL：slot 上限大於整個 user heap，若沒看到
  真正配置失敗，測試本身即失敗；所有 live chunk 仍須保有內容。
- 逆序釋放後配置並逐頁驗證 128 KiB，直接檢查 K&R free-list 跨 sbrk arena 合併與重用。
- 注入 `paging_unmap_user_page` 漏掉 `pmm_free_block` 的 mutant，要求兩輪 PMM snapshot
  產生具名 `resource snapshot drift`；driver stdout 與 QEMU debugcon log 一併保存。
- GitHub Actions 實測每輪恰好 209 個 4000-byte allocation 後返回 NULL，兩輪健康快照
  均維持 PMM `714/7478`、heap `9/25168`；三個 mutants 全數由預期 gate 擊殺。

## Phase 37: Session 37 — abnormal user-fault teardown 壓力
Status: complete
- 擴充既有 `fault`：觸發 supervisor-only address page fault 前，先 fault-in 16 頁 sbrk
  heap、32 頁 mmap，並保留一個開啟檔案與一對 pipe fd，刻意不走 user-space cleanup。
- `stress` 每輪 spawn/wait 24 次，逐次要求 page-fault handler 的 status 精確為 `-1`；
  harness 要求兩輪共 48 組 armed／USER PAGE FAULT／termination marker，不能多也不能少。
- 新增 user-fault status 與 `process_finish_exit` 漏關 fd/pipe 兩個 mutants；連同既有三個
  capacity/PMM leak mutants，GitHub Actions 實測 **5/5 killed**。
- 健康兩輪穩定在 PMM `716/7476`、heap `11/33348`、user `0/0`、process `0/0/16`、
  blocked/sleeping `0/0`、RAMFS `58`；11-page heap 是第一輪 workload 暖機高水位，
  第二輪沒有再成長。

## Phase 38: Session 38 — ring-3 CPU exception isolation（F26）
Status: complete
- 找到 F26（P0 DoS）：未註冊 handler 的 #DE/#UD/#GP 只印 `Received Exception` 就 iret，
  CPU 回到同一條 faulting instruction，任何 user process 都能讓核心陷入無窮 exception。
- generic ISR 改以 `CS.RPL` 分流；CPL3 exception 呼叫 `task_exit(-1)`，CPL0 exception
  輸出向量後停機，不能冒險恢復未知 kernel state。
- `fault` 的 24 iterations 均分為 #PF/#DE/#UD/#GP；兩輪各類精確 12 次、總 termination
  48 次，全部仍帶著 16 heap pages、32 mmap pages、file 與 pipe 做 abnormal teardown。
- 新增 CPL classification 與 generic exception status mutants；GitHub Actions 實測
  **7/7 killed**，健康快照維持 PMM `716/7476`、heap `11/33348`。

## Decisions & Assumptions Log
重大設計決策集中在 `PROJECT_STATE.md` 第 4 節；每個項目的完整分析在 `findings.md`。

## Errors Encountered
本專案刻意記錄自己犯的錯（含更正），詳見 `progress.md` 各 session 段落。
較重大的幾筆：

| Error | Resolution |
|-------|------------|
| 離開碼擷取恆為 0，造成假綠燈（Session 1） | 改用腳本內擷取，重建真正基準（Session 2 / M1） |
| 節點數斷言未隨新增程式更新，卻被誤判為通過 | 同上；此後每次新增程式都同步更新 N |
| 突變測試中斷把 mutant 留在工作樹 | 還原改放 `trap ... EXIT`，每輪驗證位元組相同 |
| 突變 pattern 對不上（CRLF 樹用 `\n`） | Python 字面替換 + LF 正規化後依原行尾寫回 |
| `bigseek` 誤用 API 導致節點數飄移（Session 25） | `sys_create` 已回傳開啟的 fd；測試改為**斷言**清理成功 |
| 往返測試沒模型化 handler 的 `ret`，在正確程式上也因錯的理由通過（Session 33） | 突變測試逼出來；補上 `useresp += 4` 才是真的往返 |
