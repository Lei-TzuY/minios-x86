#include "ata.h"
#include "io.h"
#include "irq.h"

#define ATA_DATA        0x1F0
#define ATA_SECTOR_COUNT 0x1F2
#define ATA_LBA_LOW     0x1F3
#define ATA_LBA_MID     0x1F4
#define ATA_LBA_HIGH    0x1F5
#define ATA_DRIVE       0x1F6
#define ATA_STATUS      0x1F7
#define ATA_COMMAND     0x1F7
#define ATA_CONTROL     0x3F6

#define ATA_STATUS_ERR  0x01
#define ATA_STATUS_DRQ  0x08
#define ATA_STATUS_DF   0x20
#define ATA_STATUS_BSY  0x80

#define ATA_CMD_READ_SECTOR 0x20
#define ATA_CMD_WRITE_SECTOR 0x30
#define ATA_CMD_CACHE_FLUSH  0xE7
#define ATA_CMD_IDENTIFY    0xEC
#define ATA_CONTROL_NIEN    0x02
#define ATA_POLL_LIMIT      100000

static int ata_available = 0;
static uint32_t ata_sector_count = 0;
static uint32_t ata_read_count = 0;
static uint32_t ata_write_count = 0;

static void ata_delay(void) {
    inb(ATA_CONTROL);
    inb(ATA_CONTROL);
    inb(ATA_CONTROL);
    inb(ATA_CONTROL);
}

static int ata_wait_not_busy(void) {
    for (uint32_t i = 0; i < ATA_POLL_LIMIT; i++) {
        uint8_t status = inb(ATA_STATUS);

        if (status == 0 || status == 0xFF) return 0;
        if (!(status & ATA_STATUS_BSY)) return 1;
    }
    return 0;
}

static int ata_wait_data(void) {
    for (uint32_t i = 0; i < ATA_POLL_LIMIT; i++) {
        uint8_t status = inb(ATA_STATUS);

        if (status == 0 || status == 0xFF ||
            (status & (ATA_STATUS_ERR | ATA_STATUS_DF))) {
            return 0;
        }
        if (!(status & ATA_STATUS_BSY) && (status & ATA_STATUS_DRQ)) return 1;
    }
    return 0;
}

/* Wait until the drive can be given a new command.
 *
 * Giving up on a poll abandons the command from this driver's side only -- the
 * drive is still executing it. Hardware ignores command-block writes while BSY
 * is set, so the next operation would program its task file into the void and
 * then find the DRQ raised for the PREVIOUS command: ata_wait_data() cannot
 * tell the two apart, and the transfer that follows moves the wrong sector.
 * That is a silently wrong read, or a write that reports success without
 * storing anything (F24).
 *
 * BSY clearing is not enough on its own, because a drive that finished an
 * abandoned read is sitting there with DRQ set, holding a sector it still
 * wants to hand over. Draining it completes that handshake and leaves the
 * drive idle, which is why this recovers instead of merely refusing. If DRQ
 * survives the drain -- a data-out phase, where reading the port does not
 * advance anything -- there is nothing safe left to do, so refuse. */
static int ata_wait_idle(void) {
    if (!ata_wait_not_busy()) return 0;

    if (inb(ATA_STATUS) & ATA_STATUS_DRQ) {
        for (uint32_t i = 0; i < 256; i++) (void)inw(ATA_DATA);
        if (!ata_wait_not_busy()) return 0;
        if (inb(ATA_STATUS) & ATA_STATUS_DRQ) return 0;
    }
    return 1;
}

void ata_install(void) {
    uint16_t identify[256];
    uint32_t flags = save_irq_disable();

    ata_available = 0;
    ata_sector_count = 0;
    ata_read_count = 0;
    ata_write_count = 0;

    outb(ATA_CONTROL, ATA_CONTROL_NIEN);
    outb(ATA_DRIVE, 0xA0);
    ata_delay();
    /* After the select here, unlike in read/write below: the status register
     * reflects whichever drive is currently selected, and during a probe we do
     * not yet know that it is ours. */
    if (!ata_wait_idle()) {
        restore_irq(flags);
        return;
    }
    outb(ATA_SECTOR_COUNT, 0);
    outb(ATA_LBA_LOW, 0);
    outb(ATA_LBA_MID, 0);
    outb(ATA_LBA_HIGH, 0);
    outb(ATA_COMMAND, ATA_CMD_IDENTIFY);

    if (!ata_wait_not_busy() ||
        inb(ATA_LBA_MID) != 0 || inb(ATA_LBA_HIGH) != 0 ||
        !ata_wait_data()) {
        restore_irq(flags);
        return;
    }

    for (uint32_t i = 0; i < 256; i++) {
        identify[i] = inw(ATA_DATA);
    }

    ata_sector_count = (uint32_t)identify[60] |
                       ((uint32_t)identify[61] << 16);
    ata_available = ata_sector_count != 0;
    restore_irq(flags);
}

int ata_read_sector(uint32_t lba, uint8_t *buffer) {
    uint32_t flags;

    if (!ata_available || !buffer ||
        lba >= ata_sector_count || lba > 0x0FFFFFFFU) {
        return 0;
    }

    flags = save_irq_disable();
    outb(ATA_CONTROL, ATA_CONTROL_NIEN);
    /* Before the select, unlike the probe: the drive register also carries the
     * top four LBA bits, and a write to it is ignored while the drive is busy.
     * Clearing the way first is what makes the address that follows land. */
    if (!ata_wait_idle()) {
        restore_irq(flags);
        return 0;
    }
    outb(ATA_DRIVE, (uint8_t)(0xE0 | ((lba >> 24) & 0x0F)));
    ata_delay();
    outb(ATA_SECTOR_COUNT, 1);
    outb(ATA_LBA_LOW, (uint8_t)lba);
    outb(ATA_LBA_MID, (uint8_t)(lba >> 8));
    outb(ATA_LBA_HIGH, (uint8_t)(lba >> 16));
    outb(ATA_COMMAND, ATA_CMD_READ_SECTOR);

    if (!ata_wait_data()) {
        restore_irq(flags);
        return 0;
    }

    for (uint32_t i = 0; i < 256; i++) {
        uint16_t word = inw(ATA_DATA);
        buffer[i * 2] = (uint8_t)word;
        buffer[i * 2 + 1] = (uint8_t)(word >> 8);
    }

    ata_read_count++;
    restore_irq(flags);
    return 1;
}

int ata_write_sector(uint32_t lba, const uint8_t *buffer) {
    uint32_t flags;

    if (!ata_available || !buffer ||
        lba >= ata_sector_count || lba > 0x0FFFFFFFU) {
        return 0;
    }

    flags = save_irq_disable();
    outb(ATA_CONTROL, ATA_CONTROL_NIEN);
    if (!ata_wait_idle()) {          /* see ata_read_sector */
        restore_irq(flags);
        return 0;
    }
    outb(ATA_DRIVE, (uint8_t)(0xE0 | ((lba >> 24) & 0x0F)));
    ata_delay();
    outb(ATA_SECTOR_COUNT, 1);
    outb(ATA_LBA_LOW, (uint8_t)lba);
    outb(ATA_LBA_MID, (uint8_t)(lba >> 8));
    outb(ATA_LBA_HIGH, (uint8_t)(lba >> 16));
    outb(ATA_COMMAND, ATA_CMD_WRITE_SECTOR);

    if (!ata_wait_data()) {
        restore_irq(flags);
        return 0;
    }

    for (uint32_t i = 0; i < 256; i++) {
        uint16_t word = (uint16_t)buffer[i * 2] |
                        ((uint16_t)buffer[i * 2 + 1] << 8);
        outw(ATA_DATA, word);
    }

    if (!ata_wait_not_busy()) {
        restore_irq(flags);
        return 0;
    }

    /* The drive may accept all 256 data words and only then report that the
     * WRITE command failed. ata_wait_not_busy() establishes completion but
     * deliberately ignores ERR/DF so it can also be used when recovering an
     * old command; this call site must inspect the completed command before a
     * CACHE FLUSH can replace its status. */
    {
        uint8_t status = inb(ATA_STATUS);
        if (status == 0 || status == 0xFF ||
            (status & (ATA_STATUS_ERR | ATA_STATUS_DF | ATA_STATUS_DRQ))) {
            restore_irq(flags);
            return 0;
        }
    }

    outb(ATA_COMMAND, ATA_CMD_CACHE_FLUSH);
    if (!ata_wait_not_busy() ||
        (inb(ATA_STATUS) & (ATA_STATUS_ERR | ATA_STATUS_DF))) {
        restore_irq(flags);
        return 0;
    }

    ata_write_count++;
    restore_irq(flags);
    return 1;
}

int ata_is_available(void) {
    return ata_available;
}

uint32_t ata_get_sector_count(void) {
    return ata_sector_count;
}

uint32_t ata_get_read_count(void) {
    return ata_read_count;
}

uint32_t ata_get_write_count(void) {
    return ata_write_count;
}
