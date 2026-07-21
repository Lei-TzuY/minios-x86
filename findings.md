# Findings — miniOS 審查紀錄

## 子系統清單與狀態
| 檔案 | 用途 | 已讀 | 風險等級 | 備註 |
|------|------|------|----------|------|

## 架構筆記：中斷/併發模型（先搞懂這個，才能判斷「沒上鎖」是不是 bug）
- 所有 ISR/IRQ 進入點在 interrupt.s 都以 `cli` 開頭，並只在 isr_common_stub /
  irq_common_stub 尾端 `sti; iret` 才重新開中斷。x86 是單核，且排程只發生在
  timer IRQ（cli 狀態下）或透過 syscall（isr128，一樣先 cli）呼叫的
  task_block_current/schedule。switch_task 只存 callee-saved 暫存器，不存
  EFLAGS——每個 task 恢復執行時，會回到自己當初呼叫 switch_task 的那個
  save_irq_disable/restore_irq 配對點，用自己當初存的 flags 決定要不要 sti。
  => 這是一個自洽的「全域 cli 大鎖」設計；pmm/heap/task 等全域結構不用額外
  spinlock 是刻意且正確的（只要沒有路徑在持有這些結構期間提前 sti）。
  之後看到「沒有鎖」不要預設是 bug，先確認呼叫路徑是否經由 trap/IRQ 進入。

## ⚠️ 方法論修正（Session 2 發現，影響 Session 1 的驗證可信度）

### M1 [關鍵] 先前用來擷取離開碼的指令模式會「永遠回報 0」，導致假綠燈
- 現象: Session 1（與 Session 2 初期）用
  `wsl.exe -d Ubuntu-26.04 -- bash -lc "cd ... && make test >log 2>&1; echo TEST_EXIT=\$?"`
  這個模式判讀測試結果。實測證明：這個 `\$?` 經過 Git Bash → wsl.exe → bash -lc
  多層跳脫後，**不會捕捉到真正的離開碼，恆為 0**（用 `false; echo EXIT=\$?` 與
  一個 `exit 3` 的 make target 都驗證出回報 0）。
- 後果: Session 1 加入 threadexit 這支內嵌程式後，RAMFS 節點數從 47 變 48，但
  `make test` 的 `grep -q "RAMFS nodes=47"` 斷言沒有一併更新 → test-shell 的
  斷言鏈其實在該處失敗、make 實際回傳 2（失敗）。但因為上述擷取模式恆回報 0，
  Session 1 誤判為「全數通過」。**這是 Session 1 一個真實的驗證缺陷，特此更正。**
- 根因分析: test-shell recipe 本身的 gate 邏輯是正確的（用乾淨、無跳脫層的
  腳本重現，中途斷言失敗會讓 make 正確回傳 2）。問題純粹出在「跨 wsl.exe 多層
  跳脫擷取 `$?`」這個外部判讀手法。
- 修復:
  1. 改用「把建置/測試邏輯寫進一個 .sh 腳本檔、在單一 WSL bash 行程內擷取
     `$?`」的可靠方式（scratchpad/verify.sh），離開碼真實反映成敗。
  2. 更新過時的節點數斷言（47 → 現值），重新以可靠方式確認 `make test` 真的
     回傳 0。
  3. 之後每一次驗證都走這條可靠路徑。
- 狀態: 已修正並重新建立「真正的」綠燈基準。

## 問題清單（依發現順序）

### F5 [P4][建置腳本/技術債] `make clean` 沒清掉一半的產生檔（`*_embed.c`）
- 檔案: Makefile:426（`clean` target）
- rm 清單裡漏了 jobctl_embed.c、uptime_embed.c、date_embed.c、
  printenv_embed.c、cputime_embed.c、shmtest_embed.c、semtest_embed.c、
  mmaptest_embed.c、threadtest_embed.c 這幾個（對照 OBJS 清單能看出遺漏）。
  不影響「build 出來的結果是否正確」——因為這些 `.c` 都有
  `foo_embed.c: user/foo.elf gen_embed.py` 的相依規則，`clean` 會先把
  `user/foo.elf` 砍掉，下次 `make` 重建 elf 後其 mtime 一定比殘留的
  `foo_embed.c` 新，make 仍會正確重新產生它；純粹是 `make clean` 沒把樹
  清乾淨（殘留檔案不會進 git，因為 .gitignore 有 `*_embed.c`）。
- 修復: 已把清單補齊，並在新增 threadexit 目標時一併加入。
- 狀態: 已修（附帶順手修正，非本輪主要目標）

### F6 [P4][文件] 中文版 README 系統呼叫數與英文版不一致（47 vs 51）
- 檔案: README.md（中文簡介段落）
- 狀態: 已修正為 51（與英文版一致，並隨新增的 user 程式數量更新 37→38）

### F1 [P0][正確性/記憶體安全] 多執行緒 process 的 main thread 結束時未等待其他
thread，直接摧毀共享 address space → 其餘 thread 之後被排程時對已釋放記憶體解參照
- 檔案: process.c:98-139 (`process_task_exit`)、process.c:472-479 (`thread_on_exit`)
- process_task_exit 是「主 task」(process->task) 結束時的 on_exit callback；
  它無條件呼叫 paging_destroy_user_address_space(process->address_space)，
  釋放 page directory / page table 所在的 PMM 區塊並 kfree(space) 本身。
  但若該 process 曾用 SYS_THREAD_CREATE 建立過執行緒且尚未 join（thread_count>0），
  那些 thread 的 task_t.address_space 仍指向這個剛被釋放的結構；排程器之後
  activate_task() 會對它 load_cr3(freed->directory) —— use-after-free，
  輕則載入垃圾 CR3 造成 triple fault / QEMU 重開機，重則腐蝕已被重新配置的
  heap 記憶體。目前 make test 不會觸發，因為 threadtest.c 總是在 exit 前呼叫
  sys_thread_join()；但任何「不 join 就結束」的程式（或主執行緒中途 crash 走
  task_exit(-1)）都會踩到。
- 修復方向（保守、相容現有設計）：process_task_exit 若偵測 thread_count>0，
  不要立刻拆掉 address space/檔案表，只記錄 exit status 並設一個
  `main_exited`旗標；實際收尾（原本 process_task_exit 的全部內容）搬進
  一個共用函式，改由「最後一個結束的 thread」觸發（thread_on_exit 在
  thread_count 減到 0 且 main_exited 時呼叫）。行為不變的情況（thread_count
  在 main 結束前已經是 0，也就是現有測試涵蓋的路徑）完全不受影響。
- 狀態: **已修＋已驗證**。process.h 加 `main_exited` 欄位；process.c 拆出
  `process_finish_exit()` 共用函式，`process_task_exit`/`thread_on_exit`
  依 thread_count 決定立即收尾或延後。新增 user/threadexit.c 迴歸測試
  （main 不 join 就直接 return，worker thread 在 main 結束後才跑完並印出
  `[threadexit worker done]`），掛進 Makefile 的 OBJS/embed 規則與
  test-shell 目標。`make test` 全綠，且日誌可見 worker 確實在 main 之後
  正常跑完，證明舊路徑（會在下一次排程 worker 時 load_cr3 到已釋放記憶體）
  已修正，不是只憑程式碼推理。

### F2 [P0][安全性/正確性] signal_deliver 在使用者堆疊指標上做無界限檢查的
指標運算，惡意/錯誤的使用者 ESP 可讓整個核心當機（不只是該行程）
- 檔案: syscall.c:768-836 (`signal_deliver`)
- usp = regs->useresp; usp -= sizeof(sigcontext_t) + 8; 之後直接
  `sc->eip = ...` 等原始寫入，完全沒有驗證 usp 是否落在
  [USER_STACK_BOTTOM, USER_STACK_TOP) 之內。使用者可以在 ring3 自由把 esp
  設成任意值（例如壓到 USER_STACK_BOTTOM 附近）再觸發一個訊號（自己
  alarm() 或被 shell kill -INT），usp 下溢到合法堆疊範圍之外。因為這次寫入
  發生在核心程式碼路徑內（CS=0x08），paging.c 的 page_fault_handler 對這種
  非法位址的處理分支會判定 `(regs->cs & 0x3) != 3`，落入
  「PAGE FAULT! ... System Halted.」分支，整台虛擬機/機器直接掛死在 `hlt`
  迴圈——這是使用者可觸發、影響整個核心（而非僅該行程)的 DoS。
  合法情況（堆疊頁存在但尚未 demand-page）不受影響，因為位址仍落在
  USER_STACK_BOTTOM..TOP 內，會被 page_fault_handler 正常 demand map。
- 修復方向：在寫入前檢查 usp 與 usp+sizeof(sigcontext_t)+8 是否都落在
  [USER_STACK_BOTTOM, USER_STACK_TOP)；不合法就視同無法安全遞送訊號，
  比照現有「handler==0 走預設終止」的慣例呼叫 task_exit(-128-sig) 結束該
  行程，而不是讓核心當掉。
- 狀態: **已修**。syscall.c 的 signal_deliver 在建立訊號堆疊框之前先驗證
  useresp 落在 [USER_STACK_BOTTOM, USER_STACK_TOP) 且留有 sizeof(sigcontext_t)
  +8 位元組空間，不合法就 task_exit(-128-sig)。現有 sigtest/sigipc/sigchld/
  alarmdemo/jobctl 等所有訊號相關測試在修復後仍全數通過（合法路徑行為不變），
  證明修法沒有動到正常訊號遞送。惡意 ESP 觸發下溢的情境沒有獨立寫成自動化
  測試（需要在使用者程式裡直接改寫 esp 暫存器並精準對齊分頁邊界，屬於較
  脆弱的白盒測試手法，風險大於效益），此為誠實記錄的驗證缺口。

### F3 [P3][記憶體安全] procfs 的 gen_buf[512] 固定緩衝區在極端情況下可被
`/proc/processes` 寫爆
- 檔案: procfs.c:21,51-103 (`gen_buf`, `proc_generate`)
- u_to_str/append_str 都不帶界限參數，PROC_PROCESSES 分支對 MAX_PROCESSES(16)
  個 process 逐行寫入 pid/ppid/state/name，完全沒檢查總長度是否超過
  sizeof(gen_buf)=512。pid/ppid 是 int32_t 且 next_pid 只增不減，理論上長期
  運作、大量 fork 後 pid 可達 10 位數，16 列 * (10+1+10+1+1+1+15+1)=640 bytes
  會寫出 gen_buf 邊界，破壞後面的 static 資料。現有 make test 不會觸發（pid
  數與存活行程數都遠低於門檻），但這是全域緩衝區溢位，屬於未驗證輸入長度
  的防禦缺口。
- 修復方向：在 proc_generate 的迴圈裡加上剩餘空間檢查，空間不足就停止寫入
  （截斷而非溢位），成本低、不改變現有輸出格式。
- 狀態: **已修**。PROC_PROCESSES 迴圈在每行寫入前檢查最壞情況所需空間，
  不足就提前跳出（截斷列表而非溢位）。`cat /proc/processes` 相關測試斷言
  （`R cat`、`name=cat`、`state=R cpu=` 等）修復後仍照常通過。

### F4 [P2][正確性/資料完整性] fat16 的 fs_node_t 是 16 個 slot 的環狀重用池，
不是依檔案身分穩定配置 → 長時間使用會讓舊的開檔 fd 悄悄指向別的檔案
- 檔案: fat16.c:41-42 (`fat16_node_pool`/`fat16_node_next`), fat16.c:196-219
  (`fat16_make_node`)
- 每次路徑解析（finddir）或建立檔案都會呼叫 fat16_make_node，它無條件從
  `fat16_node_pool[FAT16_NODE_POOL]`（16 個)取下一個 slot 並覆寫其內容,
  不管這個 slot 目前是否還被某個開啟中的 fd（syscall.c 的 open_file_t.node）
  持有。fat16 的 fs_node_t 也完全沒有接上 open/close callback（不像
  diskfs.c 用 diskfs_open_refs[] 計數、ramfs.c 用 impl 的 ref-count 位元）,
  所以沒有任何機制知道某個 slot「正被使用中」。只要累積對 /fat 底下路徑的
  查找（含路徑中間目錄的每一層）超過 16 次，最早那個仍被某 fd 持有的
  fs_node_t 就會被覆寫成另一個檔案的 name/length/ptr —— 之後對那個 fd 的
  read/write 會靜默地讀寫錯誤的檔案（confused deputy），不是當機而是資料
  錯置，更危險。對照 diskfs.c 用「每個檔案一個固定 slot（按 entry index）」
  的正確作法，fat16 這裡是設計上的缺口。make test 對 /fat 的操作次數少
  （遠低於 16 次)，不會踩到。
- 修復方向（保守）：fat16_make_node 配置新 slot 前，先在 pool 裡找
  `node->ptr == e`（同一個目錄項)的既有 slot 並重用/更新它，而不是無條件
  往前推進 round-robin 游標；只有找不到既有 slot 時才配置/回收一個「看起來
  沒被用到」的 slot。這不需要引入完整的 open/close 參照計數，就能讓「同一
  個檔案的重複查找」保證拿回同一個節點物件，消除最常見的別名情境（例如
  cd 來 cd 去、反覆 cat 同一個檔案時仍持有舊 fd)。仍是盡力而為（超過 16
  個「同時外流的不同檔案」引用還是會退化成 LRU 覆寫），但已顯著縮小風險面,
  且改動侷限在單一函式、不改變對外行為/格式。
- 狀態: **已修**。fat16_make_node 配置新 slot 前先線性掃描 pool 找
  `ptr == e` 的既有 slot 並重用；找不到才推進 round-robin 游標。所有 FAT16
  相關測試（ls fat、cat fat/hello.txt、cat fat/docs/note.txt、寫入/讀回/
  刪除 fat/new.txt、ush 下 cd fat + cat hello.txt）修復後仍全數通過。未額外
  寫一個「開超過 16 個 fd 觸發別名」的專屬回歸測試——這需要新增遠多於現有
  測試量的 FAT16 操作，而現有整合測試已涵蓋所有正常使用模式；此為誠實記錄
  的驗證缺口（同 F2）。

### F7 [P1][正確性/記憶體安全] execv 從「仍有存活執行緒」的行程呼叫時，會摧毀
其他 thread 仍在使用的共享 address space（與 F1 同一類的 use-after-free）
- 檔案: process.c `process_exec_reset`、syscall.c `sys_execv`
- Session 2 複查 exec 路徑時發現：process_exec_reset 會
  paging_destroy_user_address_space(old) 釋放舊 address space。若該行程先前用
  SYS_THREAD_CREATE 建立了執行緒且尚未 join（thread_count>0），那些 thread 的
  task_t->address_space 仍指向這個剛被釋放的結構，下次被排程就會 load_cr3 到
  已釋放的 page table —— 與 F1 完全同類的 UAF。真實 Unix 的 exec 會原子性地
  終止其他所有 thread，但 miniOS 沒有安全地強制停止任意 running task 的機制。
  現有測試不會踩到（execdemo 等都是單執行緒 exec），屬潛伏 bug。
- 修復（保守、正確性優先）: process_exec_reset 開頭加
  `if (process->thread_count > 0) return -1;`，在任何狀態變更之前就拒絕。
  sys_execv 收到 -1 會銷毀剛建好的新 image 並回傳 -1，呼叫者原封不動繼續執行，
  不會拆掉任何仍在使用的資源。
- 狀態: **已修＋已驗證**。新增 user/execguard.c 迴歸測試：建立一個 parked 在
  semaphore 上的 worker thread，主執行緒呼叫 execv("hello")，驗證回傳 -1
  （印出 `[execguard exec rejected]`），再放行 worker（`[execguard worker ran]`
  證明共享 address space 沒被摧毀、worker 正常跑完）並 join。已接入建置系統與
  test-shell 斷言，`make test` 以**真實離開碼 0**通過（見 M1 的驗證方式）。

### F8 [P2][正確性/安全性] vfs_resolve_path 對過長路徑「靜默截斷」，回傳成功卻
指向另一個（較短的）路徑 → 呼叫者對錯誤的檔案系統物件動作
- 檔案: fs.c `path_push` / `path_apply` / `vfs_resolve_path`
- `path_push` 在會超出 FS_MAX_PATH 時直接 `return`（原註解自承是 "best-effort
  overflow guard"），**不附加該元件也不回報錯誤**。而 `vfs_resolve_path` 結尾的
  `if (olen + 1 >= FS_MAX_PATH) return -1;` 完全偵測不到這種情況——因為 olen
  根本沒成長。結果：解析回傳 0（成功），但 `out` 是把尾端元件丟掉後的路徑，
  也就是某個**祖先目錄**。
  可觸發性：cwd 最長 127 字元（PROCESS_CWD_MAX），相對路徑最長 127 字元
  （MAX_USER_STRING），合起來遠超 FS_MAX_PATH(128)。使用者只要 mkdir 一個長
  名稱目錄再 chdir 進去，之後任何相對路徑操作都會踩到。
  危險案例：cwd 接近上限時 `rmdir("sub")` 會被截斷成 cwd 本身 → 刪到**當前
  目錄**而非子目錄；`stat("x")` 會回報 cwd 這個目錄的中繼資料。
- 修復（保守、失敗優於猜錯）: `path_push` 改為回傳 0/-1，`path_apply` 一路
  往上傳遞，`vfs_resolve_path` 只要任一元件放不下就回傳 -1。所有呼叫端本來就
  已經處理 -1（sys_open/mkdir/rmdir/unlink/chdir/stat 與 kernel shell 都是
  `!= 0` 就報錯），所以行為變成「乾淨失敗」而非「解析到別的路徑」。
- 狀態: **已修＋已驗證**。新增 user/pathlim.c：建立 125 字元目錄並 chdir 進去
  （cwd 126 字元），此時連 1 字元的相對路徑 "x" 都放不下，驗證 `sys_stat("x")`
  必須回傳 -1（`[pathlim overlong rejected]`）；同時驗證正常短路徑仍可解析
  （`[pathlim normal path ok]`，避免修過頭變成過度拒絕）。`make test` 真實通過。

### F9 [P3][正確性/中繼資料一致性] FAT16 寫入在叢集耗盡時，仍把檔案長度記成
「請求的結尾」而非「實際寫入的結尾」
- 檔案: fat16.c `fat16_vfs_write`
- 延伸叢集鏈的迴圈在 `fat16_alloc_cluster()` 回傳 0（無可用叢集）時 `break`，
  搬資料的迴圈也會提早結束，於是 `written < size`。但結尾無條件用
  `end = offset + size` 更新目錄項的檔案大小，使得檔案宣稱的長度大於實際被
  叢集鏈支撐的資料量。回傳值 `written` 是對的，長度卻是錯的。
- 修復: 改用 `offset + written`（實際寫入的資料是從 offset 起連續的，所以這就是
  真正的資料結尾）。寫入完全成功時 `written == size`，與原本完全等價，正常路徑
  行為不變。
- 狀態: **已修**。以現有全部 FAT16 測試（ls/cat/寫入/讀回/刪除、ush 下 cd fat）
  驗證無回歸。未新增「灌爆叢集」的專屬測試：那會把記憶體中的 FAT16 映像填滿並
  持續影響同一次開機後續的 fat 斷言，破壞測試套件穩定性，成本大於效益——此為
  誠實記錄的驗證缺口。

### F10 [P1][正確性/併發] process_send_signal / process_request_kill 誤用
`task_wake_one(channel)` 來喚醒「特定 task」，正確性只是巧合地依賴 LIFO 順序
- 檔案: task.c/task.h（新增 `task_wake_task`）、process.c 兩處呼叫端
- 這兩處的意圖是「把**這個** process 的 task 叫醒，讓它回到使用者態去收訊號」，
  實作卻是 `task_wake_one(process->task->wait_channel)` —— 喚醒「該 channel 上
  的任一個 task」。而**不相關的 task 經常共用同一個 wait channel**：核心 shell
  執行前景程式時用 `process_wait(pid)` 阻塞在該子行程的 process_t 上，而那個
  子行程自己若呼叫 `waitpid` 又會阻塞在**同一個** process_t 上（process_waitpid
  是 block 在自己的 process_t）。
  原本 blocked list 是頭插頭取（LIFO），剛好會挑到「最後阻塞的那個」＝通常正是
  目標 task，所以看起來能動——**純屬順序巧合，不是設計正確**。
- 如何暴露: 本輪把 blocked list 改成尾插（FIFO 公平喚醒，見下方 FAIR1）後，
  `task_wake_one` 改挑最早阻塞者＝核心 shell，shell 重新檢查條件後又睡回去，
  真正該被喚醒的 jobctl 父行程永遠沒醒 → **整個系統死鎖**（實測：log 停在
  `[jc child] finished`，之後只剩鍵盤回音、shell 不再執行任何指令）。
- 修復: 新增 `task_wake_task(task_t *)`——依**身分**把指定 task 從 blocked list
  移除並轉為 ready（不在列表中就是 no-op）；process_send_signal 與
  process_request_kill 改用它。所有阻塞點本來就是 `while (條件) block;` 迴圈，
  因此偽喚醒無害。這比原本的寫法嚴格更正確，與 FIFO/LIFO 無關。
- 狀態: **已修＋已驗證**。修好後 `[jc parent] child reaped` 恢復出現，
  `make test` 真實通過。

### F11 [P1][記憶體安全] dup2/redirect 把檔案節點掛到 stdin/stdout 時未取得 VFS
參照 → 檔案被 unlink 後節點遭 kfree，行程仍持有懸空指標（可觸發核心 UAF）
- 檔案: syscall.c `sys_dup2`、process.c `process_redirect` / `process_finish_exit`
- fd 表裡每個描述子都持有一個參照（`open_user_file` 呼叫 `open_fs`，
  `close_fd_entry` 呼叫 `close_fs`）。但 `sys_dup2` 把節點別名到
  `process->stdout_node` / `stdin_node` 時**只是指派指標，沒有 open_fs**；
  `process_redirect`（核心 shell 的 `>` / `<`）同樣沒有。
  而 dup2 的標準慣用法正是「dup2 後立刻 close 原 fd」——ush 的
  `run_command` 就是這樣寫的：
      `sys_dup2(fd, 1); sys_close(fd);`
  close 之後該節點的參照計數歸零，`unlink` 於是被允許，RAMFS 的
  `ramfs_remove_node` 執行 `kfree(node->ptr); kfree(node);`。但行程的
  `stdout_node` 仍指著那塊已釋放的記憶體，下一次 `sys_write` 會走
  `write_fs(stdout_node, ...)`，從**已釋放的堆積記憶體**讀出 `node->write`
  這個**函式指標**並呼叫它 —— 核心層級的 use-after-free，且是控制流被劫持的
  型態，不只是資料損毀。可從 shell 觸發（例如把長時間執行的程式放到背景並
  重導向輸出，然後 rm 掉那個檔案）。
  DiskFS 的節點是靜態陣列不會被 kfree，症狀較輕（變成寫入被忽略）；
  RAMFS 是 kmalloc 配置的，才是真正的 UAF。
- 修復（對稱的參照管理，遵循既有 pipe 端點的模式）:
  1. `sys_dup2` 掛上檔案節點時 `open_fs()` 取得參照；若原本已有節點則先
     `close_fs()` 釋放（含被 pipe 端點取代的情況）。
  2. `process_redirect` 同樣取得/釋放參照。
  3. `process_finish_exit` 在行程結束時釋放 stdout_node / stdin_node 的參照，
     使帳目平衡（否則檔案將永遠無法被刪除）。
- 狀態: **已修＋已驗證**。新增 user/redirref.c：把 stdin 別名到檔案後關閉 fd，
  此時該節點僅由 stdin 持有——驗證 `unlink` 必須被拒
  （`[redirref inuse unlink refused]`）、且仍能透過 stdin 讀到檔案內容
  （`[redirref stdin reads file]`）。測試腳本在程式結束後再由 shell 執行
  `rm rr.tmp`，並斷言**不得**出現 "cannot remove"，藉此同時證明參照在行程
  結束時確實被釋放（沒有反向的洩漏）。既有 shell/ush 重導向測試全數不受影響。

## 排程公平性（已實作）

### FAIR1 blocked task 改為 FIFO 喚醒（尾插）取代 LIFO（頭插）
- 檔案: task.c `task_block_current`
- 原本 `task->blocked_next = blocked_tasks; blocked_tasks = task;`（頭插），
  搭配 `task_wake_one` 取第一個相符者 ⇒ 最晚阻塞者先醒（LIFO）。理論上若同一
  channel 持續有新的等待者加入，早期等待者可能被無限期跳過（starvation）。
- 改為尾插，使列表順序為「最早 → 最晚」，`task_wake_one` 取第一個相符者即為
  **最早等待者**（FIFO），這是公平且慣用的選擇。走訪為 O(阻塞中的 task 數)，
  數量很小且本來就在關中斷區間內。
- 這項改動同時**暴露並促成了 F10 的修復**（見上）。原本列為「已知限制」，
  本輪予以實作。
- 驗證: 現有測試套件已涵蓋號誌、pipe、sleep、thread join、waitpid、鍵盤、
  job control 等所有 block/wake 路徑，修 F10 後全數通過。未另寫「喚醒順序」
  專屬測試：要讓多個行程以確定順序阻塞需依賴 sleep 交錯，時序脆弱易 flaky，
  成本大於效益——此為誠實記錄的驗證缺口。

## 效能改善（已實作）

### PERF2 ramfs_write 改成幾何成長（攤還 O(1) append，取代原本每次成長 O(n) 複製）
- 檔案: ramfs.c（`ramfs_entry_t` 加 `capacity` 欄位、重寫 `ramfs_write`）
- 原本每次寫入超出 length 都 kmalloc 新 buffer + memcpy 整份舊內容 + kfree，
  重複 append 是 O(n²)。改為：在 ramfs 私有的 entry 結構追蹤已配置容量，成長時
  容量加倍（至少滿足需求），未超過容量的成長直接就地寫入、不重配置。維持
  「[length, capacity) 恆為零」的不變式，語意與原本完全相同（讀取仍只用
  length）。這原是「已知限制」清單中的一項，Session 2 予以實作。
- 狀態: **已修＋已驗證**。新增 user/ramgrow.c：以 128 次小寫入堆出 2048 bytes，
  再整檔讀回逐位元組驗證（`[ramgrow bytes=2048]` + `[ramgrow ok]`），結束前
  刪除暫存檔以免影響其他測試的節點數斷言。`make test` 真實通過。

### PERF1 memcpy/memset 改成 4-byte 對齊批次搬移
- 檔案: utils.c
- 原本逐位元組搬移，在分頁歸零（每次 4KB）、ATA 磁區 I/O（每次 512B）、
  RAMFS/DiskFS/FAT16 讀寫、COW 複製等熱路徑上都會呼叫到，是全系統共用的
  瓶頸。改成：先用位元組迴圈把指標搬到 4-byte 對齊，中段用 uint32_t 一次
  搬 4 bytes，尾端剩餘 bytes 再用位元組迴圈收尾；memcpy 額外檢查來源指標
  是否也對齊，沒對齊就整段退回逐位元組（正確性優先，不強行做未對齊的
  32-bit 存取）。行為完全不變（純粹是同樣結果、更少迴圈次數），make test
  全綠。

## 已知限制（本輪審查有發現、但刻意不修的項目，附理由）
以下項目屬於真實觀察到的架構/效能取捨，但要嘛需要較大幅度重構、要嘛觸發
門檻在這個專案的實際使用場景下極難達到，依照「保守、相容現有設計」的原則
本輪不動，只記錄下來：

- **ata.c 整個 PIO 輪詢過程都 cli**：ata_read_sector/ata_write_sector 從
  save_irq_disable() 到 restore_irq() 之間，包含等待 BSY/DRQ 的忙等迴圈
  （最多 ATA_POLL_LIMIT=100000 次），全程關中斷。這是為了序列化對同一組
  ATA I/O 埠的存取（沒有這個保護，兩個行程交錯發指令會讓資料錯位），但
  代價是磁碟 I/O 期間會漏接 timer tick、系統體感會卡頓。要修就得改成
  IRQ-driven ATA 或用不佔用 cli 的 mutex 搭配「輪詢時允許中斷」的設計，
  對這個以 PIO 忙等為核心的簡單驅動來說改動幅度大、風險高，本輪不做。
- ~~**ramfs_write 每次成長都整檔重新配置**~~：**已於 Session 2 修復，見上方
  PERF2**（改為幾何成長、攤還 O(1) append）。
- ~~**task_wake_one 是 LIFO 不是 FIFO**~~：**已於 Session 3 改為 FIFO，見上方
  FAIR1**；此改動同時暴露並促成了 F10（依身分喚醒特定 task）的修復。
- **process_send_signal 對多執行緒 process 的定位不完整**：只會嘗試喚醒
  `process->task`（主 task），如果實際被阻塞、需要遞送訊號的是
  SYS_THREAD_CREATE 產生的其他 thread，該 thread 不會被正確喚醒。miniOS
  的 signal 模型本來就是設計給單一使用者主控流程的（thread 主要拿來做
  threadtest.c 那種共享記憶體示範），這個落差在文件/註解中沒有特別提及，
  值得記錄成已知限制，但要正確支援需要重新設計「訊號要送給 process 的哪個
  task」，改動面較大，本輪不做。
- **elf_loader 只從 RAMFS 載入可執行檔**：`elf_load_image` 呼叫的是
  `ramfs_find_file`，不是走完整 VFS（resolve_fs），所以無法直接
  `exec`/`spawn` 位於 /disk 或 /fat 底下的執行檔。目前所有可執行檔都是
  build 期內嵌進 RAMFS 的（見 kernel.c 的 ramfs_create_static_file 呼叫），
  README 與 shell 說明都沒有宣稱能從其他檔案系統執行程式，判斷是刻意的
  設計範圍而非疏漏，故只記錄不改動。

<!-- 格式：
### [ID] 標題
- 檔案:行號
- 類別: 正確性/競態/記憶體安全/效能/架構/文件
- 嚴重度: P0-P4
- 描述
- 修復方式（若已修）
-->
