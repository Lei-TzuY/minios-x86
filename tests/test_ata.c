#include "test.h"

/*
 * The ATA PIO driver (ata.c).
 *
 * This is the last driver without a unit test, and the one where that hurts
 * most: it is the bottom of the storage stack, DiskFS builds a filesystem on
 * top of whatever bytes it returns, and the QEMU suite only ever exercises the
 * one path where everything works. An emulated IDE controller answers every
 * command immediately and never reports an error, so in three hundred QEMU
 * assertions not one poll has ever timed out, not one ERR bit has ever been
 * seen, and no command has ever been issued to a drive that was still busy.
 * Every failure path in this file is therefore unexecuted code.
 *
 * The fake device below is a state machine, not a lookup table. Keeping the
 * real timing behaviour is the whole point, because the interesting bugs live
 * in the handshake rather than in the data:
 *
 *   - A command sets BSY, and BSY clears only after a configurable number of
 *     status reads. Setting that count above ATA_POLL_LIMIT is how a slow
 *     drive -- and therefore a poll timeout -- is modelled.
 *   - DRQ is asserted when the drive is ready to hand over or accept data, and
 *     clears by itself after exactly 256 words have moved. A driver that reads
 *     too few or too many words desynchronises the device, and the device
 *     notices.
 *   - Writes to the command block while BSY is set are IGNORED, which is what
 *     real hardware does with them, and the device goes on executing the
 *     command it already had. That is not a detail: it is the mechanism behind
 *     the defect this suite was written to pin down.
 *   - An absent device floats the bus, so status reads return 0xFF.
 *
 * Both io.h and irq.h are replaced by pre-defining their include guards (the
 * CAP15 method). io.h gives the device its port interface. irq.h is replaced
 * so save_irq_disable/restore_irq can be COUNTED: this driver holds interrupts
 * off across every operation and has eight separate return paths, and a
 * missing restore there does not fail an assertion anywhere -- it just leaves
 * the machine with interrupts off forever.
 */

#include <stdint.h>
#include <stddef.h>

/* Pulled in ahead of the substitutions below for ATA_SECTOR_SIZE. It declares
 * the driver's interface and nothing else -- no port access -- so including it
 * early costs nothing. */
#include "../ata.h"

/* --- interrupt accounting: replace irq.h ---------------------------------- */
#define IRQ_H

static int g_irq_disable_calls;
static int g_irq_restore_calls;
static int g_irq_depth;            /* must be 0 whenever the driver returns */
static int g_irq_depth_max;

static inline uint32_t save_irq_disable(void) {
    g_irq_disable_calls++;
    g_irq_depth++;
    if (g_irq_depth > g_irq_depth_max) g_irq_depth_max = g_irq_depth;
    return 0x200;                  /* IF was set, as it is in kernel context */
}

static inline void restore_irq(uint32_t flags) {
    (void)flags;
    g_irq_restore_calls++;
    g_irq_depth--;
}

/* --- the fake drive: replace io.h ----------------------------------------- */
#define IO_H

#define P_DATA    0x1F0
#define P_COUNT   0x1F2
#define P_LBA_LO  0x1F3
#define P_LBA_MID 0x1F4
#define P_LBA_HI  0x1F5
#define P_DRIVE   0x1F6
#define P_STATUS  0x1F7
#define P_CMD     0x1F7
#define P_CTRL    0x3F6

#define S_ERR 0x01
#define S_DRQ 0x08
#define S_DF  0x20
#define S_RDY 0x40
#define S_BSY 0x80

#define C_READ     0x20
#define C_WRITE    0x30
#define C_FLUSH    0xE7
#define C_IDENTIFY 0xEC

#define FAKE_SECTORS 64
#define WORDS_PER_SECTOR 256

typedef enum {
    PHASE_IDLE,
    PHASE_BUSY,        /* executing; BSY set */
    PHASE_DATA_IN,     /* drive -> host, DRQ set */
    PHASE_DATA_OUT,    /* host -> drive, DRQ set */
    PHASE_ERROR,       /* command aborted; ERR or DF, possibly with DRQ */
} phase_t;

static struct {
    /* Backing store. */
    uint8_t  sectors[FAKE_SECTORS][ATA_SECTOR_SIZE];
    uint32_t reported_sector_count;      /* what IDENTIFY advertises */

    /* Presence and signature. */
    int      absent;                     /* floating bus: status reads 0xFF */
    int      status_zero;                /* status reads 0 (nothing there) */
    uint8_t  sig_mid, sig_high;          /* non-zero => ATAPI, not ours */

    /* Task file, as latched by the host. */
    uint8_t  reg_count, reg_lo, reg_mid, reg_hi, reg_drive, reg_ctrl;
    uint8_t  command;

    /* Execution state. */
    phase_t  phase;
    uint32_t busy_polls;                 /* status reads before BSY clears */
    uint32_t busy_polls_next;            /* value used for the next command */
    uint32_t lba;                        /* LBA latched when the command ran */
    uint16_t buffer[WORDS_PER_SECTOR];
    uint32_t index;                      /* words transferred this phase */
    uint8_t  error_bits;                 /* ERR or DF, set on abort */
    int      error_has_drq;              /* the abort also raised DRQ */

    /* Injected error conditions. */
    int      fail_with_err;              /* raise ERR when the command lands */
    int      fail_with_df;               /* raise DF instead */
    int      fail_flush;                 /* raise ERR only on CACHE FLUSH */
    uint8_t  fail_after_data_out;        /* ERR/DF after taking 256 words */
    int      hold_drq_after_data_out;    /* data phase never completes */
    /* A failed read really does raise ERR *together with* DRQ on real
     * drives: the command aborted, and the drive still signals a transfer.
     * Modelling ERR without DRQ would let a driver that never looks at the
     * error bits pass, because it would sit waiting for a DRQ that never
     * comes and time out for the wrong reason. */
    int      err_with_drq;
    int      df_with_drq;
    /* Some controllers assert DRQ while BSY is still set, before the data
     * is really there. BSY is the bit that says "do not touch me yet". */
    int      drq_while_busy;
    /* The drive stops answering once it has taken a sector: a cable pulled
     * mid-write, which is the one place ata_wait_not_busy is the only
     * thing standing between a dead drive and a reported success. */
    int      die_after_data_out;

    /* Observations. */
    int      ctrl_writes;
    int      commands_issued;
    uint32_t status_reads;               /* how hard the driver polled */
    int      commands_refused_drq;       /* issued while data was pending */
    int      taskfile_writes_while_busy; /* the host talked over a command */
    int      data_underrun;              /* host stopped mid-transfer */
    int      data_overrun;               /* host read past the sector */
    uint32_t last_read_lba, last_write_lba;
    int      reads_served, writes_served, flushes;
} dev;

static void fake_reset(void) {
    for (unsigned s = 0; s < FAKE_SECTORS; s++)
        for (unsigned i = 0; i < ATA_SECTOR_SIZE; i++)
            dev.sectors[s][i] = (uint8_t)(s * 7 + i);

    dev.reported_sector_count = FAKE_SECTORS;
    dev.absent = 0;
    dev.status_zero = 0;
    dev.sig_mid = dev.sig_high = 0;
    dev.reg_count = dev.reg_lo = dev.reg_mid = dev.reg_hi = 0;
    dev.reg_drive = dev.reg_ctrl = dev.command = 0;
    dev.phase = PHASE_IDLE;
    dev.busy_polls = 0;
    dev.busy_polls_next = 0;
    dev.lba = 0;
    dev.index = 0;
    dev.error_bits = 0;
    dev.error_has_drq = 0;
    dev.fail_with_err = dev.fail_with_df = dev.fail_flush = 0;
    dev.fail_after_data_out = 0;
    dev.hold_drq_after_data_out = 0;
    dev.err_with_drq = dev.df_with_drq = 0;
    dev.drq_while_busy = 0;
    dev.die_after_data_out = 0;
    dev.ctrl_writes = 0;
    dev.commands_issued = 0;
    dev.status_reads = 0;
    dev.commands_refused_drq = 0;
    dev.taskfile_writes_while_busy = 0;
    dev.data_underrun = 0;
    dev.data_overrun = 0;
    dev.last_read_lba = dev.last_write_lba = 0xFFFFFFFFu;
    dev.reads_served = dev.writes_served = dev.flushes = 0;

    g_irq_disable_calls = g_irq_restore_calls = 0;
    g_irq_depth = g_irq_depth_max = 0;
}

static uint32_t fake_latched_lba(void) {
    return (uint32_t)dev.reg_lo |
           ((uint32_t)dev.reg_mid << 8) |
           ((uint32_t)dev.reg_hi << 16) |
           (((uint32_t)dev.reg_drive & 0x0F) << 24);
}

/* Load the sector the current command names into the transfer buffer. */
static void fake_load_sector_for_read(void) {
    uint32_t s = dev.lba < FAKE_SECTORS ? dev.lba : 0;

    for (unsigned i = 0; i < WORDS_PER_SECTOR; i++) {
        dev.buffer[i] = (uint16_t)dev.sectors[s][i * 2] |
                        ((uint16_t)dev.sectors[s][i * 2 + 1] << 8);
    }
}

static void fake_fill_identify(void) {
    for (unsigned i = 0; i < WORDS_PER_SECTOR; i++) dev.buffer[i] = 0;
    dev.buffer[60] = (uint16_t)(dev.reported_sector_count & 0xFFFF);
    dev.buffer[61] = (uint16_t)(dev.reported_sector_count >> 16);
}

/* Called when BSY finally clears: move into whatever phase the command wants. */
static void fake_command_completes(void) {
    /* A command that aborts leaves the drive reporting ERR (or DF), and
     * on a failed read it may raise DRQ alongside it. Both are decided
     * HERE rather than forced onto every status read: a drive that
     * answered ERR before the command was even issued would be refused
     * upstream, and the check under test would never be reached. */
    if (dev.fail_with_err || dev.fail_with_df ||
        dev.err_with_drq || dev.df_with_drq) {
        dev.error_bits = (dev.fail_with_err || dev.err_with_drq) ? S_ERR
                                                                 : S_DF;
        dev.error_has_drq = dev.err_with_drq || dev.df_with_drq;
        dev.index = 0;
        dev.phase = PHASE_ERROR;
        return;
    }

    switch (dev.command) {
    case C_IDENTIFY:
        fake_fill_identify();
        dev.index = 0;
        dev.phase = PHASE_DATA_IN;
        break;
    case C_READ:
        fake_load_sector_for_read();
        dev.index = 0;
        dev.phase = PHASE_DATA_IN;
        dev.last_read_lba = dev.lba;
        dev.reads_served++;
        break;
    case C_WRITE:
        dev.index = 0;
        dev.phase = PHASE_DATA_OUT;
        break;
    case C_FLUSH:
        dev.flushes++;
        dev.phase = PHASE_IDLE;
        break;
    default:
        dev.phase = PHASE_IDLE;
        break;
    }
}

static inline uint8_t inb(uint16_t port) {
    /* Counted before the absent check: how hard the driver polls a channel
     * with nothing on it is exactly what the absent-drive tests measure. */
    if (port == P_STATUS) dev.status_reads++;

    if (dev.absent) return 0xFF;

    if (port == P_CTRL) {
        /* The alternate status register: same value, and reading it must not
         * disturb anything (ata_delay reads it four times). */
        if (dev.status_zero) return 0;
        if (dev.phase == PHASE_BUSY) return S_BSY;
        if (dev.phase == PHASE_ERROR) {
            return (uint8_t)(S_RDY | dev.error_bits |
                             (dev.error_has_drq ? S_DRQ : 0));
        }
        return dev.phase == PHASE_IDLE ? S_RDY : (uint8_t)(S_RDY | S_DRQ);
    }

    if (port == P_STATUS) {
        if (dev.status_zero) return 0;

        if (dev.phase == PHASE_BUSY) {
            if (dev.busy_polls > 0) {
                dev.busy_polls--;
                return (uint8_t)(dev.drq_while_busy ? (S_BSY | S_DRQ)
                                                    : S_BSY);
            }
            fake_command_completes();
        }
        if (dev.phase == PHASE_ERROR) {
            return (uint8_t)(S_RDY | dev.error_bits |
                             (dev.error_has_drq ? S_DRQ : 0));
        }
        if (dev.fail_flush && dev.command == C_FLUSH && dev.phase == PHASE_IDLE)
            return (uint8_t)(S_RDY | S_ERR);
        if (dev.phase == PHASE_DATA_IN || dev.phase == PHASE_DATA_OUT)
            return (uint8_t)(S_RDY | S_DRQ);
        return S_RDY;
    }

    if (port == P_LBA_MID) return dev.sig_mid;
    if (port == P_LBA_HI)  return dev.sig_high;
    return 0;
}

static inline void outb(uint16_t port, uint8_t val) {
    if (port == P_CTRL) { dev.ctrl_writes++; dev.reg_ctrl = val; return; }

    /* Real hardware ignores command-block writes while BSY is asserted; the
     * command already running is unaffected. Recording the attempt is what
     * lets a test say the driver talked over an operation in flight. */
    if (dev.phase == PHASE_BUSY) { dev.taskfile_writes_while_busy++; return; }

    switch (port) {
    case P_COUNT:  dev.reg_count = val; break;
    case P_LBA_LO: dev.reg_lo = val;    break;
    case P_LBA_MID:dev.reg_mid = val;   break;
    case P_LBA_HI: dev.reg_hi = val;    break;
    case P_DRIVE:  dev.reg_drive = val; break;
    case P_CMD:
        /* The spec forbids writing the command register while BSY or DRQ is
         * set, and leaves the result undefined. Undefined is modelled here
         * as "ignored", the same as the BSY case above, for two reasons: it
         * is what several controllers actually do, and a test model must not
         * pick the interpretation that happens to be convenient for the
         * driver. A driver that only works when the hardware forgives a
         * protocol violation is a driver that works by luck. */
        if (dev.phase == PHASE_DATA_IN || dev.phase == PHASE_DATA_OUT) {
            dev.data_underrun++;
            dev.commands_refused_drq++;
            return;
        }
        dev.command = val;
        dev.lba = fake_latched_lba();
        dev.commands_issued++;
        dev.busy_polls = dev.busy_polls_next;
        dev.phase = PHASE_BUSY;
        if (dev.busy_polls == 0) fake_command_completes();
        break;
    default: break;
    }
}

static inline uint16_t inw(uint16_t port) {
    if (port != P_DATA) return 0;
    if (dev.absent) return 0xFFFF;

    if (dev.phase == PHASE_ERROR) {
        /* A driver that ignored the error bits gets recognisable rubbish
         * rather than a plausible sector, and the transfer still clears
         * DRQ so the drive can be used again afterwards. */
        if (dev.error_has_drq && ++dev.index >= WORDS_PER_SECTOR) {
            dev.phase = PHASE_IDLE;
            dev.error_bits = 0;
            dev.error_has_drq = 0;
            dev.index = 0;
        }
        return 0xDEAD;
    }
    if (dev.phase != PHASE_DATA_IN) { dev.data_overrun++; return 0; }

    {
        uint16_t w = dev.buffer[dev.index++];

        if (dev.index >= WORDS_PER_SECTOR) {   /* DRQ drops by itself */
            dev.phase = PHASE_IDLE;
            dev.index = 0;
        }
        return w;
    }
}

static inline void outw(uint16_t port, uint16_t val) {
    if (port != P_DATA) return;

    if (dev.phase != PHASE_DATA_OUT) { dev.data_overrun++; return; }

    dev.buffer[dev.index++] = val;
    if (dev.index >= WORDS_PER_SECTOR) {
        uint32_t s = dev.lba < FAKE_SECTORS ? dev.lba : 0;

        for (unsigned i = 0; i < WORDS_PER_SECTOR; i++) {
            dev.sectors[s][i * 2] = (uint8_t)dev.buffer[i];
            dev.sectors[s][i * 2 + 1] = (uint8_t)(dev.buffer[i] >> 8);
        }
        dev.last_write_lba = dev.lba;
        dev.writes_served++;
        dev.index = 0;
        if (dev.hold_drq_after_data_out) {
            dev.phase = PHASE_DATA_OUT;
        } else if (dev.fail_after_data_out) {
            dev.error_bits = dev.fail_after_data_out;
            dev.error_has_drq = 0;
            dev.phase = PHASE_ERROR;
        } else {
            dev.phase = PHASE_IDLE;
        }
        if (dev.die_after_data_out) dev.status_zero = 1;
    }
}

static inline void io_wait(void) { }

#include "../ata.c"

/* --- helpers -------------------------------------------------------------- */

/* Bring the driver up against a healthy drive. */
static int install_healthy(void) {
    fake_reset();
    ata_install();
    return ata_is_available();
}

static void expect_irq_balanced(void) {
    CHECK_EQ(g_irq_depth, 0);
    CHECK_EQ(g_irq_disable_calls, g_irq_restore_calls);
    CHECK(g_irq_disable_calls > 0);
    CHECK_EQ(g_irq_depth_max, 1);      /* never nested */
}

/* --- probing -------------------------------------------------------------- */

static void test_install_detects_drive(void) {
    TEST("install: healthy drive");
    fake_reset();
    ata_install();

    CHECK_EQ(ata_is_available(), 1);
    CHECK_EQ(ata_get_sector_count(), FAKE_SECTORS);
    CHECK_EQ(ata_get_read_count(), 0);
    CHECK_EQ(ata_get_write_count(), 0);
    CHECK_EQ(dev.command, C_IDENTIFY);
    expect_irq_balanced();

    /* Interrupts from the drive are masked before anything else: this driver
     * polls, and an IRQ it never handles would be left asserted. */
    CHECK(dev.ctrl_writes > 0);
    CHECK_EQ(dev.reg_ctrl, ATA_CONTROL_NIEN);
}

static void test_install_absent_drive(void) {
    uint8_t buffer[ATA_SECTOR_SIZE];

    TEST("install: no drive at all");
    fake_reset();
    dev.absent = 1;                    /* floating bus reads as 0xFF */
    ata_install();

    CHECK_EQ(ata_is_available(), 0);
    CHECK_EQ(ata_get_sector_count(), 0);
    expect_irq_balanced();

    /* And every operation refuses without touching the bus. Whether the
     * machine has a disk is decided once, at probe time. */
    {
        int commands = dev.commands_issued;

        CHECK_EQ(ata_read_sector(0, buffer), 0);
        CHECK_EQ(ata_write_sector(0, buffer), 0);
        CHECK_EQ(dev.commands_issued, commands);
    }
}

static void test_install_status_zero(void) {
    TEST("install: status reads zero");
    fake_reset();
    dev.status_zero = 1;
    ata_install();

    /* A status of zero is not a drive that is merely idle -- a present drive
     * always has at least RDY. Treating it as absent is what stops the probe
     * from waiting out the full poll limit on an empty channel. */
    CHECK_EQ(ata_is_available(), 0);
    CHECK_EQ(ata_get_sector_count(), 0);
    expect_irq_balanced();
}

static void test_install_atapi_signature(void) {
    TEST("install: ATAPI signature is rejected");
    fake_reset();
    dev.sig_mid = 0x14;                /* the ATAPI signature */
    dev.sig_high = 0xEB;
    ata_install();

    /* An ATAPI device answers IDENTIFY differently; reading it as an ATA
     * drive would produce a nonsense sector count. */
    CHECK_EQ(ata_is_available(), 0);
    CHECK_EQ(ata_get_sector_count(), 0);
    expect_irq_balanced();
}

static void test_install_zero_sectors(void) {
    TEST("install: drive reports zero sectors");
    fake_reset();
    dev.reported_sector_count = 0;
    ata_install();

    /* Nothing addressable means nothing usable, and it also keeps the bound
     * check in read/write from having to special-case an empty drive. */
    CHECK_EQ(ata_is_available(), 0);
    expect_irq_balanced();
}

static void test_install_busy_forever(void) {
    TEST("install: drive never leaves BSY");
    fake_reset();
    dev.busy_polls_next = ATA_POLL_LIMIT + 1000;
    ata_install();

    /* The probe has to give up rather than spin. Syscalls run with interrupts
     * disabled, so an unbounded poll here is the whole machine stopping. */
    CHECK_EQ(ata_is_available(), 0);
    expect_irq_balanced();
}

static void test_install_error_bit(void) {
    TEST("install: IDENTIFY reports ERR");
    fake_reset();
    dev.fail_with_err = 1;
    ata_install();
    CHECK_EQ(ata_is_available(), 0);
    expect_irq_balanced();

    TEST("install: IDENTIFY reports DF");
    fake_reset();
    dev.fail_with_df = 1;
    ata_install();
    CHECK_EQ(ata_is_available(), 0);
    expect_irq_balanced();
}

static void test_install_large_sector_count(void) {
    TEST("install: 32-bit sector count");
    fake_reset();
    dev.reported_sector_count = 0x12345678u;
    ata_install();

    /* The count is assembled from two IDENTIFY words; getting the halves the
     * wrong way round still yields a plausible number. */
    CHECK_EQ(ata_is_available(), 1);
    CHECK_EQ(ata_get_sector_count(), 0x12345678u);
}

/* --- the read path -------------------------------------------------------- */

static void test_read_success(void) {
    uint8_t buffer[ATA_SECTOR_SIZE];

    TEST("read: success path");
    CHECK(install_healthy());
    fake_reset();                      /* clear the probe's IRQ accounting */
    ata_install();
    g_irq_disable_calls = g_irq_restore_calls = 0;

    for (unsigned i = 0; i < sizeof(buffer); i++) buffer[i] = 0xCC;
    CHECK_EQ(ata_read_sector(3, buffer), 1);

    /* The right LBA was programmed ... */
    CHECK_EQ(dev.last_read_lba, 3);
    CHECK_EQ(dev.reg_count, 1);
    CHECK_EQ(dev.reg_drive & 0xF0, 0xE0);      /* LBA mode, master */

    /* ... and every byte arrived, in the right order. The device stores
     * little-endian words, so a driver that swapped the halves would still
     * fill the buffer and still return 1. */
    for (unsigned i = 0; i < ATA_SECTOR_SIZE; i++)
        CHECK_EQ(buffer[i], (uint8_t)(3 * 7 + i));

    CHECK_EQ(ata_get_read_count(), 1);
    CHECK_EQ(dev.data_overrun, 0);     /* exactly 256 words, no more */
    expect_irq_balanced();
}

static void test_read_lba_encoding(void) {
    uint8_t buffer[ATA_SECTOR_SIZE];

    TEST("read: LBA is split across four registers");
    fake_reset();
    dev.reported_sector_count = 0x0FFFFFFFu + 1;   /* whole 28-bit range */
    ata_install();
    CHECK(ata_is_available());

    /* A value with a distinct byte in each field, so a misplaced shift shows
     * up as a wrong sector rather than as a wrong bit. */
    CHECK_EQ(ata_read_sector(0x0A3B4C5Du, buffer), 1);
    CHECK_EQ(dev.reg_lo, 0x5D);
    CHECK_EQ(dev.reg_mid, 0x4C);
    CHECK_EQ(dev.reg_hi, 0x3B);
    CHECK_EQ(dev.reg_drive & 0x0F, 0x0A);
    CHECK_EQ(dev.last_read_lba, 0x0A3B4C5Du);
}

static void test_read_bounds(void) {
    uint8_t buffer[ATA_SECTOR_SIZE];
    int commands;

    TEST("read: bounds and arguments");
    CHECK(install_healthy());
    commands = dev.commands_issued;

    /* One past the end, and far past it. */
    CHECK_EQ(ata_read_sector(FAKE_SECTORS, buffer), 0);
    CHECK_EQ(ata_read_sector(FAKE_SECTORS + 1, buffer), 0);
    CHECK_EQ(ata_read_sector(0xFFFFFFFFu, buffer), 0);
    /* The last addressable sector is still fine. */
    CHECK_EQ(ata_read_sector(FAKE_SECTORS - 1, buffer), 1);

    /* A NULL destination must be refused before any command goes out -- the
     * transfer loop would write through it 512 bytes later. */
    CHECK_EQ(ata_read_sector(0, NULL), 0);

    /* Only the in-range read above may have issued a command. */
    CHECK_EQ(dev.commands_issued, commands + 1);
    expect_irq_balanced();
}

static void test_read_beyond_28_bit_lba(void) {
    uint8_t buffer[ATA_SECTOR_SIZE];

    TEST("read: 28-bit addressing limit");
    fake_reset();
    dev.reported_sector_count = 0xFFFFFFFFu;   /* drive claims more than LBA28 */
    ata_install();
    CHECK(ata_is_available());

    /* The register file only carries 28 bits. A sector the drive advertises
     * but the command cannot name must be refused rather than silently
     * truncated into a different, valid sector. */
    CHECK_EQ(ata_read_sector(0x0FFFFFFFu + 1, buffer), 0);
    CHECK_EQ(ata_read_sector(0x1FFFFFFFu, buffer), 0);
    CHECK_EQ(dev.commands_issued, 1);          /* only IDENTIFY */
}

static void test_read_timeout(void) {
    uint8_t buffer[ATA_SECTOR_SIZE];

    TEST("read: drive never produces data");
    CHECK(install_healthy());
    for (unsigned i = 0; i < sizeof(buffer); i++) buffer[i] = 0xCC;

    dev.busy_polls_next = ATA_POLL_LIMIT + 1000;
    CHECK_EQ(ata_read_sector(1, buffer), 0);

    /* Bounded, balanced, and it did not touch the caller's buffer. */
    CHECK_EQ(buffer[0], 0xCC);
    CHECK_EQ(ata_get_read_count(), 0);         /* failures are not counted */
    expect_irq_balanced();
}

static void test_read_error_bits(void) {
    uint8_t buffer[ATA_SECTOR_SIZE];

    TEST("read: ERR and DF");
    CHECK(install_healthy());
    for (unsigned i = 0; i < sizeof(buffer); i++) buffer[i] = 0xCC;

    dev.fail_with_err = 1;
    CHECK_EQ(ata_read_sector(1, buffer), 0);
    CHECK_EQ(buffer[0], 0xCC);
    CHECK_EQ(ata_get_read_count(), 0);

    dev.fail_with_err = 0;
    dev.fail_with_df = 1;
    CHECK_EQ(ata_read_sector(1, buffer), 0);
    CHECK_EQ(buffer[0], 0xCC);
    CHECK_EQ(ata_get_read_count(), 0);
    expect_irq_balanced();
}

static void test_read_drive_vanishes(void) {
    uint8_t buffer[ATA_SECTOR_SIZE];

    TEST("read: drive disappears after probing");
    CHECK(install_healthy());
    dev.absent = 1;                    /* cable pulled, controller wedged */

    CHECK_EQ(ata_read_sector(1, buffer), 0);
    CHECK_EQ(ata_get_read_count(), 0);
    expect_irq_balanced();
}

/* --- the write path ------------------------------------------------------- */

static void test_write_success(void) {
    uint8_t out[ATA_SECTOR_SIZE];
    uint8_t back[ATA_SECTOR_SIZE];

    TEST("write: success path and round trip");
    CHECK(install_healthy());
    for (unsigned i = 0; i < sizeof(out); i++) out[i] = (uint8_t)(i * 3 + 1);

    CHECK_EQ(ata_write_sector(5, out), 1);
    CHECK_EQ(dev.last_write_lba, 5);
    CHECK_EQ(ata_get_write_count(), 1);

    /* A cache flush is issued and completes: without it the data is only
     * promised, not stored. */
    CHECK_EQ(dev.flushes, 1);

    /* Read it back through the driver: this is the only check that the write
     * and read byte orders agree with each other AND with the device. */
    CHECK_EQ(ata_read_sector(5, back), 1);
    for (unsigned i = 0; i < ATA_SECTOR_SIZE; i++) CHECK_EQ(back[i], out[i]);

    CHECK_EQ(dev.data_overrun, 0);
    expect_irq_balanced();
}

static void test_write_bounds(void) {
    uint8_t out[ATA_SECTOR_SIZE];
    int commands;

    TEST("write: bounds and arguments");
    CHECK(install_healthy());
    for (unsigned i = 0; i < sizeof(out); i++) out[i] = 0x5A;
    commands = dev.commands_issued;

    CHECK_EQ(ata_write_sector(FAKE_SECTORS, out), 0);
    CHECK_EQ(ata_write_sector(0xFFFFFFFFu, out), 0);
    CHECK_EQ(ata_write_sector(0, NULL), 0);
    CHECK_EQ(dev.commands_issued, commands);   /* nothing was issued */

    CHECK_EQ(ata_write_sector(FAKE_SECTORS - 1, out), 1);
    expect_irq_balanced();
}

static void test_write_timeout_before_data(void) {
    uint8_t out[ATA_SECTOR_SIZE];

    TEST("write: drive never asks for the data");
    CHECK(install_healthy());
    for (unsigned i = 0; i < sizeof(out); i++) out[i] = 0x5A;

    dev.busy_polls_next = ATA_POLL_LIMIT + 1000;
    CHECK_EQ(ata_write_sector(1, out), 0);
    CHECK_EQ(ata_get_write_count(), 0);
    CHECK_EQ(dev.writes_served, 0);            /* nothing reached the platter */
    expect_irq_balanced();
}

static void test_write_flush_failure(void) {
    uint8_t out[ATA_SECTOR_SIZE];

    TEST("write: cache flush reports an error");
    CHECK(install_healthy());
    for (unsigned i = 0; i < sizeof(out); i++) out[i] = 0x77;

    dev.fail_flush = 1;
    CHECK_EQ(ata_write_sector(6, out), 0);

    /* The data did reach the drive, but the flush failed, so the write must be
     * reported as failed: DiskFS uses the return value to decide whether its
     * metadata is consistent, and a lost flush is exactly the case where it
     * is not. */
    CHECK_EQ(ata_get_write_count(), 0);
    expect_irq_balanced();
}

static void test_write_error_bits(void) {
    uint8_t out[ATA_SECTOR_SIZE];

    TEST("write: ERR and DF before the transfer");
    CHECK(install_healthy());
    for (unsigned i = 0; i < sizeof(out); i++) out[i] = 0x33;

    dev.fail_with_err = 1;
    CHECK_EQ(ata_write_sector(1, out), 0);
    CHECK_EQ(dev.writes_served, 0);

    dev.fail_with_err = 0;
    dev.fail_with_df = 1;
    CHECK_EQ(ata_write_sector(1, out), 0);
    CHECK_EQ(dev.writes_served, 0);
    CHECK_EQ(ata_get_write_count(), 0);
    expect_irq_balanced();
}

static void test_write_error_after_data(void) {
    uint8_t out[ATA_SECTOR_SIZE];

    TEST("write: ERR after the data phase is not reported as success");
    CHECK(install_healthy());
    for (unsigned i = 0; i < sizeof(out); i++) out[i] = 0x3C;

    dev.fail_after_data_out = S_ERR;
    CHECK_EQ(ata_write_sector(3, out), 0);
    CHECK_EQ(dev.writes_served, 1);       /* the drive accepted the data words */
    CHECK_EQ(dev.flushes, 0);             /* completion error stops the command */
    CHECK_EQ(ata_get_write_count(), 0);
    expect_irq_balanced();

    CHECK(install_healthy());
    dev.fail_after_data_out = S_DF;
    CHECK_EQ(ata_write_sector(3, out), 0);
    CHECK_EQ(dev.writes_served, 1);
    CHECK_EQ(dev.flushes, 0);
    CHECK_EQ(ata_get_write_count(), 0);
    expect_irq_balanced();
}

static void test_write_drq_stuck_after_data(void) {
    uint8_t out[ATA_SECTOR_SIZE];

    TEST("write: lingering DRQ after the data phase is not success");
    CHECK(install_healthy());
    for (unsigned i = 0; i < sizeof(out); i++) out[i] = 0x7A;

    dev.hold_drq_after_data_out = 1;
    CHECK_EQ(ata_write_sector(4, out), 0);
    CHECK_EQ(dev.writes_served, 1);
    CHECK_EQ(dev.commands_refused_drq, 0); /* no flush issued over live DRQ */
    CHECK_EQ(dev.flushes, 0);
    CHECK_EQ(ata_get_write_count(), 0);
    expect_irq_balanced();
}

/* --- state carried between calls ------------------------------------------ */

static void test_repeated_calls(void) {
    uint8_t out[ATA_SECTOR_SIZE];
    uint8_t back[ATA_SECTOR_SIZE];

    TEST("repeated calls stay consistent");
    CHECK(install_healthy());

    for (unsigned round = 0; round < 8; round++) {
        uint32_t lba = round % FAKE_SECTORS;

        for (unsigned i = 0; i < sizeof(out); i++)
            out[i] = (uint8_t)(round * 31 + i);

        CHECK_EQ(ata_write_sector(lba, out), 1);
        CHECK_EQ(ata_read_sector(lba, back), 1);
        for (unsigned i = 0; i < ATA_SECTOR_SIZE; i++)
            CHECK_EQ(back[i], out[i]);
    }

    CHECK_EQ(ata_get_read_count(), 8);
    CHECK_EQ(ata_get_write_count(), 8);
    CHECK_EQ(dev.data_overrun, 0);
    CHECK_EQ(dev.data_underrun, 0);
    expect_irq_balanced();
}

static void test_counters_only_count_success(void) {
    uint8_t buffer[ATA_SECTOR_SIZE];

    /* The QEMU suite asserts exact read/write totals at shutdown, so a failed
     * operation that still bumped a counter would drift that assertion --
     * and it is one of the suite's leak detectors. */
    TEST("counters track successes only");
    CHECK(install_healthy());

    CHECK_EQ(ata_read_sector(0, buffer), 1);
    CHECK_EQ(ata_get_read_count(), 1);

    CHECK_EQ(ata_read_sector(FAKE_SECTORS, buffer), 0);   /* out of range */
    CHECK_EQ(ata_get_read_count(), 1);

    dev.fail_with_err = 1;
    CHECK_EQ(ata_read_sector(0, buffer), 0);
    CHECK_EQ(ata_get_read_count(), 1);
    dev.fail_with_err = 0;

    CHECK_EQ(ata_read_sector(0, buffer), 1);
    CHECK_EQ(ata_get_read_count(), 2);
}

static void test_install_resets_state(void) {
    uint8_t buffer[ATA_SECTOR_SIZE];

    TEST("install resets driver state");
    CHECK(install_healthy());
    CHECK_EQ(ata_read_sector(0, buffer), 1);
    CHECK(ata_get_read_count() > 0);

    /* Probing again starts from nothing: a stale sector count from a previous
     * drive would let a read address past the end of the new one. */
    fake_reset();
    dev.reported_sector_count = 8;
    ata_install();
    CHECK_EQ(ata_get_read_count(), 0);
    CHECK_EQ(ata_get_write_count(), 0);
    CHECK_EQ(ata_get_sector_count(), 8);
    CHECK_EQ(ata_read_sector(8, buffer), 0);   /* bound follows the new drive */
}

/* --- the defect this suite was written for -------------------------------- */

static void test_timeout_does_not_leak_into_next_command(void) {
    uint8_t buffer[ATA_SECTOR_SIZE];

    /*
     * F24. A poll timeout abandons a command that the drive is still
     * executing. Nothing resynchronises before the next one, so:
     *
     *   1. read(lba=5) times out -- the driver gives up, the DRIVE does not.
     *   2. read(lba=9) writes the task file. The drive is still BSY, so
     *      hardware ignores those writes and keeps working on LBA 5.
     *   3. ata_wait_data() sees the DRQ raised for LBA 5 and reads 512 bytes.
     *
     * The call returns 1 and the buffer holds sector 5 while the caller asked
     * for sector 9. DiskFS checksums its superblock but not the sectors it
     * reads afterwards, so it would take those bytes as directory or file
     * content without complaint.
     *
     * The assertion is not "the second read must fail" -- recovering and
     * returning the right data is better still. It is that a call which
     * returns success must return the sector it was asked for.
     */
    TEST("a timed-out command must not answer the next one");
    CHECK(install_healthy());

    /* Make the drive slower than the driver is willing to wait, but only just:
     * it finishes shortly after the first call gives up. */
    dev.busy_polls_next = ATA_POLL_LIMIT + 5;
    CHECK_EQ(ata_read_sector(5, buffer), 0);
    CHECK_EQ(dev.last_read_lba, 0xFFFFFFFFu);   /* it never completed */

    /* From here on the drive is prompt again. */
    dev.busy_polls_next = 0;
    for (unsigned i = 0; i < sizeof(buffer); i++) buffer[i] = 0xCC;

    {
        int ok = ata_read_sector(9, buffer);

        if (ok) {
            /* Success means the bytes must be sector 9's. */
            for (unsigned i = 0; i < ATA_SECTOR_SIZE; i++)
                CHECK_EQ(buffer[i], (uint8_t)(9 * 7 + i));
        } else {
            /* Refusing is acceptable; handing back the wrong sector is not. */
            CHECK_EQ(buffer[0], 0xCC);
        }
    }
    expect_irq_balanced();
}

static void test_recovers_after_a_timeout(void) {
    uint8_t buffer[ATA_SECTOR_SIZE];

    /*
     * The other half of the same property: after the drive comes back, the
     * driver has to become usable again. Refusing every subsequent request
     * would be safe but would take the disk down for good on one slow seek.
     */
    TEST("the driver recovers once the drive responds again");
    CHECK(install_healthy());

    dev.busy_polls_next = ATA_POLL_LIMIT + 5;
    CHECK_EQ(ata_read_sector(5, buffer), 0);

    /* The very next request must work. Draining the sector the drive was still
     * holding is what makes that possible; a fix that only refused to issue a
     * command onto a busy drive would be safe but would take the disk out of
     * service for good after one slow seek, so the two are worth separating. */
    dev.busy_polls_next = 0;
    CHECK_EQ(ata_read_sector(9, buffer), 1);
    for (unsigned i = 0; i < ATA_SECTOR_SIZE; i++)
        CHECK_EQ(buffer[i], (uint8_t)(9 * 7 + i));

    /* And the one after that, i.e. the recovery is not a one-off. */
    CHECK_EQ(ata_read_sector(2, buffer), 1);
    for (unsigned i = 0; i < ATA_SECTOR_SIZE; i++)
        CHECK_EQ(buffer[i], (uint8_t)(2 * 7 + i));
    CHECK_EQ(ata_get_read_count(), 2);         /* the timed-out one is not counted */
    expect_irq_balanced();
}

static void test_write_after_timeout(void) {
    uint8_t out[ATA_SECTOR_SIZE];
    uint8_t back[ATA_SECTOR_SIZE];

    /* The same hazard on the write side, where the consequence is worse: the
     * bytes could land on a sector the caller never named. */
    TEST("a timed-out command must not misdirect the next write");
    CHECK(install_healthy());
    for (unsigned i = 0; i < sizeof(out); i++) out[i] = 0xA5;

    dev.busy_polls_next = ATA_POLL_LIMIT + 5;
    CHECK_EQ(ata_read_sector(5, out), 0);

    dev.busy_polls_next = 0;
    for (unsigned i = 0; i < sizeof(out); i++) out[i] = 0xA5;

    if (ata_write_sector(7, out)) {
        CHECK_EQ(dev.last_write_lba, 7);
        CHECK_EQ(ata_read_sector(7, back), 1);
        for (unsigned i = 0; i < ATA_SECTOR_SIZE; i++) CHECK_EQ(back[i], 0xA5);
    }

    /* Whatever happened, sector 5 must not have been overwritten. */
    CHECK_EQ(ata_read_sector(5, back), 1);
    for (unsigned i = 0; i < ATA_SECTOR_SIZE; i++)
        CHECK_EQ(back[i], (uint8_t)(5 * 7 + i));
    expect_irq_balanced();
}

static void test_absent_drive_is_cheap(void) {
    TEST("an absent channel is detected without polling to the limit");
    fake_reset();
    dev.absent = 1;
    ata_install();

    CHECK_EQ(ata_is_available(), 0);
    /* A floating bus reads 0xFF, which HAS the BSY bit set -- so a driver that
     * only tested for BSY would poll the full limit before giving up. With
     * interrupts disabled that is a hundred thousand port reads of dead air on
     * every boot without a disk. Recognising 0xFF as "nobody home" is what
     * makes the probe cheap, and the count is the only thing that shows it. */
    CHECK(dev.status_reads < 100);
    CHECK(dev.status_reads > 0);
}

static void test_status_zero_is_cheap(void) {
    TEST("a silent channel is detected without polling to the limit");
    fake_reset();
    dev.status_zero = 1;
    ata_install();

    CHECK_EQ(ata_is_available(), 0);
    CHECK(dev.status_reads < 100);
}

static void test_drive_dies_during_write(void) {
    uint8_t out[ATA_SECTOR_SIZE];

    /*
     * The drive accepts the sector and then stops answering -- status reads
     * come back as zero. This is the one path where ata_wait_not_busy is the
     * only check between a dead drive and a reported success: the transfer is
     * already done, so nothing later looks at DRQ, and the flush that follows
     * would also "succeed" against a silent bus.
     */
    TEST("write: drive goes silent after taking the data");
    CHECK(install_healthy());
    for (unsigned i = 0; i < sizeof(out); i++) out[i] = 0x11;

    dev.die_after_data_out = 1;
    CHECK_EQ(ata_write_sector(4, out), 0);
    CHECK_EQ(ata_get_write_count(), 0);
    expect_irq_balanced();
}

static void test_read_error_with_drq(void) {
    uint8_t buffer[ATA_SECTOR_SIZE];

    /*
     * A drive that fails a read raises ERR and may raise DRQ with it. Testing
     * ERR on its own is not enough: without DRQ the driver simply waits and
     * times out, so it fails for the wrong reason and a missing error check
     * goes unnoticed. With both set, only an explicit ERR test stops the
     * driver from transferring 512 bytes of nothing and calling it success.
     */
    TEST("read: ERR raised together with DRQ");
    CHECK(install_healthy());
    for (unsigned i = 0; i < sizeof(buffer); i++) buffer[i] = 0xCC;

    dev.err_with_drq = 1;
    CHECK_EQ(ata_read_sector(1, buffer), 0);
    CHECK_EQ(buffer[0], 0xCC);                 /* nothing was transferred */
    CHECK_EQ(ata_get_read_count(), 0);

    /* The drive is healthy again once the condition clears, and the next
     * read works: an error must not wedge the driver. */
    dev.err_with_drq = 0;
    CHECK_EQ(ata_read_sector(1, buffer), 1);
    for (unsigned i = 0; i < ATA_SECTOR_SIZE; i++)
        CHECK_EQ(buffer[i], (uint8_t)(1 * 7 + i));

    TEST("read: DF raised together with DRQ");
    fake_reset();
    ata_install();
    for (unsigned i = 0; i < sizeof(buffer); i++) buffer[i] = 0xCC;
    dev.df_with_drq = 1;
    CHECK_EQ(ata_read_sector(1, buffer), 0);
    CHECK_EQ(buffer[0], 0xCC);
    CHECK_EQ(ata_get_read_count(), 0);
    expect_irq_balanced();
}

static void test_drq_while_busy(void) {
    uint8_t buffer[ATA_SECTOR_SIZE];

    /*
     * DRQ asserted while BSY is still set does not mean the data is ready --
     * BSY is precisely the bit that says the register file is not yet the
     * host's to touch. Reading on DRQ alone starts the transfer early and gets
     * whatever the controller has staged, which is not the sector.
     */
    TEST("read: DRQ while BSY is not a go-ahead");
    CHECK(install_healthy());
    dev.drq_while_busy = 1;
    dev.busy_polls_next = 50;                  /* busy for a while, DRQ set */

    CHECK_EQ(ata_read_sector(3, buffer), 1);   /* it waits, then succeeds */
    for (unsigned i = 0; i < ATA_SECTOR_SIZE; i++)
        CHECK_EQ(buffer[i], (uint8_t)(3 * 7 + i));
    CHECK_EQ(dev.data_overrun, 0);             /* never read during BSY */
    expect_irq_balanced();
}

static void test_write_beyond_28_bit_lba(void) {
    uint8_t out[ATA_SECTOR_SIZE];

    TEST("write: 28-bit addressing limit");
    fake_reset();
    dev.reported_sector_count = 0xFFFFFFFFu;
    ata_install();
    CHECK(ata_is_available());
    for (unsigned i = 0; i < sizeof(out); i++) out[i] = 0x42;

    /* The same limit as the read path, and it matters more here: a truncated
     * address does not return the wrong data, it overwrites the wrong sector. */
    CHECK_EQ(ata_write_sector(0x0FFFFFFFu + 1, out), 0);
    CHECK_EQ(ata_write_sector(0x1FFFFFFFu, out), 0);
    CHECK_EQ(dev.commands_issued, 1);          /* only IDENTIFY */
    CHECK_EQ(dev.writes_served, 0);
}

static void test_atapi_signature_high_byte(void) {
    TEST("install: ATAPI signature in the high byte alone");
    fake_reset();
    dev.sig_mid = 0;                           /* only the high byte is set */
    dev.sig_high = 0xEB;
    ata_install();
    CHECK_EQ(ata_is_available(), 0);

    TEST("install: ATAPI signature in the mid byte alone");
    fake_reset();
    dev.sig_mid = 0x14;
    dev.sig_high = 0;
    ata_install();
    CHECK_EQ(ata_is_available(), 0);
}

int main(void) {
    test_install_detects_drive();
    test_install_absent_drive();
    test_install_status_zero();
    test_install_atapi_signature();
    test_install_zero_sectors();
    test_install_busy_forever();
    test_install_error_bit();
    test_install_large_sector_count();

    test_read_success();
    test_read_lba_encoding();
    test_read_bounds();
    test_read_beyond_28_bit_lba();
    test_read_timeout();
    test_read_error_bits();
    test_read_drive_vanishes();

    test_write_success();
    test_write_bounds();
    test_write_timeout_before_data();
    test_write_flush_failure();
    test_write_error_bits();
    test_write_error_after_data();
    test_write_drq_stuck_after_data();

    test_repeated_calls();
    test_counters_only_count_success();
    test_install_resets_state();

    test_absent_drive_is_cheap();
    test_status_zero_is_cheap();
    test_atapi_signature_high_byte();
    test_read_error_with_drq();
    test_drq_while_busy();
    test_write_beyond_28_bit_lba();
    test_drive_dies_during_write();

    test_timeout_does_not_leak_into_next_command();
    test_recovers_after_a_timeout();
    test_write_after_timeout();

    TEST_REPORT("ata");
}
