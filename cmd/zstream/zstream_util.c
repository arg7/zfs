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
