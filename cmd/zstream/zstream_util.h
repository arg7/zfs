// SPDX-License-Identifier: CDDL-1.0
/*
 * CDDL HEADER START
 *
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 *
 * CDDL HEADER END
 */

/*
 * Copyright (c) 2026 by Garth Snyder. All rights reserved.
 */

#ifndef	_ZSTREAM_UTIL_H
#define	_ZSTREAM_UTIL_H

#ifdef	__cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <sys/zfs_ioctl.h>
#include <sys/zio_checksum.h>

/*
 * The safe_ versions of the functions below terminate the process if the
 * operation doesn't succeed instead of returning an error.
 */
extern void *
safe_malloc(size_t size);

extern void *
safe_calloc(size_t n);

extern int
sfread(void *buf, size_t size, FILE *fp);

/*
 * 1) Update checksum with the record header up to drr_checksum.
 * 2) Update checksum field in the record header.
 * 3) Update checksum with the checksum field in the record header.
 * 4) Update checksum with the contents of the payload.
 * 5) Write header and payload to fd.
 */
extern int
dump_record(dmu_replay_record_t *drr, void *payload, size_t payload_len,
	zio_cksum_t *zc, int outfd);

extern uint64_t
token_stream_offset(const char *token);

extern uint64_t
parse_size_suffix(const char *str);

extern uint64_t
record_payload_size(dmu_replay_record_t *drr);

/*
 * Copy records from infp to outfd, recomputing checksums into zc.
 * If skip_begin is set, the first record (DRR_BEGIN + payload) is
 * skipped.
 *
 * Returns: 1 if DRR_END was seen, 0 on EOF, -1 on error.
 */
extern int
stream_copy_records(FILE *infp, int outfd, zio_cksum_t *zc, int skip_begin);

/*
 * Validate that a stream file has a clean tail (no partial records at EOF).
 * Reads all records, accumulates checksum into zc, but writes nothing.
 * A "dirty" tail means EOF occurred mid-record-payload (file truncated
 * within a WRITE/OBJECT/SPILL record), producing unrecoverable garbage.
 *
 * Returns: 1 if DRR_END was seen (stream complete),
 *          0 on clean EOF at a record boundary (no DRR_END),
 *          -1 on error,
 *          -2 on dirty tail (EOF mid-record payload).
 */
extern int
stream_validate_tail(FILE *infp, zio_cksum_t *zc);

/*
 * O(1) variant: validates the tail by reading only the last record.
 * Recovers the running checksum from the embedded per-record checksum
 * via fletcher-4 inversion.  Same return values as stream_validate_tail().
 * Does NOT verify DRR_BEGIN at the head — caller must do that if needed.
 */
extern int
stream_validate_tail_fast(FILE *infp, zio_cksum_t *zc);

#ifdef __cplusplus
}
#endif

#endif  /* _ZSTREAM_UTIL_H */
