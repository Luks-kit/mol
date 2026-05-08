; ─── mol_syscall.asm — DVM syscall stubs ─────────────────────────────────────
;
; Calling convention (Mol / DVM):
;   Caller pushes args right-to-left before CALL.
;   After ENTER, args sit above bp:
;     first arg  → [bp+8]
;     second arg → [bp+16]
;     third arg  → [bp+24]
;     ...
;   Result returned in ax.
;   ax = syscall number (set by stub before SYSCALL)
;   bx = a1, cx = a2, dx = a3, di = a4, si = a5
;
; Each stub follows the pattern:
;   enter 0          ; no locals needed
;   mov bx, [bp+8]  ; a1
;   mov cx, [bp+16]  ; a2  (if needed)
;   ...
;   mov ax, <NR>
;   syscall
;   leave
;   ret
;
; All stubs are global so the linker can resolve them from any Mol object.

.text

; ─── exit(code) ───────────────────────────────────────────────────────────────
global sys_exit
sys_exit:
    enter 0
    mov bx, [bp+8]
    mov ax, 0
    syscall
    leave
    ret

; ─── read(fd, buf, len) → n ───────────────────────────────────────────────────
global sys_read
sys_read:
    enter 0
    mov bx, [bp+8]
    mov cx, [bp+16]
    mov dx, [bp+24]
    mov ax, 1
    syscall
    leave
    ret

; ─── write(fd, buf, len) → n ──────────────────────────────────────────────────
global sys_write
sys_write:
    enter 0
    mov bx, [bp+8]
    mov cx, [bp+16]
    mov dx, [bp+24]
    mov ax, 2
    syscall
    leave
    ret

; ─── open(path, flags, mode) → fd ────────────────────────────────────────────
global sys_open
sys_open:
    enter 0
    mov bx, [bp+8]
    mov cx, [bp+16]
    mov dx, [bp+24]
    mov ax, 3
    syscall
    leave
    ret

; ─── close(fd) → 0 ───────────────────────────────────────────────────────────
global sys_close
sys_close:
    enter 0
    mov bx, [bp+8]
    mov ax, 4
    syscall
    leave
    ret

; ─── mmap(len, prot) → ptr ───────────────────────────────────────────────────
global sys_mmap
sys_mmap:
    enter 0
    mov bx, [bp+8]
    mov cx, [bp+16]
    mov ax, 5
    syscall
    leave
    ret

; ─── munmap(ptr, len) → 0 ────────────────────────────────────────────────────
global sys_munmap
sys_munmap:
    enter 0
    mov bx, [bp+8]
    mov cx, [bp+16]
    mov ax, 6
    syscall
    leave
    ret

; ─── getpid() → pid ──────────────────────────────────────────────────────────
global sys_getpid
sys_getpid:
    enter 0
    mov ax, 7
    syscall
    leave
    ret

; ─── time() → unix seconds ───────────────────────────────────────────────────
global sys_time
sys_time:
    enter 0
    mov ax, 8
    syscall
    leave
    ret

; ─── clock() → nanoseconds (CLOCK_MONOTONIC) ─────────────────────────────────
global sys_clock
sys_clock:
    enter 0
    mov ax, 9
    syscall
    leave
    ret

; ─── isatty(fd) → 1/0 ────────────────────────────────────────────────────────
global sys_isatty
sys_isatty:
    enter 0
    mov bx, [bp+8]
    mov ax, 10
    syscall
    leave
    ret

; ─── thread_spawn(bytecode_ptr, stack_size) → handle ─────────────────────────
global sys_thread_spawn
sys_thread_spawn:
    enter 0
    mov bx, [bp+8]
    mov cx, [bp+16]
    mov ax, 11
    syscall
    leave
    ret

; ─── thread_join(handle) → exit_code ─────────────────────────────────────────
global sys_thread_join
sys_thread_join:
    enter 0
    mov bx, [bp+8]
    mov ax, 12
    syscall
    leave
    ret

; ─── thread_detach(handle) → 0 ───────────────────────────────────────────────
global sys_thread_detach
sys_thread_detach:
    enter 0
    mov bx, [bp+8]
    mov ax, 13
    syscall
    leave
    ret

; ─── thread_self() → handle ──────────────────────────────────────────────────
global sys_thread_self
sys_thread_self:
    enter 0
    mov ax, 14
    syscall
    leave
    ret

; ─── thread_id() → id ────────────────────────────────────────────────────────
global sys_thread_id
sys_thread_id:
    enter 0
    mov ax, 15
    syscall
    leave
    ret

; ─── mutex_create() → handle ─────────────────────────────────────────────────
global sys_mutex_create
sys_mutex_create:
    enter 0
    mov ax, 16
    syscall
    leave
    ret

; ─── mutex_lock(handle) → 0 ──────────────────────────────────────────────────
global sys_mutex_lock
sys_mutex_lock:
    enter 0
    mov bx, [bp+8]
    mov ax, 17
    syscall
    leave
    ret

; ─── mutex_unlock(handle) → 0 ────────────────────────────────────────────────
global sys_mutex_unlock
sys_mutex_unlock:
    enter 0
    mov bx, [bp+8]
    mov ax, 18
    syscall
    leave
    ret

; ─── mutex_destroy(handle) → 0 ───────────────────────────────────────────────
global sys_mutex_destroy
sys_mutex_destroy:
    enter 0
    mov bx, [bp+8]
    mov ax, 19
    syscall
    leave
    ret

; ─── atomic_fetchadd(ptr, delta) → old ───────────────────────────────────────
global sys_atomic_fetchadd
sys_atomic_fetchadd:
    enter 0
    mov bx, [bp+8]
    mov cx, [bp+16]
    mov ax, 20
    syscall
    leave
    ret

; ─── atomic_cmpxchg(ptr, expected, desired) → old ────────────────────────────
global sys_atomic_cmpxchg
sys_atomic_cmpxchg:
    enter 0
    mov bx, [bp+8]
    mov cx, [bp+16]
    mov dx, [bp+24]
    mov ax, 21
    syscall
    leave
    ret
