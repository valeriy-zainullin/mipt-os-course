#include <inc/types.h>
#include <inc/assert.h>
#include <inc/string.h>
#include <inc/memlayout.h>
#include <inc/stdio.h>
#include <inc/x86.h>
#include <inc/uefi.h>
#include <kern/timer.h>
#include <kern/kclock.h>
#include <kern/picirq.h>
#include <kern/trap.h>
#include <kern/pmap.h>

#define kilo      (1000ULL)
#define Mega      (kilo * kilo)
#define Giga      (kilo * Mega)
#define Tera      (kilo * Giga)
#define Peta      (kilo * Tera)
#define ULONG_MAX ~0UL

#if LAB <= 6
/* Early variant of memory mapping that does 1:1 aligned area mapping
 * in 2MB pages. You will need to reimplement this code with proper
 * virtual memory mapping in the future. */
void *
mmio_map_region(physaddr_t pa, size_t size) {
    void map_addr_early_boot(uintptr_t addr, uintptr_t addr_phys, size_t sz);
    const physaddr_t base_2mb = 0x200000;
    uintptr_t org = pa;
    size += pa & (base_2mb - 1);
    size += (base_2mb - 1);
    pa &= ~(base_2mb - 1);
    size &= ~(base_2mb - 1);
    map_addr_early_boot(pa, pa, size);
    return (void *)org;
}
void *
mmio_remap_last_region(physaddr_t pa, void *addr, size_t oldsz, size_t newsz) {
    return mmio_map_region(pa, newsz);
}
#endif

struct Timer timertab[MAX_TIMERS];
struct Timer *timer_for_schedule;

struct Timer timer_hpet0 = {
        .timer_name = "hpet0",
        .timer_init = hpet_init,
        .get_cpu_freq = hpet_cpu_frequency,
        .enable_interrupts = hpet_enable_interrupts_tim0,
        .handle_interrupts = hpet_handle_interrupts_tim0,
};

struct Timer timer_hpet1 = {
        .timer_name = "hpet1",
        .timer_init = hpet_init,
        .get_cpu_freq = hpet_cpu_frequency,
        .enable_interrupts = hpet_enable_interrupts_tim1,
        .handle_interrupts = hpet_handle_interrupts_tim1,
};

struct Timer timer_acpipm = {
        .timer_name = "pm",
        .timer_init = acpi_enable,
        .get_cpu_freq = pmtimer_cpu_frequency,
};

void
acpi_enable(void) {
    FADT *fadt = get_fadt();
    outb(fadt->SMI_CommandPort, fadt->AcpiEnable);
    while ((inw(fadt->PM1aControlBlock) & 1) == 0) /* nothing */
        ;
}

// Some people may think comments are bad, things to read.
//   But I actually try to explain stuff there. Others will
//   find it useful, someone cared that they'll understand
//   what's going on and why some decisions were made in
//   the past, they'll be able to make dicisions in future.
//   These comments may be the reason they'll get what's
//   going on also. It's alright to skip reading comments
//   if needed.

static bool
acpi_verify_sdt_header(ACPISDTHeader const* sdt_header, char const* signature) {
    uint8_t checksum = 0;

    // Length is total size of the header. For checksum we have to sum the whole
    //   table. Header is included into Length.
    assert(sdt_header->Length >= sizeof(ACPISDTHeader));
    for (size_t offset = 0; offset < sdt_header->Length; ++offset) {
        uint8_t const * byte_ptr = (uint8_t const *) sdt_header + offset;
        uint8_t byte = *byte_ptr;
        checksum += byte;
    }

    // If SDT header is invalid, note this in log.
    // No corruption happened yet, so it's not an assert, if
    //   the service who requested the table didn't get it, it
    //   should panic on it's own, in case needed.
    if (checksum != 0) {
        warn("sdt header's checksum is invalid: got 0x%02x, expected 0x00.", (unsigned int) checksum);
        return false;
    }
    if (signature != NULL) {
        if (strncmp(sdt_header->Signature, signature, sizeof(sdt_header->Signature)) != 0) {
            char header_signature[sizeof(sdt_header->Signature) / sizeof(char) + 1] = {0};
            memcpy(header_signature, sdt_header->Signature, sizeof(sdt_header->Signature));
            warn("sdt header's signature is invalid: got \"%s\", expected \"%s\".", header_signature, signature);
            return false;
        }
    }
    
    return true;
}

// TODO: use mmio_map_region, mmio_remap_last_region
static void *
acpi_find_table(const char *signature, size_t header_size) {
    /*
     * This function performs lookup of ACPI table by its signature
     * and returns valid pointer to the table mapped somewhere.
     *
     * It is a good idea to checksum tables before using them.
     *
     * HINT: Use mmio_map_region/mmio_remap_last_region
     * before accessing table addresses
     * (Why mmio_remap_last_region is required?)
     * HINT: RSDP address is stored in uefi_lp->ACPIRoot
     * HINT: You may want to distunguish RSDT/XSDT
     */

    // LAB 5: Your code here

    // Map region before usage, so that
    //   to not hit a page fault.
    RSDP* rsdp = (RSDP*) mmio_map_region(uefi_lp->ACPIRoot, sizeof(RSDP));

    // JOS assumes it's not ACPI 1.0 in the structure typedef.
    // And we later assume it has extended checksum.
    assert(rsdp->Revision >= 2);

    uint8_t checksum = 0;
    for (size_t offset = 0; offset < offsetof(RSDP, Length); ++offset) {
        uint8_t* byte_ptr = (uint8_t*) rsdp + offset;
        uint8_t byte = *byte_ptr;
        checksum += byte;
    }
    if (checksum != 0) {
        return NULL;
    }

    uint8_t extended_checksum = 0;
    for (uint8_t offset = 0; offset < sizeof(RSDP) - sizeof(rsdp->reserved); ++offset) {
        uint8_t* byte_ptr = (uint8_t*) rsdp + offset;
        uint8_t byte = *byte_ptr;
        extended_checksum += byte;
    }
    if (extended_checksum != 0) {
        return NULL;
    }

    // Check signature.
    if (strncmp(rsdp->Signature, "RSD PTR ", 8) != 0) {
        return NULL;
    }

    // Both rsdt and xsdt are present (we expect acpi 2.0 and higher),
    //   we should pick xsdt, even in compatibility mode (which is not
    //   our case). We'd have to cast xsdt address to (uint32_t) and
    //   read xsdt.
    // We assume this is true. Although it's not used in the code later.
    // Identify hardware that doesn't conform to this in debug builds.
    // If that happens, write it here and make it a warning in log.
    // Such hardware could have two copies of xsdt in different places.
    assert((uint32_t) rsdp->XsdtAddress == rsdp->XsdtAddress);

    // Map region before usage, so that
    //   to not hit a page fault.
    XSDT* xsdt = (XSDT*) mmio_map_region(rsdp->XsdtAddress, sizeof(XSDT));

    if (!acpi_verify_sdt_header((ACPISDTHeader const*) xsdt, "XSDT")) {
        return NULL;
    }
    
    size_t num_tables = (xsdt->h.Length - sizeof(xsdt->h)) / sizeof(uint64_t);
    // Find the right table.
    ACPISDTHeader* sdt_header = NULL;
    for (size_t i = 0; i < num_tables; ++i) {
        uint64_t table_address = xsdt->PointerToOtherSDT[i];

        // Map region before usage, so that
        //   to not hit a page fault.
        // Remap previously mapped region.
        //   So that we don't exhaust mmio
        //   area.
        // ACPISDTHeader* sdt_header = NULL;
        if (i == 0) {
            // First region is mapped.
            sdt_header = (ACPISDTHeader*) mmio_map_region(table_address, sizeof(ACPISDTHeader));
        } else {
            // Remap last region.
            sdt_header = (ACPISDTHeader*) mmio_remap_last_region(table_address, sdt_header, sizeof(ACPISDTHeader), sizeof(ACPISDTHeader));            
        }
        if (strncmp(sdt_header->Signature, signature, sizeof(sdt_header->Signature)) != 0) {
            continue;
        }

        // Signature is verified already :)
        if (!acpi_verify_sdt_header(sdt_header, NULL)) {
            // Maybe there's another table with the right checksum?
            //   Continue the loop, but it shouldn't be there, it's unlikely.
            //   Maybe it's even out of spec.
            continue;
        }

        // Remap to real size of structure, we looked at some number
        //   of sizeof(ACPISDTHeader) first bytes (we didn't know
        //   if memory is correct, map what's needed).
        // Expand this region. It's great it's
        //   the last region (and we don't have
        //   SMP in that sense).
        return mmio_remap_last_region(table_address, sdt_header, sizeof(ACPISDTHeader), header_size);
    }
    // Didn't find the table, but used a mmio region.
    //   Not a problem, because we use functions
    //   like get_fadt, they won't call us anymore
    //   for that table.

    return NULL;
}

/* Obtain and map FADT ACPI table address. */
FADT *
get_fadt(void) {
    // LAB 5: Your code here
    // (use acpi_find_table)
    // HINT: ACPI table signatures are
    //       not always as their names

    static bool tried_to_find = false;
    static FADT *kfadt = NULL;
    if (kfadt == NULL && !tried_to_find) {
        kfadt = acpi_find_table("FACP", sizeof(FADT));
        // If returns NULL, there's no such table.
        //   It won't appear out of nothing, so
        //   it's ok.
        tried_to_find = true;
    }

    if (kfadt == NULL) {
        panic("FADT acpi table wasn't found.");
    }

    return kfadt;
}

/* Obtain and map RSDP ACPI table address. */
HPET *
get_hpet(void) {
    // LAB 5: Your code here
    // (use acpi_find_table)

    static bool tried_to_find = false;
    static HPET *khpet = NULL;
    if (khpet == NULL) {
        khpet = acpi_find_table("HPET", sizeof(HPET));
        // If returns NULL, there's no such table.
        //   It won't appear out of nothing, so
        //   it's ok.
        tried_to_find = true;
    }

    if (khpet == NULL) {
        panic("HPET acpi table wasn't found.");
    }

    // From hpet spec:
    //   This indicates which revision of the function is implemented.
    //   The value must NOT be 00h.
    if (khpet->hardware_rev_id == 0) {
        panic("HPET hardware rev id is zero.");
    }

    // From hpet spec:
    //   This bit is a 0 to indicate that the main counter is 32 bits
    //   wide (and cannot operate in 64-bit mode).
    if (khpet->counter_size == 0) {
        panic("HPET main counter cannot operate in 64-bit mode.");
    }

    // From hpet spec:
    //   LegacyReplacement Route Capable: If this bit is a 1, it indicates that the
    //   hardware supports the LegacyReplacement Interrupt Route option.
    if (khpet->legacy_replacement != 1) {
        panic("HPET doesn't support legacy relacement interrupt route.");
    }

    return khpet;
}

/* Getting physical HPET timer address from its table. */
HPETRegister *
hpet_register(void) {
    HPET *hpet_timer = get_hpet();
    if (!hpet_timer->address.address) panic("hpet is unavailable\n");

    uintptr_t paddr = hpet_timer->address.address;
    return mmio_map_region(paddr, sizeof(HPETRegister));
}

/* Debug HPET timer state. */
void
hpet_print_struct(void) {
    HPET *hpet = get_hpet();
    assert(hpet != NULL);
    cprintf("signature = %s\n", (hpet->h).Signature);
    cprintf("length = %08x\n", (hpet->h).Length);
    cprintf("revision = %08x\n", (hpet->h).Revision);
    cprintf("checksum = %08x\n", (hpet->h).Checksum);

    cprintf("oem_revision = %08x\n", (hpet->h).OEMRevision);
    cprintf("creator_id = %08x\n", (hpet->h).CreatorID);
    cprintf("creator_revision = %08x\n", (hpet->h).CreatorRevision);

    cprintf("hardware_rev_id = %08x\n", hpet->hardware_rev_id);
    cprintf("comparator_count = %08x\n", hpet->comparator_count);
    cprintf("counter_size = %08x\n", hpet->counter_size);
    cprintf("reserved = %08x\n", hpet->reserved);
    cprintf("legacy_replacement = %08x\n", hpet->legacy_replacement);
    cprintf("pci_vendor_id = %08x\n", hpet->pci_vendor_id);
    cprintf("hpet_number = %08x\n", hpet->hpet_number);
    cprintf("minimum_tick = %08x\n", hpet->minimum_tick);

    cprintf("address_structure:\n");
    cprintf("address_space_id = %08x\n", (hpet->address).address_space_id);
    cprintf("register_bit_width = %08x\n", (hpet->address).register_bit_width);
    cprintf("register_bit_offset = %08x\n", (hpet->address).register_bit_offset);
    cprintf("address = %08lx\n", (unsigned long)(hpet->address).address);
}

static volatile HPETRegister *hpetReg;
/* HPET timer period (in femtoseconds) */
static uint64_t hpetFemto = 0;
/* HPET timer frequency */
static uint64_t hpetFreq = 0;

/* HPET timer initialization */
void
hpet_init() {
    if (hpetReg == NULL) {
        nmi_disable();
        hpetReg = hpet_register();
        uint64_t cap = hpetReg->GCAP_ID;
        hpetFemto = (uintptr_t)(cap >> 32);
        if (!(cap & HPET_LEG_RT_CAP)) panic("HPET has no LegacyReplacement mode");

        /* cprintf("hpetFemto = %llu\n", hpetFemto); */
        hpetFreq = (1 * Peta) / hpetFemto;
        /* cprintf("HPET: Frequency = %d.%03dMHz\n", (uintptr_t)(hpetFreq / Mega), (uintptr_t)(hpetFreq % Mega)); */
        /* Enable ENABLE_CNF bit to enable timer */
        hpetReg->GEN_CONF |= HPET_ENABLE_CNF;
        nmi_enable();
    }
}

/* HPET register contents debugging. */
void
hpet_print_reg(void) {
    cprintf("GCAP_ID = %016lx\n", (unsigned long)hpetReg->GCAP_ID);
    cprintf("GEN_CONF = %016lx\n", (unsigned long)hpetReg->GEN_CONF);
    cprintf("GINTR_STA = %016lx\n", (unsigned long)hpetReg->GINTR_STA);
    cprintf("MAIN_CNT = %016lx\n", (unsigned long)hpetReg->MAIN_CNT);
    cprintf("TIM0_CONF = %016lx\n", (unsigned long)hpetReg->TIM0_CONF);
    cprintf("TIM0_COMP = %016lx\n", (unsigned long)hpetReg->TIM0_COMP);
    cprintf("TIM0_FSB = %016lx\n", (unsigned long)hpetReg->TIM0_FSB);
    cprintf("TIM1_CONF = %016lx\n", (unsigned long)hpetReg->TIM1_CONF);
    cprintf("TIM1_COMP = %016lx\n", (unsigned long)hpetReg->TIM1_COMP);
    cprintf("TIM1_FSB = %016lx\n", (unsigned long)hpetReg->TIM1_FSB);
    cprintf("TIM2_CONF = %016lx\n", (unsigned long)hpetReg->TIM2_CONF);
    cprintf("TIM2_COMP = %016lx\n", (unsigned long)hpetReg->TIM2_COMP);
    cprintf("TIM2_FSB = %016lx\n", (unsigned long)hpetReg->TIM2_FSB);
}

/* HPET main timer counter value. */
uint64_t
hpet_get_main_cnt(void) {
    return hpetReg->MAIN_CNT;
}

/* - Configure HPET timer 0 to trigger every 0.5 seconds on IRQ_TIMER line
 * - Configure HPET timer 1 to trigger every 1.5 seconds on IRQ_CLOCK line
 *
 * HINT To be able to use HPET as PIT replacement consult
 *      LegacyReplacement functionality in HPET spec.
 * HINT Don't forget to unmask interrupt in PIC */
void
hpet_enable_interrupts_tim0(void) {
    // LAB 5: Your code here

    // fixme: finish this function with comments what lines do, for some lines also why.

    // 
    hpetReg->MAIN_CNT = 0;

    // Enable legacy replacement mode.
    hpetReg->GEN_CONF |= HPET_LEG_RT_CNF;

    // TODO: do we need to zero-out previous value?
    hpetReg->TIM0_CONF |= HPET_TN_TYPE_CNF;
    hpetReg->TIM0_CONF |= HPET_TN_INT_ENB_CNF;
    hpetReg->TIM0_CONF |= HPET_TN_VAL_SET_CNF;
    hpetReg->TIM0_COMP = (hpetFemto / (hpetReg->GCAP_ID >> 32));

    hpetReg->GEN_CONF |= HPET_ENABLE_CNF;

    pic_irq_unmask(IRQ_TIMER);
}

void
hpet_enable_interrupts_tim1(void) {
    // LAB 5: Your code here

    // fixme: finish this function with comments what lines do, for some lines also why.

    // 
    hpetReg->MAIN_CNT = 0;

    // Enable legacy replacement mode.
    hpetReg->GEN_CONF |= HPET_LEG_RT_CNF;

    // TODO: do we need to zero-out previous value?
    hpetReg->TIM0_CONF |= HPET_TN_TYPE_CNF;
    hpetReg->TIM0_CONF |= HPET_TN_INT_ENB_CNF;
    hpetReg->TIM0_CONF |= HPET_TN_VAL_SET_CNF;
    hpetReg->TIM1_COMP = 3 * (hpetFemto / (hpetReg->GCAP_ID >> 32)) / 2;

    hpetReg->GEN_CONF |= HPET_ENABLE_CNF;

    pic_irq_unmask(IRQ_CLOCK);
}

void
hpet_handle_interrupts_tim0(void) {
    pic_send_eoi(IRQ_TIMER);
}

void
hpet_handle_interrupts_tim1(void) {
    pic_send_eoi(IRQ_CLOCK);
}

/* Calculate CPU frequency in Hz with the help with HPET timer.
 * HINT Use hpet_get_main_cnt function and do not forget about
 * about pause instruction. */
uint64_t
hpet_cpu_frequency(void) {
    static uint64_t cpu_freq = 0;

    // LAB 5: Your code here

    if (cpu_freq != 0) {
        return cpu_freq;
    }

    uint64_t hpet_counter_start = hpet_get_main_cnt();
    uint64_t tsc_start = read_tsc();

    for (int i = 0; i < 10000; ++i) {
        asm volatile("pause");
    }

    uint64_t hpet_counter_end = hpet_get_main_cnt();
    uint64_t tsc_end = read_tsc();

    // TODO: clarify why this always holds. Related to range
    //   of counter values, won't overflow for 30 years at
    //   least :)
    assert(hpet_counter_start <= hpet_counter_end);
    uint64_t hpet_counter_diff = hpet_counter_end - hpet_counter_start;
    uint64_t tsc_diff = tsc_end - tsc_start; // TODO: Can this overflow? If not, assert and explain why.

    cpu_freq = tsc_diff * hpetFreq / hpet_counter_diff;

    return cpu_freq;
}

uint32_t
pmtimer_get_timeval(void) {
    FADT *fadt = get_fadt();
    return inl(fadt->PMTimerBlock);
}

/* Calculate CPU frequency in Hz with the help with ACPI PowerManagement timer.
 * HINT Use pmtimer_get_timeval function and do not forget that ACPI PM timer
 *      can be 24-bit or 32-bit. */
uint64_t
pmtimer_cpu_frequency(void) {
    static uint64_t cpu_freq = 0;

    // LAB 5: Your code here

    if (cpu_freq != 0) {
        return cpu_freq;
    }

    uint64_t pm_counter_start = pmtimer_get_timeval();
    uint64_t tsc_start = read_tsc();

    for (int i = 0; i < 10000; ++i) {
        asm volatile("pause");
    }

    uint64_t pm_counter_end = pmtimer_get_timeval();
    uint64_t tsc_end = read_tsc();

    uint64_t pm_counter_diff = 0;
    uint64_t tsc_diff = tsc_end - tsc_start; // TODO: Can this overflow? If not, assert and explain why.

    // If counter overflows, we don't know what timer
    //   it would be. We know that for 24-bit timer
    //   the difference is also 24-bit, otherwise
    //   it's 32-bit timer for sure. But if
    //   difference fits into 24 bits, we don't know
    //   if timer is 24 bit.
    if (pm_counter_start <= pm_counter_end) {
        pm_counter_diff = pm_counter_end - pm_counter_start;
    } else if (pm_counter_start - pm_counter_end > ((1u << 24) - 1u)) {
        // Surely 32-bit timer.
        // We don't know how many times it overflown though. Assume it did so once.
        pm_counter_diff = UINT32_MAX - pm_counter_start + pm_counter_end;
    } else {
        // Might have also been 32-bit timer..
        //   But we assume it's 24-bit.
        pm_counter_diff = (1u << 24) - pm_counter_start + pm_counter_end;
    }

    cpu_freq = tsc_diff * PM_FREQ / pm_counter_diff;

    return cpu_freq;
}
