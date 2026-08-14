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
- **後續**: Session 5 的 F12 為 fat16 補上完整的開啟計數，`fat16_make_node`
  改為只挑 `refs == 0` 的 slot，本項殘留風險已一併消除。

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
- 狀態: **已修＋已驗證（驗證缺口已於 Session 11 關閉）**。
  當初只以既有 FAT16 測試（ls/cat/寫入/讀回/刪除、ush 下 cd fat）驗證無回歸，
  並記錄「無法寫灌爆叢集的專屬測試」——在 QEMU 裡填滿記憶體中的 FAT16 映像會
  持續影響同一次開機後續所有 fat 斷言。
  **CAP3 的原生單元測試解決了這個問題**：每個測試都重新掛載一份乾淨映像，因此
  可以安全地寫入 40000 bytes 撐爆磁碟區，直接驗證 `written < 請求量` 且
  `node->length == written`，並把宣稱持有的資料完整讀回比對。突變測試也確認：
  把長度改回 `offset + size` 會被抓到。

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

### F12 [P2][資料完整性/資訊洩漏] FAT16 是三個檔案系統中唯一沒有開啟計數的，
`unlink` 會在檔案仍被開啟時就釋放叢集鏈 → 舊描述子可讀到別的檔案的資料
- 檔案: fat16.c（新增 `fat16_node_refs` / `fat16_vfs_open` / `fat16_vfs_close`
  / `fat16_entry_is_open`，並修改 `fat16_make_node` 與 `fat16_vfs_unlink`）
- RAMFS 用節點 `impl` 的參照位元、DiskFS 用 `diskfs_open_refs[]`，兩者的
  remove 路徑都會在檔案仍開啟時拒絕。FAT16 的節點**完全沒有接上 open/close
  callback**，`fat16_vfs_unlink` 因此毫無檢查地把叢集鏈逐一
  `fat16_set_cluster(cluster, 0)` 歸還free pool。此後任何寫入若配置到那些
  叢集，仍持有舊描述子的行程讀下去就會讀到**另一個檔案的內容**——不是當機，
  而是靜默的跨檔案資料洩漏／損毀。
- 同時修掉 F4 的殘留風險：`fat16_make_node` 先前雖已優先重用「同一目錄項」的
  slot（F4 的修法），但當需要**新** slot 時仍是無條件推進 round-robin 游標，
  只要查找超過 FAT16_NODE_POOL(16) 個不同目錄項，就可能覆寫掉仍被某 fd 持有
  的節點。現在改為只挑 `refs == 0` 的 slot；全部都被開啟時回傳 NULL
  （呼叫端視為「找不到」），寧可查找失敗也不要靜默指向錯誤的檔案。
- 狀態: **已修＋已驗證**。新增 user/fatref.c：開啟 `/fat/hello.txt` 後嘗試
  unlink，驗證必須被拒（`[fatref inuse unlink refused]`）、描述子仍能讀出正確
  內容（`[fatref content ok]`）、且檔案確實還在（`[fatref file intact]`）。
  測試刻意**不真的刪除**該檔，因此後續既有的 FAT16 測試（cat fat/hello.txt、
  fat/docs/note.txt、寫入 fat/new.txt、ush 下 cd fat）全部照常通過。

### F13 [P2][正確性/Unix 語意] fork 沒有繼承標準串流（fd 0/1），造成描述子繼承
行為不一致
- 檔案: process.c `process_fork`
- `syscall_copy_user_files` 會把 fd 3 以上的整張表複製給子行程，但 fd 0/1 的
  狀態存在 `process_t` 的獨立欄位（stdout_node / stdin_node / stdout_pipe /
  stdin_pipe），fork 完全沒有複製 → 子行程一律退回預設裝置。
  因此下列 Unix 標準寫法在 miniOS 上會得到錯誤結果：
      `dup2(fd, 1); if (fork() == 0) { write(1, ...); }`
  子行程會寫到**終端機**而不是重導向的目標檔案。
- 修復: process_fork 一併繼承四個欄位，並各自取得參照（檔案用 `open_fs`、
  pipe 端點用 `pipe_ref_write`/`pipe_ref_read`），由 `process_finish_exit`
  在子行程結束時釋放，帳目平衡。create_task 失敗的清理路徑也一併釋放。
  這是建立在 F11 的參照管理之上才得以安全實作的。
- 狀態: **已修＋已驗證**。新增 user/forkredir.c：main 不重導向（保留報告能力）→
  fork 出 child A → A 把 stdout 指向檔案後再 fork 出 child B → B 在**沒有自行
  重導向**的情況下寫入，位元組必須落在檔案中。main 讀回檔案確認
  （`[forkredir inherited]`）。測試同時斷言 `^childout$` **不得**出現在終端機
  日誌（證明真的走了繼承的重導向而非預設裝置），並在最後刪除暫存檔——若繼承的
  參照洩漏，unlink 會失敗、檔案殘留，RAMFS 節點數斷言會抓到。

### F14 [P0][安全性] SYS_SIGRETURN 完全未驗證就解參照使用者 ESP —— 任何程式都能
用一行組語讓**整台機器停機**（與 F2 同類，但我在前幾輪只修了寫入側、漏掉讀取側）
- 檔案: syscall.c `sys_sigreturn`
- `const sigcontext_t *sc = (const sigcontext_t *)(regs->useresp + 4);` 之後直接
  讀取 40 bytes，**沒有任何邊界檢查**。而且 SYS_SIGRETURN 是一般系統呼叫，
  任何程式都能直接 `int $0x80`（eax=24）觸發——**根本不需要身處訊號處理常式中**
  （程式碼也沒檢查 `in_signal`）。
  攻擊只要三行：把 ESP 設成未映射位址（例如 0x1000）再 int $0x80。從 ring 3
  進入中斷時 CPU 會透過 TSS 切到核心堆疊，所以錯誤的 ESP 完全不妨礙 int 本身。
  核心接著在 **CS=0x08（ring 0）** 情境下讀取該位址 → page fault →
  paging.c 的 handler 判定 `(regs->cs & 0x3) != 3` 為核心錯誤 →
  「PAGE FAULT! ... System Halted.」→ **整台虛擬機/機器停在 hlt 迴圈**。
  這是使用者可觸發、影響整個核心（非僅該行程）的 DoS，嚴重度與 F2 相同。
- **自我檢討**：F2 修的是 `signal_deliver` 建立訊號框的**寫入**側，我當時沒有
  同步檢查對稱的**讀取**側（sys_sigreturn），這是那一輪審查的疏漏。
- 修復: 在解參照前驗證整個 sigcontext 框都落在
  [USER_STACK_BOTTOM, USER_STACK_TOP)（堆疊區是需求分頁，範圍內的位址讀取安全），
  不合法就 `task_exit(-1)` 只終止該行程——與 F2 的寫入側檢查對稱。
  已驗算合法路徑（trampoline 送出 sigreturn 時 useresp = 原 esp - sizeof - 4）
  必定通過此檢查，不會誤殺正常訊號流程。
- 狀態: **已修＋已驗證**。新增 user/sigretguard.c **實際執行該攻擊**（內嵌組語
  把 esp 設為 0x1000 後 int $0x80）。實測日誌：`[sigretguard arming]` →
  `[program exited]`（只有該行程死亡）→ 緊接著 `> ls /proc` **系統繼續運作**，
  且 `[sigretguard SURVIVED]` 不存在。修復前這裡日誌會直接停住（整機停機），
  所以這個測試對回歸有非常強的鑑別力。

### F15 [P3][未定義行為] sys_sbrk 對 INT32_MIN 取負值是有號溢位 UB
- 檔案: syscall.c `sys_sbrk`
- `uint32_t dec = (uint32_t)(-increment);` 其中 increment 是直接來自使用者暫存器
  的 int32_t。當 increment == INT32_MIN 時 `-increment` 是有號溢位（UB）。
  實務上 gcc 會產生 `neg` 指令而回繞成 0x80000000，接著界限檢查會擋下來，
  所以目前行為正確——但那是「碰巧」，不是語言保證。
- 修復: 改用 `0u - (uint32_t)increment`（無號運算，對所有負值都定義良好且給出
  正確的絕對值）。此檔其他地方（如 paging_zero_user 的 `size > UINT32_MAX - vaddr`）
  本來就很小心處理這類溢位，這裡補齊一致性。
- 狀態: 已修。

### F16 [P3][使用者空間] umalloc 向 sbrk 要求記憶體時的 int 溢位可能變成「縮小堆積」
- 檔案: user/umalloc.h `umalloc_morecore`
- `sys_sbrk((int)(nu * sizeof(Header)))`：nu 為 unsigned，sizeof(Header)==8。
  當 nu >= 2^28（即 malloc 要求約 2GB 以上）時 `nu * 8` 轉成 int 會變**負值**，
  被 sbrk 解讀成**縮小**堆積，並回傳一個看似合法的指標——程式接著就會寫入
  不屬於自己的記憶體。屬使用者空間問題（分頁機制使其無法傷害核心）。
- 修復: 轉型前先檢查 `nu > 0x7FFFFFFF / sizeof(Header)` 就直接回傳失敗。
- 狀態: 已修。觸發需要約 2GB 的配置請求，在這個 32 位元、堆積上限約 1MB 的系統
  上不會自然發生，故未另寫測試（誠實記錄的驗證缺口）；malloctest/tail/sort
  等既有的配置器測試修改後全數通過。

### F17 [P2][正確性] kill 一個多執行緒行程只殺得掉「當下正在執行的那一個 task」，
其餘 thread 存活且請求已被清除 → 行程永遠停在 RUNNING
- 檔案: process.c `process_check_kill`
- 該函式在計時器中斷中執行，殺掉「當前 task 且屬於目標行程」的那一個，然後
  **立刻** `kill_request_pid = -1`。單執行緒行程沒問題（那一個 task 就是全部），
  但多執行緒行程只死一個 task：其餘 thread 繼續跑，而 kill 請求已消失，
  於是**永遠不會再被殺**。行程停在 PROCESS_RUNNING，任何 wait 它的人也永遠阻塞。
- 修復（一行移除）: 不在此清除請求，讓函式開頭既有的 stale 檢查
  （`proc->state != PROCESS_RUNNING`）在最後一個 task 結束、
  `process_finish_exit` 把狀態轉為 ZOMBIE（或行程被釋放）之後自然清掉。
  其餘 thread 會在各自成為 current 時陸續被殺。pid 是單調遞增的，
  所以請求殘留一個 tick 不會誤殺別的行程。
- ~~**誠實記錄的殘留限制**：若某個 thread 長期停在 `while (cond) task_block_current(ch);`
  這類等待迴圈中，它不會成為 current，因此**仍然殺不到**~~：**已於 Session 21
  修復，見下方 F19**。
- 狀態: **已修＋已驗證**。新增 user/killthread.c：建立一個**持續可排程**（忙碌
  迴圈，確保計時器中斷必定在它是 current 時發生）的 worker thread，接著
  `sys_kill(getpid(), SIGKILL)` 殺自己所屬的行程。以背景方式（`&`）執行，因此
  失敗時不會有人阻塞等待、不會變成測試 hang。
  驗證方式很強：若 worker 存活，行程會維持 RUNNING，測試結尾既有的
  `Processes: running=0 zombies=0` 斷言就會失敗。實測結果為
  `[killthread armed]` 出現、`[killthread SURVIVED]` 不存在、且結尾
  `running=0 / blocked=0 / sleeping=0`，證明兩個 task 都確實被終止。

### F19 [P2][正確性/架構] 停在阻塞等待中的 task 完全殺不到 —— 行程永遠停在
RUNNING，等它的人永遠阻塞（F17 誠實記錄的殘留限制，本輪根治）
- 檔案: task.c `task_kill_blocked`/`task_kill_pending`、task.h `task_block_killable`、
  process.c `process_request_kill`、timer.c `timer_sleep`、
  以及全部 10 個阻塞點（kb/pipe x2/sem/syscall/process x4/timer）
- **問題**：kill 請求只由計時器中斷裡的 `process_check_kill()` 施行，而它只看
  **當前** task。停在 `while (cond) task_block_current(ch);` 的 task 永遠不會是
  當前 task —— 它只在「被喚醒到再次阻塞」之間執行少數幾行，而且全程關中斷，
  沒有任何一個 tick 會落在它身上。舊的 `process_request_kill` 雖然會
  `task_wake_task(proc->task)`，但被喚醒的 task 條件沒滿足就直接回去繼續睡，
  等於什麼也沒發生。後果：該行程停在 PROCESS_RUNNING，`wait` 它的人永遠阻塞。
  額外兩個盲點：(a) 只喚醒 `proc->task`，多執行緒行程的其他 thread 根本沒被碰到；
  (b) 被 SIGSTOP 停住的行程（`signal_deliver` 裡的 `while (process->stopped)`）
  同樣殺不掉。
- **修法（不做 EINTR 上拋，因此不動系統呼叫 ABI）**：
  1. `task_t` 加 `kill_pending` 旗標；`task_kill_blocked(process)` 標記該行程的
     **所有** task（ready 環 + blocked 串列）並把 blocked 的那些喚醒。
     連 ready 的也標，是為了關掉「還沒被 tick 打到就先進去睡」的窗口。
  2. `task_block_killable()`（task.h 的 static inline）＝ **阻塞前後都**檢查旗標，
     成立就 `task_exit(TASK_KILL_STATUS)`，而不是回到迴圈繼續睡。
     10 個阻塞點裡有 9 個直接換成它。
     **前置檢查不是多餘的**：task 也可能在「正在執行」時被標記（第 1 點會標記
     ready 的 task），若它接著進入等待，就會停在一個沒有人會來喚醒的地方——kill
     已經發送過了，而 per-tick 檢查只看當前 task。被標記＝從此不再等待。
  3. `process_request_kill` 改呼叫 `task_kill_blocked`，取代原本只喚醒
     `proc->task` 的寫法。
  4. `timer_sleep` 是唯一不能直接用 `task_block_killable` 的：它**持有一個
     sleep slot**，必須先歸還再離開，否則那個 slot 會帶著一個指向已釋放記憶體的
     指標卡到原本的到期時間。歸還時要判斷 slot 還是不是自己的 —— 若已到期，
     `timer_callback` 早就清掉了，而且可能已經有別的 sleeper 接手，清錯會害它
     永遠睡下去。
- **層次切分**：「要殺哪個行程」的政策留在 process.c，task.c 只負責
  「標記了就在醒來時離開」的機制，因此不需要 task.c 反向相依 process.c。
- 狀態: **已修＋已驗證（單元＋端對端＋突變）**。
  - 單元（scheduler）：tests/test_task.c 加 21 個檢查（72 total）——只喚醒目標行程的
    blocked task、旗標同時落在 ready 的 task 上、其他行程完全不受影響、回傳計數、
    NULL 為 no-op、`task_kill_pending` 只反映當前 task。
  - 單元（timer_sleep 的三條 kill 路徑）：tests/test_timer.c 加 13 個檢查
    （50→63）。`task_exit` 在測試中用 `longjmp` 模擬「不會回來」，因此三條路徑
    都能確定性走到：(1) 已被標記時**根本不取 slot** 就離開（同時斷言
    `g_blocks == 0`，否則後置檢查會遮蔽前置檢查的缺失）；(2) 睡著時才被標記 →
    離開前必須歸還 slot；(3) 自己的期限已到、slot 已被**別的 sleeper** 接手時，
    不得清掉對方的 slot（否則對方永遠睡下去）。另加一條反向測試：沒有 kill 時
    sleep 必須正常返回，確保這些檢查不會自己亂觸發。
  - 端對端：新增 user/killwait.c。worker thread 卡在沒人 post 的 semaphore，
    main 卡在 `sys_thread_join()`，**兩個 task 都在睡**的時候由 fork 出來的子
    行程發出 kill。以背景執行，回歸時不會 hang 整個測試。
  - 突變測試：9 個注入全部被抓到（task.c 6 + timer.c 3；最初只做了 task.c 的 4 個，
    timer 的三條 kill 路徑是 Session 23 才補上的，補的過程又逼出兩個測試缺口）。
    其中 M1
    （`task_kill_pending` 恆回 0＝修復前的行為）同時被單元測試與 killwait
    端對端測試抓到：QEMU 跑完的結尾變成
    `Processes: running=1 zombies=1`、`Tasks: blocked=2`、`spaces=1`
    （正是那兩個睡著沒死成的 task 與它們沒被釋放的位址空間），修好後全為 0。
    這證明新測試確實有鑑別力，不是「反正沒印 SURVIVED 就算過」。
- **本輪未處理、誠實記錄**：`timer_sleep` 若被**訊號**（非 kill）提早喚醒，會直接
  回傳 0（沒睡滿），且 slot 會一直佔到原本的到期時間才由 `timer_callback` 清掉。
  這是既有行為（`process_send_signal` 一直都會喚醒 `proc->task`），本輪刻意不改：
  提早返回本身符合 POSIX `sleep()` 語意，slot 滯留是有界且會自癒的（不會被
  解參照，只是佔位），而要修就得改動 test_timer 對「睡著」的建模方式，與本輪
  主題無關。

### F20 [P1][記憶體安全] ELF 載入器全程不持有 VFS 參照 —— 載入中被 unlink 就是
核心 use-after-free（與 F11 同型，可從 shell 觸發）
- 檔案: elf_loader.c `elf_load_image`
- **問題**：`elf_load_image` 用 `resolve_fs(name)` 取得節點後，**沒有 `open_fs`**
  就一路讀下去：ELF header、每一個 program header、再把每個 segment 以 256 bytes
  為單位讀進來——中間會經過很多次計時器中斷而被排程。此時另一個行程若把該檔
  `rm` 掉，RAMFS 的 `ramfs_remove_node` 因為參照為 0 而放行，`kfree(node->ptr)`
  ＋`kfree(node)`；載入器手上的 `file` 立刻變成懸空指標，下一次 `read_fs(file, ...)`
  會**從已釋放的堆積記憶體讀出 `node->read` 函式指標並呼叫它**。這正是 F11 的
  模式（控制流層級的核心 UAF）。
- **為什麼現在才成立**：Session 6 的 FEAT1 讓可執行檔改走 VFS 查找之後，程式不再
  限於唯讀的內嵌 RAMFS 映像。內嵌程式是 `ramfs_create_static_file`（非
  RAMFS_DYNAMIC，本來就刪不掉），但 `cp hello prog` 產生的複本、FAT16 與 DiskFS
  上的檔案全都刪得掉——三者都有開啟計數，卻沒有人替載入器取得那個參照。
- **時序窗口有多大（不誇大）**：從 RAMFS 載入時，每次讀取只是 memcpy，整個載入
  只要幾微秒，遠短於一個 10ms 的 timer tick，實務上幾乎撞不到。**但從 FAT16 或
  DiskFS 載入就完全不同**：每個 256-byte chunk 都要走 ATA PIO 讀磁區，載入一支
  程式跨越多個 tick，窗口是毫秒級而非微秒級——而「從其他檔案系統執行程式」正是
  FEAT1 加進來的能力（測試裡就有 `cp hello fat/hello` 然後執行 `fat/hello`）。
  所以這不是純理論問題，但也不是隨手就能撞到的；修它的理由是「安全性不該建立在
  時序假設上」，而不是「很容易被利用」。
- 修法：`elf_load_image` 取得節點後 `open_fs(file)`，載入結束 `close_fs(file)`。
  為了讓「取得/釋放」不會在任何一條失敗路徑上漏掉，把原本有 6 個 return 的主體
  拆成 `elf_load_from_node()`，外層只剩單一出口成對呼叫。三個檔案系統的 unlink
  在檔案開啟時都會拒絕，所以安全性來自這個參照，而不是任何時序假設。
- 狀態: **已修＋已驗證**（tests/test_elf.c 的 open/close 配對測試；突變 E1「不取
  參照」與 E1b「取了不放」分別被抓到 5 個與 6 個失敗）。

### F21 [P2][安全性/TOCTOU] program header 驗證過後又重新讀取，中間可被改寫 ——
未經驗證的 p_vaddr 會直接送進不做任何範圍檢查的 paging_map_user_page
- 檔案: elf_loader.c `phdr_in_user_range`、`validate_segments`、`load_segments`
- **問題**：`validate_segments()` 把每個 program header 讀出來檢查通過後，
  `load_segments()` **再從檔案讀一次同樣的 header** 才拿去用。兩次讀取之間載入器
  會被排程（見 F20），檔案內容可被另一個行程改寫——F20 的參照只擋得住 unlink，
  擋不住覆寫。於是「驗證時合法、使用時不合法」的 p_vaddr 會被送進
  `paging_map_user_page()`，而那個函式**自己完全不做範圍檢查**。
- **實際影響範圍（誠實評估）**：`user_pte()` 只對低 0-4MB 與 32-36MB 兩個視窗回傳
  頁表項，其餘一律 NULL，所以核心位址映射不進去。可達的傷害是讓該行程自己的
  0x0-0x300000（含 null page）變成可存取，削弱它自身的保護，不影響核心。因此列
  P2 而非 P0——但這條防線本來就是唯一的一條，不該靠下游巧合。
- 修法：把邊界檢查抽成 `phdr_in_user_range(phdr, file_length)`，`validate_segments`
  與 `load_segments` **共用同一份**，後者在使用前重驗一次。每個比較都寫成不會
  溢位的形式（先確立 `p_vaddr < USER_STACK_BOTTOM`，再用
  `p_memsz <= USER_STACK_BOTTOM - p_vaddr`）。
- 狀態: **已修＋已驗證**（tests/test_elf.c 直接模擬「第二次讀取時被改寫」；
  突變 E2「移除第二次檢查」被抓到）。

### F22 [P0][安全性/DoS] ramfs_write 的容量倍增溢位守衛差一步 —— 使用者程式可讓
**整台機器凍結**（無窮迴圈，且全程關中斷）
- 檔案: ramfs.c `ramfs_write`
- **問題**：幾何成長（PERF2）從 64 開始倍增直到容量覆蓋 `offset + size`：
  ```c
  while (new_cap < new_length) {
      if (new_cap > 0x80000000U) { new_cap = new_length; break; }
      new_cap *= 2;
  }
  ```
  當 `new_cap == 0x80000000` 時，`> 0x80000000` **不成立**，於是執行 `*= 2` →
  0x100000000 截斷成 **0**。之後每一輪都是「0 < new_length 成立、0 > 0x80000000
  不成立、0 *= 2 還是 0」→ **無窮迴圈**。守衛必須在乘法**之前**擋下，改成
  `new_cap > 0xFFFFFFFFU / 2`。
- **可觸發性（這是 P0 的原因）**：`sys_seek` 允許 offset 最大到 0x7FFFFFFF，接著
  寫 2 bytes 就得到 `new_length == 0x80000001` —— 剛好需要跨過倍增上限。而
  `int $0x80` 的 IDT 閘門是 `0xEE`，**type nibble = E 是 interrupt gate**，CPU 進入
  時自動清 IF，所以整個系統呼叫全程關中斷。於是這個迴圈會在**關中斷**下永遠轉：
  收不到 timer tick、跑不到排程器、鍵盤也沒反應——**整台機器停機**，連 Ctrl+C 都
  救不了。與 F2、F14 同一級（使用者程式可讓核心整台當掉），但成因完全不同：那兩個
  是未驗證的指標，這個是純算術的 off-by-one。
- 修法：守衛改為在倍增前檢查會不會繞回（`new_cap > 0xFFFFFFFFU / 2`）。之後
  `kmalloc(0x80000001)` 自然失敗、fallback 的精確配置也失敗，於是乾淨回傳 0。
- **為什麼前幾輪沒發現**：PERF2 是 Session 2 加的，Session 13 用 `make bench` 量過
  它的**效能**（重新配置次數 N→log2(N)），但從來沒有人測過它的**算術邊界**。
  「量過效能」被誤當成「驗證過正確性」。
- 狀態: **已修＋已驗證**。單元測試 tests/test_ramfs.c 直接呼叫
  `write(0x7FFFFFFF, 2 bytes)`，並**為整個測試檔裝上 30 秒看門狗**，讓這個
  回歸表現為失敗而不是把 `make test` 掛住（突變 R1 把守衛改回去，看門狗實測開火）。
  端對端 user/bigseek.c 在 QEMU 中**實際執行這個攻擊**：`[bigseek arming]` →
  `[bigseek refused]` → `[bigseek survived]`，能跑到最後一行就證明核心還活著、
  還在排程它。

### F23 [P3][正確性] FAT16 的寫入即使「一個位元組都沒寫進去」也照樣把檔案長度
推到 seek 的位置 —— 檔案於是宣稱一段自己的叢集鏈根本背不起來的長度
- 檔案: fat16.c `fat16_vfs_write`
- **問題**：F9 的修法是「只記錄真正寫進去的位元組」：
  ```c
  uint32_t written_end = offset + written;
  if (written_end > <目前長度>) { 更新目錄項與 node->length; }
  ```
  當時的註解寫著「寫入的位元組從 offset 起連續，所以 offset + written 就是資料
  的真正結尾」——**這個等式只有在 written > 0 時才成立**。當 `written == 0`
  （寫入的起點落在叢集鏈根本延伸不到的地方，也就是磁碟空間用盡時），
  `offset + written` 只是呼叫者要求的那個 offset，它不標示任何東西的結尾；
  把它記下來等於**憑空把檔案長大到 seek 的位置，卻一個位元組都沒存**。
- **後果（不誇大）**：檔案之後宣稱一個自己的叢集鏈背不起來的長度。`stat` 報出
  假的大小；讀取會把最後一個真實叢集裡 EOF 之後的內容當成檔案資料交出去。
  那些位元組在配置時已被 `fat16_alloc_cluster` 清零過，**所以不會洩漏別的檔案
  的資料**——這是正確性問題，不是資訊洩漏。
- **可觸發性**：/fat 這個磁區只有 32 KB，而 `sys_seek` 允許 offset 到
  0x7FFFFFFF；把磁區寫滿、再對一個既有檔案 seek 過尾端寫入即可，全部都是 ring-3
  的普通操作。與 F22 是同一種形狀（seek 過尾端再寫入），只是打在另一個檔案系統上。
- 修法：用 `if (written > 0)` 包住長度更新——什麼都沒寫，就什麼都不改。
- **為什麼 F9 那輪沒抓到**：F9 的測試（tests/test_fat16.c「write past end of
  volume」）測的是 `0 < written < size` 的部分成功情形，`written == 0` 這個邊界
  從來沒被測過。修一個 bug 時只測自己當時想到的那條路徑，隔壁那條就會留下來
  ——與 CAP13 的 R7（兩段清零互為備援）同型的教訓。
- 狀態: **已修＋已驗證**。單元測試 tests/test_fat16.c「write that stores
  nothing」：把磁區寫滿 → 對只有一個叢集的檔案 seek 到 4096 再寫 → 寫入必須回 0、
  `node->length` 必須還是 4、**重新 finddir 出來的目錄項也必須是 4**（只在 RAM
  裡修對、沒寫回目錄項的話下次查找就會錯）、讀回必須恰好只有 4 bytes。
  突變 N1（把 `written > 0` 拿掉，也就是修復前的程式碼）**只被這個新測試殺掉**，
  N2（退回 F9 的 bug）被舊的「write past end of volume」殺掉，N3（拿掉「只增不減」
  比較）被「partial overwrite」殺掉——三個各由不同測試抓到，證明新測試補上的是
  先前確實沒有的鑑別力。端對端 user/fatgrow.c 在 QEMU 中從 ring 3 實際跑完整個
  序列，並在結束前 unlink 兩個檔案把磁區還原給後面的 FAT 斷言。

### HARD1 [強化] resolve_fs 不檢查「中途的組件必須是目錄」，與姊妹函式
resolve_parent_fs 不一致
- 檔案: fs.c `resolve_fs`
- **觀察**：`resolve_parent_fs` 每往下一層都檢查 `parent->flags != FS_DIRECTORY`，
  但 `resolve_fs` 沒有。它靠的是「檔案節點的 finddir 是 NULL，所以下一次查找自然
  回 NULL」——也就是說，**這條正確性變成每個後端的性質，而不是這個解析器自己的
  性質**。而 procfs 的 `proc_finddir` 完全忽略傳進來的 node 參數（`(void)node`），
  正是那種「不看自己是誰」的後端。
- **誠實評估：目前不可觸發**。所有後端的檔案節點 finddir 都是 NULL，這條路徑走不
  到。這不是一個被修掉的 bug，是縱深防禦。之所以值得補：同一個路徑在兩個進入點會
  有不同意義（`resolve_fs("/odd/deep")` 找得到，`unlink_fs("/odd/deep")` 被拒），
  而這種「兩個解析器對同一個字串意見不同」正是 F8 那類錯誤的溫床。
- 修法：在 resolve_fs 的「還有路徑要走」分支加上與 resolve_parent_fs 完全相同的
  `node->flags != FS_DIRECTORY` 檢查。
- 狀態: **已加＋已驗證**。tests/test_fs.c 用一個「flags 是 FS_FILE 卻帶著完整目錄
  操作」的 mock 節點來隔離這條檢查——真實後端造不出這種節點，所以只有 mock 能證明
  擋下來的是 fs.c 自己而不是後端。突變 M8（把檢查拿掉）被抓到。`make test` 全綠，
  procfs / fat16 / diskfs 的巢狀路徑都不受影響。

## 驗證能力建設（Session 10）

### CAP1 補上 GRUB/ISO 開機路徑的測試（原本完全沒被驗證過）
- 檔案: Makefile `test-iso`
- 先前 `make test` 只有 test-ata-absent / test-boot / test-shell，**全部走 QEMU 的
  `-kernel` multiboot 載入器**，完全繞過 GRUB。也就是說 README 主打、要人拿去燒
  ISO 在 VM/實機開機的那條路徑，**一次都沒被驗證過**。
- 新增 `test-iso`：建出 ISO、以 `-cdrom` 經 GRUB 開機，並同時掛上 ATA 磁碟讓
  磁碟型檔案系統也在這條路徑下被涵蓋。斷言包含
  **`Initialized PMM from Multiboot memory map.`**——這一條特別重要，它證明 GRUB
  真的傳入了合法的 multiboot 資訊結構（含記憶體映射），正是 GRUB 路徑與
  `-kernel` 路徑的實質差異——以及三個檔案系統掛載與 shell 啟動。
- ISO 工具鏈（grub-mkrescue/xorriso/mtools）屬選用相依，缺少時**印出 SKIP 並通過**
  而非失敗，以免 `make test` 對所有人硬性要求這些套件。
- 實測：ISO 建置 <1s、8 秒內開到 shell，DiskFS/FAT16/procfs 全數掛載成功。

### CAP2 建立原生單元測試框架（tests/），並用突變測試驗證它真的有效
- 檔案: tests/test.h、tests/test_{utils,fs_path,pmm,heap}.c、Makefile `unit`
- 動機：先前**完全沒有單元測試**，214 條 grep 斷言全靠 QEMU 送鍵盤跑端對端，
  單次約 4 分鐘且時序脆弱；任一斷言失敗都難以定位。
- 作法：把純邏輯模組以 `-m32`（比照核心的指標寬度）編給 host 直接呼叫。
  `-fno-builtin` 避免 gcc 把我們自己的 memcpy/memset 實作轉成對自身的呼叫、
  也避免受測呼叫被換成內建版本。heap 以 stub 取代 pmm（真 pmm 回傳的是不可
  解參照的假指標）。全部 4 個套件約 50,500 個檢查，**執行時間 <1 秒**。
- 涵蓋重點刻意對準「端對端測不到、且我改過」的地方：memcpy/memset 的
  **每一種對齊組合與長度**（含越界守衛位元組）、vfs_resolve_path 的
  **長度邊界**（F8 修的那個）、pmm 的 frame 0 保留與錯誤 free、heap 的
  分割/重用/合併。
- **關鍵：用突變測試證明這套測試有牙齒**。新測試第一次就全綠是可疑的，所以我
  故意注入 5 個 bug 驗證：memcpy 尾端少一 byte、memset 字組填充丟低位元組、
  fs 路徑溢位靜默忽略（即 F8 的舊 bug）、pmm 發出 frame 0、heap 的 kfree 不標記
  free。結果 **4 個被抓到**，證明測試確實在把關；第 5 個沒被抓到，反而讓我
  找到一個真 bug（見下方 F18）。
- 備註：`make test` 現在先跑 `unit`（<1 秒）再跑 QEMU 各階段，邏輯錯誤會在
  4 分鐘的模擬開始前就先爆出來。

### F18 [P3][正確性] pmm_init_region 重新保留 frame 0 時沒有修正計數
——**由 CAP2 的突變測試間接發現**
- 檔案: pmm.c `pmm_init_region`
- 迴圈會把區間內的 frame 逐一標為 free 並 `used_blocks--`；若區間起點是 0，
  frame 0 也會被釋放並計數減一。迴圈結束後的 `mmap_set(0)` 把它重新標為已用，
  **但沒有把 used_blocks 加回去**。結果 `pmm_get_free_blocks()` 比實際可配置量
  多一個。實測：回報 256，實際只配置得出 255，且耗盡後計數仍停在 1。
- 發現經過：我原本的「frame 0 不得被配置」測試用突變（把搜尋起點由 1 改為 0）
  驗證時**沒有失敗**，追查後發現是因為 `mmap_set(0)` 提供了第二層防護，所以那個
  突變打不到。但順著這條線讀下去，就看到計數沒補回來的問題。**兩輪人工審查都
  漏掉了它。**
- 可觸發性：實際核心不會踩到——`free_usable_subrange` 會把起點夾到 `free_start`
  （核心結尾之上，約 1MB+），所以 frame 0 從不落在被釋放的區間內。屬**潛在**
  缺陷而非現行 bug，但這是公開 API 的正確性問題，未來新增記憶體區間就可能中招。
- 修復: 改成 `if (!mmap_test(0)) { mmap_set(0); used_blocks++; }`，只在原本為
  free 時才重新保留並補回計數。
- 狀態: **已修＋已驗證**。新增 `test_free_count_is_honest`：以「一直配置到失敗」
  的實際數量對照 `pmm_get_free_blocks()` 的回報值。修復前該測試失敗
  （got 255 / want 256），修復後通過。

### CAP3 FAT16 單元測試：補上覆蓋最薄的叢集鏈邏輯，並**關閉 F9 的驗證缺口**
- 檔案: tests/test_fat16.c、Makefile
- FAT16 整個檔案系統就是一個位元組陣列，因此最適合原生測試。端對端測試只碰得到
  三個各自塞得進單一叢集的小檔案，**叢集鏈的延伸與走訪幾乎完全沒被涵蓋**。
- 測試直接連結**核心實際內嵌的同一份映像**（fat16_image_embed.c），不是手工湊的
  近似品；pmm 以 host 記憶體 stub（fat16 只用到 pmm_alloc_blocks）。
  每個測試都重新 `fat16_install` 一份乾淨映像，彼此不互相污染。
- 涵蓋：掛載與根目錄列舉（含 8.3 轉小寫、"." / ".." 過濾）、巢狀目錄、
  跨叢集的 3000-byte 寫入/讀回、**刻意選在叢集邊界上下的偏移讀取**
  （1/511/512/513/1023/1024/2047）、部分覆寫不得波及鄰近資料、
  建立/刪除/叢集回收、開啟中不得 unlink、8.3 名稱長度限制。
- **關閉 F9 的驗證缺口**：F9（叢集耗盡時檔案長度記成請求值而非實際寫入值）
  當初明確記錄為「無法測試」——在 QEMU 裡灌爆映像會破壞同次開機後續所有 FAT
  斷言。單元測試每次重新掛載，因此可以安全地寫入 40000 bytes 撐爆磁碟區，
  驗證 `written < 請求量` 且 `node->length == written`，並把宣稱持有的資料
  完整讀回比對。
- **突變測試證明有效**：注入 5 個 bug，**全部 5 個被抓到**——包含 F9 與 F12 的
  回歸（把長度改回 `offset + size`、移除 unlink 的開啟檢查）、讀取時叢集鏈
  多走一格、8.3 長度放寬、readdir 不再過濾點目錄項。
- 規模：37,351 個檢查，與其他套件合計 <1 秒。

### CAP4 DiskFS 單元測試：把「解析不受信任磁碟資料」這條信任邊界測起來
- 檔案: tests/test_diskfs.c、Makefile
- DiskFS 是唯一會**解析自己沒有產生的資料**的檔案系統：superblock 與目錄項都是
  從磁碟讀進來的，內容可以是任何東西。`diskfs_mount()` 因此是一條信任邊界，
  而它大部分的驗證（父鏈走訪、重名偵測、長度上限）**從 shell 完全構不到**——
  你沒辦法叫執行中的核心去掛載一顆刻意損壞的磁碟。
- 作法：用 RAM 陣列 stub 掉 ATA 的四個函式（因此不連結 ata.c），先用正常 API
  建立合法檔案系統，再直接竄改磁碟上的特定位元組，驗證掛載必須拒絕。
- 涵蓋的拒絕案例：magic 錯誤、版本錯誤、checksum 錯誤、宣稱的磁區數與裝置不符、
  空白 superblock；目錄項方面：自己當自己的父目錄、**父鏈成環**、把檔案當作父
  目錄、父索引越界、同一父目錄下重名、檔案長度超過上限、目錄卻有長度、
  未知的 entry 型別、used 旗標非 1、名稱未終止、名稱為空。
- 功能面另外涵蓋：格式化/掛載、寫入讀回與重新掛載後仍在（證明真的寫到磁碟）、
  **跨磁區寫入**、**寫入造成的空洞必須補零**、檔案大小上限、巢狀目錄、
  非空目錄不得 rmdir、開啟中不得移除。
- 規模：943 個檢查。

#### 這一輪測試設計上的兩個修正（都由突變測試逼出來）
1. **原本的 magic 測試其實在測 checksum**：竄改 magic 會**連帶讓 checksum 失效**
   （checksum 是對含 magic 的九個欄位做 XOR），所以真正擋下它的是 checksum 檢查。
   修正：在測試中重算並修好 checksum，讓每個欄位的檢查都被**獨立**驗證。
   修正後「停用 magic 檢查」的突變即被抓到。
2. **原本的補零測試打不到目標**：對**全新檔案**寫入時，該路徑本來就會把整個磁區
   緩衝區 memset 為 0，補零是多餘的。改成模擬真實的資訊洩漏情境：
   slot 被前一個檔案寫滿 → unlink → 新檔案重用同一 slot（磁碟上仍是舊資料）→
   先寫少量讓磁區「部分存活」→ 再寫到更後面，中間形成空洞。

#### 觀察：`diskfs_write_slot` 的兩段清零互為備援，因此無法被個別覆蓋
- 突變測試顯示：單獨停用「明確補零」或單獨把 `else` 分支的整片 memset 改成填
  0xEE，**兩者都無法被任何黑箱測試觀察到**——因為它們互相掩護（前者處理讀回
  磁區中的空洞，後者處理全新磁區）。**同時**停用兩者才會被抓到（已實測驗證）。
- 結論：這是**刻意保留的防禦性冗餘**，不是死碼也不是 bug；但誠實記錄「沒有任何
  測試能個別覆蓋這兩行」。測試守住的是**性質**（新檔案絕不得讀到前一個檔案的
  資料），而非特定某一行，這也正是應該守住的東西。

#### 另一項修正：我的測試假設錯了，程式碼是對的
- 我原本斷言「裝置消失後 `diskfs_get_root_node()` 應回傳 NULL」。實測失敗後追查
  發現：`diskfs_mount()` 在「無裝置」或「有開啟中的檔案」時會**提前返回**，
  刻意不清掉既有掛載狀態。這是正確的——而且是**被依賴的**：
  `test_open_blocks_removal` 正是靠「有開啟檔案時拒絕重新掛載、但保留既有掛載」
  才能繼續使用檔案系統；若改成一律 unmount，開啟中的節點會被懸空。
  已把測試改為斷言真正的契約，並在註解中寫明這個區別。

## 功能擴充（已實作）

### FEAT1 執行檔改走 VFS 查找：可從任何已掛載的檔案系統執行程式
- 檔案: elf_loader.c `elf_load_image`
- 原本用 `ramfs_find_file(name)`，只查得到內嵌在 RAMFS 的程式；位於 /disk 或
  /fat 的執行檔無法 exec/spawn。實際上 ELF 載入器**其餘部分早就是檔案系統
  無關的**（讀檔一律走 `read_fs()` 與 `node->length`），限制只在「查找」這一步。
- 改為 `resolve_fs(name)` 並檢查 `flags == FS_FILE`（目錄不是可執行映像）。
- **刻意不改成相對於工作目錄解析**：不含前導 '/' 的名稱仍從根目錄解析，與原本
  RAMFS-only 的行為一致。若改成 cwd 相對，`cd fat` 之後執行 `cat`（位於 /cat）
  就會失敗——ush 的測試正是這樣用的，會直接壞掉。這是保守相容的關鍵取捨。
- 狀態: **已實作＋已驗證**。測試腳本新增：`cp hello fat/hello`（把 8.9KB 的
  ELF 複製進 FAT16 映像）→ `fat/hello`（**從 FAT16 載入並執行**）→
  `rm fat/hello`。實測日誌顯示該程式完整輸出三行、"Hello from user space!"
  出現次數由 2 增為 3、且無任何 `exec:` 錯誤，證明真的是從 FAT16 讀出 ELF 並
  執行，而非退回 RAMFS。
  註：DiskFS 每檔上限 4 磁區（2048 bytes）放不下 8.9KB 的 ELF，故以 FAT16
  （資料區約 31KB）驗證；跨檔案系統的載入路徑兩者共用同一段程式碼。

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

### CAP5 IPC 單元測試（pipe / sem）：把阻塞邏輯與環狀緩衝邊界測起來
- 檔案: tests/test_pipe.c、tests/test_sem.c、pipe.c、sem.c、Makefile
- pipe 是阻塞式環狀緩衝，兩個性質從 shell 都很難測：環狀索引的 wrap 只有在
  read_pos/write_pos 跨越 PIPE_BUF_SIZE 時才會現形（shell 每次只搬幾個 bytes），
  而阻塞轉換在 shell 裡完全無法單步。
- **前置障礙**：pipe.c/sem.c 的 `save_irq_disable` 內含 `cli`/`sti`，在 host
  ring 3 執行會 SIGSEGV（已實測）。用一個核心正式建置**永不定義**的
  `HOSTED_TEST` 巨集把這兩個特權指令在測試建置下編成 no-op；核心建置的 codegen
  完全不變（`flags=0` 初始化會被 write-only 輸出運算元消除）。
- **測試阻塞邏輯的手法**：用一個腳本化的 hook 取代 `task_block_current`——當程式
  碼阻塞時，hook 精確地模擬對端 task 會做的事（寫入資料、關閉、或排空緩衝），
  於是阻塞迴圈的退出條件（資料到達／EOF／空間釋放／broken pipe）能在**單執行緒**
  上確定性地被走到。沒安裝 hook 時，一次非預期的阻塞會被當成失敗並強制中止迴圈
  （而非 hang）。
- pipe 涵蓋：寫入讀回、部分讀取、零長度/NULL、**環狀緩衝 wrap（連續推 8 輪
  3000-byte，強制 read_pos/write_pos 多次跨越 4096 邊界並比對每個位元組）**、
  EOF（含殘留資料先排空）、broken pipe、參照計數與「兩端都關才 kfree」、
  以及三種腳本化的阻塞轉換（讀阻塞→寫入者供資料 / 讀阻塞→寫入者關閉見 EOF /
  寫阻塞於滿→讀取者排空）。sem 涵蓋：id 驗證、未初始化拒絕、計數增減、
  以及腳本化的「wait 阻塞於 0 → post 釋放」與 re-init 重設值。
- **突變測試**：pipe 注入 6 個 bug（環狀模數錯誤 ×2、讀取不再阻塞而丟失 EOF 等待、
  寫入忽略 broken pipe、pipe 永不釋放、以及一個**我自己標為 benign** 的
  「close_read 不再喚醒 writer」），**5 個被抓到、1 個 benign 者 PASS**——後者
  在 stub 化排程器下不可觀察，且不影響正確性（writer 甦醒後會自行重檢
  readers==0），是誠實的預期結果，不是測試漏洞。sem 注入 4 個 bug（wait 不阻塞、
  wait 不遞減、post 不遞增、init 接受負值），**全部 4 個被抓到**。
- 規模：pipe 68 檢查、sem 32 檢查。

#### 附帶記錄（技術債）→ **已於 Session 15 處理（REFACTOR1）**
`save_irq_disable`/`restore_irq` 這一對完全相同的函式原本在 **7 個檔案**重複。

### REFACTOR1 把重複 7 次的 irq save/restore 抽成共用的 irq.h（已證明 codegen 不變）
- 檔案: 新增 irq.h；timer/task/pipe/sem/process/ata/kb 各刪除本地定義、改
  `#include "irq.h"`
- irq.h 以**與原本完全相同的函式名**（save_irq_disable/restore_irq）定義為
  `static inline`，因此 102 個呼叫點**零改動**；並統一帶上 `HOSTED_TEST` 守護
  （核心正式建置永不定義），使這 7 個模組全部變得可原生單元測試。
- **codegen 等價性：已實測證明，非僅宣稱**。以 `git show HEAD:<file>` 取出重構前
  版本、分別編出 7 個 .o，與重構後的 .o 用 `cmp` 逐一比對——**全部 7 個 object
  檔位元組完全相同**（scratchpad/codegen_check.sh）。`static` vs `static inline`
  在 -O2 下同樣被 inline，`flags=0` 初始化被 write-only 輸出運算元消除，故機器碼
  一致。
- 動機與時機：這對函式的重複是一直存在的技術債，但先前不敢動——直到累積了 8 個
  模組的單元測試 + 完整端對端測試，重構的回歸風險才降到可接受。這正是「先建立
  驗證能力、再安全重構」的順序。
- **解鎖的能力（記錄為後續機會）**：timer/task/process/ata/kb 現在也繼承了
  HOSTED_TEST 守護，未來可原生單元測試。其中 timer 的 `tick_reached`
  （`(int32_t)(current - deadline) >= 0` 的 wrap-around 比較）與睡眠佇列管理
  是值得測的純邏輯，但完整測 timer 需為 process_*/schedule/task_* 準備一批 stub，
  屬另一個聚焦工作，本輪不順手做。
- 狀態: **已完成＋已驗證（build + make test 全綠、7 個 .o codegen 位元組相同）**。

### CAP6 timer 單元測試：wrap-around tick 比較 + 睡眠佇列（用上 REFACTOR1 解鎖的能力）
- 檔案: tests/test_timer.c、io.h、Makefile
- 這是 REFACTOR1 把 HOSTED_TEST 守護延伸到 timer 之後、第一個實際做出來的
  timer 原生測試。最有價值的目標是 `tick_reached`：
  `(int32_t)(current - deadline) >= 0`——用**有號差**比較，才能在 32-bit tick
  計數器繞回 2^32 時仍正確。naive 的無號 `current >= deadline` 會在計數器繞過
  deadline 的瞬間就誤觸發，這個 bug 要 100Hz 下約 497 天 uptime 才會現形，
  從 shell 永遠測不到，卻是真實存在的。
- **測試手法**：`timer_callback`（推進 tick、喚醒到期者）是 static，測試用核心
  同樣的方式捕捉它——stub 的 `register_interrupt_handler` 記下 `timer_install()`
  註冊的 handler，測試再呼叫它模擬計時中斷。`timer_ticks` 是全域，直接設成
  `0xFFFFFFFE` 就能測繞回。schedule/process_* 都 stub 成 no-op。
- **前置：io.h 的 port I/O 也加 HOSTED_TEST 守護**。`timer_install()` 會 `outb`
  設定 PIT，特權指令在 host 會 fault。比照 irq.h 的做法把 outb/inb/outw/inw 在
  測試建置編成 no-op；**已實測證明對核心 codegen 中性**——7 個 include io.h 的
  object 檔（ata/idt/isr/kb/rtc/timer/vga）重構前後 `cmp` 位元組完全相同
  （scratchpad/codegen_io.sh）。這也順帶解鎖了 ata/kb 等模組未來的可測性。
- 涵蓋：install 捕捉 handler、sleep 引數驗證（0 為 no-op 且不佔 slot、
  超過 0x7FFFFFFF 拒絕、無 current task 拒絕）、**在確切 deadline 才喚醒**、
  **繞過 2^32 的 deadline 正確**（設 timer_ticks=0xFFFFFFFE、sleep 3、驗證在
  0xFFFFFFFF/0x00000000 不觸發、到 0x00000001 才觸發）、多個獨立 deadline 依序
  喚醒、睡眠表 16 格滿載拒絕。50 檢查。
- **突變測試**：注入 6 個 bug，**全部 6 個被抓到**——關鍵是「tick_reached 改成
  naive 無號比較」被 wrap-around test 抓到（正是這套測試存在的理由）；另含
  strict `>`（晚一 tick）、deadline off-by-one、放寬時長上限、callback 不釋放
  slot、zero-tick 不再 no-op。
- **測試健壯性修正**：測試的 `reset()` helper 靠 sleeping_count 歸零來排空舊
  sleeper，「callback 不釋放 slot」那個突變會讓它無窮迴圈（實測時一度 hang，
  靠 pkill 才解開）。已為排空迴圈加上 64 次上限，使該突變乾淨失敗而非 hang。

### CAP7 task 排程器單元測試：ready-ring 與 blocked-list（含 F10 的 task_wake_task）
- 檔案: tests/test_task.c、Makefile
- 排程器的 ready 環狀串列與 blocked 串列是純指標邏輯，錯一個 link 更新就會讓
  run queue 損壞、而症狀往往是很久之後才出現的 hang。**context switch 本身是
  組語（switch_task），stub 成 no-op** 之後，串列操作就完全可測。
- 這也讓 **F10 新增的 `task_wake_task`（依身分喚醒）第一次獲得直接覆蓋**——先前
  它只被 job-control 的端對端測試間接測到。
- 手法：stub switch_task / set_kernel_stack / paging_* / pmm（RAM 池）/
  terminal_writestring；透過設定 `current_task` + 呼叫 `task_block_current`
  把選定的 task 放進 blocked list（switch 是 no-op，函式正常返回）。
- 涵蓋：tasking_init 建環、block 把 task 移出 ready 環、**FIFO 喚醒順序**
  （task_block_current 尾插、task_wake_one 取頭 → 最舊者先醒，能區分 LIFO）、
  **task_wake_task 依身分從串列中段/頭/尾移除**、not-blocked 與 NULL 為 no-op、
  wait channel 選擇性（wake_one/all 只喚醒相符 channel）、以及 block/wake 的
  **state 轉換**（BLOCKED↔READY）與 wait_channel 清除。51 檢查。
- **突變測試（Python 字面替換，因多行 pattern 經 bash 傳 sed/perl 太脆弱）**：
  注入 5 個 bug，**全部 5 個被抓到**：blocked list 改頭插（LIFO，破壞 FIFO）、
  wake_one 忽略 channel、wake_task 不依身分、wake_task 不 unlink、add_ready_task
  不設 TASK_READY。
- **突變測試又補上一個測試缺口**：第一版測試沒斷言 `state` 欄位，導致
  「add_ready_task 不設 TASK_READY」的突變 PASS（未被抓到）。這是真的缺口——
  喚醒後若仍是 BLOCKED，之後對它 block 會因守衛提前返回。已在測試補上
  block→BLOCKED、wake→READY 且 wait_channel 清除的斷言，該突變隨即被抓到。

### CAP8 rtc 解碼單元測試（附一個行為保持的可測性重構）
- 檔案: rtc.c（抽出 `rtc_decode`）、tests/test_rtc.c、Makefile
- RTC 真正的工作是把 CMOS 暫存器 bytes 解碼：BCD vs binary、12 小時制的 PM 標誌
  與 12→0 / 12→noon 特例。這段邏輯很 fiddly，而 QEMU 永遠以固定的 binary/24h
  時間啟動，所以 shell 的 `date` 測試只走得到其中一條路徑。
- **可測性重構（行為保持）**：`cmos_read` 讀真實 port（HOSTED_TEST 下是 no-op、
  回 0），無法注入值。把純解碼邏輯從硬體讀取分離，抽出
  `rtc_decode(out, sec, min, hour_raw, day, month, year, regB)`；`rtc_read` 讀完
  暫存器後呼叫它。行為完全不變——由端對端 `date` 測試驗證（重構後仍輸出兩次
  `2020-01-01`，rtc_read 行為未變）。
- 測試以 `#include "../rtc.c"` 取得 static 的 rtc_decode 與 bcd_to_bin（測 static
  函式的標準做法，rtc.c 相依極少、port I/O 由 HOSTED_TEST 編掉）。
- 涵蓋：bcd_to_bin（0x00/0x09/0x10/0x42/0x59/0x99）、binary+24h、BCD+24h（同一
  時刻兩種編碼）、binary+12h 的 AM（12→0、1、11）與 PM（1→13、11→23、
  **12 PM→12 noon 不是 24**）、BCD+12h（3 PM→15、12 PM→12、12 AM→0，且
  日期/年也 BCD 解碼）、世紀 +2000。35 檢查。**這些正是真實硬體會用、但 QEMU
  從不觸發的路徑**。
- **突變測試**：注入 6 個 bug，**全部 6 個被抓到**：bcd 乘數錯（×16）、PM 轉換
  漏 %12（12 PM→24）、12 AM 不再映射為 0、世紀基底錯（1900）、BCD 小時解碼丟失
  PM 位、完全不當作 BCD。

### CAP9 process 環境變數單元測試（用 --gc-sections 攻克高耦合模組）
- 檔案: tests/test_process_env.c、Makefile
- 目標: `process_setenv`（overwrite-or-append，帶長度上限）、`process_getenv`
  （查找 + bounded 截斷複製）、以及純的 `env_copy`（`max - 1` 的 bounded copy
  是經典 off-by-one）。shell 的 `export`/`printenv`/`$VAR` 端對端測試只存一個
  短變數，所以長度驗證、ENV_MAX 上限、overwrite 路徑、截斷複製全沒被走到。
- **技術關鍵：用 `--gc-sections` 把高耦合模組的 stub 面縮到最小**。process.c
  相依 30+ 外部符號（paging/elf/fs/pipe/syscall/task/terminal），過去被視為
  「太耦合、不值得單元測試」。但 env 函式的傳遞閉包只到
  `process_get_current` → `task_get_current`。以 `#include "../process.c"` +
  `-ffunction-sections -fdata-sections -Wl,--gc-sections` 編譯，連結器丟掉所有
  沒被 main 觸及的函式（fork/exec/signal…）與其相依，**stub 面縮到只剩
  task_get_current + strlen/strcmp/memcpy 三四個**。這開啟了測試其他高耦合模組
  純邏輯部分的路徑。
- **踩到的坑**：syscall.h 的 `SEEK_SET` enum 與 test.h→stdio.h 的 `SEEK_SET`
  巨集衝突。解法是把 `#include "test.h"` 移到 `#include "../process.c"` 之後，
  讓 enum 先於巨集處理（已在測試檔註解說明）。
- 涵蓋：set/get、缺鍵回 -1、第二個相異鍵 append 且不影響第一個、overwrite 不
  增長 table 且較短值不留殘尾、NULL/空鍵/過長鍵值拒絕、最長可接受長度
  （ENV_*_MAX - 1）、ENV_MAX 滿載拒絕但仍可 overwrite 既有鍵、getenv 對小緩衝
  截斷（size 4→3 字元、size 1→空字串仍 NUL 結尾）並回傳截斷後長度。47 檢查。
- **突變測試**：注入 6 個 bug，**全部 6 個被抓到**：env_copy off-by-one（寫滿
  max、沒留 NUL 空間）、無 ENV_MAX 上限、無長度驗證、接受空鍵、setenv 永不
  overwrite（總是 append）、getenv 回 0 而非值長度。

### CAP10 syscall 使用者指標驗證單元測試（安全前線，F2/F14 同類）
- 檔案: tests/test_syscall_valid.c、Makefile
- 目標: `user_buffer_valid`（每個 raw 使用者指標都會過這關）與 `user_string_valid`，
  外加純的 `alloc_fd`。F2/F14 兩個核心 DoS 都是這類邊界檢查缺失；而安全關鍵的
  「指標接近使用者範圍頂端 + 巨大長度、不得因整數溢位放行」從 shell 幾乎無法觸發。
- 手法（同 CAP9 的 --gc-sections）：syscall.c 相依十幾個模組，但驗證函式只走到
  `paging_user_range_mapped`（stub），連結器丟掉所有 sys_* 與其相依。
  user_buffer_valid **不解參照** buffer，用假指標即可完整測；user_string_valid
  **會解參照**，用 `mmap MAP_FIXED_NOREPLACE` 在 [0x300000, 0x3F0000) 取得真實
  記憶體（放不下就跳過那組，核心的溢位測試不受影響）。
- 涵蓋: buffer 的 count==0 恆過（連 NULL）、範圍上下界、**count 不得越過頂端**、
  **巨大 count 不得因溢位繞過**（start 在頂端下方、count=4GB-1 或使 start+count
  回繞到頂端下方）、映射檢查（未映射／跨邊界）；string 的範圍、正常 NUL 結尾、
  MAX_USER_STRING 內無 NUL 拒絕、逐位元組映射檢查、近頂端的掃描 clamp；
  alloc_fd 的循序配發/滿載/中段重用/NULL。36 檢查。
- **突變測試（7 個）＋一個關於測試設計的關鍵教訓**：第一版有 6/7 被抓到，
  **漏掉的正是最重要的「整數溢位繞過」**（`count <= TOP-start` 改成
  `start+count <= TOP`）。追查發現：我的 paging stub 對那個 4GB 範圍**獨立**回傳
  「未映射」，把 bound 檢查的 bug 遮蔽了。修正：加一個「強制已映射」模式，在測
  溢位 bound 時**隔離**該邏輯，讓只有 bound 算術決定結果。修正後 **7 個全抓到**。
  這再次印證：stub 太嚴格會遮蔽受測邏輯的 bug，突變測試能把這種盲點揪出來。

### CAP11 paging COW 參照計數 + user_pte 單元測試（fork 正確性核心）
- 檔案: tests/test_paging_cow.c、Makefile
- 目標: `user_pte`（把使用者 vaddr 映射到正確頁表項——低 0-4MB 表 vs 32-36MB
  mmap 表，index 是裸移位，邊界 off-by-one 會靜默讀寫錯頁）與 COW 參照計數
  `cow_ref_inc`/`cow_ref_release`（`page_ref[]` 記錄「超出唯一擁有者的額外參照
  數」，0=一個擁有者；release 必須只在最後一個參照消失時回「該釋放」，邊界錯了
  就釋放仍被別的位址空間共享的頁，或永久洩漏）。這些是 fork/COW 的核心，錯誤會
  導致難以追查的記憶體損毀。
- 手法（同 --gc-sections）：兩者傳遞閉包**不需任何外部函式**，連結器丟掉頁錯誤
  處理常式、組語、pmm/heap，零 stub。用真實 page_table_t 讓 user_pte 指入、直接
  讀寫檔案範圍的 page_ref[] 陣列。
- 涵蓋: user_pte 低窗（0/同頁/相鄰頁/1023 邊界）、ext 窗（rebase 到視窗起點、
  1023 邊界）、兩窗之間的 gap 與各邊界回 NULL、NULL space/缺表回 NULL；
  refcount 的 inc 計數與越界忽略、release 的最後參照才釋放/共享時保留/歸零後不
  下溢、以及一個完整的 fork→exit 序列。31 檢查。
- **突變測試（7 個）＋一個關於「無功能訊號的 bug」的解法**：6/7 功能上被抓到；
  漏掉的是 `cow_ref_inc` 的 `frame < FRAME_COUNT` 改成 `<=`——那是純粹的**越界
  寫入** `page_ref[FRAME_COUNT]`，功能斷言看不到。解法：這個測試改用
  **UBSan 陣列邊界陷阱**（`-fsanitize=undefined,bounds` +
  `-fsanitize-undefined-trap-on-error`，trap 模式不需 libubsan、-m32 可用），
  把越界存取變成硬陷阱（ud2/Illegal instruction）。加上後 **7 個全抓到**。
  這補足了突變測試的一類盲點：功能上不可觀察的記憶體越界，可用 sanitizer 陷阱
  轉成可觀察的失敗。

### CAP12 ELF 載入器單元測試（第三條「解析不受信任資料」的信任邊界）
- 檔案: tests/test_elf.c、Makefile
- 為什麼是信任邊界：Session 6 的 FEAT1 之後，可執行檔改走 VFS 查找，使用者可以把
  **任意位元組**寫到 FAT16/DiskFS/RAMFS 的檔案再執行它。於是 `e_phoff`、`e_phnum`、
  `p_offset`、`p_vaddr`、`p_filesz`、`p_memsz` 全部是攻擊者可控，而下游沒有人複查
  ——`paging_map_user_page()` 不做任何範圍檢查。這與 CAP4（diskfs mount 解析不受
  信任的磁碟資料）同型，是第三條這類邊界。
- shell 幾乎測不到：它頂多能造出「根本不是 ELF」的檔案（端對端測試涵蓋這一個），
  造不出「header 格式正確但 p_vaddr 惡意」、「e_phnum 溢位」、「offset/length 刻意
  選來讓減法繞回」的映像。所有拒絕路徑在此之前**從未被執行過**。
- 手法（同 --gc-sections）：`#include "../elf_loader.c"` 取得 static 驗證函式，
  連結器丟掉 elf_spawn/elf_exec 與其 process 相依；stub 面只剩 VFS 進入點、
  paging 呼叫與 terminal_writestring。在記憶體中組出映像，一次只汙染一個欄位。
- 每個拒絕都檢查**兩件事**，不只是回傳 NULL：建立位址空間前被拒的不得建立、
  建立後才失敗的必須銷毀（否則每個壞映像都洩漏一個位址空間與其 frame）。
- 涵蓋：magic/class/machine/type、截斷檔、e_phentsize、header table 越界、
  e_phnum 上界與剛好合法的邊界、segment 起點/終點與使用者視窗、entry 必須落在
  已載入的段內、非 PT_LOAD 被忽略、heap_base 取最高段且與順序無關、目錄不是
  可執行檔、晚期失敗的位址空間釋放、**F20 的參照配對**、**F21 的載入中改寫**。
- **突變測試（5 個）與一個真實的測試缺口**：
  - E1（不取參照）、E1b（取了不放）、E2（移除第二次驗證）都被抓到。
  - **E3（把 `p_memsz <= BOTTOM - p_vaddr` 改成會繞回的 `p_vaddr + p_memsz <=
    BOTTOM`）一開始存活**。追查原因：繞回後的上界必然**低於** p_vaddr，於是
    entry-point 檢查（entry 必須落在該段內）順手擋掉了單段的溢位案例——溢位路徑
    被另一個檢查遮蔽了，與 CAP10 的教訓同型。補上「**第二個** segment 用繞回的
    p_memsz」的案例（entry 已由第一段滿足，沒有那層意外保護）後，E3 被抓到。
  - **E4（`p_filesz <= file_length - p_offset` 改成會繞回的
    `p_offset + p_filesz <= file_length`）存活，且經推算確認是等價突變**：要讓和
    繞回需要 `p_filesz >= 2^32 - file_length`（約 0xFFFFFEC0），但短路求值中
    `p_filesz <= p_memsz` 與 `p_memsz <= USER_STACK_BOTTOM - p_vaddr`（上限約
    0xE8000）都排在它前面，那條路徑到不了。誠實記錄為「無法被單獨觸發」，不硬寫
    一個假測試。該條件本身仍有作用（擋一般的「p_filesz 超出檔尾」），已有測試。
- 139 檢查。

### CAP13 RAMFS 單元測試（F11/F20 所依賴的參照機制，以及 PERF2 的算術邊界）
- 檔案: tests/test_ramfs.c、Makefile
- 為什麼值得測：RAMFS 是根檔案系統，所有內嵌程式都在上面。兩個結構性理由——
  1. `fs_node_t::impl` **一個欄位身兼兩用**：bit 0 是 RAMFS_DYNAMIC 旗標、
     其上是開啟參照計數。每次 open/close 都必須不動到旗標，否則檔案會變成
     「永遠刪不掉」或**「開啟中卻刪得掉」**，而後者就是 F11/F20 的核心 UAF。
     「開啟中不得 unlink」這個性質先前只被端對端間接測到。
  2. PERF2 的幾何成長只被 `make bench` 量過**效能**，沒有人測過**算術邊界**——
     F22 就藏在那裡。
- 手法：連結 ramfs.c + 真實 utils.c，只 stub 配置器；ramfs.c 只需要 `fs_root`
  這個全域，所以不連結 fs.c，stub 面就只有 kmalloc/kfree。
- **兩個測試設計上的關鍵決定**：
  1. **看門狗**：F22 是無窮迴圈，若沒有 `alarm(30)`，回歸會把 `make test` 掛住
     而不是讓它失敗。裝了看門狗後 R1 突變實測「開火 → FAIL」。
  2. **配置器毒化**：stub 的 kmalloc 把新區塊全部填 0xAA。真實的 kmalloc 回收的是
     還帶著前一個檔案位元組的堆積區塊，**清零是檔案系統的責任**；若 stub 回傳乾淨
     記憶體，稀疏檔案的空洞會因為「malloc 剛好給了零」而讀到 0，缺失的清零就被
     遮蔽了。這與 CAP10「stub 太嚴格反而遮蔽受測 bug」是同一類陷阱的反面。
- 涵蓋：建立/讀寫/覆寫/短讀、稀疏寫入補零（**兩種**：在既有容量內、以及跨越容量
  而觸發重新配置）、幾何成長的容量重用（用配置計數區分「重用」與「重新配置」）、
  F22 的巨大 offset、開啟參照阻止 unlink 且可嵌套、旗標在 open/close 後完好、
  未配對的 close 不得偽造參照數、static 檔案唯讀且永不可刪、目錄巢狀與非空拒刪、
  root 不可刪、路徑解析邊界（`//`、結尾 `/`、`.`、`..`、過長路徑、過長 component）、
  配置失敗時的乾淨失敗、節點表上限與釋放後可再用。4300 檢查。
- **突變測試 7 個，6 個被抓到，並逼出一個真實的測試缺口**：
  - R1（F22 守衛改回去）→ **看門狗開火**；R2（移除 refs 檢查）5 個失敗；
    R3（open 用 `|=` 不計數）2 個失敗；R4（close 無下溢保護）3 個失敗；
    R5（PERF2 回退成每次精確配置）3 個失敗。
  - **R7（移除成長路徑的清零）一開始存活**。追查發現：原本的稀疏測試把空洞放在
    既有容量之內（41 < 初始容量 64），只走「重用」分支，於是**兩段清零互為備援**，
    單獨移除任一段都看不出來（Session 12 在 diskfs 記錄過同型現象）。這次可以做得
    更好：補上「稀疏寫入同時跨越容量」的案例（空洞由重新配置本身產生，重用分支
    不執行），R7 隨即被抓到。
  - **R6（移除重用路徑的清零）仍存活，且確認是冗餘防禦**：程式碼註解本來就寫著
    「the gap is already zero by the invariant, but make it explicit for
    clarity/safety」——那段 memset 本來就是備援。誠實記錄，不硬寫測試去殺它。
  - 兩個測試設計錯誤是我的假設錯、程式碼對：OOM 測試原本沒讓寫入跨越**容量**
    （只跨越長度，走重用分支根本不配置）；節點表測試原本斷言絕對節點數，但
    node_count 是 file-static 且前面的測試刻意留下檔案。都已改成測真正的性質。

### CAP14 VFS 核心單元測試（fs.c 的兩個嚴格解析器與六個 dispatch wrapper）
- 檔案: tests/test_fs.c（277 檢查）、tests/test_fs_path.c（+2 檢查：輸出緩衝區
  金絲雀）、Makefile
- **為什麼選它**：每一個帶路徑的系統呼叫都會經過 fs.c 的兩個函式——
  `resolve_fs()`（找到這個物件）與 `resolve_parent_fs()`（找到擁有這個名字的
  目錄）。在這之前只有*正規化器* `vfs_resolve_path()` 有單元測試
  （tests/test_fs_path.c，34 檢查，是所有套件裡最薄的），而**消費它輸出的那兩個
  嚴格解析器一個測試都沒有**，只被 shell 剛好打出來的路徑順帶掃到。F8 就住在這裡。
- **三個設計決定**：
  - **用 mock 檔案系統，不連結 ramfs.c**。連 ramfs 會變成主要在測 ramfs 自己的
    私有解析器；更糟的是 ramfs 會自己驗證名稱，**後端太嚴格就分不出是哪一層擋
    下來的**（CAP10 的教訓）。這個 mock 刻意比任何真實後端寬鬆。
  - **樹裡放進「名字就叫 `.`、`..` 和空字串」的節點**。如果 fs.c 哪天真的把這種
    組件傳下去，mock 會回答成功、測試就會失敗。沒有這些節點的話，查找是因為
    「剛好沒東西符合」而回 NULL，少掉一道檢查也看不出來。
  - **斷言「被派送到哪個節點、帶著什麼名字」，而不只是成功或失敗**。路徑解析器
    壞掉的樣子不是當機，是**安靜地回答錯的物件**——F8 正是如此。突變 M13
    （名字切得對、但交給錯的目錄）造成 23 個檢查失敗，就是這個設計買到的東西。
- **覆蓋**：六個 dispatch wrapper 的 NULL 安全與參數/回傳值原樣轉交；resolve_fs
  的根/巢狀/相對/缺失/中途非目錄、`.`/`..`/`//`/結尾斜線、長度與組件邊界；
  resolve_parent_fs 經由 create/unlink/mkdir/rmdir 四個進入點的派送目標與名字、
  各種拒絕情形、後端缺少操作指標時的乾淨失敗、名字緩衝區的終止（0xAA 毒化偵測）；
  以及**正規化器與解析器的一致性語料庫**（12 組 (cwd, path) 都指向同一個節點，
  斷言 vfs_resolve_path 接受的東西解析器也一定接受，且解析到的是呼叫者要的那個
  節點——這正是 F8 那類「悄悄縮短成祖先目錄」用回傳字串看不出來的部分）。
- **突變測試 22 個，最終 22/22 全抓到**，其中 4 個一開始存活，逼出 3 個真實缺口
  與 1 個測試設計不足：
  - **M6（parse_component 接受空組件）存活**：因為 mock 裡沒有名字是空字串的
    節點，查找失敗只是「剛好沒符合的」。加進一個名為 `""` 的目錄（底下還有小孩）
    並改用 `//x` 這個唯一能讓空組件後面還有路徑可走的拼法後才被抓到。
  - **M9（resolve_parent_fs 不檢查父節點是不是目錄）存活**：我原本的案例是
    `/a/f/x`，而 `f` 是個 function pointer 全 NULL 的檔案，於是**是 `!parent->create`
    先擋下來的**，被測的那條檢查根本沒被隔離。加進「FS_FILE 卻帶著完整目錄操作」
    的 mock 節點後才有鑑別力——與 CAP12 的 E3 同型（被另一條檢查遮蔽）。
  - **M12（path_push 的界限放寬一個位元組）存活**：因為函式尾端還有第二道長度
    檢查，**回傳值仍然是 -1**，但那一個位元組**已經寫出去了**。兩道檢查互為備援
    對核心是好事，對測試是壞事。解法是在輸出緩衝區後面放金絲雀並斷言它沒被動過
    ——這是真正的契約（`out` 只有 FS_MAX_PATH 個位元組），不是為了殺突變而寫的
    假測試。
  - **M10（resolve_parent_fs 少檢查一半的空組件條件）一度被判為等價突變**：推導
    確實成立——它漏掉的情形後面一定會被 parse_component 的空組件檢查攔下，回傳值
    完全相同。但**它們的副作用不同**：突變版會多做一次 finddir 才失敗。改成斷言
    「畸形路徑在哪裡停止走訪」（`/a//b` 是 0 次查找、`/a/b//c` 是 1 次）之後被
    抓到。這個斷言本身是有意義的性質：在磁碟後端上一次查找就是一次關中斷的
    ATA PIO，早點拒絕就是便宜地拒絕。
- **一個我算錯、程式碼是對的案例**：我原以為 `parse_component` 的長度上限會擋掉
  127 字元的組件，實際上它允許到 127（正好塞滿 `fs_node_t::name` 的 128 bytes）。
  真正的邊界是：**絕對路徑最多只能承載 126 字元的組件**（再多就超過 FS_MAX_PATH），
  但**相對路徑可以到 127**——而 exec 解析程式名走的正是相對路徑那條。測試改成
  釘住這個真實邊界，並誠實記錄 parse_component 自己那道長度守衛因為路徑長度檢查
  永遠先發而**不可觸發**（縱深防禦，不硬寫測試去假裝它會發生）。

### CAP15 鍵盤驅動單元測試（環狀緩衝、修飾鍵狀態機、Ctrl+C 派送）
- 檔案: tests/test_kb.c（1675 檢查）、Makefile、.gitignore
- **為什麼選它**：kb.c 全是純邏輯，而且一個測試都沒有。QEMU 那套只走一條很窄的
  車道——`send_keys` 打的是短短幾個字、馬上被消費掉，所以**緩衝區從來不會接近滿、
  寫入索引從來不會繞過 128、也從來沒有任何一個按鍵被丟掉**。有意思的狀態剛好就是
  shell session 永遠碰不到的那些。
- **兩個真正值得釘住的性質**：
  - 環狀緩衝是核心裡**唯一一處中斷處理常式與 task 同時碰同一個結構**的地方。它的
    「滿」判斷犧牲一格（`next == read` 是滿而不是空），這裡差一格的後果不是當機，
    是**寫入者套圈讀取者**、把字元以錯誤的順序交出去。
  - Ctrl+C 要在「可捕捉的 SIGINT」與「強制 kill」之間二選一，而這個決定是在中斷
    處理常式裡、對著驅動自己記住的那個 pid 做的。**送錯訊號給錯的行程，從 shell
    看起來完全一樣**（程式都停了），端對端測試分辨不出來。
- **測試接線的兩個決定**：
  - 直接 `#include "../kb.c"`（與 test_rtc 相同）以取得 static 的環狀索引與修飾鍵
    旗標。
  - **用 `#define IO_H` 把 io.h 整個換掉**，自備 `inb`：HOSTED_TEST 下的 `inb` 永遠
    回 0，對一個「輸入就是從埠讀進來」的驅動來說等於完全不能測。這樣做**沒有動到
    任何核心標頭**，所以核心 codegen 不可能受影響——比為了可測性去改 io.h 更保守。
  - `task_exit` 是 noreturn，用 `setjmp`/`longjmp` 接住，才能真的測到「被 kill 的
    task 從等待迴圈裡離開」（F19 的性質）而不是假設它會。
- **結論：kb.c 本身沒有找到 bug**。誠實記錄——這一輪的產出是「把一個沒人檢查過的
  模組變成有 18 個突變證明過的測試在守」，而不是修好了什麼。
- **突變測試 18 個，18/18 全抓到**（其中 3 個是以**逾時**被抓到的：未繞回的索引、
  少了 count 檢查的傳輸迴圈，這些突變的表現是**掛住**而不是答錯，所以突變腳本
  對每次執行都套上 `timeout 20s`，讓「掛住」被記為 KILLED 而不是把腳本卡死）。
  - **K17 一開始存活，逼出一個真實缺口**：`if (!buffer || count == 0) return 0;`
    把 `count == 0` 拿掉之後我的測試還是綠的——因為那個案例**緩衝區裡剛好已經有
    一個字元**，於是傳輸迴圈自己的 `bytes_read < count` 讓兩邊都回 0，參數檢查看
    起來是多餘的。改成**對空緩衝區**呼叫才看得出差別：正確的程式立刻回 0，突變版
    會為了一個它根本不會消費的字元把呼叫者**永遠停住**（`read(fd, buf, 0)` 掛死）。
    這是本專案第三次踩到同一個形狀——**我的案例被另一個條件先滿足，被測的那條根本
    沒被隔離**（CAP10 的 stub、CAP12 的 E3、CAP14 的 M9/M12）。
- **一個「不是 bug、但沒人檢查過」的行為，已轉成有註解的測試**：驅動**完全不處理
  0xE0 延伸掃描碼前綴**。0xE0 的 bit 7 是 1，所以它掉進「按鍵放開」那條分支，低七
  位 0x60 不對應任何修飾鍵而被丟棄，**接在後面的那個位元組就被當成沒有前綴一樣
  處理**。逐一驗過每個後果都是良性的：右 Ctrl（0xE0 0x1D）因為落在與左 Ctrl 相同的
  索引而**照樣能用**（連放開都對）；方向鍵/Home/End/Delete 對應到表裡的 0 而被安靜
  丟掉，不會在 shell 正在讀的那一行裡留下雜訊；小鍵盤 Enter 與 `/` 則剛好落在等價
  主鍵盤鍵的索引上，是正確的字元。**這是意外而不是設計**，所以把它寫成測試並註明
  推導過程——將來若有人「順手把 0xE0 處理加上去」，右 Ctrl 會立刻壞掉，而突變 K18
  （正是那個看起來很合理的改法）就是為此加的，實測被抓到。
- **另一個被釘住的判斷**：Ctrl+C 的比對讀的是**未加 shift 的表**，所以 Ctrl+Shift+C
  也是中斷。這是正確的讀法（重要的是**哪個實體鍵**跟 Ctrl 一起按，不是它會產生哪個
  字形），寫成測試以免有人改成比對 shift 表、讓 Ctrl+Shift+C 變成往命令列打一個
  大寫 C。

### CAP16 procfs 單元測試（F3 的守衛第一次被執行；兩個完全沒有守衛的產生器）
- 檔案: tests/test_procfs.c（183 檢查）、Makefile、.gitignore
- **為什麼選它**：**F3 是這裡的緩衝區溢位**，修法是在 /proc/processes 的產生器加一個
  界限檢查——而**那個檢查從來沒有執行過一次**。它只在每行變長時才會發火，而行要變長
  需要 pid 到十位數（`next_pid` 永不重置）或行程名塞滿欄位，兩者在「開機、打字、
  結束」的 QEMU 執行裡都不會發生。**一個唯一的守衛從沒跑過的修復，等於沒有人測過。**
- **另外兩個產生器根本沒有任何界限檢查**。它們今天安全純粹是因為欄位剛好夠窄
  （/proc/self/status 最壞算出來是 75 bytes，緩衝區 512）。那是「目前欄位寬度」的
  事實，不是任何程式碼在維護的性質——把 PROCESS_NAME_MAX 改大，procfs.c 不會有任何
  意見。**這些測試就是那個會有意見的東西。**
- **緩衝區不變式用結構化的方式檢查，而不是靠肉眼**：每次產生前把 `gen_buf` 灌毒
  （0x7F），產生後**從回報長度到緩衝區結尾的每個位元組都必須還是毒**。這證明產生器
  「剛好寫了它宣稱的那些位元組、一個都不多」——只斷言回傳值會漏掉寫過頭的情況。
  另外掛上 `-fsanitize=bounds`（trap 模式，沿用 CAP11 的手法）當第二道獨立的網。
- **突變測試 16 個，14 個抓到，2 個確認為等價突變**：
  - **P1（把 F3 的守衛整個拿掉，也就是原始的 F3 溢位）被 sanitizer 直接 trap**
    （rc=132）。這一項本身就是這輪的價值：**F3 若被重新引入，現在會當場爆掉**。
  - **P2 與 P3 一開始存活，逼出一個真實且我沒想到的缺口**：截斷測試讓每行都剛好
    40 bytes，於是 `pos` 只會取到 40 的倍數，**守衛的判斷在每個倍數附近的一整段
    閾值裡都相同**——把界限放寬一個位元組（P2）、或把名稱欄位少預留七個（P3），
    兩個都是真實溢位，卻**沒有改變任何一條斷言**。解法是刻意讓 `pos` 精準落在
    邊界上：11 行滿寬（440）＋ 1 行 33 bytes ＝ **473**，而 473 正是守衛必須仍然
    拒絕的最大值（473 + 40 = 513，比 512 多一）。補上這個案例後兩個突變都被
    sanitizer 抓到。**教訓：均勻的測試資料看起來覆蓋得很好，卻可能完全碰不到
    被測的那個邊界。**
  - 同一個測試也從另一側夾住邊界（第 12 行改成 32 bytes → `pos` = 472 → 第 13 行
    塞得下、總長剛好 512），這樣「一律拒絕」的錯誤修法不會通過。
  - **P4（`break` 改成 `continue`）確認為等價突變**：守衛的條件只看 `pos`，而略過
    一個行程不會改變 `pos`，所以它一旦成立就對其後每一輪都成立——產生的位元組完全
    相同，差別只有純檢查的迴圈次數（上限 16），沒有可觀察的副作用。誠實記錄。
  - **P12（`offset >= len` 改成 `offset > len`）確認為等價突變**：`offset == len`
    時突變版往下走，但 `size > len - offset` 立刻把 size 夾成 0、`memcpy` 0 bytes
    是 no-op，回傳同樣是 0。原本的提早返回是刻意的縱深防禦（也避免算出越界指標），
    與 ramfs 的 R6 同型。誠實記錄，不硬寫測試去殺它。
- **一個我算錯、程式碼是對的案例**（本專案的慣例是把這些記下來）：我第一版把截斷後的
  長度斷言成 `lines * 33`，實際是 480 = 12 × **40**。每行的最壞寬度就是守衛預留的
  那 40 bytes（10+1+10+1+1+1+15+1），而 512 / 40 = 12.8 → 12 行。測試改成釘住
  `lines == 12`、`len == 480` 這兩個精確值，這樣緩衝區大小或預留量一改就會被發現。

### SEEK1 [調查／決定不改] `sys_seek` 的 offset 上界該不該收窄
- 檔案: syscall.c `sys_seek`（**未改動**）、tests/fs_conformance.h（改採的替代方案）
- **問題陳述**：F22（P0）與 F23 都是「seek 過檔尾 → 小量寫入」進來的。`sys_seek`
  接受任何 ≤ 0x7FFFFFFF 的 offset，接下來會發生什麼**完全取決於收到它的那個後端**。
  PROJECT_STATE 第 6 節 C 類把「把上界收到合理範圍」列為「消除整類問題」的候選，
  並在兩次命中後標記為應提高優先度。本輪對它做了實證調查。
- **實測到的三件事**：
  1. **三個後端目前都正確**。新建的一致性契約（見 CONF1）對 RAMFS/DiskFS/FAT16
     各打了 0x7FFFFFFF、0x80000000、0xC0000000、0xFFFFFFFE 等 offset 與一個會讓
     `offset + size` 繞回的組合，全部通過：都回傳 0、都不改長度、都不改內容。
     **沒有現存的缺陷需要靠 syscall 上界來擋。**
  2. **沒有一個有依據的常數可放在 syscall 邊界**。各後端真正的上限相差四個數量級：
     DiskFS 每個檔案 `DISKFS_FILE_SECTORS * 512` = **2048 bytes**；FAT16 受限於
     32 KB 的磁區；RAMFS 則是經由 `pmm_alloc_blocks` 受限於**實體記憶體**，
     沒有編譯期常數。`sys_seek` 在它那一層看不出 fd 屬於哪個後端，所以唯一能取的
     全域上界是「最大的那個」，也就是實體記憶體——一個執行期才知道、而且對 DiskFS
     來說寬鬆了一千倍的數字。**上限的知識屬於後端，不屬於 syscall。**
  3. **收窄會讓 user/bigseek.c 失效**。任何低於 2^31 的上界都會讓那支端對端程式
     無法再從 ring 3 發動 F22 的攻擊——而它目前是**唯一**證明「ring 3 → syscall →
     ramfs 整條路徑撐得住」的產物（修復前它會讓 QEMU 直接凍結）。用一個活著的、
     證明過的回歸測試，去換一個對假想中未來後端的預防性守衛，是壞交易。
- **決定：不改 `sys_seek` 的語義**。理由如上，且現行行為（seek 成功、寫入乾淨失敗）
  比「seek 就失敗」更貼近 POSIX（允許 seek 過檔尾）。
- **但殘留風險確實存在**，就是 PROJECT_STATE 寫的那句「下一個後端還是得自己重擋
  一次」。本輪改用 **CONF1** 處理它：把那句散文變成**可執行的契約**，三個後端都跑，
  新後端只要接上就自動被檢查。這比一個武斷的常數更直接命中真正的風險。
- **重新評估的優先度**：C 類此項由「應提高」改為 **已處理（以契約取代上界）**。
  若將來出現一個 offset 上限已知且統一的設計（例如所有後端共用一個 max file size
  常數），可再重啟討論。

### CONF1 檔案系統後端一致性契約（把「下一個後端要自己重擋」變成可執行的檢查）
- 檔案: tests/fs_conformance.h（新增）、tests/test_ramfs.c、tests/test_diskfs.c、
  tests/test_fat16.c、Makefile
- **動機**：F22 與 F23 是同一種形狀打在兩個不同後端上。兩個都修好了、也各有單元
  測試，但**沒有任何地方寫下這個「要求」本身**，所以第三個後端得自己重新發現一次。
- **契約內容**（給定一個已有內容的檔案，對一個後端不可能觸及的位置寫入必須）：
  1. **要回來**。系統呼叫全程關中斷（`int 0x80` 是 interrupt gate），不會終止的
     迴圈不是「慢」，是整台機器停住——這是 F22。由**看門狗**而非斷言來守，因為
     回歸的症狀是掛住而不是答錯。
  2. 一個位元組都不存（回傳 0）。
  3. **檔案完全不變**——長度與內容都是。存了 0 個位元組卻把檔案長到 seek 的位置，
     就是 F23。
  4. 用不可能的 offset 讀取要安全地回 EOF。
- **實測結果：三個後端全部通過**，這也是 SEEK1 判斷的依據。
- **突變測試 3 個（各還原一個真實的、出貨過的 bug），3/3 抓到——但歸因必須分清楚**：
  - **C3（把 DiskFS 的界限改寫成會繞回的加法）只有契約抓到**：6 個失敗全部來自
    契約，既有的 **943 個 diskfs 檢查一個都沒抓到**。這是契約價值最清楚的證明——
    而且那正是 PROJECT_STATE 第 4 節「界限檢查寫成不會溢位的減法」所警告的形狀。
    突變版在 offset 0x7FFFFFFF 上**宣稱寫了 4 個位元組**、把長度變成 256。
  - **C2（還原 F23）由既有測試（4 個失敗）與契約（9 個）雙重抓到**，契約獨立有效。
  - **C1（還原 F22）是被 test_ramfs 既有的 CAP13 看門狗抓到的，不是契約**：
    `test_huge_offset_terminates` 先掛住，契約根本沒跑到。**誠實記錄：契約在 RAMFS
    上未被獨立證明**——它在那裡與既有測試重疊，這本來就是預期的（契約正是從 RAMFS
    的教訓推導出來的）。
- **接線上踩到的兩個坑**（都是測試之間共享靜態狀態）：`test_node_table_limit` 會把
  RAMFS 節點表填滿，之後任何建檔都失敗；`test_out_of_memory_is_clean` 會把配置器
  stub 留在拒絕狀態。契約改為自己重設配置器上限，並排在填表測試之前。

### CAP17 VGA 文字主控台單元測試（捲動、環繞、退格、十進位格式化）
- 檔案: tests/test_vga.c（4769 檢查）、tests/test.h（失敗時 flush）、Makefile、
  .gitignore
- **為什麼它一直沒被測到**：核心印出的每個字都經過 `terminal_putchar`，而 QEMU 那套
  的數百條斷言全是對它輸出的 grep——但那些 grep 讀的是 **port 0xE9 的位元組流，而
  putchar 在做任何游標運算之前就先把字送出去了**。所以捲動、環繞、退格的錯誤對
  端對端測試**完全不可見**：位元組照樣依序抵達，畫面卻是亂的。
- **兩個替換，都不動核心標頭**（CAP15 的手法）：`#define IO_H` 換掉 io.h 以捕捉
  `outb`（順帶讓 port 0xE9 這條 QEMU 套件真正消費的介面有自己的斷言）；
  `terminal_buffer` 是全域變數，直接指向測試的陣列。唯一無法在 host 執行的是
  `terminal_initialize()`，因為它的函式主體就是 0xB8000 這個硬體位址的指派。
- **畫面兩端都放護欄**：只在後面放不夠——捲動會讀它要寫的那一列的**上一列**，所以
  迴圈早一列開始就是往畫面**前面**讀。畫面放在陣列中間、前後都有護欄之後，那個
  突變（V6）從「靠 segfault 被發現」變成**由斷言指名道姓**。
- **`tests/test.h` 的一個真正修正**：失敗訊息現在逐筆 `fflush`。stdout 在管線裡是
  區塊緩衝的（`make unit` 與所有突變腳本都是這樣跑的），所以**一個先報了失敗、隨後
  才崩潰的測試會把所有失敗訊息一起丟掉**，只剩下崩潰。那會把「斷言抓到了」變成
  「不知道哪裡出事」——正是突變測試最需要分清楚的那條界線。加上 flush 之後，V1 與
  V10 的歸因從「rc=139、0 個失敗」變成「rc=139，但先有 3 / 77 個具名斷言失敗」。
- **突變測試 21 個，21/21 全抓到，且全部由斷言抓到**（崩潰只是後果，不是偵測手段）。
  一個 SKIPPED 的（V1 的樣式在 `terminal_putentryat` 與 `terminal_clear` 裡各出現
  一次）已改成含函式簽名的唯一樣式後補測。
- **順帶釘住的既有行為**：`'\t'` **沒有**任何特殊處理，被當成普通字元放進畫面並前進
  一格（鍵盤確實會送出 `\t`，所以這條路徑可達）；`terminal_write_dec_pad` 在數字
  位數多於欄寬時丟掉高位以維持欄寬（日期格式要的就是這個）。

### HEAP1 heap.c 的合併／碎片化／邊界稽核（沒找到缺陷，但補上四個真實測試缺口）
- 檔案: tests/test_heap.c（378 → 720 檢查）、heap.h／heap.c（新增
  `heap_first_block()` 供測試走訪自由串列）
- **稽核結論：沒有可觸發的缺陷**。逐項推導：分割門檻
  `> size + sizeof(heap_block_t) + 4` 保證剩餘區塊的 payload > 4；`heap_grow` 的
  溢位守衛寫成不會繞回的減法；合併迴圈在合併後**停留原地**，所以三個以上相鄰的
  自由區塊會連鎖合併；分割時把新區塊插在 `best_fit` 之後，維持位址排序。
  `kmalloc` 對接近 SIZE_MAX 的請求會先被 `heap_grow` 的守衛擋掉，所以分割算式的
  溢位路徑到不了。
- **但突變測試找出四個真實的測試缺口**（15 個突變，一開始只殺掉 11）：
  - **H14（拿掉 4-byte 對齊）存活**：既有測試用的每個大小都已經是 4 的倍數，所以
    對齊看起來是多餘的。補上 5/6/7 bytes 的請求與 payload 位址對齊的斷言後抓到。
  - **H4（分割剩餘量少算一個 header）存活**：我原本的「不得越過下一個區塊」檢查在
    **剩餘區塊剛好是串列最後一個時被跳過**——而那正是它沒有鄰居可撞、宣稱錯誤卻還
    不致命的情形。改成直接斷言剩餘量的精確算式後抓到。
  - **H7（合併不檢查實體相鄰）存活兩輪**，逼出兩個更深的問題：
    第一輪是我的測試裡沒有「在串列中相鄰、在記憶體中不相鄰」的自由區塊對；
    第二輪是我寫的 `growth_only_by_adjacency()` **本身有缺陷**——連鎖合併時只有
    吸收方的 header 會更新，被吸收的區塊保留舊的 size，所以只驗第一個鄰居就通過了。
    改成「成長必須正好等於後續一串實體連續區塊的總和」，並加上一條與狀態無關的
    直接斷言（stub 記錄它刻意跳過的位址範圍，任何區塊都不得涵蓋它）後抓到。
  - **H11（`insert_block` 永遠插在頭部）存活**：它不會讓任何配置失敗，只是讓
    **跨成長邊界的合併失效**，於是每次成長都碎片化一次。要測到它必須先讓 stub 誠實
    ——見下。
- **stub 的一個實質改進**：原本用 `posix_memalign`，兩次成長的相對位址由 host 配置器
  決定，於是**兩個性質變得不可測**：連續的新區域會不會與舊的合併、自由串列有沒有
  維持位址排序。真實的 `pmm_alloc_blocks` 是**連續配發 frame** 的，所以「新區域緊接
  在舊區域之後」在核心裡是常態而非例外。改成從單一 arena 依序配發（並保留一個
  「刻意留洞」的開關給需要不相鄰區域的那個測試），H11 才變得可觀測。
- **最終 15/15，沒有等價突變**——四個存活的全部是測試缺口，沒有一個是「程式碼本來
  就等價」。

## 效能改善（已實作）

### PERF2 ramfs_write 改成幾何成長（攤還 O(1) append，取代原本每次成長 O(n) 複製）
- 檔案: ramfs.c（`ramfs_entry_t` 加 `capacity` 欄位、重寫 `ramfs_write`）
- 原本每次寫入超出 length 都 kmalloc 新 buffer + memcpy 整份舊內容 + kfree，
  重複 append 是 O(n²)。改為：在 ramfs 私有的 entry 結構追蹤已配置容量，成長時
  容量加倍（至少滿足需求），未超過容量的成長直接就地寫入、不重配置。維持
  「[length, capacity) 恆為零」的不變式，語意與原本完全相同（讀取仍只用
  length）。這原是「已知限制」清單中的一項，Session 2 予以實作。
- 狀態: **已修＋已驗證＋已量測**。正確性：user/ramgrow.c 以 128 次小寫入堆出
  2048 bytes 再整檔讀回逐位元組驗證；FAT/DiskFS 的攤還成長也有 CAP3/CAP4 覆蓋。
  **效能量測（Session 13，`make bench` / tests/bench_ramfs.c）**：這是計數式而
  非計時式量測，數值精確且與 host/編譯器無關。以 N 次 K-byte append 堆出一個
  檔案，重新配置次數由 **N 降為約 log2(N·K/64)**（實測 N=512 時 512→8），
  成長造成的複製位元組由 **O(N²) 降為 O(最終大小)**（N=512 時 2,093,056→8,128
  bytes，少 258 倍；N=1024 時少 516 倍）。舊版行為以精確封閉式呈現（程式碼已不
  存在，但其成本可精確推導）。這正是當初宣稱此改善時**該附上而未附上**的證據。

### PERF1 memcpy/memset 改成 4-byte 對齊批次搬移
- 檔案: utils.c
- 原本逐位元組搬移，在分頁歸零（每次 4KB）、ATA 磁區 I/O（每次 512B）、
  RAMFS/DiskFS/FAT16 讀寫、COW 複製等熱路徑上都會呼叫到，是全系統共用的
  瓶頸。改成：先用位元組迴圈把指標搬到 4-byte 對齊，中段用 uint32_t 一次
  搬 4 bytes，尾端剩餘 bytes 再用位元組迴圈收尾；memcpy 額外檢查來源指標
  是否也對齊，沒對齊就整段退回逐位元組（正確性優先，不強行做未對齊的
  32-bit 存取）。行為完全不變（純粹是同樣結果、更少迴圈次數），make test
  全綠。
- **效能量測（Session 13，`make bench` / tests/bench_mem.c）**：連結真正的
  utils.c（受測方即核心實際出貨的程式碼），對逐位元組基準線計時。**數值是 host
  x86-64、gcc -O1 的相對數據，核心是 32-bit -O2 -ffreestanding，故看比值不看
  絕對 ns**。三次執行比值穩定：
  - memset：頁(4KB)/磁區(512B)/64B 對齊皆約 **3.4–5.2x**；**即使起點未對齊也
    加速**（memset 只需目的對齊），7 bytes 約 1.0x（快路徑跳過，無退化）。
  - memcpy：對齊的頁/磁區約 **3.0–4.7x**——這正是核心熱路徑（分頁 4KB、ATA
    磁區 512B、堆積區塊皆 ≥4-byte 對齊）。
  - **誠實補述（當初宣稱時未講清楚的限制）**：memcpy **只有在來源與目的互相
    對齊時**才加速；兩者相對未對齊時會退回逐位元組，加速比約 **1.0x（毫無
    改善）**。這是刻意的正確性選擇（不做未對齊的 32-bit 存取），但確實是此改動
    的限制。數據見 tests/BENCHMARKS.md。

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
- ~~**多執行緒行程的訊號/終止仍無法觸及「長期阻塞中的 thread」**~~：**終止的部分
  已於 Session 21 修復，見上方 F19**（kill_pending 旗標 + task_kill_blocked +
  task_block_killable，被殺的 task 醒來就離開而不是回到等待迴圈）。
  **剩下的部分是「訊號遞送」**：`process_send_signal` 仍只喚醒 `process->task`，
  而且訊號只在返回使用者模式時遞送（`signal_deliver` 檢查 `regs->cs != 0x1B`），
  所以停在等待中的 thread 收不到可捕捉的訊號——它不會返回使用者模式。這才是真正
  需要「可中斷睡眠 + EINTR 上拋」的部分：每個阻塞點都要能中途放棄並讓系統呼叫
  回傳 EINTR，牽涉 sem_wait / pipe_read / pipe_write / keyboard_read /
  process_wait / process_waitpid / process_pause / process_thread_join /
  timer_sleep 全部呼叫點**以及使用者空間對「系統呼叫可能被中斷」的預期**，
  是會動到 ABI 的架構改動，依保守原則不做。F19 之所以能用小得多的改動完成，
  正是因為「終止」不需要回到使用者空間，直接 task_exit 即可。
- ~~**elf_loader 只從 RAMFS 載入可執行檔**~~：**已於 Session 6 實作，見下方
  FEAT1**（改走 VFS，可從任何已掛載的檔案系統執行程式）。

<!-- 格式：
### [ID] 標題
- 檔案:行號
- 類別: 正確性/競態/記憶體安全/效能/架構/文件
- 嚴重度: P0-P4
- 描述
- 修復方式（若已修）
-->
