# Progress Log

## Session 1 — 2026-07-20
- 探索專案結構：flat-layout 32-bit x86 hobby OS，~33K LOC，Makefile 建置，
  gen_*.py 產生內嵌 user ELF 與 FAT16 image，`make test` 透過 QEMU monitor
  送鍵盤事件跑一套 end-to-end shell 測試。
- 確認 WSL Ubuntu-26.04 有完整工具鏈 (gcc 15.2 -m32, as 2.46, qemu-system-i386
  10.2.1, python3 3.14.4)。之後所有 build/test 都經由 WSL 執行。
- 建立 task_plan.md / findings.md / progress.md。
- **Baseline 確認**：`make clean && make -j4` 在 WSL Ubuntu-26.04 上 0 warning / 0 error
  （-Wall -Wextra 全開）。`make test`（含 test-ata-absent/test-boot/test-shell，QEMU monitor
  模擬鍵盤跑全套 shell/syscall/fs 測試）exit 0，全數通過。這是後續修改的回歸基準。

- **通讀原始碼**：依序精讀 pmm/paging/heap（記憶體管理）、isr/interrupt.s/timer/task
  （中斷與排程，並確認全域 cli 併發模型的正確性）、process/syscall（行程生命週期、
  系統呼叫參數驗證）、elf_loader、pipe/sem（IPC）、fs/ramfs/diskfs/fat16/procfs（VFS
  三個後端 + 合成檔案系統）、ata/kb/rtc/vga/utils（驅動與工具）、gdt/idt 與各組合語言
  進入點（boot.s/usermode_s.s/switch_s.s/gdt_s.s/paging_s.s）、kernel.c（開機流程＋內建
  shell）、user/ush.c 與 user_syscall.h（ring-3 shell 與系統呼叫包裝）、gen_*.py／
  Makefile／linker.ld（建置腳本）。核心 C/組合語言（不含產生檔與各 demo 程式）約 9000
  行、user/ 約 2900 行，全數讀過。

- **發現並修復 4 個具體 bug + 2 個順手修正**（完整描述見 findings.md）：
  - F1 (P0，記憶體安全)：多執行緒 process 的主 task 結束時沒等其他 thread，就直接
    釋放共享 address space，其餘 thread 之後被排程會 load_cr3 到已釋放記憶體。
    修法：process.c 拆出 process_finish_exit()，依 thread_count 決定立即收尾或延後到
    最後一個 thread 結束時才做。process.h 加 main_exited 欄位。
  - F2 (P0，安全性)：signal_deliver 直接對使用者堆疊指標做無界限檢查的寫入，惡意/
    錯誤的使用者 ESP 可讓整個核心（不只該行程）在 page_fault_handler 判定為
    kernel-mode fault 後 halt。修法：syscall.c 寫入訊號框前先驗證位址落在
    [USER_STACK_BOTTOM, USER_STACK_TOP) 且空間足夠，不合法就終止該行程而非讓核心當機。
  - F3 (P3，記憶體安全)：procfs.c 的 gen_buf[512] 在極端情況（大量 fork 後 pid 位數
    增加）可能被 /proc/processes 寫爆。修法：加剩餘空間檢查，不足就截斷。
  - F4 (P2，資料完整性)：fat16.c 用 16 個 slot 的環狀重用池配置 fs_node_t，累積超過
    16 次路徑查找就可能讓仍被某 fd 持有的節點被覆寫成另一個檔案（confused deputy，
    靜默資料錯置）。修法：配置前先找同一目錄項的既有 slot 並重用。
  - F5（順手修）：Makefile 的 `clean` target 漏刪 9 個 `*_embed.c` 產生檔。
  - F6（順手修）：README 中文版系統呼叫數與英文版不一致（47 vs 51）。

- **新增迴歸測試**：user/threadexit.c——main 不呼叫 sys_thread_join() 就直接 return，
  worker thread 在 main 結束後仍需跑一段時間才結束，專門驗證 F1 修復。已完整接入
  user/Makefile、根目錄 Makefile（OBJS/embed 規則/clean）、kernel.c（RAMFS 註冊）、
  Makefile 的 test-shell 目標（送鍵、檢查 `[threadexit main done]` 與
  `[threadexit worker done]` 依序出現）。**已在 QEMU 中實際跑過，確認通過**：
  main 立刻結束後 worker 仍正常跑完，系統沒有當機或重開機，證明修復是真的生效，
  不是只憑程式碼推理。

- **效能改善**：utils.c 的 memcpy/memset 從逐 byte 迴圈改成 4-byte 對齊批次搬移
  （不對齊的頭尾仍逐 byte 處理，來源/目的對齊不一致時整段安全退回逐 byte，行為完全
  不變）。這兩個函式是分頁歸零、ATA 磁區、三個檔案系統讀寫的共用熱路徑。

- **正確性微調**：vga.c 的 terminal_write_dec 原本把 uint32_t 轉成 int 再走
  int_to_ascii（signed），數值一旦超過 INT32_MAX 會有 implementation-defined 轉換，
  剛好等於 INT32_MIN 時 `-n` 甚至是 signed overflow UB。改成直接用 unsigned 邏輯格式化，
  不經過任何有號轉換。此路徑在目前系統規模下幾乎不可能觸發（需要 tick/pid 數超過
  2^31），純粹是防禦性修正。

- **每次修改後都重新驗證**：F1-F4 + PERF1 + terminal_write_dec 修正後都各自跑過
  `make clean && make -j4`（0 warning/0 error）與 `make test`（QEMU 全套測試，exit 0）。
  最後再做一次完整 clean build + make test 作收尾驗證，同樣全綠。

- **本輪刻意不修的已知限制**（詳細理由見 findings.md「已知限制」段落）：ata.c 整個
  PIO 輪詢期間持續 cli（會漏 tick，但要修需要改成 IRQ-driven，風險大）、
  ramfs_write 每次成長都整檔重配置（O(n²)，但目前檔案規模下無實際影響）、
  task_wake_one 是 LIFO（只影響公平性不影響正確性）、process_send_signal 對多執行緒
  process 的訊號投遞只認主 task（thread 目前設計本來就是共享記憶體 demo 用途）、
  elf_loader 只從 RAMFS 載入執行檔（屬設計範圍而非疏漏）。

## Session 2 — 2026-07-21

- **⚠️ 重大方法論修正（誠實更正 Session 1 的驗證）**：發現 Session 1 用來判讀
  測試結果的指令模式 `wsl.exe -- bash -lc "... make test ...; echo TEST_EXIT=\$?"`
  **恆回報 0**——那個 `\$?` 經過 Git Bash → wsl.exe → bash -lc 多層跳脫後不會
  捕捉到真正的離開碼（用 `false; echo EXIT=\$?` 與一個 `exit 3` 的 make target
  實測都回報 0）。後果：Session 1 加 threadexit 後 RAMFS 節點數 47→48，但斷言
  `RAMFS nodes=47` 沒更新，test-shell 其實在該斷言失敗、make 實際回傳 2，卻被
  誤判為通過。詳見 findings.md 的 M1。**這是 Session 1 一個真實的驗證缺陷。**
  修正：改用「把邏輯寫進 .sh 腳本、在單一 WSL bash 行程內擷取 `$?`」的可靠方式
  （scratchpad/verify.sh），並更新過時斷言，重新確立**真正的**綠燈基準。

- **修 F7（P1，記憶體安全）**：execv 從仍有存活 thread 的行程呼叫時，會釋放
  其他 thread 仍在用的共享 address space（與 F1 同類的 UAF）。修法：
  process_exec_reset 開頭 `if (process->thread_count > 0) return -1;`，在任何
  狀態變更前就拒絕；sys_execv 收到 -1 會銷毀剛建的新 image 並回傳 -1。
  新增 user/execguard.c 迴歸測試（thread parked 在 sem 上時 execv 必須被拒），
  實測 `[execguard exec rejected]` / `[execguard worker ran]` / `[execguard done]`。

- **效能 PERF2**：ramfs_write 改為幾何成長（entry 追蹤 capacity、加倍配置），
  把重複 append 從 O(n²) 降為攤還 O(1)，語意不變。新增 user/ramgrow.c 驗證
  128 次小寫入堆出 2048 bytes 後逐位元組讀回正確（`[ramgrow ok]`），暫存檔在
  結束前刪除以免影響節點數斷言。這原是「已知限制」之一，本輪予以實作。

- **測試 harness 調整**：test-shell 的 QEMU 逾時 210s → 260s（新增的 execguard/
  ramgrow 按鍵把鍵盤注入總時間推過原本已接近上限的 210s，導致 QEMU 在 pmtest
  處被 timeout 砍掉、第二個 mem 沒跑到）；節點數斷言 47 →（48）→ 50（反映
  threadexit + execguard + ramgrow 三支新內嵌程式）；新增對應的
  execguard/ramgrow 輸出斷言。

- **每次修改都以可靠腳本驗證**：最終 `make clean && make -j4` = 0 warning/0 error；
  `make test`（test-ata-absent + test-boot + test-shell）以**真實離開碼 0** 通過，
  且日誌可見 F7、ramgrow、既有全部斷言都命中（非空過）。

## Session 3 — 2026-07-21

- **修 F8（P2，正確性/安全性）**：`vfs_resolve_path` 對過長路徑靜默截斷後仍
  回傳成功，`out` 變成某個祖先目錄 → 呼叫者對錯的物件動作（例如 cwd 接近上限時
  `rmdir("sub")` 會刪到當前目錄）。修法：`path_push` 改回傳 0/-1，
  `path_apply`／`vfs_resolve_path` 一路傳遞，放不下就乾淨失敗（所有呼叫端本來
  就處理 -1）。新增 user/pathlim.c：建 125 字元目錄並 chdir（cwd 126 字元），
  驗證 `stat("x")` 必須被拒（`[pathlim overlong rejected]`），同時驗證正常短路徑
  仍可解析（`[pathlim normal path ok]`，防止修過頭）。

- **修 F9（P3，中繼資料一致性）**：FAT16 叢集耗盡時，`fat16_vfs_write` 仍把長度
  記成請求的結尾而非實際寫入結尾，使檔案宣稱的長度超過叢集鏈能支撐的資料。
  改用 `offset + written`；完全成功時與原本等價，正常路徑不變。

- **FAIR1（排程公平性）**：blocked list 由頭插（LIFO 喚醒）改為尾插（FIFO），
  消除理論上的 starvation。原為「已知限制」之一。

- **修 F10（P1，併發正確性）——由 FAIR1 暴露出的真 bug**：
  `process_send_signal` / `process_request_kill` 想喚醒「特定 task」，卻用
  `task_wake_one(wait_channel)`（喚醒該 channel 上任一 task）。而不相關的 task
  常共用 channel：核心 shell 用 `process_wait(pid)` 阻塞在子行程的 process_t 上，
  該子行程呼叫 `waitpid` 時又阻塞在**同一個** process_t 上。舊的 LIFO 剛好挑到
  目標，**純屬順序巧合**。改 FIFO 後挑到 shell → shell 重新檢查後又睡回去、
  目標永不甦醒 → **整個系統死鎖**（實測 log 停在 `[jc child] finished`，之後
  只剩鍵盤回音）。修法：新增 `task_wake_task(task_t *)` 依身分喚醒，兩處改用它；
  所有阻塞點本就是 `while (條件) block;` 迴圈，偽喚醒無害。修後
  `[jc parent] child reaped` 恢復，測試全過。
  這是本輪最有價值的發現：一個原本被 LIFO 巧合掩蓋的併發缺陷。

- **驗證**：每步都用可靠腳本（單一 WSL bash 行程內擷取 `$?`）。最終
  `make clean && make -j4` = 0 warning / 0 error；`make test` **真實離開碼 0**；
  日誌確認 jobctl、訊號/等待/回收、以及本輪四支新測試標記全部命中。
  節點數斷言隨新增程式更新為 51。

## Session 4 — 2026-07-21

- **修 F11（P1，記憶體安全）**：`dup2` 把檔案節點掛到 stdin/stdout 時**沒有取得
  VFS 參照**（fd 表裡每個描述子都有），`process_redirect`（shell 的 `>`/`<`）
  同樣沒有。而 dup2 的標準慣用法就是「dup2 後立刻 close 原 fd」——ush 的
  `run_command` 正是如此。close 後參照歸零 → `unlink` 被允許 → RAMFS
  `kfree(node)` → 行程的 `stdout_node` 成為懸空指標 → 下一次寫入會從**已釋放
  的堆積記憶體**讀出 `node->write` **函式指標並呼叫它**。這是控制流層級的核心
  use-after-free，可從 shell 觸發（背景程式重導向輸出後 rm 掉該檔）。
  修法（比照既有 pipe 端點的對稱模式）：dup2 與 process_redirect 掛上節點時
  `open_fs()`、替換既有節點時先 `close_fs()`；`process_finish_exit` 在行程結束
  釋放兩個串流的參照使帳目平衡。
  新增 user/redirref.c 迴歸測試：把 stdin 別名到檔案後關閉 fd，驗證此時
  `unlink` 必須被拒（`[redirref inuse unlink refused]`）且仍能經 stdin 讀到內容
  （`[redirref stdin reads file]`）；測試腳本再由 shell 執行 `rm rr.tmp` 並斷言
  **不得**出現 "cannot remove"，**同時證明參照在行程結束時確實被釋放**（沒有
  反向的洩漏）。既有 shell/ush 重導向測試全數不受影響。

- **驗證**：`make clean && make -j4` = 0 warning / 0 error；`make test`
  **真實離開碼 0**（可靠腳本擷取）。節點數斷言更新為 52。

- **已 commit**：分支 `kernel-safety-fixes`，commit 6dfa529，涵蓋 Session 1-4
  的全部修復、5 支新測試程式與文件。（開分支而非直接進 main，方便檢視後合併。）

## Session 5 — 2026-07-21

- **修 F12（P2，資料完整性/資訊洩漏）**：FAT16 是三個檔案系統中**唯一沒有開啟
  計數**的（RAMFS 用 impl 參照位元、DiskFS 用 diskfs_open_refs，兩者的 remove
  都會在檔案開啟時拒絕）。fat16 的節點完全沒接 open/close callback，
  `fat16_vfs_unlink` 因此毫無檢查就把叢集鏈歸還 free pool；之後任何寫入若配置
  到那些叢集，仍持舊描述子的行程會讀到**另一個檔案的內容**——靜默的跨檔案資料
  洩漏／損毀。
  修法：新增 `fat16_node_refs[]` 與 `fat16_vfs_open/close`，接到檔案節點上；
  `fat16_vfs_unlink` 在檔案仍開啟時拒絕（與另兩個檔案系統一致）。
  同時消除 F4 的殘留風險：`fat16_make_node` 需要**新** slot 時改為只挑
  `refs == 0` 者，全滿則回傳 NULL（呼叫端視為找不到），寧可查找失敗也不要靜默
  把開啟中的描述子指向別的檔案。
  新增 user/fatref.c：開啟 /fat/hello.txt 後嘗試 unlink，驗證被拒
  （`[fatref inuse unlink refused]`）、描述子仍讀出正確內容（`[fatref content ok]`）、
  檔案確實還在（`[fatref file intact]`）。測試**刻意不真的刪除**該檔，因此後續
  既有 FAT16 測試全部照常通過（實測 log 確認 hello.txt / docs/note.txt /
  fatwrite / ush 下 cd fat 都正常）。

- **驗證**：clean build 0 warning / 0 error；`make test` **真實離開碼 0**。
  節點數斷言更新為 53。已 commit（962ccde）。

## Session 6 — 2026-07-21

- **FEAT1（功能擴充）**：執行檔查找改走 VFS，**可從任何已掛載的檔案系統執行
  程式**。原本 `elf_load_image` 用 `ramfs_find_file`，只查得到內嵌在 RAMFS 的
  程式。實際上載入器其餘部分早就是檔案系統無關的（一律走 `read_fs()`／
  `node->length`），限制只在查找那一步；改為 `resolve_fs()` 並檢查
  `flags == FS_FILE`。
  **關鍵取捨**：刻意**不**改成 cwd 相對解析——不含前導 '/' 的名稱仍從根解析。
  若改成 cwd 相對，ush 測試裡的 `cd fat` 之後執行 `cat`（位於 /cat）就會失敗。
  驗證：測試腳本新增 `cp hello fat/hello`（8.9KB ELF 複製進 FAT16）→
  `fat/hello`（從 FAT16 載入執行）→ `rm fat/hello`。實測日誌顯示完整三行輸出、
  "Hello from user space!" 次數由 2 增為 3、無任何 `exec:` 錯誤。
  （DiskFS 每檔上限 2048 bytes 放不下 8.9KB ELF，故以 FAT16 驗證；兩者共用
  同一段跨檔案系統載入程式碼。）

- **驗證**：clean build 0 warning / 0 error；`make test` **真實離開碼 0**。
  已 commit（88d44ce）。

## Session 7 — 2026-07-21

- **修 F13（P2，Unix 語意）**：`fork` 沒有繼承標準串流。
  `syscall_copy_user_files` 會複製 fd 3 以上的整張表，但 fd 0/1 的狀態存在
  `process_t` 的獨立欄位（stdout_node/stdin_node/stdout_pipe/stdin_pipe），
  fork 完全沒複製 → 子行程一律退回預設裝置。標準寫法
  `dup2(fd,1); if (fork()==0) write(1,...)` 在 miniOS 上會寫到**終端機**而非
  重導向的檔案。
  修法：process_fork 一併繼承這四個欄位並各自取得參照（檔案 `open_fs`、
  pipe 端點 `pipe_ref_write`/`pipe_ref_read`），由 process_finish_exit 釋放；
  create_task 失敗的清理路徑也一併釋放。**這是建立在 F11 的參照管理之上**
  才得以安全實作。
  新增 user/forkredir.c：main 不重導向（保留報告能力）→ fork child A → A 把
  stdout 指向檔案後再 fork child B → B 在沒有自行重導向的情況下寫入，位元組
  必須落在檔案。實測 `[forkredir inherited]`，且斷言 `^childout$` **不得**出現
  在終端機日誌（證明走的是繼承的重導向），最後刪除暫存檔——若參照洩漏則
  unlink 會失敗、檔案殘留，會被 RAMFS 節點數斷言抓到。

- **驗證**：clean build 0 warning / 0 error；`make test` **真實離開碼 0**；
  管線與重導向（redirected / piped via dup2 / redirok / `1 3 6`）不受影響。
  節點數斷言 54，README 程式數 46。
