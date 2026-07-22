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

## Session 8 — 2026-07-21

- **修 F14（P0，安全性）——本輪最重要，也是對前幾輪的自我更正**：
  `SYS_SIGRETURN` 在解參照使用者 ESP 前**完全沒有驗證**。而且它是一般系統呼叫，
  任何程式都能直接 `int $0x80`（eax=24）觸發，**根本不需要身處訊號處理常式中**。
  攻擊只要把 ESP 設成未映射位址再 int（從 ring 3 進中斷時 CPU 會透過 TSS 切到
  核心堆疊，錯誤的 ESP 不妨礙 int 本身），核心就會在 **ring 0** 讀取該位址 →
  page fault → 被判定為核心錯誤 → **整台機器停機**。
  **這與 F2 是同一類問題**：F2 修的是 signal_deliver 建立訊號框的「寫入」側，
  我當時沒有同步檢查對稱的「讀取」側，是那一輪審查的疏漏。
  修法：解參照前驗證整個 sigcontext 框落在 [USER_STACK_BOTTOM, USER_STACK_TOP)，
  不合法就 task_exit(-1) 只終止該行程。已驗算合法 trampoline 路徑必定通過。
  新增 user/sigretguard.c **實際執行該攻擊**：實測 `[sigretguard arming]` →
  `[program exited]`（只有該行程死亡）→ 緊接著 `> ls /proc` 系統繼續運作。
  修復前日誌會在此直接停住，因此這個測試對回歸鑑別力極強。

- **修 F15（P3）**：`sys_sbrk` 的 `-increment` 在 increment == INT32_MIN 時是
  有號溢位 UB（值直接來自使用者暫存器）。改用 `0u - (uint32_t)increment`。
  原本靠 gcc 產生 neg 指令回繞而「碰巧正確」，現在是語言保證正確。

- **修 F16（P3，使用者空間）**：`umalloc_morecore` 的 `(int)(nu * sizeof(Header))`
  在配置量約 2GB 以上時會溢位成負值，被 sbrk 解讀為**縮小**堆積並回傳看似合法
  的指標。轉型前先擋掉。

- **驗證**：clean build 0 warning / 0 error；`make test` **真實離開碼 0**。
  另確認動到 umalloc.h 後 malloctest/tail/sort（`[malloc test passed]`、`1 12 62`、
  `alpha`/`charlie`）與 sbrk 相關路徑（`[demand paging ok]`）全數不受影響。
  節點數斷言 55、README 程式數 47。

## Session 9 — 2026-07-21

- **修 F17（P2）**：kill 一個多執行緒行程只殺得掉「當下正在執行的那個 task」。
  `process_check_kill` 殺掉當前 task 後**立刻**清除 `kill_request_pid`，於是其餘
  thread 繼續存活、請求已消失，行程永遠停在 PROCESS_RUNNING，任何 wait 它的人
  也永遠阻塞。
  修法是一行移除：不在此清除請求，交給函式開頭既有的 stale 檢查在最後一個 task
  結束、狀態轉為 ZOMBIE 後自然清掉；其餘 thread 各自成為 current 時陸續被殺。
  pid 單調遞增，請求殘留一個 tick 不會誤殺別的行程。
  **殘留限制（已在程式碼註解與 findings 誠實記錄）**：長期停在
  `while (cond) task_block_current(ch);` 的 thread 不會成為 current，仍殺不到；
  要處理需要「可中斷睡眠」，會動到所有阻塞呼叫點，屬較大架構改動，本輪不做。
  新增 user/killthread.c：worker 用忙碌迴圈維持可排程（確保計時器中斷必定在它
  是 current 時發生），main 再 `sys_kill(getpid(), SIGKILL)`。以背景 `&` 執行，
  失敗時不會有人阻塞等待、不會變成測試 hang。驗證很強：worker 若存活，行程會
  維持 RUNNING，結尾既有的 `Processes: running=0` 斷言就會失敗。實測
  `[killthread armed]` 出現、`SURVIVED` 不存在、結尾
  `running=0 / blocked=0 / sleeping=0`，證明兩個 task 都被終止。

- **驗證**：clean build 0 warning / 0 error；`make test` **真實離開碼 0**。
  節點數斷言 56、README 程式數 48。

## Session 10 — 2026-07-21（方向轉為「建立量測與驗證能力」）

前一輪我對「這個 OS 是否已經完善」給了否定的評估，並指出**我自己工作的弱點**：
效能改動從未量測、ISO 開機路徑從未驗證、沒有任何單元測試、且第 8 輪才找到一個
第 2 輪就該發現的 P0。本輪依此排序處理最根本的兩項缺口。

- **CAP1：補上 GRUB/ISO 開機路徑測試**。先前三個測試目標全部走 QEMU `-kernel`，
  完全繞過 GRUB——README 主打的 ISO 開機路徑**一次都沒被驗證過**。新增
  `test-iso`：建 ISO、經 GRUB 開機，並同時掛 ATA 磁碟。斷言包含
  `Initialized PMM from Multiboot memory map.`（證明 GRUB 真的傳入合法的
  multiboot 資訊結構，這正是兩條路徑的實質差異）與三個檔案系統掛載。
  ISO 工具鏈屬選用相依，缺少時印 SKIP 並通過，不讓 `make test` 硬性要求。
  實測：ISO <1s 建好、8 秒內開到 shell、DiskFS/FAT16/procfs 全掛載成功。

- **CAP2：建立原生單元測試框架**（tests/）。純邏輯模組以 `-m32` 編給 host 直接
  呼叫，4 個套件約 **50,500 個檢查、<1 秒**跑完。涵蓋刻意對準「端對端測不到、
  且我改過」的地方：memcpy/memset 的每一種對齊組合與長度（含越界守衛）、
  vfs_resolve_path 的長度邊界（F8）、pmm 的 frame 0 與錯誤 free、heap 的
  分割/重用/合併。`make test` 現在先跑 unit 再跑 QEMU。

- **關鍵做法：用突變測試驗證新測試真的有牙齒**。新測試第一次全綠很可疑，所以
  故意注入 5 個 bug 驗證，**4 個被抓到**。第 5 個沒被抓到反而更有價值——追查後
  發現一個真 bug：

- **修 F18（P3）——由突變測試間接發現**：`pmm_init_region` 的迴圈把 frame 0 標為
  free 並 `used_blocks--`，之後 `mmap_set(0)` 重新保留卻**沒把計數加回去**，
  導致回報的可用區塊比實際多一個。實測回報 256、實際只配置得出 255。
  **兩輪人工審查都漏掉了它。** 實際核心不會踩到（起點被夾在 free_start 之上，
  frame 0 從不落在釋放區間內），屬潛在缺陷，但是公開 API 的正確性問題。
  修法：只在原本為 free 時才重新保留並補回計數。新增
  `test_free_count_is_honest` 以「配置到失敗的實際數量」對照回報值，
  修復前失敗（255/256）、修復後通過。

- **附帶發現（未修，僅記錄）**：本專案從 Git-for-Windows 提交時做了 CRLF 轉換，
  而 WSL 內的 git `core.autocrlf` 未設定，因此同一份工作目錄從 WSL 看整棵樹都是
  「已修改」。不影響建置（gcc 吃 CRLF 沒問題），但會讓跨環境的 git 操作誤判。
  要修需加 .gitattributes 並統一行尾，那會產生涵蓋全樹的巨大 diff，故不在本輪
  順手處理。

- **驗證**：clean build 0 warning / 0 error；`make test`（現含 unit + test-iso）
  **真實離開碼 0**，且已確認 unit 階段與 ISO 階段都實際執行（非 SKIP）。

## Session 11 — 2026-07-21

- **CAP3：FAT16 單元測試**（承接上一輪建立的框架，補覆蓋最薄的區域）。
  FAT16 整個檔案系統就是一個位元組陣列，最適合原生測試；而端對端測試只碰得到
  三個各自塞得進單一叢集的小檔案，**叢集鏈的延伸與走訪幾乎沒被涵蓋**。
  測試直接連結核心實際內嵌的同一份映像（fat16_image_embed.c），不是手工近似品；
  每個測試重新掛載乾淨映像，彼此不污染。37,351 個檢查。
  涵蓋：掛載/根目錄列舉（8.3 轉小寫、"." ".." 過濾）、巢狀目錄、跨叢集
  3000-byte 寫入讀回、**刻意選在叢集邊界上下的偏移讀取**（1/511/512/513/
  1023/1024/2047）、部分覆寫不波及鄰近、建立/刪除/叢集回收、開啟中不得 unlink、
  8.3 名稱長度限制。

- **關閉 F9 的驗證缺口**：F9（叢集耗盡時長度記成請求值而非實際寫入值）當初明確
  記錄為「無法測試」——在 QEMU 裡灌爆映像會破壞同次開機後續所有 FAT 斷言。
  單元測試每次重新掛載，因此能安全寫入 40000 bytes 撐爆磁碟區，驗證
  `written < 請求量` 且 `node->length == written`，並完整讀回比對。
  findings.md 中該項的「驗證缺口」註記已更新為已關閉。

- **突變測試（延續上一輪的作法）**：對 FAT16 套件注入 5 個 bug，**全部 5 個
  被抓到**——含 F9 與 F12 的回歸（長度改回 `offset + size`、移除 unlink 的開啟
  檢查）、讀取時叢集鏈多走一格、8.3 長度放寬、readdir 不再過濾點目錄項。

- **驗證**：clean build 0 warning / 0 error；`make test`（unit + ISO + QEMU 全部）
  **真實離開碼 0**。

## Session 12 — 2026-07-21

- **CAP4：DiskFS 單元測試**（943 檢查）。DiskFS 是唯一**解析自己沒有產生的資料**
  的檔案系統——superblock 與目錄項都從磁碟讀入，內容可以是任何東西，因此
  `diskfs_mount()` 是一條信任邊界。它大部分的驗證從 shell 完全構不到（沒辦法叫
  執行中的核心掛載一顆刻意損壞的磁碟）。用 RAM 陣列 stub 掉 ATA 四個函式後，
  就能先建立合法檔案系統、再竄改磁碟位元組、驗證掛載必須拒絕。
  涵蓋 16 種拒絕案例（magic/版本/checksum/磁區數不符/空白 superblock；
  自我父目錄、**父鏈成環**、檔案當父目錄、父索引越界、重名、長度超限、
  目錄有長度、未知型別、used 非 1、名稱未終止、名稱為空），
  以及功能面的跨磁區寫入、空洞補零、大小上限、巢狀目錄、開啟中不得移除。

- **突變測試逼出兩個測試設計缺陷（都已修正）**：
  1. 原本的 magic 測試其實在測 checksum——竄改 magic 會連帶讓 checksum 失效。
     修正：測試中重算並修好 checksum，讓每個欄位檢查被**獨立**驗證。
  2. 原本的補零測試打不到目標——對全新檔案寫入時該路徑本來就整片清零。
     改成模擬真實資訊洩漏情境（slot 重用、磁區部分存活後在中間留空洞）。

- **一項關於程式碼本身的觀察（誠實記錄，不改）**：`diskfs_write_slot` 的兩段清零
  （明確補零 vs `else` 分支的整片 memset）**互為備援**，單獨停用任一段都無法被
  黑箱測試觀察到；**同時**停用兩者才會被抓到（已實測）。這是刻意的防禦性冗餘，
  不是死碼；但誠實記錄「沒有測試能個別覆蓋這兩行」。測試守住的是**性質**
  （新檔案絕不得讀到前一個檔案的資料），這才是該守住的東西。

- **一項我的假設錯、程式碼對的案例**：我原本斷言裝置消失後
  `diskfs_get_root_node()` 應回傳 NULL。實測失敗後查明：`diskfs_mount()` 在
  「無裝置」或「有開啟檔案」時提前返回、刻意保留既有掛載——這是正確且**被依賴
  的**行為（`test_open_blocks_removal` 靠它才能在拒絕重掛後繼續使用檔案系統；
  若一律 unmount，開啟中的節點會懸空）。已改為斷言真正的契約。

- **驗證**：clean build 0 warning / 0 error；`make test` **真實離開碼 0**。
  單元測試現為 6 套件（utils / fs-path / pmm / heap / fat16 / diskfs）。

## Session 13 — 2026-07-22（效能量測：把未量測的宣稱補上證據）

我先前的自我檢討列了「效能改動從未量測」為一項弱點。本輪用兩種互補方法收尾，
並新增 `make bench`（資訊性，**不**納入 `make test`，因計時有雜訊）。

- **memcpy/memset（計時式，tests/bench_mem.c）**：連結真正的 utils.c（受測方即
  核心出貨程式碼），對逐位元組基準線計時。數值為 host x86-64/gcc -O1 的相對
  數據（核心是 32-bit -O2 -ffreestanding），故**看比值不看絕對值**；三次執行
  比值穩定。
  - memset：對齊的頁/磁區/64B 約 3.4–5.2x，未對齊起點仍加速（只需目的對齊）。
  - memcpy：對齊的頁(4KB)/磁區(512B) 約 3.0–4.7x——正是核心熱路徑。
  - **誠實補述（當初未講清楚的限制）**：memcpy 只有在來源與目的**互相對齊**時
    才加速；相對未對齊時退回逐位元組、加速比約 1.0x（毫無改善）。

- **RAMFS 幾何成長（計數式，tests/bench_ramfs.c）**：計數而非計時，數值精確且
  與 host/編譯器無關，是此改善的**決定性**證據。以 N 次 K-byte append 堆檔：
  重新配置由 N 降為約 log2（N=512：512→8），成長複製位元組由 O(N²) 降為
  O(最終大小)（N=512 少 258 倍、N=1024 少 516 倍）。舊版以精確封閉式並列。

- **量測結果存成可提交的 tests/BENCHMARKS.md**，數據不再只是曇花一現的終端輸出。
  findings.md 的 PERF1/PERF2 也更新為「已量測」並補上誠實限制。

- **驗證**：clean build 0 warning / 0 error；`make test` **真實離開碼 0**
  （bench 不在 test 內，但 Makefile 改動不影響既有目標）。

至此，我先前自我檢討列出的四項弱點已處理三項：ISO 路徑（Session 10）、單元測試
（Session 10–12）、效能量測（本輪）。第四項「第 8 輪才找到第 2 輪該發現的 P0」
是過程性觀察，已透過建立單元測試 + 突變測試的紀律性做法降低未來重演的機率。

## Session 14 — 2026-07-22

- **CAP5：IPC 單元測試（pipe / sem）**。pipe 是阻塞式環狀緩衝，環狀 wrap 與阻塞
  轉換從 shell 都很難測。
  - **前置障礙**：pipe.c/sem.c 的 save_irq_disable 含 cli/sti，host ring 3 執行
    會 SIGSEGV（已實測）。用核心正式建置**永不定義**的 HOSTED_TEST 巨集把兩個
    特權指令在測試建置編成 no-op；核心 codegen 不變（flags=0 被 write-only 輸出
    消除）。這是本輪唯一動到核心原始碼之處。
  - **測阻塞邏輯的手法**：腳本化 hook 取代 task_block_current，阻塞時精確模擬對端
    task（寫入/關閉/排空），讓退出條件在單執行緒上確定性走到；無 hook 的非預期
    阻塞被當失敗並強制中止（不 hang）。
  - pipe 涵蓋 wrap（8 輪 3000-byte 強制多次跨 4096 邊界逐位元組比對）、EOF、
    broken pipe、參照計數與兩端關才 kfree、三種阻塞轉換。68 檢查。
  - sem 涵蓋 id 驗證、未初始化拒絕、計數增減、阻塞於 0→post 釋放、re-init。32 檢查。
- **突變測試**：pipe 6 個注入 → 5 抓到、1 個我自標 benign 者 PASS（stub 排程器下
  不可觀察且不影響正確性，誠實預期）；sem 4 個注入 → 全 4 抓到。
- **附帶記錄（技術債，不改）**：save_irq_disable/restore_irq 這對相同函式在 7 個
  檔案重複；抽成共用 irq.h 是合理去重機會，但會動到多個未受測檔案、風險較高，
  本輪維持最小改動。
- **驗證**：clean build 0 warning / 0 error；`make test` **真實離開碼 0**。
  單元測試現為 8 套件（utils/fs-path/pmm/heap/fat16/diskfs/pipe/sem），
  約 88,900 檢查、<1 秒。

## Session 15 — 2026-07-22

- **REFACTOR1：把重複 7 次的 irq save/restore 抽成共用 irq.h**。這對完全相同的
  `save_irq_disable`/`restore_irq` 原本 copy-paste 在 timer/task/pipe/sem/
  process/ata/kb 七個檔案。新增 irq.h 以**相同函式名** static inline 定義，
  各檔刪除本地定義、`#include "irq.h"`——**102 個呼叫點零改動**，並統一帶上
  HOSTED_TEST 守護（核心永不定義）。
- **codegen 等價性：實測證明而非宣稱**。用 `git show HEAD:<file>` 取重構前版本
  分別編出 7 個 .o，與重構後逐一 `cmp`——**全部位元組完全相同**
  （scratchpad/codegen_check.sh）。static vs static inline 在 -O2 同樣 inline，
  flags=0 被 write-only 輸出消除。
- **時機的意義**：這技術債一直存在但先前不敢動；累積 8 個模組單元測試 + 完整
  端對端測試後，重構回歸風險才降到可接受——「先建立驗證能力、再安全重構」。
- **解鎖能力（記錄為後續機會）**：timer/task/process/ata/kb 現在也繼承 HOSTED_TEST
  守護、可原生單元測試。timer 的 tick_reached wrap-around 比較值得測，但需為
  process_*/schedule/task_* 準備 stub，屬另一個聚焦工作，本輪不順手做。
- **驗證**：clean build 0 warning / 0 error；`make test` **真實離開碼 0**；
  7 個模組 .o codegen 位元組相同。

## Session 16 — 2026-07-22

- **CAP6：timer 單元測試**（用上 REFACTOR1 解鎖的能力）。核心目標是 tick_reached
  的 wrap-around 比較 `(int32_t)(current - deadline) >= 0`——naive 無號比較會在
  32-bit tick 計數器繞回時誤觸發，這 bug 要約 497 天 uptime 才現形、shell 永遠
  測不到。
- **手法**：timer_callback 是 static，用核心同樣方式捕捉——stub 的
  register_interrupt_handler 記下 timer_install 註冊的 handler，測試再呼叫它模擬
  中斷；timer_ticks 全域直接設 0xFFFFFFFE 測繞回；schedule/process_* stub 掉。
- **前置：io.h 的 port I/O 也加 HOSTED_TEST 守護**（timer_install 的 outb 是特權
  指令、host 會 fault）。**已實測證明對核心 codegen 中性**——7 個 include io.h 的
  object（ata/idt/isr/kb/rtc/timer/vga）重構前後 cmp 位元組完全相同。順帶解鎖
  ata/kb 未來可測性。
- 涵蓋：install 捕捉、sleep 引數驗證、確切 deadline 喚醒、**繞過 2^32 的 deadline
  正確**、多獨立 deadline 依序喚醒、睡眠表滿載拒絕。50 檢查。
- **突變測試**：6 個注入全部被抓到，關鍵是「tick_reached 改 naive 無號」被
  wrap-around test 抓到。
- **測試健壯性修正**：reset() 排空迴圈靠 sleeping_count 歸零，「callback 不釋放
  slot」突變會讓它無窮迴圈（實測一度 hang、靠 pkill 解開）；已加 64 次上限使該
  突變乾淨失敗而非 hang。
- **驗證**：clean build 0 warning / 0 error；`make test` **真實離開碼 0**；
  io.h 守護 codegen 中性（7 個 .o 位元組相同）。單元測試現為 9 套件
  （utils/fs-path/pmm/heap/fat16/diskfs/pipe/sem/timer）。

## Session 17 — 2026-07-22

- **CAP7：task 排程器單元測試**（51 檢查）。ready 環狀串列與 blocked 串列是純
  指標邏輯；把組語 context switch（switch_task）stub 成 no-op 後即可測。這也讓
  **F10 新增的 task_wake_task（依身分喚醒）第一次獲得直接覆蓋**（先前只被
  job-control 端對端測試間接測到）。
- 手法：stub switch_task/set_kernel_stack/paging_*/pmm(RAM 池)/terminal_writestring；
  設 current_task + 呼叫 task_block_current 把選定 task 放進 blocked list。
- 涵蓋：建環、block 移出 ready 環、**FIFO 喚醒順序**（能區分 LIFO）、task_wake_task
  從串列中段/頭/尾依身分移除、not-blocked 與 NULL 為 no-op、wait channel 選擇性、
  block/wake 的 **state 轉換（BLOCKED↔READY）** 與 wait_channel 清除。
- **突變測試（改用 Python 字面替換）**：注入 5 個全部被抓到（LIFO 頭插、wake_one
  忽略 channel、wake_task 不依身分、wake_task 不 unlink、add_ready_task 不設 READY）。
- **突變測試又補上一個測試缺口**：第一版沒斷言 state 欄位，「不設 TASK_READY」的
  突變一度 PASS；補上 state 斷言後被抓到。喚醒後若仍 BLOCKED，之後對它 block 會
  因守衛提前返回——是真的缺口。
- **工具註記**：多行 pattern 經 bash 傳給 sed/perl 太脆弱（一度全 NOT-APPLIED），
  改用 Python 字面替換 + LF 正規化最可靠；動原始碼的突變流程每次都確認 task.c
  還原乾淨。
- **驗證**：clean build 0 warning / 0 error；`make test` **真實離開碼 0**。
  單元測試現為 10 套件（utils/fs-path/pmm/heap/fat16/diskfs/pipe/sem/timer/task）。
