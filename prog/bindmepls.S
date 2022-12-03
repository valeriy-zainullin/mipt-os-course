.bss

.align 4
.byte 0

.global sched_yield # Results in STB_GLOBAL in the ELF's symbol table. 
sched_yield: .quad 0
.type sched_yield, @object # Results in STT_OBJECT in the ELF's symbol table.

# In order to figure out how to set these flags, so that the binding thinks it's a legitimate function pointer to bind, compile with gcc -S main.c file main.c with contents `void (* volatile sched_yield)();`.
