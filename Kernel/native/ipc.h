#pragma once

/* Operations 58 and 59 share the caller's 4096-byte utility mailbox.
 * Command occupies bits 56..63, raw descriptor bits 32..39, and argument
 * bits 0..31. Local sendmsg/recvmsg reserve the last 40 bytes for rights. */
/* Operation 26 uses the high bit of the byte count for MSG_PEEK. */
#define LX_TCP_PEEK_FLAG (1u << 31)
#define LX_DESCRIPTOR_LIMIT 256 /* The largest encoded descriptor is 255. */
#define LX_IPC_RIGHTS 8
#define LX_IPC_CONTROL (4096 - (2 + LX_IPC_RIGHTS) * 4)
#define LX_SHARED_SPACE 9 /* returns total and free backing bytes as two u64s */
#define LX_SHARED_BASE (48UL << 30)
#define LX_SHARED_LIMIT (16UL << 20)
#define LX_SHARED_APERTURE (1UL << 30)
