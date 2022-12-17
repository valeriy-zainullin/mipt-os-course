#include <inc/types.h>
#include <inc/stdio.h>
#include <inc/x86.h>

#include <kern/net.h>

#include <assert.h>
#include <inttypes.h>

static const unsigned int PCI_CONFIGURATION_ADDRESS_PORT = 0xCF8;
static const unsigned int PCI_CONFIGURATION_DATA_PORT = 0xCFC;

// TODO: maybe implement pci_device_read_dword(). If we see we need to read 32 bits.
// https://wiki.osdev.org/PCI#Configuration_Space_Access_Mechanism_.231
static uint16_t
pci_device_read_word(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    // From osdev.org:
    //   The CONFIG_ADDRESS is a 32-bit register with the format shown in following
    //   figure. Bit 31 is an enable flag for determining when accesses to
    //   CONFIG_DATA should be translated to configuration cycles.
    //   Bits 23 through 16 allow the configuration software to choose a specific
    //   PCI bus in the system.
    //   Bits 15 through 11 select the specific device on the PCI Bus.
    //   Bits 10 through 8 choose a specific function in a device (if the device
    //   supports multiple functions).
    // I assume if we need to read device vendor id, we just need to use function 0.
    //   I think it should be always present.

    static const uint32_t PCI_CONFIG_ADDR_ENABLE_BIT_MASK = ((uint32_t) 1) << 31;

    uint32_t config_address = 0;

    // Сборка адреса конфигурации по шине, слоту, функции и смещению.
    config_address |= PCI_CONFIG_ADDR_ENABLE_BIT_MASK;
    config_address |= ((uint32_t)  bus) << 16;
    config_address |= ((uint32_t) slot) << 11;
    config_address |= ((uint32_t) func) <<  8;

    // Register Offset has to point to consecutive DWORDs, ie. bits 1:0 are
    //   always 0b00 (they are still part of the Register Offset). 
    config_address |= (uint32_t) offset & 0xFC;

    outl(PCI_CONFIGURATION_ADDRESS_PORT, config_address);

    uint32_t result = inl(PCI_CONFIGURATION_DATA_PORT);
    // Assuming little endian. TODO: check if it is specified in spec.
    if (offset % 4 == 0) {
        // Select lower word.
        result &= 0xFFFF;
        return (uint16_t) result;
    } else {
        // Select higher word.
        result >>= 16;
        result &= 0xFFFF;
        return (uint16_t) result;
    }
}

static uint16_t
pci_device_read_byte(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint8_t dword_offset = offset & (~0x01); // Unset the last bit, to make it even.
    uint16_t result = pci_device_read_word(bus, slot, func, dword_offset);
    if (offset % 2 == 0) {
        result &= 0xFF;
        return (uint8_t) result;
    } else {
        result >>= 8;
        result &= 0xFF;
        return (uint8_t) result;
    }
}

/* From osdev.
Common fields for all PCI devices (all of configuration space headers):
Register    Offset  Bits 31-24  Bits 23-16  Bits 15-8     Bits 7-0
     0x0       0x0         Device ID               Vendor ID 
     0x1       0x4         Status                  Command
     0x2       0x8  Class code  Subclass    Prog IF       Revision ID
     0x3       0xC  BIST        Header type Latency Timer Cache Line Size
Device ID: Identifies the particular device. Where valid IDs are allocated by the vendor.
Vendor ID: Identifies the manufacturer of the device. Where valid IDs are allocated by
  PCI-SIG (the list is here) to ensure uniqueness and 0xFFFF is an invalid value that
  will be returned on read accesses to Configuration Space registers of non-existent devices.
Status: A register used to record status information for PCI bus related events.
Command: Provides control over a device's ability to generate and respond to PCI cycles.
  Where the only functionality guaranteed to be supported by all devices is, when a 0 is
  written to this register, the device is disconnected from the PCI bus for all accesses
  except Configuration Space access.
Class Code: A read-only register that specifies the type of function the device performs.
Subclass: A read-only register that specifies the specific function the device performs.
Prog IF(Programming Interface Byte): A read-only register that specifies a register-level
  programming interface the device has, if it has any at all.
Revision ID: Specifies a revision identifier for a particular device. Where valid IDs are
  allocated by the vendor.
BIST: Represents that status and allows control of a devices BIST (built-in self test).
Header Type: Identifies the layout of the rest of the header beginning at byte 0x10 of
  the header and also specifies whether or not the device has multiple functions. Where a
  value of 0x0 specifies a general device, a value of 0x1 specifies a PCI-to-PCI bridge,
  and a value of 0x2 specifies a CardBus bridge. If bit 7 of this register is set, the
  device has multiple functions; otherwise, it is a single function device.
Latency Timer: Specifies the latency timer in units of PCI bus clocks.
Cache Line Size: Specifies the system cache line size in 32-bit units. A device can limit
  the number of cacheline sizes it can support, if a unsupported value is written to this
  field, the device will behave as if a value of 0 was written.
*/

// Common fields of PCI device header have many fields of the same size adjacent.
//   I think it's better to count sum of numbers of fields times their size.
//   The reader could count fields instead of addresseses. And
//   addresses are written on the right, in comments. I think
//   writing raw hex addresses discouarages a person, who reads
//   them, to compare it with the source of information.
//   I think that counting fields of a structure is a more pleasant thing.
static uint8_t PCI_CONFIG_SPACE_VENDOR_ID_OFFSET   =  0 * 2;     // 00 = 0x00
static uint8_t PCI_CONFIG_SPACE_DEVICE_ID_OFFSET   =  1 * 2;     // 02 = 0x02
static uint8_t PCI_CONFIG_SPACE_STATUS_OFFSET      =  3 * 2;     // 06 = 0x06
static uint8_t PCI_CONFIG_SPACE_SUBCLASS_OFFSET    =  2 + 4 * 2; // 09 = 0x09
static uint8_t PCI_CONFIG_SPACE_CLASS_CODE_OFFSET  =  3 + 4 * 2; // 11 = 0x0B
// static uint8_t PCI_CONFIG_SPACE_HEADER_TYPE_OFFSET =  4 * 2 + ?; // 13 = 0x0D

static const uint16_t PCI_VENDOR_ID_DEVICE_NOT_EXISTS = 0xFFFF;
// https://wiki.osdev.org/PCI#PCI_Device_Structure
// From osdev:
//   The PCI Specification defines the organization of the 256-byte
//   Configuration Space registers and imposes a specific
//   template for the space. Figures 2 & 3 show the layout of the
//   256-byte Configuration space. All PCI compliant devices must
//   support the Vendor ID, Device ID, Command and Status,
//   Revision ID, Class Code and Header Type fields.
//   Implementation of the other registers is optional, depending
//   upon the devices functionality. 
// All devices should have vendor id in their configuration space
//   and if a device is not present at bus and slot, then 0xFFFF
//   is returned for all reads. This way we can check if device
//   is present, as there's no vendor with id 0xFFFF. Found this
//   in the osdev's article.
// If returns PCI_VENDOR_ID_DEVICE_NOT_EXISTS, the device is not present.
static uint16_t
pci_read_vendor_id(uint8_t bus, uint8_t slot, uint8_t function) {
    // Vendor id goes first in the configuration space and device id is second.
    return pci_device_read_word(bus, slot, function, PCI_CONFIG_SPACE_VENDOR_ID_OFFSET);
}

// Similar to pci_read_vendor_id. For symmetricity and also
//   both of these values are needed to find a particular
//   device, so makes sense to have functions for both of
//   them.
static uint16_t
pci_read_device_id(uint8_t bus, uint8_t slot, uint8_t function) {
    // Device id is straight after vendor id, which goes first in the configuration space.
    return pci_device_read_word(bus, slot, function, PCI_CONFIG_SPACE_DEVICE_ID_OFFSET);
}

static bool trace_pci = true;

static bool
pci_detect_device(uint16_t vendor_id, uint16_t device_id, uint8_t* bus, uint8_t* slot, uint8_t* function) {
    // We try to enumerate all possible buses and slots to find the device.
    //  That's the easiest way and we don't need something more complex
    //  right now.
    //  https://wiki.osdev.org/PCI#Enumerating_PCI_Buses

    // This vendor is should not exist anyway and we won't find this device.
    assert(vendor_id != PCI_VENDOR_ID_DEVICE_NOT_EXISTS);

    uint16_t target_vendor_id = vendor_id;
    uint16_t target_device_id = device_id;

    for (uint16_t cur_bus = 0; cur_bus < UINT8_MAX; ++cur_bus) {
        for (uint16_t cur_slot = 0; cur_slot < UINT8_MAX; ++cur_slot) {
            // One device may have many functions. These functions technically
            //   may have different vendor ids and device ids...
            //   In qemu there is a device with many (more than one) functions,
            //   it has different device id for each function. I think, such
            //   situation may also happen on real hardware, we have to scan
            //   all of the functions to find our vendor_id and device_id pair.
            // Not more than 8 functions though. Function is only 3 bits wide.
            for (uint8_t cur_function = 0; cur_function < 8; ++cur_function) {
                uint16_t dev_vendor_id = pci_read_vendor_id(cur_bus, cur_slot, cur_function);
                if (dev_vendor_id == PCI_VENDOR_ID_DEVICE_NOT_EXISTS) {
                    continue;
                }

                // Warning: may not print every device, as it stops when the request is fullfilled.
                if (trace_pci) {
                    uint16_t dev_device_id  = pci_read_device_id(cur_bus, cur_slot, cur_function);
                    uint16_t dev_status     = pci_device_read_word(cur_bus, cur_slot, cur_function, PCI_CONFIG_SPACE_STATUS_OFFSET);
                    uint8_t  dev_class_code = pci_device_read_byte(cur_bus, cur_slot, cur_function, PCI_CONFIG_SPACE_CLASS_CODE_OFFSET);
                    uint8_t  dev_subclass   = pci_device_read_byte(cur_bus, cur_slot, cur_function, PCI_CONFIG_SPACE_SUBCLASS_OFFSET);
                    
                    cprintf("Found pci device 0x%04" PRIx16 ":0x%04" PRIx16 ", status = 0b%016hb, class_code = %" PRIu8 ", subclass = %" PRIu8 " at %02" PRIu16 ":%02" PRIu16 ".%" PRIu16 ".\n", dev_vendor_id, dev_device_id, dev_status, dev_class_code, dev_subclass, cur_bus, cur_slot, cur_function);
                }

                if (dev_vendor_id != target_vendor_id) {
                    continue;
                }
            
                uint16_t dev_device_id = pci_read_device_id(cur_bus, cur_slot, cur_function);
                if (dev_device_id != target_device_id) {
                    continue;
                }

                *bus = cur_bus;
                *slot = cur_slot;
                *function = cur_function;
                return true;
            }
        }
    }

    return false;
}

static bool
net_detect_e1000(uint8_t* bus, uint8_t* slot, uint8_t* function) {
    // Got info about what vendor id I should enter from these links:
    //   https://wiki.osdev.org/PCI#Common_Header_Fields
    //   https://pcisig.com/membership/member-companies?combine=Intel
    // Intel e1000 is a family of ethernet controllers, so there's
    //   no single device id to look for. Add them to an array you
    //   encounter them. It should work for our purposes for now, we
    //   won't be running this operating system on real hardware with
    //   variety values for device id any time soon.
    // To see list of emulated devices in QEMU, use info from here
    //   https://serverfault.com/questions/587189/how-to-list-all-devices-emulated-in-a-qemu-virtual-machine
    //   Also, there's a command "info pci".
    // To see more device ids, you could use this database:
    //   https://pci-ids.ucw.cz/read/PC
    //   https://pci-ids.ucw.cz/read/PC/8086
    static const uint16_t NET_E1000_VENDOR_ID = 0x8086;
    static const uint16_t NET_E1000_DEVICE_IDS[] = {0x100E};

    for (size_t i = 0; i < sizeof(NET_E1000_DEVICE_IDS) / sizeof(*NET_E1000_DEVICE_IDS); ++i) {
        if (pci_detect_device(NET_E1000_VENDOR_ID, NET_E1000_DEVICE_IDS[i], bus, slot, function)) {
            cprintf("Found e1000 of id %02" PRIx16 ":%02" PRIx16 " at pci %02" PRIu8 ":%02" PRIu8 ".%" PRIu8 ".\n", NET_E1000_VENDOR_ID, NET_E1000_DEVICE_IDS[i], *bus, *slot, *function);
            return true;
        }
    }

    return false;
}

void
net_init() {
    // https://courses.cs.washington.edu/courses/cse451/16au/readings/e1000.pdf
    //   Chapter ...: initialization.
    uint8_t bus = 0;
    uint8_t slot = 0;
    uint8_t function = 0;
    net_detect_e1000(&bus, &slot, &function);
}