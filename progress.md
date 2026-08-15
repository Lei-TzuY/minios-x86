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

## Session 18 — 2026-07-22

- **CAP8：rtc 解碼單元測試 + 行為保持的可測性重構**（35 檢查）。RTC 的核心工作
  是把 CMOS bytes 解碼（BCD/binary、12h PM、12→0/12→noon 特例），而 QEMU 永遠
  以固定 binary/24h 啟動，`date` 端對端測試只走一條路徑。
- **重構（行為保持）**：cmos_read 讀真實 port（HOSTED_TEST 下 no-op），無法注入
  值；把純解碼從硬體讀取分離、抽出 rtc_decode()。rtc_read 讀完暫存器後呼叫它。
  行為不變——由 `date` 端對端測試確認（仍輸出兩次 2020-01-01）。
- 測試以 #include "../rtc.c" 取得 static 的 rtc_decode/bcd_to_bin。涵蓋 BCD/binary、
  12h AM（12→0）、12h PM（1→13、12 PM→12 noon 不是 24）、BCD+12h、世紀 +2000。
  **這些正是真實硬體會用、QEMU 從不觸發的路徑**。
- **突變測試**：6 個注入全部被抓到（bcd 乘數、PM 漏 %12、12 AM 不映射 0、世紀
  基底、BCD 小時丟 PM 位、完全不當 BCD）。
- **驗證**：clean build 0 warning / 0 error；`make test` **真實離開碼 0**。
  單元測試現為 11 套件（+rtc）。

## Session 19 — 2026-07-23

- **CAP9：process 環境變數單元測試**（47 檢查）。目標 process_setenv/getenv 與純
  的 env_copy（`max-1` 的 bounded copy 是經典 off-by-one）；shell 的 export/
  printenv 端對端只存一個短變數，長度驗證/ENV_MAX 上限/overwrite/截斷全沒走到。
- **技術關鍵：用 --gc-sections 攻克高耦合模組**。process.c 相依 30+ 外部符號，
  過去被視為太耦合不值得測。但 env 函式的傳遞閉包只到 process_get_current →
  task_get_current，以 #include "../process.c" + -ffunction-sections +
  -Wl,--gc-sections 編譯，連結器丟掉所有沒被 main 觸及的函式（fork/exec/signal）
  與其相依，**stub 面縮到只剩 task_get_current + strlen/strcmp/memcpy**。這開啟
  了測試其他高耦合模組純邏輯部分的路徑。
- 坑：syscall.h 的 SEEK_SET enum 與 stdio.h 的 SEEK_SET 巨集衝突；把 test.h 移到
  process.c 之後 include 即可（enum 先於巨集）。
- 涵蓋：set/get、append、overwrite 不增長且不留殘尾、NULL/空鍵/過長拒絕、最長
  可接受長度、ENV_MAX 滿載拒絕但仍可 overwrite、getenv 截斷（size 4→3、size 1→
  空）並回傳截斷長度。
- **突變測試**：6 個注入全部被抓到（env_copy off-by-one、無 ENV_MAX、無長度驗證、
  空鍵、setenv 不 overwrite、getenv 回 0）。
- **驗證**：clean build 0 warning / 0 error；`make test` **真實離開碼 0**。
  單元測試現為 12 套件（+process-env）。

## Session 20 — 2026-07-23

- **CAP10：syscall 使用者指標驗證單元測試**（36 檢查）。安全前線——每個 raw
  使用者指標都會過 user_buffer_valid；F2/F14 兩個核心 DoS 都是這類檢查缺失。
  安全關鍵的「指標近頂端 + 巨大長度不得因溢位放行」從 shell 幾乎無法觸發。
- 手法（同 CAP9 --gc-sections）：驗證函式只走到 paging_user_range_mapped（stub）。
  user_buffer_valid 不解參照，用假指標完整測；user_string_valid 會解參照，用
  mmap MAP_FIXED_NOREPLACE 在 [0x300000,0x3F0000) 取真實記憶體（放不下則跳過）。
- **突變測試的關鍵教訓**：第一版 6/7 抓到，**漏掉最重要的整數溢位繞過**。追查
  發現我的 paging stub 對 4GB 範圍**獨立**回「未映射」，遮蔽了 bound 檢查的 bug。
  加「強制已映射」模式**隔離**該邏輯後，7 個全抓到。stub 太嚴格會遮蔽受測 bug，
  突變測試把這盲點揪出來。
- **驗證**：clean build 0 warning / 0 error；`make test` **真實離開碼 0**。
  單元測試現為 13 套件（+syscall-valid）。

## Session 21 — 2026-07-29

- **F19（P2，正確性/架構）：停在阻塞等待中的 task 完全殺不到**。這是 Session 9
  修 F17 時**自己誠實記錄下來的殘留限制**，當時判斷「需要可中斷睡眠、屬較大架構
  改動」而不動。本輪回頭把它根治——但**沒有做 EINTR 上拋**（那才是當初評估的大
  改動），而是選了範圍小得多的做法：被殺的 task 醒來後直接離開，不回到等待迴圈。
- **問題的機制**：kill 請求只由計時器中斷中的 `process_check_kill()` 施行，而它
  只看**當前** task。停在 `while (cond) task_block_current(ch);` 的 task 永遠不會
  是當前 task——它只在「被喚醒到再次阻塞」之間跑幾行，而且全程關中斷，不會有
  tick 落在它身上。舊的 `process_request_kill` 雖然會 `task_wake_task(proc->task)`，
  但醒來的 task 條件沒滿足就回去繼續睡，等於白喚醒。順帶暴露另外兩個盲點：
  只喚醒 `proc->task`（多執行緒行程的其他 thread 根本沒碰到）、被 SIGSTOP 停住的
  行程也殺不掉。
- **修法**：`task_t` 加 `kill_pending`；`task_kill_blocked(process)` 標記該行程
  **所有** task（ready 環 + blocked 串列）並喚醒 blocked 的；`task_block_killable()`
  （task.h 的 static inline）＝阻塞後檢查旗標，成立就 `task_exit`。10 個阻塞點
  換掉 9 個。政策（殺誰）留在 process.c，機制（醒來就走）在 task.c，**不需要
  task.c 反向相依 process.c**。
- **timer_sleep 是唯一的例外**，因為它**持有一個 sleep slot**：必須先歸還再離開，
  否則 slot 會帶著一個指向已釋放記憶體的指標卡到原本的到期時間。歸還時要判斷
  slot 還是不是自己的——若已到期，`timer_callback` 早就清了，而且可能已有別的
  sleeper 接手，清錯會害它永遠睡下去。
- **新測試**：
  - 端對端 user/killwait.c：worker thread 卡在沒人 post 的 semaphore，main 卡在
    `sys_thread_join()`，**兩個 task 都在睡**的時候由 fork 出來的子行程發 kill。
    以背景執行，回歸時不會 hang 整個測試，而是在結尾的 `Processes: running=0` /
    `Tasks: blocked=0` 上現形。
  - 單元 tests/test_task.c +21 檢查（51→72）：只喚醒目標行程的 blocked task、
    旗標也落在 ready 的 task 上、其他行程不受影響、回傳計數、NULL no-op、
    `task_kill_pending` 只反映當前 task。
  - 單元 tests/test_timer.c +13 檢查（50→63）：timer_sleep 三條 kill 路徑。
    `task_exit` 用 `longjmp` 模擬「不會回來」的語意，於是可確定性驗證：已被標記
    時**不取 slot** 就離開（並斷言真的沒 park，否則後置檢查會遮蔽前置檢查的
    缺失）、睡著時被標記則離開前歸還 slot、以及期限已到且 slot 被別人接手時
    **不得**清掉對方的 slot。外加一條「沒有 kill 時 sleep 照常返回」的反向測試。
- **突變測試（9 個注入，全部被抓到）**。task.c 六個、timer.c 三個；下列 M1–M4
  是最初的四個，M5–M9 見 Session 23（timer 的三個一開始有兩個逃掉，反過來補強了
  上面那些測試）：
  - M1 `task_kill_pending` 恆回 0（＝修復前的行為）→ 單元 + killwait 端對端都抓到。
    端對端的失敗訊號很具體：結尾 `Processes: running=1 zombies=1`、
    `Tasks: blocked=2`、`User pages: spaces=1`（就是那兩個沒死成的 task），
    修好後全為 0。這排除了「沒印 SURVIVED 就算過」的假綠燈疑慮。
  - M2 blocked 迴圈不設 kill_pending → 單元 2 個檢查失敗。
  - M3 完全跳過 ready 環 → 單元「ready 的 task 也要被標」失敗。
  - M4 忽略 `task->process`（無差別殺）→ 單元 7 個檢查失敗（含「其他行程不受影響」）。
- **誠實記錄、本輪不改**：`timer_sleep` 被**訊號**（非 kill）提早喚醒時會直接回傳 0
  且 slot 佔到原到期時間才被清。這是既有行為，提早返回符合 POSIX `sleep()` 語意，
  slot 滯留有界且會自癒（不會被解參照）；要修得改 test_timer 對「睡著」的建模，
  與本輪主題無關。已寫進 findings.md F19。
- 節點數 57、README 程式數 49、test-shell 逾時 260s→270s（新增指令的時間預算）。

## Session 22 — 2026-07-29

- **CAP11：paging COW 參照計數 + user_pte 單元測試**（31 檢查）。fork/COW 核心：
  user_pte 把 vaddr 映射到正確頁表項（低 0-4MB 表 vs 32-36MB mmap 表，裸移位
  index 邊界易 off-by-one）；page_ref[] 記錄「超出唯一擁有者的額外參照數」，
  release 必須只在最後參照消失時回「該釋放」。錯誤導致難查的記憶體損毀。
- 手法（--gc-sections）：兩者閉包**不需任何外部函式**，零 stub。
- **突變測試 7 個 ＋ 無功能訊號 bug 的解法**：6/7 功能上抓到；漏掉的是
  cow_ref_inc 的 `< FRAME_COUNT` 改 `<=`——純越界寫入 page_ref[FRAME_COUNT]，
  功能斷言看不到。解法：此測試改用 **UBSan 陣列邊界陷阱**
  （-fsanitize=undefined,bounds + trap-on-error，不需 libubsan、-m32 可用），
  把越界變硬陷阱。加上後 7 個全抓到。補足突變測試對「不可觀察記憶體越界」的盲點。
- **驗證**：clean build 0 warning / 0 error；單元測試現為 14 套件（+paging-cow）。
  注意：組合式 `make test` 首跑出現 exit 2，但四個 QEMU 整合測試個別重跑皆 exit 0；
  本輪改動全為 host 端測試檔，未觸及任何 kernel 原始碼，kernel.bin 位元組相同，
  判定為連續 QEMU 測試的時序 flakiness，重跑確認綠燈後提交。

## Session 23 — 2026-07-29

- **開場先清掉一個危險的殘留：工作目錄裡留著一個突變**。`task_kill_pending()`
  當時是 `return 0;`，帶著 `/* MUTANT M1: pre-fix behaviour */` 註解——上一輪跑
  M1 的端對端驗證時被中斷，原始碼沒還原。**這正是 F19 修好的那個 bug 本身**：
  留著的話 kill 對停在等待中的 task 完全無效，而且單元測試會紅。先還原成
  `return task && task->kill_pending;` 才動其他東西。
  **教訓**：改原始碼的突變流程，還原必須是流程的一部分並**當場驗證**，不能等到
  結尾——被中斷時「未還原」是預設結果。本輪的 mutate.py 因此每個突變跑完就寫回
  並 `assert` 檔案與原檔逐位元組相同，最後再跑一次乾淨的 `make unit` 收尾。
- **把 F19 的突變測試從 4 個補到 9 個**（task.c 6 + timer.c 3）。原本那 4 個
  全在 task.c；**timer_sleep 的三條 kill 路徑一條都沒被突變測試驗證過**，而
  timer_sleep 正是唯一手寫、不走 `task_block_killable()` 的例外，最需要驗證。
- **新增的 timer 突變一開始有兩個逃掉，各暴露一個真的測試缺口**：
  - **M7（不歸還 slot）逃掉**：當時 test_timer.c 根本沒有 kill 路徑的測試——
    `task_kill_pending()` 是寫死回 0 的 stub。補上以 `longjmp` 模擬 `task_exit`
    「不會回來」語意的測試後抓到。
  - **M9（拿掉睡前的 kill 檢查）逃掉**：**後置檢查遮蔽了前置檢查**——兩者都以
    「已離開且 slot 已歸還」收場，唯一差別是有沒有先 park。補一個 `g_blocks`
    計數器、斷言已被標記時 park 次數為 0，才把兩者分開。
  - 另補 M8（無條件清 slot）確認「slot 已被別人接手時不得清掉對方的」那條分支：
    需要在 park 當中讓期限到期、再讓另一個 task 接手同一個 slot 才測得到。
  - **測試 harness 自身的 bug 也被這條測試抓出來**：第一版讓 kill 旗標對「接手者」
    的內層 sleep 也生效，結果接手者自己死掉並歸還 slot，斷言因此失敗。改成只在
    接手完成後才標記外層 task 才正確。
- **9/9 全抓到**；tests/test_timer.c 50→63 檢查（不是先前記錄的 61）。
  順帶修正 Session 21 記錄裡這兩個數字。
- **驗證**：`make unit` 14 套件全綠（約 89,180 檢查）；`make test` **真實離開碼 0**
  （用 .sh 包起來跑，避開 wsl.exe 吞離開碼的坑）。killwait 端對端在日誌中確認
  `[killwait parked]`、`[killwait killer done]`，兩條 SURVIVED 都沒出現，結尾
  `Processes: running=0 zombies=0`、`Tasks: blocked=0`——停在等待中的 worker
  thread 與 parent 都真的被殺掉了。

## Session 24 — 2026-07-30

- **主題：ELF 載入器這條信任邊界**。Session 6 的 FEAT1 讓可執行檔改走 VFS 查找
  之後，使用者就能把**任意位元組**寫進 FAT16/DiskFS/RAMFS 的檔案再執行它——
  `e_phoff`/`e_phnum`/`p_offset`/`p_vaddr`/`p_filesz`/`p_memsz` 全部變成攻擊者可控，
  而下游 `paging_map_user_page()` 不做任何範圍檢查。審查後找到兩個問題。
- **F20（P1，記憶體安全）**：`elf_load_image` 取得節點後**完全沒有 `open_fs`**。
  載入會讀很多次（header、每個 program header、每個 segment 分 256 bytes 一段），
  中間反覆被排程；此時別的行程 `rm` 掉該檔，RAMFS 因參照為 0 而放行 `kfree(node)`，
  載入器的 `file` 立刻懸空，下一次 `read_fs` 會**從已釋放記憶體讀出 `node->read`
  函式指標並呼叫它**。與 F11 完全同型的核心 UAF。內嵌程式因為是 static file
  刪不掉，所以這個洞是 FEAT1 之後才打開的。
  **窗口大小要說清楚，不誇大**：從 RAMFS 載入只是 memcpy，幾微秒就結束，遠短於
  一個 10ms tick，實務上撞不到；但**從 FAT16/DiskFS 載入時每個 256-byte chunk 都
  要走 ATA PIO**，載入跨越多個 tick，窗口是毫秒級——而那正是 FEAT1 加進來的能力
  （測試裡就有 `cp hello fat/hello` 再執行它）。修它的理由是「安全性不該建立在
  時序假設上」，不是「很容易被利用」。
  修法：`open_fs`/`close_fs` 包住整個載入；為了讓成對呼叫不會在任何一條失敗路徑
  上漏掉（原本有 6 個 return），把主體拆成 `elf_load_from_node()`，外層單一出口。
- **F21（P2，TOCTOU）**：program header 被讀**兩次**——`validate_segments` 驗一次，
  `load_segments` 重新讀出來才用。兩次之間會被排程，而 F20 的參照只擋 unlink、
  擋不住覆寫。於是「驗證時合法、使用時不合法」的 p_vaddr 會直達不做檢查的
  `paging_map_user_page()`。誠實評估影響：`user_pte` 只對低 4MB 與 mmap 視窗回傳
  頁表項，核心位址映不進去，可達的傷害是讓行程自己的 null page 變可存取——所以
  是 P2 不是 P0。但這是唯一一條防線，不該靠下游巧合。
  修法：抽出 `phdr_in_user_range()` 給兩處共用，`load_segments` 使用前重驗一次。
- **CAP12：tests/test_elf.c**（139 檢查）。第三條「解析不受信任資料」的信任邊界
  測試（前兩條是 CAP4 diskfs mount、CAP10 syscall 指標驗證）。shell 只造得出
  「根本不是 ELF」，造不出格式正確但欄位惡意的映像，所以**所有拒絕路徑在此之前
  從未被執行過**。每個拒絕都檢查兩件事：建立位址空間前被拒的不得建立、建立後
  才失敗的必須銷毀。
- **突變測試 5 個，逼出一個真實的測試缺口**：
  - E1（不取參照）5 個失敗、E1b（取了不放）6 個失敗、E2（移除第二次驗證）2 個
    失敗——三個都抓到，證明 F20/F21 確實被測到。
  - **E3（把界限改成會繞回的寫法）一開始存活**。追查發現：繞回後的上界必然低於
    p_vaddr，entry-point 檢查順手擋掉了單段的溢位案例——**溢位路徑被另一個檢查
    遮蔽**，與 CAP10「stub 太嚴格遮蔽受測 bug」同型。補上「**第二個** segment 用
    繞回的 p_memsz」（entry 已由第一段滿足，沒有那層意外保護）後 E3 被抓到。
  - **E4 存活且經推算確認是等價突變**：要讓 `p_offset + p_filesz` 繞回需要
    p_filesz ≥ 約 0xFFFFFEC0，但短路求值中 `p_filesz <= p_memsz` 與
    `p_memsz <= USER_STACK_BOTTOM - p_vaddr`（上限約 0xE8000）都排在它前面，
    那條路徑到不了。誠實記錄，不硬寫一個假測試。
- **工具註記**：突變腳本第一版全部 NOT-APPLIED——這棵樹在此主機上是 CRLF，用 `\n`
  寫的 pattern 對不上原始位元組。改成「讀入後正規化成 LF、替換、依原行尾寫回」
  才可靠（Session 17 已學過同一課，這次是漏了正規化那一步）。突變流程改為
  「備份→突變→編譯→執行→還原」全在單一 bash 程序內完成。
- **第二組獨立突變（同一個檔案，另一組 12 個注入）**：本輪有兩個 session 各自
  對 elf_loader.c 做了突變測試，用的是**不同的注入集**，因此各自逼出了不同的
  測試缺口——這本身就說明單一組突變不等於「測夠了」。第二組 12/12 全抓到，
  其中兩個一開始漏網，原因是**同一種測試設計錯誤：我的案例被另一條子句先擋掉，
  被測的那條根本沒被隔離**。
  - 「拿掉 `p_filesz <= p_memsz`」逃掉：我的 filesz 同時也超出檔案長度，於是是
    檔案界限那條先拒絕。把映像加大、讓那些 bytes **真的存在於檔案中**，才隔離
    出這條子句。
  - 「拿掉 e_phnum 上界」逃掉：`read_exact` 本來就會逐個拒絕出界的 header，所以
    回傳值看不出差別——這是刻意的縱深防禦。改為斷言它真正買到的東西：離譜的
    e_phnum 必須**在讀取任何一個 program header 之前**就被擋掉
    （`g_phdr_reads == 0`），而不是驅動 65535 次由攻擊者指定的讀取嘗試。
- **並行編輯的危害（本輪最重要的過程教訓）**：兩個 session 同時在改同一棵樹，
  而**會改原始碼的突變腳本，其「還原」會覆蓋掉另一方的並行編輯**。實際後果：
  第一輪突變結果完全不可信——有兩個注入因為當下檔案裡還沒有那段程式碼而
  NOT-APPLIED，收尾的 `make unit` 也一度 exit 2（其實是撞上對方寫入的瞬間，
  並非真的壞掉）。修正：腳本改為在開始時固定 baseline，每輪開始前比對檔案是否
  仍等於 baseline，**不同就中止而不是覆蓋**，並把該輪標為不可信。之後重跑才得到
  可信的 12/12。結論：任何會寫入原始碼的自動化，都必須假設檔案可能同時被別人動過。

## Session 25 — 2026-07-30

- **主題：RAMFS**。選它的理由不是「還沒測」，而是**它的開啟計數正是 F11 與 F20
  兩個已修 P1 記憶體安全問題所依賴的基礎機制**（「開啟中不得 unlink」），而那個
  性質先前只被端對端間接測到。審查時找到一個比預期嚴重得多的問題。
- **F22（P0，安全性/DoS）：使用者程式可讓整台機器凍結**。PERF2 的幾何成長：
  ```c
  while (new_cap < new_length) {
      if (new_cap > 0x80000000U) { new_cap = new_length; break; }
      new_cap *= 2;
  }
  ```
  `new_cap == 0x80000000` 時 `> 0x80000000` **不成立** → `*= 2` 截斷成 **0** →
  之後永遠「0 < new_length、0 不 > 0x80000000、0*2 還是 0」→ **無窮迴圈**。
  守衛必須在乘法**之前**擋，改成 `new_cap > 0xFFFFFFFFU / 2`。
- **為什麼是 P0**：`sys_seek` 允許 offset 到 0x7FFFFFFF，再寫 2 bytes 就得到
  `new_length == 0x80000001`，剛好跨過倍增上限。而 `int $0x80` 的 IDT 閘門是
  `0xEE`——**type nibble E 是 interrupt gate，CPU 進入時自動清 IF**，所以系統呼叫
  全程關中斷。這個迴圈於是在關中斷下永遠轉：沒有 timer tick、沒有排程、鍵盤無反應，
  **整台機器停機**。與 F2/F14 同級，但成因完全不同：那兩個是未驗證指標，這個是
  純算術的 off-by-one。
- **為什麼前 24 輪沒抓到（值得記下的教訓）**：PERF2 是 Session 2 加的，Session 13
  用 `make bench` 量過它的**效能**（重新配置 N→log2(N)），但沒有人測過它的**算術
  邊界**。「量過效能」被我當成「驗證過正確性」——這是兩件事。
- **CAP13：tests/test_ramfs.c**（4300 檢查）。兩個測試設計上的關鍵決定：
  - **看門狗 `alarm(30)`**：F22 是無窮迴圈，沒有看門狗的話回歸會把 `make test`
    掛住而不是讓它失敗。R1 突變實測「看門狗開火 → FAIL」。
  - **配置器毒化**：stub 的 kmalloc 把新區塊填 0xAA。真實 kmalloc 回收的是還帶著
    前一個檔案位元組的區塊，**清零是檔案系統的責任**；stub 回傳乾淨記憶體會讓
    稀疏檔案的空洞「因為 malloc 剛好給了零」而讀到 0，缺失的清零就被遮蔽。
    這是 CAP10「stub 太嚴格遮蔽 bug」的反面版本。
- **端對端 user/bigseek.c**：在 QEMU 裡**實際執行這個攻擊**（seek 到 0x7FFFFFFF
  再寫）。`[bigseek arming]` → `[bigseek refused]` → `[bigseek survived]`，能跑到
  最後一行就證明核心還活著、還在排程它。修復前 QEMU 會直接凍結、整個 test-shell
  死在 timeout 上——鑑別力極強。
- **突變測試 7 個，6 個抓到**：R1（看門狗開火）、R2（移除 refs 檢查，5 失敗）、
  R3（open 用 `|=` 不計數，2 失敗）、R4（close 無下溢保護，3 失敗）、
  R5（PERF2 回退，3 失敗）、R7（移除成長清零，2 失敗）。
  - **R7 一開始存活，逼出真實缺口**：原本的稀疏測試把空洞放在既有容量內
    （41 < 初始 64），只走「重用」分支，於是兩段清零互為備援。補上「稀疏寫入同時
    跨越容量」的案例（空洞由重新配置本身產生）後才抓到。
  - **R6 仍存活且確認是冗餘防禦**：程式碼註解本來就說那段 memset 是
    「already zero by the invariant, but make it explicit」。誠實記錄，不硬寫測試。
- **兩個我的假設錯、程式碼對的案例**（已修測試）：OOM 測試原本沒讓寫入跨越**容量**
  （只跨長度，走重用分支根本不配置，所以本來就該成功）；節點表測試原本斷言絕對
  節點數，但 node_count 是 file-static 且前面測試刻意留檔。
- **突變腳本強化**（依上一輪的教訓）：釘住基準、每輪比對，若與基準不同就**中止而非
  覆寫**（防並行編輯被蓋掉）；還原放在 trap 裡並驗證位元組相同。
- 節點數 58、README 程式數 50、test-shell 逾時 270s→280s。
- **第一次完整驗證失敗（EXIT=2），誠實記錄根因是我自己的測試程式寫錯**：
  `RAMFS nodes=59` 而斷言 58。追查發現 **`sys_create` 本身就回傳一個已開啟的 fd**
  （syscall.c 的 `open_user_file(node)`），而 bigseek 又多呼叫了一次 `sys_open`，
  於是持有兩個參照卻只 close 一個；結尾的 `unlink` 被**正確拒絕**，`bs.tmp` 留在
  RAMFS 裡（完整 log 的 `ls` 輸出可見）。核心沒錯——**這反而證明 F11/F20 依賴的
  「開啟中不得 unlink」機制正在運作**。修法：直接用 `sys_create` 的 fd，並且
  **斷言 unlink 成功**（`[bigseek] cleanup failed` 會被既有的
  `! grep -q "\[bigseek\] "` 抓到），避免將來又靜默地讓節點數飄移。
- **F22 修復在 QEMU 中的實證**（第一次執行的完整 log 就已確認，與節點數斷言無關）：
  `[bigseek arming]` → `[bigseek refused]` → `[bigseek survived]` 三行依序出現，
  攻擊被實際執行、寫入乾淨失敗、系統存活並繼續排程後續指令。

## Session 26 — 2026-08-13

- **主題：VFS 核心（fs.c）**。選它的理由來自 PROJECT_STATE 第 6 節的「投報率可能
  最高」：每一個帶路徑的系統呼叫都經過這裡的兩個解析器，而在此之前只有*正規化器*
  `vfs_resolve_path()` 有測試（34 檢查，全專案最薄），**消費它輸出的兩個嚴格解析器
  一個測試都沒有**。F8 就住在這裡。
- **CAP14：tests/test_fs.c**（277 檢查）＋ tests/test_fs_path.c 加上輸出緩衝區
  金絲雀。用自建的 mock 檔案系統，不連結任何真實後端——連 ramfs 會變成主要在測
  ramfs 自己的解析器，而且**後端太嚴格就分不出是哪一層擋下來的**（CAP10 的教訓）。
  mock 刻意寬鬆，樹裡還放進「名字就叫 `.`、`..`、空字串」的節點：fs.c 若真把這種
  組件傳下去，mock 會回答成功、測試就會失敗。
- **HARD1（強化）：`resolve_fs` 補上「中途的組件必須是目錄」的檢查**，與姊妹函式
  `resolve_parent_fs` 一致。**誠實說：目前不可觸發**（所有後端的檔案節點 finddir
  都是 NULL），這不是修掉的 bug 而是縱深防禦。值得補的理由是原本同一個路徑在兩個
  進入點意義不同（`resolve_fs("/odd/deep")` 找得到、`unlink_fs("/odd/deep")` 被拒），
  而 procfs 的 `proc_finddir` 正是那種完全忽略自己是誰的後端（`(void)node`）。
- **F23（P3，正確性）：FAT16 的寫入即使一個位元組都沒寫進去，也照樣把長度推到
  seek 的位置**。F9 的修法記錄 `offset + written` 為新的檔尾，並在註解裡論證
  「寫入的位元組從 offset 起連續」——這個等式**只有 written > 0 時成立**。磁區寫滿
  之後對既有檔案 seek 過尾端再寫，`written == 0`，於是檔案憑空長到 seek 的位置卻
  一個位元組都沒存，宣稱一段自己的叢集鏈背不起來的長度。修法是 `if (written > 0)`
  包住長度更新。
  - **不誇大**：讀取多出來的那段會拿到最後一個真實叢集在 EOF 之後的內容，但那些
    位元組在配置時已被 `fat16_alloc_cluster` 清零，**不會洩漏別的檔案的資料**。
    是正確性問題，不是資訊洩漏。
  - **為什麼 F9 那輪沒抓到**：F9 的測試測的是 `0 < written < size` 的部分成功，
    `written == 0` 從來沒被測過。修 bug 時只測自己當時想到的那條路徑，隔壁那條
    就會留下來。
  - 端對端 **user/fatgrow.c**：從 ring 3 把 /fat 寫滿、seek 4096 再寫、驗證長度與
    內容都沒變，最後 unlink 兩個檔案把磁區還給後面的 FAT 斷言。
- **突變測試 22 個（fs.c），最終 22/22 全抓到**——但**一開始 4 個存活，逼出 3 個
  真實缺口**，這是本輪最有價值的部分：
  - **M6（接受空組件）存活**：mock 裡沒有名字是空字串的節點，查找失敗只是「剛好
    沒符合的」。加進名為 `""` 的目錄並改用 `//x`（唯一能讓空組件後面還有路徑可走
    的拼法）後才抓到。
  - **M9（父節點不檢查是不是目錄）存活**：我的案例 `/a/f/x` 裡 `f` 是個指標全 NULL
    的檔案，**是 `!parent->create` 先擋下來的**，被測的檢查根本沒被隔離。加進
    「FS_FILE 卻帶著完整目錄操作」的 mock 節點後才有鑑別力。與 CAP12 的 E3 同型。
  - **M12（path_push 界限放寬一個位元組）存活**：函式尾端還有第二道長度檢查，
    **回傳值仍是 -1**——但那一個位元組已經寫出去了。兩道檢查互為備援對核心是好事、
    對測試是壞事。解法是在輸出緩衝區後面放金絲雀：`out` 只有 FS_MAX_PATH 個位元組
    是真正的契約，這不是為了殺突變而寫的假測試。
  - **M10 一度被我判為等價突變，重新推導後發現不是**：回傳值確實完全相同（漏掉的
    情形一定會被 parse_component 的空組件檢查攔下），但**副作用不同**——突變版會
    多做一次 finddir 才失敗。改成斷言「畸形路徑在哪裡停止走訪」後被抓到。這個性質
    本身有意義：在磁碟後端上一次查找就是一次關中斷的 ATA PIO。
- **fat16.c 突變 3 個，3/3 抓到，且各由不同測試抓到**：N1（修復前的程式碼）**只被
  新測試殺掉**、N2（退回 F9 的 bug）被舊的 out-of-space 測試殺掉、N3（拿掉「只增
  不減」）被 partial overwrite 殺掉——證明新測試補上的是先前確實沒有的鑑別力。
- **一個我算錯、程式碼是對的案例**：我以為 `parse_component` 會擋掉 127 字元的
  組件，實際上它允許到 127（正好塞滿 `fs_node_t::name`）。真正的邊界是絕對路徑
  最多承載 126 字元的組件、**相對路徑可以到 127**——而 exec 解析程式名走的正是
  相對路徑。測試改成釘住這個真實邊界，並誠實記錄 parse_component 自己那道長度
  守衛因為路徑長度檢查永遠先發而**不可觸發**。
- 節點數 58→59、README 程式數 50→51、test-shell 逾時 280s→285s。

## Session 27 — 2026-08-13

- **主題：kb.c（鍵盤驅動）**。接續 PROJECT_STATE 第 6 節 A 類。選它的理由不是
  「還沒測」，而是**它是核心裡唯一一處中斷處理常式與 task 同時碰同一個資料結構的
  地方**（環狀緩衝），而 QEMU 那套只走最窄的一條路：短指令、立刻被消費，緩衝區
  永遠不會滿、索引永遠不會繞回、沒有任何按鍵被丟掉。
- **CAP15：tests/test_kb.c**（1675 檢查）。用 `#define IO_H` 把 io.h 整個換掉自備
  `inb`（HOSTED_TEST 下的 `inb` 永遠回 0，等於讓一個靠埠讀取input 的驅動無法測），
  **完全不動核心標頭**，所以沒有任何 codegen 風險；`task_exit` 的 noreturn 用
  `setjmp`/`longjmp` 接住，才真的測得到 F19 那條「被 kill 的 task 從等待迴圈離開」。
- **誠實結論：kb.c 沒有找到 bug**。這一輪的產出是把一個沒人檢查過的模組變成有 18 個
  突變證明過的測試在守，不是修好了什麼。
- **突變測試 18 個，18/18 抓到**，其中 3 個是**以逾時被抓到**的——未繞回的索引、
  少了 count 檢查的傳輸迴圈，這些突變的症狀是**掛住**而不是答錯。突變腳本因此對
  每次執行都套 `timeout 20s`，讓「掛住」被記成 KILLED 而不是把腳本卡死（沿用
  CAP13 看門狗的想法，只是移到腳本層）。
  - **K17 一開始存活，逼出真實缺口**：拿掉 `count == 0` 檢查之後測試還是綠的，因為
    我那個案例**緩衝區裡剛好已經有字元**，傳輸迴圈自己的 `bytes_read < count` 讓
    兩邊都回 0。改成**對空緩衝區**呼叫才看得出來：正確的程式立刻回 0，突變版會為了
    一個它根本不會消費的字元把呼叫者永遠停住。**這是本專案第三次踩到同一個形狀**
    ——我的案例被另一個條件先滿足，被測的那條沒被隔離。
- **把一個「意外而非設計」的行為轉成有推導的測試**：驅動完全不處理 0xE0 延伸掃描碼
  前綴（0xE0 的 bit 7 是 1，掉進「放開」分支被丟掉，後面那個位元組就當成沒有前綴）。
  逐一驗過每個後果都良性：右 Ctrl 因為與左 Ctrl 同索引而照樣能用、方向鍵對應到 0 而
  被安靜丟掉、小鍵盤 Enter 與 `/` 剛好落在等價鍵上。把它寫成測試並註明推導，另加
  突變 K18（就是那個看起來很合理的「把 0xE0 處理加上去」的改法）證明右 Ctrl 會立刻
  壞掉——實測被抓到。
- Makefile 的 UNIT_BINS/規則、`.gitignore` 一併更新（上一輪剛加進 CLAUDE.md 的
  三點接線清單，這輪第一次照著用）。

## Session 28 — 2026-08-13

- **主題：procfs.c**。接續 PROJECT_STATE 第 6 節 A 類裡我自己標為「投報率最高」的
  那一項。理由很具體：**F3 是這裡的緩衝區溢位，而它的修法（/proc/processes 的界限
  檢查）從來沒有執行過一次**——那個檢查只在 pid 到十位數或行程名塞滿欄位時才發火，
  兩者在 QEMU 那種「開機、打字、結束」的執行裡都不會發生。一個唯一的守衛從沒跑過的
  修復，等於沒有人測過。
- **CAP16：tests/test_procfs.c**（183 檢查）。緩衝區不變式用結構化方式檢查：每次
  產生前把 `gen_buf` 灌毒 0x7F，產生後**從回報長度到結尾都必須還是毒**——這證明
  「剛好寫了宣稱的那些位元組、一個都不多」，只斷言回傳值會漏掉寫過頭的情況。
  外加 `-fsanitize=bounds`（trap 模式）當第二道獨立的網。
- **誠實結論：procfs.c 沒有找到 bug**。但這輪產出了兩件實質的東西：F3 的守衛第一次
  被實際執行並釘住了精確邊界；以及**兩個完全沒有界限檢查的產生器**
  （/proc/self/name、/proc/self/status）的最壞情況被算出來、寫成測試守住了
  （status 最壞 75 bytes vs 512）——它們今天安全純粹是欄位剛好夠窄，不是任何程式碼
  在維護的性質。
- **突變 16 個，14 抓到、2 個確認等價**：
  - **P1（把 F3 守衛整個拿掉）被 sanitizer 直接 trap**。F3 若被重新引入會當場爆掉。
  - **P2/P3 一開始存活，逼出我沒想到的缺口**：截斷測試讓每行都剛好 40 bytes，
    `pos` 只會取到 40 的倍數，**守衛的判斷在每個倍數附近一整段閾值裡都相同**。
    把界限放寬一個位元組、或把名稱欄位少預留七個，兩個都是真實溢位卻沒改變任何斷言。
    改成讓 `pos` 精準落在 473（守衛必須仍拒絕的最大值，473+40=513）之後兩個都被抓到，
    並從另一側（472 → 第 13 行塞得下、總長剛好 512）夾住，避免「一律拒絕」也通過。
    **教訓：均勻的測試資料看起來覆蓋得很好，卻可能完全碰不到被測的那個邊界。**
  - P4（`break`→`continue`）與 P12（`offset >= len`→`> len`）**推導確認為等價**，
    誠實記錄不硬殺。
- **又一個我算錯、程式碼是對的案例**：我先把截斷長度斷言成 `lines * 33`，實際是
  480 = 12 × **40**（每行最壞寬度正好等於守衛預留的 40 bytes，512/40 = 12.8）。
  改成釘住 `lines == 12`、`len == 480` 兩個精確值。
- **操作面**：上一輪寫進 CLAUDE.md 的「改文件一律用 .py 腳本檔」這輪全程遵守，
  沒有再發生反引號被 bash 當命令替換執行的事。

## Session 29 — 2026-08-14

- **主題：`sys_seek` 上界的實證調查（SEEK1）＋ 三項延伸**。先把 Session 26–28 的
  已驗證工作做成 checkpoint commit（branch `session-26-28-checkpoint`，未 push）。
- **SEEK1：調查後決定不改 `sys_seek` 的語義**，理由完整記錄在 findings.md。
  三個實測到的事實：三個後端目前全部正確（見 CONF1）；**沒有一個有依據的常數**
  可放在 syscall 邊界（各後端上限相差四個數量級，DiskFS 2 KB vs RAMFS 受限於實體
  記憶體，且 syscall 那層看不出 fd 屬於哪個後端）；**收窄會讓 user/bigseek.c 失效**
  ——那是唯一證明 ring-3 整條路徑撐得住 F22 的產物。上限的知識屬於後端，不屬於
  syscall。
- **CONF1：改用「可執行的契約」處理殘留風險**。PROJECT_STATE 原本把它寫成散文
  （「下一個後端還是得自己重擋一次」），現在是 tests/fs_conformance.h，三個後端
  都跑，新後端接上就自動被檢查。
  - **突變 3/3，但歸因分清楚**：**C3（DiskFS 界限改成會繞回的加法）只有契約抓到**
    ——6 個失敗全來自契約，既有 943 個 diskfs 檢查一個都沒抓到；C2（還原 F23）由
    既有測試與契約雙重抓到；**C1（還原 F22）是被 test_ramfs 既有的看門狗抓到的，
    不是契約**，誠實記錄契約在 RAMFS 上未被獨立證明。
- **CAP17：tests/test_vga.c**（4769 檢查）。VGA 從沒被測過的原因值得記：QEMU 那套
  讀的是 **port 0xE9 的位元組流，而 putchar 在做任何游標運算之前就先送出去了**，
  所以捲動／環繞／退格的錯誤對端對端測試完全不可見。突變 21/21，**全部由斷言抓到**。
- **`tests/test.h` 的一個真正修正**：失敗訊息逐筆 `fflush`。stdout 在管線裡是區塊
  緩衝的，**先報失敗、隨後崩潰的測試會把失敗訊息全部丟掉**，只剩崩潰——那正是
  「斷言抓到了」與「不知道哪裡出事」的分界。加上之後 V1/V10 的歸因才正確。
- **HEAP1：heap.c 稽核（沒找到缺陷）＋ 四個真實測試缺口**（378 → 720 檢查，
  突變最終 15/15、無等價突變）。最值得記的是 **我自己寫的不變式檢查有缺陷**：
  連鎖合併時只有吸收方的 header 會更新，被吸收的區塊保留舊 size，所以「只驗第一個
  鄰居」的版本讓兩個真實突變溜過去。另外把 stub 從 `posix_memalign` 改成單一 arena
  依序配發——真實 PMM 是連續配發 frame 的，原本的 stub 讓「跨成長邊界的合併」與
  「自由串列位址排序」兩個性質根本不可測。
- **本輪沒有找到任何新的可觸發缺陷**。產出是：一個有完整推導的設計決定（不改
  sys_seek）、一個把散文變成可執行檢查的契約、兩個新測試套件、以及一個讓所有套件
  的失敗歸因變可信的 test.h 修正。

## Session 30 — 2026-08-15

- **主題：`ata.c`**，最後一個沒有單元測試的驅動，也是最需要的一個：它在儲存堆疊最底層，
  DiskFS 在它回傳的位元組上蓋檔案系統，而 QEMU 的模擬 IDE **每個命令都立刻回應、從不
  報錯**——所以三百條 QEMU 斷言裡沒有一次逾時、沒有一次 ERR、沒有一個命令下給忙碌的
  磁碟。這個檔案的每條失敗路徑都是從未執行過的程式碼。
- **F24（P2）：逾時的命令會把資料交給下一個操作**。輪詢逾時只讓驅動這側放棄，磁碟
  仍在執行；硬體在 BSY 期間忽略 command block 的寫入，於是下一個操作的 task file
  寫進虛空，而 `ata_wait_data()` 看到的是**前一個命令**的 DRQ。實測症狀：
  `ata_read_sector(9,…)` **回傳成功但給的是 sector 5**；`ata_write_sector(7,…)`
  **回傳成功但一個位元組都沒寫進磁碟**。DiskFS 對非 superblock 的 sector 沒有校驗，
  會直接採信。
  - 修法 `ata_wait_idle()`：等 BSY，**再排空滯留的 DRQ**（只等 BSY 不夠——放棄的讀取
    留下的是「BSY 清、DRQ 立」的磁碟），所以修法是**復原**而非只是拒絕。
  - **誠實界定可觸發性**：需要一次輪詢逾時，QEMU 裡不會發生（模擬 IDE 立刻回應），
    真實硬體的慢碟/壞軌重試/spin-up 會。
- **CAP18：tests/test_ata.c**（8967 檢查）。假裝置是**狀態機**：BSY 依可設定的輪詢
  次數才清除（設成大於 `ATA_POLL_LIMIT` 就是逾時模型）、DRQ 在 256 words 後自己落下、
  **BSY/DRQ 期間的 command block 寫入一律忽略**、讀取失敗時 **ERR 與 DRQ 同時拉起**。
  irq.h 也被換掉用來**計數**——漏掉 restore 不會讓任何斷言失敗，只會讓機器從此收不到
  中斷。
- **突變 28 個，最終 28/28、零存活、零等價突變**，但**一開始 8 個存活**，其中兩個
  暴露的是**我的測試模型本身的問題**，值得記：
  - **A4/A5**：我讓錯誤注入覆蓋**每一次**狀態讀取，於是 `ata_wait_idle` 在命令下出去
    之前就先拒絕——**測試在正確程式上也是因為錯的理由通過的**。改成由命令完成觸發的
    錯誤狀態才有鑑別力。
  - **A8**：我的 fake 原本讓「DRQ 期間下命令」中止舊傳輸並接受新命令。規格說那是
    **未定義行為**——模型不該替驅動挑一個剛好方便的解釋。改成保守的「忽略」之後 A8
    立刻被抓到，而那也正是 F24 修法需要排空 DRQ 的理由。
- **ASSESS1：IRQ-driven ATA 評估後決定現在不做**，而且阻礙**不是**測試覆蓋。假裝置
  已能編寫任意失敗序列、28 個突變證明有牙齒；真正的問題是 `ata_read_sector` 目前
  **永不阻塞**，而 `diskfs.c` **完全沒有序列化**（整個檔案沒有鎖、沒有 cli），它的
  狀態全靠「syscall 全程關中斷」這個從 interrupt gate 繼承來的假設。改成阻塞式等於
  要先替儲存堆疊引入併發模型——與「可中斷睡眠 + EINTR」同一性質的架構決定。
  已知限制 1 的理由因此更新為「**缺的是上層的併發模型，不是驅動的測試**」。

## Session 31 — 2026-08-15

- **主題：每行程檔案描述子表的所有權契約（CAP19）**。承接 Session 30 尾聲的稽核：
  `process.c` 配置 slot 時只 memset `process_t`、**從不碰 `open_files[]`**，
  正確性完全靠「每條釋放 slot 的路徑都先關描述子」——一個**跨兩個檔案、散落在七條
  路徑上、而且一個字都沒寫在程式碼裡**的不變式。
- **稽核結論：七條路徑全部正確，沒有現存缺陷**（四條回收已經過 `process_finish_exit`
  的 zombie、一條緊接其後、一條在 fork 失敗時明確關檔、一條發生在任何描述子存在之前）。
- **CAP19：tests/test_fdtable.c**（474 檢查）。**核心決定是觀察所有權而非回傳值**：
  `open_fs`/`close_fs` 與四個 pipe 參照函式換成保有真實計數語意的 stub，**沒有對應
  open 的 close 記為 underflow**。只看 `sys_close()` 回傳值的測試，對本輪設計的
  每一個突變都會通過。
- **突變 25 個：23 抓到、2 個確認等價**。一開始 6 個存活，其中五個是真實缺口，
  兩個教訓值得記：
  - **對稱的狀態會讓「搞反」看不出來**：fork 把 pipe 讀寫端 bump 反了，在兩端都開著
    時兩邊都是 1→2，完全對稱。先關掉一端製造不對稱才抓得到。
  - **只開兩三個描述子的測試，蓋不住迴圈邊界**：exit/fork 的迴圈少跑一格，剛好把
    那些測試檢查的都涵蓋了。**填滿整張表**才讓邊界可觀察。
  - **D21（dup2 自我複製檢查）是等價突變，但只在全域 cli 模型下成立**：突變版
    close→restore→open 讓參照 1→0→1，最終狀態相同；中間那一瞬間參照為 0，而系統
    呼叫全程關中斷，沒有別的行程能在縫隙裡 unlink。**併發模型一改它就變成真的 UAF**，
    所以那個檢查不是死碼——這一點已寫進 findings。
  - **D14 是等價突變**：`close_fd_entry` 已清零 offset，沒有任何路徑能產生
    「OF_NONE 但 offset 非零」的 entry。兩處清零互為備援，與 ramfs R6 同型。
