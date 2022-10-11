/* See COPYRIGHT for copyright information. */

#include <inc/x86.h>
#include <kern/kclock.h>
#include <kern/timer.h>
#include <kern/trap.h>
#include <kern/picirq.h>

/* HINT: Note that selected CMOS
 * register is reset to the first one
 * after first access, i.e. it needs to be selected
 * on every access.
 *
 * Don't forget to disable NMI for the time of
 * operation (look up for the appropriate constant in kern/kclock.h)
 *
 * Why it is necessary?
 */

uint8_t
cmos_read8(uint8_t reg) {
    /* MC146818A controller */
    // LAB 4: Your code here

    // Do we have to use nmi_disable() here?
    //   Because we have nmi_enable and we
    //   wouldn't enable non-masked interrupts
    //   before this code, this code may
    //   need protection, not allowing
    //   something else to execute.

    outb(CMOS_CMD, reg);
    uint8_t res = inb(CMOS_DATA);

    nmi_enable();
    return res;
}

void
cmos_write8(uint8_t reg, uint8_t value) {
    // LAB 4: Your code here

    // Do we have to use nmi_disable() here?
    //   Because we have nmi_enable and we
    //   wouldn't enable non-masked interrupts
    //   before this code, this code may
    //   need protection, not allowing
    //   something else to execute.

    // There are maskable and non-maskable interrupts.
    // Clock interrupt is not maskable, but it can
    //   be disabled via telling CMOS to not fire the
    //   interrupt for some time. Also maybe we
    //   should disable it while talking with CMOS, so
    //   that interrupt handler won't overwrite what
    //   we just written to the ports. Imagine clock
    //   interrupt happening between these two calls :)
    //   I'll also talk about this with someone to see
    //   if this is true.
    // Also there is an interrupt that was recharging
    //   memory cells of RAM. Cells of RAM store data
    //   as long as it has power. It's made out of
    //   condensers. There are some charge leaks
    //   always. So to not lose the data, it has to be
    //   recharged. It's recharged when it's accessed.
    //   So there is an interrupt that is fired
    //   periodically that accesses all of the memory,
    //   so that RAM recharges itself. That interrupt
    //   is not maskable, as far as I remember. And
    //   interrupt may take some time, cells would
    //   discharge. But maybe not, because I don't
    //   know what time it takes for cells to discharge
    //   due to charge leaks.

    outb(CMOS_CMD, reg);
    outb(CMOS_DATA, value);

    nmi_enable();
}

uint16_t
cmos_read16(uint8_t reg) {
    return cmos_read8(reg) | (cmos_read8(reg + 1) << 8);
}

static void
rtc_timer_pic_interrupt(void) {
    // LAB 4: Your code here
    // Enable PIC interrupts.
    pic_irq_unmask(IRQ_CLOCK);
}

static void
rtc_timer_pic_handle(void) {
    // Tell the RTC that we have processed the interrupt.
    // The same we would do with a keyboard, blinking to the port dedicated to it.
    rtc_check_status();

    // Tell the interrupt controller we are
    //   done with this interrupt.
    // In fact, we are not. If this is a maskable
    //   interrupt, it will be ignored (Right? Maybe I'm wrong.)
    //   until iret is executed, which enables maskable interrupts.
    // Iret is done by env_run.
    // But we should not get another interrupt from clock. How do we deal with that?
    pic_send_eoi(IRQ_CLOCK);
}

static const uint8_t RTC_REG_A = 10;
static const uint8_t RTC_REG_B = 11;
static const uint8_t RTC_REG_C = 12;

struct Timer timer_rtc = {
        .timer_name = "rtc",
        .timer_init = rtc_timer_init,
        .enable_interrupts = rtc_timer_pic_interrupt,
        .handle_interrupts = rtc_timer_pic_handle,
};

void
rtc_timer_init(void) {
    // LAB 4: Your code here
    // (use cmos_read8/cmos_write8)

    uint8_t value = 0;

    static const uint8_t RTC_FREQUENCY_BIT_MASK = 0xF;
    static const uint8_t RTC_HALF_SECOND_PERIOD_RATE_MASK = 0xF; // For 2hz frequency we should set all bits to ones. 1111 for the low bits of reg A.

    value = cmos_read8(RTC_REG_A);
    value &= ~RTC_FREQUENCY_BIT_MASK;
    value |= RTC_HALF_SECOND_PERIOD_RATE_MASK;
    cmos_write8(RTC_REG_A, value);
    // cmos_write8(RTC_REG_A, (cmos_read8(RTC_REG_A) & 0xF0) | 0x0F);

    // Enable periodic interrupts (PIE -- periodic interrupt enable).
    cmos_read8(RTC_REG_B);
    value |= RTC_PIE;
    cmos_write8(RTC_REG_B, value);
}

uint8_t
rtc_check_status(void) {
    // LAB 4: Your code here
    // (use cmos_read8)

    return cmos_read8(RTC_REG_C);
}
