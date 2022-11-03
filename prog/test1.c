void (*volatile sys_yield)(void);

int (*volatile cprintf)(char const* fmt, ...);

void
umain(int argc, char **argv) {
    cprintf("Hello from test1!!\n");
    int i, j;

    for (j = 0; j < 3; ++j) {
        for (i = 0; i < 10000; ++i)
            ;
        sys_yield();
    }

    cprintf("Hello another time from test1!!\n");
}
