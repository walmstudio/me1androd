#ifndef HLE_D3D9_CMD_H
#define HLE_D3D9_CMD_H
/* hle_d3d9_cmd.h - the D3D9 command stream (v1.2.1, ME1_D3D_CMD; the "scheduler" of the 1.2 preview, reworked).
 *
 * The front end (hle_d3d9.c, the game's rendering thread) records every renderer call - state deltas, draws, clears,
 * copies, texture uploads, shader creations, frees, occlusion queries, Present - in order into one ring buffer; the
 * consumer replays them against the renderer (hle_d3d9_gl.h / hle_d3d9_vk.h):
 *   ME1_D3D_CMD=0  off: the renderer is called directly (as up to v1.2)
 *   ME1_D3D_CMD=1  deferred on the same thread: the frame is replayed at Present (a check of the recording; no gain)
 *   ME1_D3D_CMD=2  render thread: the replay runs on its own thread, in parallel with the game's next frame (CSMT-like)
 *
 * Single producer (all D3D9 calls hold the device mutex), single consumer. The ring lives in guest memory, so captured
 * data (*UP vertices/indices, rects, texture lock contents) keeps a guest address and the renderer reads it unchanged.
 * Positions are monotonic byte counters: the end position of a record identifies it ("pos"); the consumer publishes
 * the position it has finished ("done"). Records never wrap: a PAD record fills the end of the buffer. */
#include <stdint.h>
#include <stddef.h>

typedef struct CmdHdr {
    uint32_t size;          /* bytes of the whole record (16-aligned) */
    uint16_t type, flags;   /* type 0 = PAD */
    const char* name;       /* the D3D9 method that recorded it (g_cur in the consumer's logs) */
} CmdHdr;

typedef void (*cmd_exec_fn)(CmdHdr* h);
/* consumer events: 0 = thread start, 1 = queue drained, 2 = idle tick (while cmd_idle_ms() > 0) */
typedef void (*cmd_idle_fn)(int ev);

/* mode 1 or 2; ring: host pointer + guest address of `bytes` (multiple of 16) */
int cmd_init(int mode, uint8_t* host, uint32_t guest, uint32_t bytes, cmd_exec_fn exec, cmd_idle_fn idle);
int cmd_mode(void);
void* cmd_begin(uint32_t bytes);                  /* reserve a record (header included); waits / replays while full */
uint64_t cmd_end(void* rec, uint32_t bytes, uint16_t type, const char* name);   /* commit; returns its pos */
void cmd_cancel(void);                            /* drop the reserved record */
uint32_t cmd_max(void);                           /* the largest record */
uint64_t cmd_pos(void);                           /* producer: end of the last committed record */
uint64_t cmd_done(void);                          /* consumer: end of the last executed record */
void cmd_wait(uint64_t pos);                      /* until the record ending at pos has run (mode 1: runs it here) */
void cmd_kick(void);                              /* wake the consumer now */
uint32_t cmd_guest(const void* p);                /* guest address of a pointer into the ring */
int cmd_on_consumer(void);                        /* 1 on the thread that replays */
void cmd_set_idle_ms(int ms);                     /* consumer: idle ticks every ms while > 0 (pending queries) */
void cmd_stats(char* out, size_t cap);            /* records, MB, waits of the producer */
void cmd_counters(uint64_t* bytes, uint64_t* waits, double* wait_s);   /* totals: bytes recorded, producer waits */
#endif
