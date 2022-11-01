/* Simple command-line kernel monitor useful for
 * controlling the kernel and exploring the system interactively. */

#include <inc/stdio.h>
#include <inc/string.h>
#include <inc/memlayout.h>
#include <inc/assert.h>
#include <inc/env.h>
#include <inc/x86.h>

#include <kern/console.h>
#include <kern/monitor.h>
#include <kern/kdebug.h>
#include <kern/tsc.h>
#include <kern/timer.h>
#include <kern/env.h>
#include <kern/kclock.h>
#include <kern/kdebug.h>
#include <kern/monitor.h>
#include <kern/pmap.h>
#include <kern/trap.h>

#define WHITESPACE "\t\r\n "
#define MAXARGS    16

/* Functions implementing monitor commands */
int mon_help(int argc, char **argv, struct Trapframe *tf);
int mon_kerninfo(int argc, char **argv, struct Trapframe *tf);
int mon_backtrace(int argc, char **argv, struct Trapframe *tf);
int mon_hello(int argc, char **argv, struct Trapframe *tf);
int mon_dumpcmos(int argc, char **argv, struct Trapframe *tf);
int mon_timer_start(int argc, char **argv, struct Trapframe *tf);
int mon_timer_stop(int argc, char **argv, struct Trapframe *tf);
int mon_timer_frequency(int argc, char **argv, struct Trapframe *tf);
int mon_memory(int argc, char **argv, struct Trapframe *tf);
int mon_pagetable(int argc, char **argv, struct Trapframe *tf);
int mon_virt(int argc, char **argv, struct Trapframe *tf);

struct Command {
    const char *name;
    const char *desc;
    /* return -1 to force monitor to exit */
    int (*func)(int argc, char **argv, struct Trapframe *tf);
};

static struct Command commands[] = {
    {"help", "Display this list of commands", mon_help},
    {"kerninfo", "Display information about the kernel", mon_kerninfo},
    {"backtrace", "Print stack backtrace", mon_backtrace},
    {"hello", "Greet the user", mon_hello},
    {"dumpcmos", "Print CMOS contents", mon_dumpcmos},
    {"timer_start", "Starts timer ...", mon_timer_start},
    {"timer_stop", "Starts timer ...", mon_timer_stop},
    {"timer_freq", "Starts timer ...", mon_timer_frequency},
};
#define NCOMMANDS (sizeof(commands) / sizeof(commands[0]))

/* Implementations of basic kernel monitor commands */

int
mon_help(int argc, char **argv, struct Trapframe *tf) {
    for (size_t i = 0; i < NCOMMANDS; i++)
        cprintf("%s - %s\n", commands[i].name, commands[i].desc);
    return 0;
}

int
mon_kerninfo(int argc, char **argv, struct Trapframe *tf) {
    extern char _head64[], entry[], etext[], edata[], end[];

    cprintf("Special kernel symbols:\n");
    cprintf("  _head64 %16lx (virt)  %16lx (phys)\n", (unsigned long)_head64, (unsigned long)_head64);
    cprintf("  entry   %16lx (virt)  %16lx (phys)\n", (unsigned long)entry, (unsigned long)entry - KERN_BASE_ADDR);
    cprintf("  etext   %16lx (virt)  %16lx (phys)\n", (unsigned long)etext, (unsigned long)etext - KERN_BASE_ADDR);
    cprintf("  edata   %16lx (virt)  %16lx (phys)\n", (unsigned long)edata, (unsigned long)edata - KERN_BASE_ADDR);
    cprintf("  end     %16lx (virt)  %16lx (phys)\n", (unsigned long)end, (unsigned long)end - KERN_BASE_ADDR);
    cprintf("Kernel executable memory footprint: %luKB\n", (unsigned long)ROUNDUP(end - entry, 1024) / 1024);
    return 0;
}

int
mon_hello(int argc, char **argv, struct Trapframe *tf) {
    cprintf("Hello!\n");
    return 0;
}

int
mon_backtrace(int argc, char **argv, struct Trapframe *tf) {
    // LAB 2: Your code here

    cprintf("\a");
    cprintf("Stack backtrace:\n");

    uint64_t cur_rbp  = read_rbp();

    struct Ripdebuginfo debug_info;
    while(cur_rbp != 0){
        
        uint64_t ret_addr = *(uint64_t*)(cur_rbp + 8);
        debuginfo_rip(ret_addr, &debug_info);

        cprintf("  rbp %016lx  rip %016lx\n", cur_rbp, ret_addr);
        
        cprintf("    %s:%d: %s+%lu\n", 
            debug_info.rip_file, debug_info.rip_line, debug_info.rip_fn_name, ret_addr - debug_info.rip_fn_addr);
        cur_rbp = *(uint64_t*)(cur_rbp);
    }

    return 0;
}

int
mon_dumpcmos(int argc, char **argv, struct Trapframe *tf) {
    // Dump CMOS memory in the following format:
    // 00: 00 11 22 33 44 55 66 77 88 99 AA BB CC DD EE FF
    // 10: 00 ..
    // Make sure you understand the values read.
    // Hint: Use cmos_read8()/cmos_write8() functions.
    // LAB 4: Your code here

    static const uint32_t CMOS_MEMORY_SIZE = 128;
    static const uint32_t BYTES_PER_LINE = 16;

    for (uint32_t address = 0; address < CMOS_MEMORY_SIZE; address += BYTES_PER_LINE) {
        cprintf("%02x:", address);

        for (uint8_t offset = 0; offset < BYTES_PER_LINE; ++offset) {
            cprintf(" %02X", cmos_read8((uint8_t) (address + offset)));
        }

        cprintf("\n");
    }

    return 0;
}

/* Implement timer_start (mon_timer_start), timer_stop (mon_timer_stop), timer_freq (mon_timer_frequency) commands. */
// LAB 5: Your code here:

static bool
mon_validate_timer_name(char* command, char* timer) {
    for (size_t i = 0; i < MAX_TIMERS; ++i) {
        if (strcmp(timertab[i].timer_name, timer) == 0) {
            return true;
        }
    }

    return false;
}

static void
mon_timer_print_cmd_usage(char* command) {
    cprintf("Format: %s [timer name]\n", command);
    cprintf("Timer name is one of ");
    for (size_t i = 0; i < MAX_TIMERS; ++i) {
        cprintf("%s", timertab[i].timer_name);
        if (i == MAX_TIMERS - 1) {
            cprintf(".\n");
        } else {
            cprintf(", ");
        }
    }
}

int
mon_timer_start(int argc, char **argv, struct Trapframe *tf) {
    (void) tf;

    if (argc != 2 || !mon_validate_timer_name(argv[0], argv[1])) {
        mon_timer_print_cmd_usage(argv[0]);
        return 0;
    }

    // TODO: account timer can be already running.

    timer_start(argv[1]);

    return 0;
}

int
mon_timer_stop(int argc, char **argv, struct Trapframe *tf) {
    (void) tf;

    timer_stop();

    return 0;
}

int
mon_timer_frequency(int argc, char **argv, struct Trapframe *tf) {
    (void) tf;

    if (argc != 2 || !mon_validate_timer_name(argv[0], argv[1])) {
        mon_timer_print_cmd_usage(argv[0]);
        return 0;
    }

    timer_cpu_frequency(argv[1]);

    return 0;
}

/* Implement memory (mon_memory) command.
 * This command should call dump_memory_lists()
 */
// LAB 6: Your code here
int mon_memory(int argc, char **argv, struct Trapframe *tf){
    dump_memory_lists();
    return 0;
}

/* Implement mon_pagetable() and mon_virt()
 * (using dump_virtual_tree(), dump_page_table())*/
// LAB 7: Your code here

/* Kernel monitor command interpreter */

static int
runcmd(char *buf, struct Trapframe *tf) {
    int argc = 0;
    char *argv[MAXARGS];

    argv[0] = NULL;

    /* Parse the command buffer into whitespace-separated arguments */
    for (;;) {
        /* gobble whitespace */
        while (*buf && strchr(WHITESPACE, *buf)) *buf++ = 0;
        if (!*buf) break;

        /* save and scan past next arg */
        if (argc == MAXARGS - 1) {
            cprintf("Too many arguments (max %d)\n", MAXARGS);
            return 0;
        }
        argv[argc++] = buf;
        while (*buf && !strchr(WHITESPACE, *buf)) buf++;
    }
    argv[argc] = NULL;

    /* Lookup and invoke the command */
    if (!argc) return 0;
    for (size_t i = 0; i < NCOMMANDS; i++) {
        if (strcmp(argv[0], commands[i].name) == 0)
            return commands[i].func(argc, argv, tf);
    }

    cprintf("Unknown command '%s'\n", argv[0]);
    return 0;
}

void
monitor(struct Trapframe *tf) {

    cprintf("Welcome to the JOS kernel monitor!\n");
    cprintf("Type 'help' for a list of commands.\n");

    if (tf) print_trapframe(tf);

    char *buf;
    do buf = readline("K> ");
    while (!buf || runcmd(buf, tf) >= 0);
}
