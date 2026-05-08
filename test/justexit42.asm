
.text

extern sys_exit

global main
main:
    enter 0
    mov ax, 42
    push ax
    call sys_exit, die
    leave
    ret


die:
    halt

