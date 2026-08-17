#ifndef PIPE_H
#define PIPE_H

#include <stdint.h>

#define PIPE_BUF_SIZE 4096

/*
 * Anonymous in-kernel pipe: a blocking ring buffer with separate reference
 * counts for the read and write ends. Used to connect two processes in a
 * shell pipeline (cmd1 | cmd2): cmd1's stdout writes into the pipe, cmd2's
 * stdin reads out of it.
 *
 *   - A read blocks while the buffer is empty and at least one writer remains;
 *     it returns 0 (EOF) once the buffer is empty and all writers have closed.
 *   - A write blocks while the buffer is full and at least one reader remains;
 *     it stops early (broken pipe) once all readers have closed.
 */
typedef struct pipe {
    uint8_t  buf[PIPE_BUF_SIZE];
    uint32_t read_pos;
    uint32_t write_pos;
    uint32_t count;     /* bytes currently buffered */
    int      readers;   /* live read ends, including in-flight reads */
    int      writers;   /* live write ends, including in-flight writes */
} pipe_t;

/* Allocate a pipe with one read end and one write end open (NULL on failure). */
pipe_t *pipe_create(void);

/* Read up to len bytes; blocks until data is available or EOF. Returns the
 * number of bytes read, or 0 at end of stream. */
uint32_t pipe_read(pipe_t *p, uint8_t *buffer, uint32_t len);

/* Write up to len bytes; blocks while full. Returns bytes written (may be less
 * than len if all readers have gone away). */
uint32_t pipe_write(pipe_t *p, const uint8_t *buffer, uint32_t len);

/* Close one end. When both ends are closed the pipe is freed. Closing the last
 * writer wakes blocked readers (EOF); closing the last reader wakes blocked
 * writers (broken pipe). */
void pipe_close_read(pipe_t *p);
void pipe_close_write(pipe_t *p);

/* Add a reference to an end (e.g. when an fd is duplicated). */
void pipe_ref_read(pipe_t *p);
void pipe_ref_write(pipe_t *p);

#endif
