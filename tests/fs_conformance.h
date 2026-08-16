#ifndef MINIOS_FS_CONFORMANCE_H
#define MINIOS_FS_CONFORMANCE_H

/*
 * Requirements every filesystem backend must satisfy, written once and run by
 * each backend's own suite (tests/test_ramfs.c, tests/test_diskfs.c,
 * tests/test_fat16.c).
 *
 * Why this file exists
 * --------------------
 * Two of this project's bugs came in through the same door. F22 (P0, froze the
 * whole machine) and F23 both started with a seek far past the end of a file
 * followed by a small write: sys_seek accepts any offset up to 0x7FFFFFFF, and
 * what happened next was up to whichever backend received it. RAMFS spun
 * forever doubling a capacity that had wrapped to zero; FAT16 recorded a length
 * for bytes it never stored.
 *
 * Both are fixed, and each has a unit test in its own suite. What neither fix
 * produced is a statement of the requirement itself -- so the next backend
 * someone writes has to rediscover it. PROJECT_STATE recorded that gap as
 * "下一個後端還是得自己重擋一次", i.e. as prose. This is that prose made
 * executable: include the header, hand it a writable file, and the backend
 * either satisfies the contract or the suite fails.
 *
 * The alternative considered and rejected was capping sys_seek itself. See
 * findings.md (SEEK1) for the derivation; the short version is that the real
 * limit is per-backend and differs by four orders of magnitude (2 KB for
 * DiskFS against tens of megabytes for RAMFS), so there is no principled
 * constant to put at the syscall boundary -- and capping there would retire
 * user/bigseek.c, currently the only artifact proving the whole ring-3 path
 * survives the F22 attack.
 *
 * The contract
 * ------------
 * Given a file that already holds some content, a write whose target the
 * backend cannot possibly reach must:
 *
 *   1. RETURN. Kernel syscalls run with interrupts disabled (int 0x80 is an
 *      interrupt gate), so a loop that does not terminate is not a slow
 *      write -- it is the whole machine stopping. This is F22.
 *   2. Store nothing: report zero bytes written.
 *   3. Leave the file exactly as it was -- same length, same contents. A write
 *      that stored nothing must not grow the file to the offset it was aimed
 *      at. This is F23.
 *   4. Survive being read at an impossible offset without faulting.
 *
 * Requirement 1 is enforced by a watchdog rather than an assertion: a
 * regression there hangs, and a hang has to fail the suite instead of stalling
 * it (the lesson from CAP13, and from the kb.c mutants that were only ever
 * caught by a timeout).
 */

#include <signal.h>
#include <stdio.h>
#include <unistd.h>

#include "../fs.h"

/* Armed for the whole run by fs_conformance_arm_watchdog(). Static so each
 * test binary gets its own; the handler only makes async-signal-safe calls. */
__attribute__((unused))
static void fs_conformance_on_alarm(int sig) {
    static const char msg[] =
        "  FAIL fs-conformance watchdog: a write did not terminate "
        "(F22-class regression)\n";
    ssize_t ignored = write(2, msg, sizeof(msg) - 1);

    (void)sig;
    (void)ignored;
    _exit(1);
}

__attribute__((unused))
static void fs_conformance_arm_watchdog(unsigned seconds) {
    signal(SIGALRM, fs_conformance_on_alarm);
    alarm(seconds);
}

/*
 * Run the contract against `node`, which must be a writable file holding
 * `length` bytes equal to `expected`. `backend` names the filesystem so a
 * failure says which one.
 *
 * The offsets below are the ones that actually matter rather than a spread of
 * round numbers:
 *   - 0x7FFFFFFF is the largest sys_seek permits, so it is the worst a ring-3
 *     program can aim at today. It is the exact value that triggered F22.
 *   - 0x80000000 is where unsigned capacity doubling wraps to zero.
 *   - 0xFFFFFFFF - 1 makes offset + size overflow for any size above one,
 *     which is the other way a bounds check written as addition goes wrong.
 */
static void fs_conformance_extreme_offsets(fs_node_t *node,
                                           const uint8_t *expected,
                                           uint32_t length,
                                           const char *backend) {
    static const uint32_t offsets[] = {
        0x7FFFFFFFu, 0x80000000u, 0x80000001u, 0xC0000000u, 0xFFFFFFFEu,
    };
    uint8_t payload[4] = { 'x', 'y', 'z', 'w' };
    uint8_t probe[64];
    uint32_t original_length;

    TEST("fs conformance: unreachable writes");
    CHECK(node != NULL);
    if (!node || !node->write) return;

    original_length = node->length;
    CHECK_EQ(original_length, length);

    for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        uint32_t written;

        /* Requirement 1: this call returning at all is the assertion. If the
         * backend loops, the watchdog fires and the suite fails. */
        written = node->write(node, offsets[i], sizeof(payload), payload);

        /* Requirement 2: nothing was stored. No backend in this system can
         * reach two gigabytes -- RAMFS would need the allocation to succeed,
         * DiskFS caps a file at four sectors, FAT16 at a 32 KB volume. */
        CHECK_EQ(written, 0);

        /* Requirement 3: the file is untouched. Length first, because that is
         * what F23 got wrong: it stored nothing yet grew the file anyway. */
        CHECK_EQ(node->length, original_length);
    }

    /* A size that overflows offset + size, in case a bound is written as an
     * addition that wraps rather than as a subtraction that cannot. */
    CHECK_EQ(node->write(node, 0xFFFFFF00u, 0x00000200u, payload), 0);
    CHECK_EQ(node->length, original_length);

    /* Requirement 3, contents: read the whole file back and compare. A backend
     * that grew its length would hand back bytes it never wrote. */
    if (node->read && length <= sizeof(probe)) {
        uint32_t got;

        for (unsigned i = 0; i < sizeof(probe); i++) probe[i] = 0xCC;
        got = node->read(node, 0, sizeof(probe), probe);
        CHECK_EQ(got, length);
        for (uint32_t i = 0; i < length && i < got; i++)
            CHECK_EQ(probe[i], expected[i]);
        CHECK_EQ(probe[length], 0xCC);      /* nothing past the real end */
    }

    /* Requirement 4: reads at impossible offsets are end-of-file, not faults
     * and not stale bytes from wherever the arithmetic landed. */
    if (node->read) {
        for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
            for (unsigned j = 0; j < sizeof(probe); j++) probe[j] = 0xCC;
            CHECK_EQ(node->read(node, offsets[i], sizeof(probe), probe), 0);
            CHECK_EQ(probe[0], 0xCC);
        }
    }

    /* And a zero-length write at an impossible offset is still a no-op rather
     * than a resize. */
    CHECK_EQ(node->write(node, 0x7FFFFFFFu, 0, payload), 0);
    CHECK_EQ(node->length, original_length);

    (void)backend;
}

#endif /* MINIOS_FS_CONFORMANCE_H */
