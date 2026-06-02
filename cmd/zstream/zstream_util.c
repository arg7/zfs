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
 * Copyright (c) 2005, 2010, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2011, 2020 by Delphix. All rights reserved.
 * Copyright (c) 2012, Joyent, Inc. All rights reserved.
 * Copyright (c) 2012 Pawel Jakub Dawidek <pawel@dawidek.net>.
 * All rights reserved
 * Copyright (c) 2013 Steven Hartland. All rights reserved.
 * Copyright 2015, OmniTI Computer Consulting, Inc. All rights reserved.
 * Copyright 2016 Igor Kozhukhov <ikozhukhov@gmail.com>
 * Copyright (c) 2018, loli10K <ezomori.nozomu@gmail.com>. All rights reserved.
 * Copyright (c) 2019 Datto Inc.
 * Copyright (c) 2024, Klara, Inc.
 */

#include <sys/debug.h>
#include <stddef.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <libzfs.h>
#include <zfs_fletcher.h>
#include "zstream_util.h"

/*
 * From libzfs_sendrecv.c
 */
int
dump_record(dmu_replay_record_t *drr, void *payload, size_t payload_len,
    zio_cksum_t *zc, int outfd)
{
	ASSERT3U(offsetof(dmu_replay_record_t, drr_u.drr_checksum.drr_checksum),
	    ==, sizeof (dmu_replay_record_t) - sizeof (zio_cksum_t));
	fletcher_4_incremental_native(drr,
	    offsetof(dmu_replay_record_t, drr_u.drr_checksum.drr_checksum), zc);
	if (drr->drr_type != DRR_BEGIN) {
		ASSERT(ZIO_CHECKSUM_IS_ZERO(&drr->drr_u.
		    drr_checksum.drr_checksum));
		drr->drr_u.drr_checksum.drr_checksum = *zc;
	}
	fletcher_4_incremental_native(&drr->drr_u.drr_checksum.drr_checksum,
	    sizeof (zio_cksum_t), zc);
	if (write(outfd, drr, sizeof (*drr)) == -1)
		return (errno);
	if (payload_len != 0) {
		fletcher_4_incremental_native(payload, payload_len, zc);
		if (write(outfd, payload, payload_len) == -1)
			return (errno);
	}
	return (0);
}

void *
safe_malloc(size_t size)
{
	void *rv = malloc(size);
	if (rv == NULL) {
		(void) fprintf(stderr, "Error: failed to allocate %zu bytes\n",
		    size);
		exit(1);
	}
	return (rv);
}

void *
safe_calloc(size_t size)
{
	void *rv = calloc(1, size);
	if (rv == NULL) {
		(void) fprintf(stderr,
		    "Error: failed to allocate %zu bytes\n", size);
		exit(1);
	}
	return (rv);
}

/*
 * Safe version of fread(), exits on error.
 */
int
sfread(void *buf, size_t size, FILE *fp)
{
	int rv = fread(buf, size, 1, fp);
	if (rv == 0 && ferror(fp)) {
		(void) fprintf(stderr, "Error while reading file: %s\n",
		    strerror(errno));
		exit(1);
	}
	return (rv);
}

/*
 * Parse the stream byte offset from a resume token.
 * Extracts "stream_offset" from the token's nvlist payload.
 * This is available on tokens from zfs recv -s or zstream token -g
 * on kernels that include it in the resume nvlist.
 * Returns the offset, or 0 if not present.
 */
uint64_t
token_stream_offset(const char *token)
{
	libzfs_handle_t *hdl = libzfs_init();
	if (hdl != NULL) {
		nvlist_t *nvl = zfs_send_resume_token_to_nvlist(hdl, token);
		if (nvl != NULL) {
			uint64_t val = 0;
			(void) nvlist_lookup_uint64(nvl, "stream_offset",
			    &val);
			nvlist_free(nvl);
			libzfs_fini(hdl);
			if (val > 0)
				return (val);
		} else {
			libzfs_fini(hdl);
		}
	}
	return (0);
}

/*
 * Parse a size string with optional K/M/G/T/P/E suffix (base-2).
 * Accepts plain integers (1024), integers with suffixes (1K, 2G),
 * and floating-point values (2.5K, 1.5G).
 * Returns the parsed value, or 0 on error.
 */
uint64_t
parse_size_suffix(const char *str)
{
	char *end;
	double num = strtod(str, &end);

	if (*end == '\0')
		return ((uint64_t)num);

	uint64_t multiplier = 1;
	switch (*end) {
	case 'K': case 'k':
		multiplier = 1024ULL; break;
	case 'M':
		multiplier = 1024ULL * 1024; break;
	case 'G': case 'g':
		multiplier = 1024ULL * 1024 * 1024; break;
	case 'T': case 't':
		multiplier = 1024ULL * 1024 * 1024 * 1024; break;
	case 'P': case 'p':
		multiplier = 1024ULL * 1024 * 1024 * 1024 * 1024; break;
	case 'E': case 'e':
		multiplier = 1024ULL * 1024 * 1024 * 1024 * 1024 * 1024; break;
	default:
		return (0);
	}

	if (end[1] != '\0')
		return (0);

	return ((uint64_t)(num * multiplier));
}

/*
 * Return the payload size for a given DRR record type.
 */
uint64_t
record_payload_size(dmu_replay_record_t *drr)
{
	switch (drr->drr_type) {
	case DRR_BEGIN:
		return (drr->drr_payloadlen);
	case DRR_OBJECT: {
		struct drr_object *drro = &drr->drr_u.drr_object;
		return (DRR_OBJECT_PAYLOAD_SIZE(drro));
	}
	case DRR_SPILL: {
		struct drr_spill *drrs = &drr->drr_u.drr_spill;
		return (DRR_SPILL_PAYLOAD_SIZE(drrs));
	}
	case DRR_WRITE: {
		struct drr_write *drrw = &drr->drr_u.drr_write;
		return (DRR_WRITE_PAYLOAD_SIZE(drrw));
	}
	case DRR_WRITE_EMBEDDED: {
		struct drr_write_embedded *drrwe =
		    &drr->drr_u.drr_write_embedded;
		return (P2ROUNDUP(drrwe->drr_psize, 8));
	}
	default:
		return (0);
	}
}

/*
 * Copy records from infp to outfd, recomputing checksums into zc.
 * If skip_begin is set, the first record (DRR_BEGIN + payload) is skipped.
 *
 * Returns: 1 if DRR_END was seen, 0 on EOF, -1 on error.
 */
int
stream_copy_records(FILE *infp, int outfd, zio_cksum_t *zc, int skip_begin)
{
	dmu_replay_record_t drr;
	int first = 1;

	for (;;) {
		int nread = sfread(&drr, sizeof (drr), infp);
		if (nread == 0)
			return (0);

		uint64_t psize = record_payload_size(&drr);
		void *buf = NULL;
		if (psize > 0) {
			buf = safe_malloc(psize);
			if (fread(buf, psize, 1, infp) != 1) {
				free(buf);
				if (feof(infp))
					return (0);
				return (-1);
			}
		}

		if (skip_begin && first) {
			first = 0;
			free(buf);
			continue;
		}
		first = 0;

		if (drr.drr_type == DRR_END) {
			/*
			 * DRR_END is consumed but not written.  Do not
			 * include it in the running checksum — the caller
			 * will write a fresh END via write_end_record(),
			 * and zc must reflect only the records that were
			 * actually written to output.
			 */
			free(buf);
			return (1);
		}

		bzero(&drr.drr_u.drr_checksum.drr_checksum,
		    sizeof (zio_cksum_t));
		int err = dump_record(&drr, buf, psize, zc, outfd);
		free(buf);
		if (err != 0)
			return (-1);
	}
}

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
int
stream_validate_tail(FILE *infp, zio_cksum_t *zc)
{
	dmu_replay_record_t drr;

	for (;;) {
		int nread = sfread(&drr, sizeof (drr), infp);
		if (nread == 0)
			return (0);

		uint64_t psize = record_payload_size(&drr);
		void *buf = NULL;
		if (psize > 0) {
			buf = safe_malloc(psize);
			if (fread(buf, psize, 1, infp) != 1) {
				free(buf);
				if (feof(infp))
					return (-2);
				return (-1);
			}
		}

		if (drr.drr_type == DRR_END) {
			free(buf);
			return (1);
		}

		fletcher_4_incremental_native(&drr,
		    offsetof(dmu_replay_record_t,
		    drr_u.drr_checksum.drr_checksum), zc);
		if (drr.drr_type != DRR_BEGIN)
			drr.drr_u.drr_checksum.drr_checksum = *zc;
		fletcher_4_incremental_native(
		    &drr.drr_u.drr_checksum.drr_checksum,
		    sizeof (zio_cksum_t), zc);

		if (psize > 0) {
			fletcher_4_incremental_native(buf, psize, zc);
			free(buf);
		}
	}
}

/*
 * Undo a single fletcher-4 update: given output state Z and the word W
 * that was added, recover the input state.  Process data words in LIFO
 * order — the buffer's 32-bit words in reverse of the order
 * fletcher_4_incremental_native traversed them.
 */
static void
fletcher_4_decremental_native(const void *buf, uint64_t size, zio_cksum_t *zcp)
{
	const uint32_t *start = buf;
	const uint32_t *ip = start + (size / sizeof (uint32_t));
	uint64_t a = zcp->zc_word[0];
	uint64_t b = zcp->zc_word[1];
	uint64_t c = zcp->zc_word[2];
	uint64_t d = zcp->zc_word[3];

	while (ip > start) {
		--ip;
		d -= c;
		c -= b;
		b -= a;
		a -= *ip;
	}

	zcp->zc_word[0] = a;
	zcp->zc_word[1] = b;
	zcp->zc_word[2] = c;
	zcp->zc_word[3] = d;
}

/*
 * O(1) tail validation: reads only the last record from a stream file
 * that ends on a clean record boundary.  Recovers the running checksum
 * from the embedded per-record checksum using fletcher-4 inversion,
 * then forward-computes the final zc through that last record.
 *
 * Strategy:
 *  1. Try S-sizeof(drr) first -- last record may be DRR_END (psize=0).
 *  2. If not, read the trailing portion of the file into a buffer and
 *     scan in 4-byte steps looking for a valid record header (drr_type
 *     in [0, DRR_NUMTYPES) and file size alignment
 *     S == pos + sizeof(drr) + psize).
 *  3. If the trailing-buffer scan fails, fall back to the full O(n)
 *     stream_validate_tail().
 *
 * The buffer size (2 MB) covers any realistic ZFS record size; records
 * larger than that are handled by the fallback.
 *
 * Returns: 1 if DRR_END was seen (stream complete),
 *          0 on clean tail (no DRR_END),
 *          -1 on error,
 *          -2 if the file appears to have a dirty tail (should not
 *          happen for a clean-tail file, but checked defensively).
 *
 * Does NOT validate that the stream starts with DRR_BEGIN; the caller
 * should do that separately if needed.
 */

#define	TRAILER_BUFSZ	(2ULL * 1024 * 1024)

int
stream_validate_tail_fast(FILE *infp, zio_cksum_t *zc)
{
	dmu_replay_record_t drr;
	long S;

	if (fseek(infp, 0, SEEK_END) != 0)
		return (-1);
	S = ftell(infp);
	if (S < (long)sizeof (drr))
		return (-1);

	long pos = -1;
	uint64_t psize = 0;

	/*
	 * Fast path #1: assume the last record has no payload (DRR_END,
	 * or another type with a zero-size payload).
	 */
	if (fseek(infp, S - (long)sizeof (drr), SEEK_SET) == 0 &&
	    fread(&drr, sizeof (drr), 1, infp) == 1 &&
	    drr.drr_type >= DRR_BEGIN && drr.drr_type < DRR_NUMTYPES) {
		uint64_t p = record_payload_size(&drr);
		if (S - (long)sizeof (drr) + (long)sizeof (drr) +
		    (long)p == S) {
			pos = S - (long)sizeof (drr);
			psize = p;
		}
	}

	/*
	 * Fast path #2: scan the trailing TRAILER_BUFSZ bytes in 4-byte
	 * steps searching for a valid record header.
	 */
	if (pos < 0) {
		uint64_t bufsz = TRAILER_BUFSZ;
		if ((long)bufsz > S)
			bufsz = (uint64_t)S;

		unsigned char *buf = safe_malloc(bufsz);
		if (fseek(infp, S - (long)bufsz, SEEK_SET) != 0 ||
		    fread(buf, 1, bufsz, infp) != bufsz) {
			free(buf);
			return (-1);
		}

		for (long off = (long)bufsz - (long)sizeof (drr);
		    off >= 0; off -= 4) {
			uint32_t type;
			memcpy(&type, buf + off, sizeof (type));

			if (type >= DRR_NUMTYPES)
				continue;

			memcpy(&drr, buf + off, sizeof (drr));
			uint64_t p = record_payload_size(&drr);

			long filepos = S - (long)bufsz + off;
			if (filepos + (long)sizeof (drr) +
			    (long)p == S) {
				pos = filepos;
				psize = p;
				break;
			}
		}
		free(buf);
	}

	/* Fall back to full O(n) scan if fast paths didn't find it */
	if (pos < 0) {
		if (fseek(infp, 0, SEEK_SET) != 0)
			return (-1);
		return (stream_validate_tail(infp, zc));
	}

	/* Read the payload if any */
	void *payload = NULL;
	if (psize > 0) {
		if (fseek(infp, pos + (long)sizeof (drr), SEEK_SET) != 0) {
			return (-1);
		}
		payload = safe_malloc(psize);
		if (fread(payload, psize, 1, infp) != 1) {
			free(payload);
			return (-2);
		}
	}

	if (drr.drr_type == DRR_END) {
		/*
		 * Recover zc_before (stream checksum prior to DRR_END)
		 * from the embedded record checksum.
		 */
		size_t hdrsize = offsetof(dmu_replay_record_t,
		    drr_u.drr_checksum.drr_checksum);
		*zc = drr.drr_u.drr_checksum.drr_checksum;
		fletcher_4_decremental_native(&drr, hdrsize, zc);
		free(payload);
		return (1);
	}

	/*
	 * The embedded checksum in a non-BEGIN record written by
	 * dump_record() equals fletcher4(zc_before, header_part).
	 * Undo the header-part contribution to recover zc_before,
	 * then forward-compute the final zc the same way
	 * dump_record() would.
	 */
	size_t hdrsize = offsetof(dmu_replay_record_t,
	    drr_u.drr_checksum.drr_checksum);

	*zc = drr.drr_u.drr_checksum.drr_checksum;
	fletcher_4_decremental_native(&drr, hdrsize, zc);

	fletcher_4_incremental_native(&drr, hdrsize, zc);
	if (drr.drr_type != DRR_BEGIN)
		drr.drr_u.drr_checksum.drr_checksum = *zc;
	fletcher_4_incremental_native(
	    &drr.drr_u.drr_checksum.drr_checksum,
	    sizeof (zio_cksum_t), zc);

	if (psize > 0) {
		fletcher_4_incremental_native(payload, psize, zc);
		free(payload);
	}

	return (0);
}
