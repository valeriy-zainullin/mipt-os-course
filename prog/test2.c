
int (*volatile cprintf)(const char *fmt, ...);
void (*volatile sys_yield)(void);

#if defined(GRADE3_TEST)
#define xstr(s)  str(s)
#define str(s)   #s
#define xc(a, b) c(a, b)
#define c(a, b)  a##b
void (*volatile GRADE3_FUNC)(unsigned);
void (*volatile xc(GRADE3_FAIL, GRADE3_PFX1))(void);
#endif

void great_function_impl() { cprintf("Great function pointer was not overriden!\n"); }
void (*volatile great_function)(void) = great_function_impl;


void
umain(int argc, char **argv) {
    int test2_i;
    int test2_j;

    great_function();

#if !defined(GRADE3_TEST)
    cprintf("TEST2 LOADED.\n");
#else
    GRADE3_FUNC(xstr(GRADE3_FUNC)[0]);
    if (xc(GRADE3_FAIL, GRADE3_PFX1)) {
        xc(GRADE3_FAIL, GRADE3_PFX1)();
    }
#endif

    cprintf("TEST2 STARTED.\n");
    for (test2_j = 0; test2_j < 5; ++test2_j) {
        for (test2_i = 0; test2_i < 100; ++test2_i)
            cprintf("TEST2 LOADED.\n");
            ;
        sys_yield();
    }
    cprintf("TEST2 DONE.\n");
}
