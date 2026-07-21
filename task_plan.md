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
