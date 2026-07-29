# Task Plan — miniOS 全面審查與持續改善

## Goal
對 miniOS（32-bit x86 教學型作業系統，~33K LOC C/ASM，位於本專案根目錄）進行全面程式碼審查，
找出未完成項目、Bug、競態條件、記憶體安全問題、效能瓶頸、架構缺陷與技術債，依優先級修復、
重構、補測試、補文件，每階段修改後都要在 WSL Ubuntu-26.04 中 `make` 編譯並執行 `make test`
驗證不回歸。不得捏造測試結果；無法確認的需求採取最保守、相容現有設計的方案並記錄假設。

## Environment
- Windows host, WSL distro `Ubuntu-26.04` 提供建置環境：gcc 15.2.0 (-m32 可用), GNU as 2.46,
  qemu-system-i386 10.2.1, python3 3.14.4.
- 建置指令一律透過 `wsl.exe -d Ubuntu-26.04 -- bash -lc "cd '/mnt/c/Users/User/Desktop/All project/systems-programming/作業系統' && ..."`
- 專案倉庫是 flat layout：核心原始碼在根目錄 (*.c/*.h/*.s)，`user/` 是 ring-3 程式，
  `gen_*.py` 產生內嵌資源，`Makefile` 為唯一建置系統（無 CMake/Meson）。
- git repo，目前僅 1 個 initial commit，尚無 .gitignore 排除的 .o/.elf 追蹤情況需確認。

## Baseline status (before changes)
- [x] `make clean && make -j4`：0 warning / 0 error（-Wall -Wextra）
- [x] `make test`：全數通過（exit 0）— 這是回歸基準

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

## 本輪結論
所有已識別、可驗證觸發的 P0/P1 bug 均已修復並在 QEMU 中實測驗證；P2/P3
記憶體安全與效能問題也已修復；文件與建置腳本的小瑕疵已順手修正。剩餘項目
（ATA 驅動 cli 持有時間、ramfs 成長策略、排程公平性、多執行緒訊號投遞、
elf_loader 僅讀 RAMFS）都是需要更大幅度重構或觸發門檻在目前系統規模下極難
達到的架構取捨，依保守原則本輪不動，已記錄供未來參考。

## Decisions & Assumptions Log
(見 findings.md 中對應條目；重大假設會複製一份到這裡)

## Errors Encountered
| Error | Attempt | Resolution |
|-------|---------|------------|
