// SPDX-License-Identifier: CDDL-1.0
/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or https://opensource.org/licenses/CDDL-1.0.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * Copyright 2010 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 *
 * Portions Copyright 2012 Martin Matuska <martin@matuska.org>
 */

/*
 * Copyright (c) 2020 by Datto Inc. All rights reserved.
 * Copyright (c) 2026 CompEd Software Design srl.
 */

#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <libnvpair.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stddef.h>
#include <zlib.h>

#include <libzfs.h>
#include <libzfs_core.h>

#include <sys/dmu.h>
#include <sys/dmu_send.h>
#include <sys/zfs_ioctl.h>
#include <sys/zio_checksum.h>
#include <zfs_fletcher.h>
#include "zstream.h"
#include "zstream_util.h"

static void
token_usage(void)
{
	(void) fprintf(stderr,
	    "usage:\n"
	    "  zstream token <resume_token>           decode a resume token\n"
	    "  zstream token -g [-i FILE]             generate a resume token\n"
	    "                                         from a (possibly truncated)\n"
	    "                                         send stream\n"
	    "\n"
	    "Exit codes:\n"
	    "  0  token generated successfully\n"
	    "  1  invalid input (no DRR_BEGIN or no data records)\n"
	    "  2  stream is complete (DRR_END found)\n");
	exit(1);
}

/*
 * Encode an nvlist into a resume token string in the standard 3-dash
 * format compatible with zfs recv -s:
 *   <version>-<checksum>-<packedlen>-<hex_payload>
 *
 * The stream offset is stored inside the nvlist, not in the token string.
 * The caller must free the returned string.
 */
static char *
nvlist_to_resume_token(nvlist_t *nvl)
{
	/* 1. Pack the nvlist into a binary buffer */
	size_t packed_len;
	char *packed = fnvlist_pack(nvl, &packed_len);

	/* 2. Gzip compress the packed nvlist */
	uLongf compressed_bound = compressBound((uLong)packed_len);
	unsigned char *compressed = safe_malloc(compressed_bound);
	uLongf compressed_len = compressed_bound;

	if (compress2(compressed, &compressed_len,
	    (const Bytef *)packed, (uLong)packed_len,
	    Z_BEST_COMPRESSION) != Z_OK) {
		(void) fprintf(stderr, "Error: failed to compress token "
		    "payload\n");
		fnvlist_pack_free(packed, packed_len);
		free(compressed);
		return (NULL);
	}
	fnvlist_pack_free(packed, packed_len);

	/* 3. Compute Fletcher-4 checksum of the compressed data */
	zio_cksum_t cksum;
	fletcher_4_native_varsize(compressed, compressed_len, &cksum);

	/* 4. Hex-encode the compressed data */
	size_t hex_len = compressed_len * 2 + 1;
	char *hex = safe_malloc(hex_len);
	for (size_t i = 0; i < compressed_len; i++) {
		(void) snprintf(hex + i * 2, 3, "%02x", compressed[i]);
	}
	hex[hex_len - 1] = '\0';
	free(compressed);

	/* 5. Format the complete token: 1-cksum-packedlen-hex */
	char *token;
	if (asprintf(&token, "%u-%llx-%llx-%s",
	    ZFS_SEND_RESUME_TOKEN_VERSION,
	    (unsigned long long)cksum.zc_word[0],
	    (unsigned long long)packed_len,
	    hex) == -1) {
		free(hex);
		(void) fprintf(stderr, "Error: failed to format resume token\n");
		return (NULL);
	}
	free(hex);
	return (token);
}

/*
 * Generate a resume token from a (possibly truncated) send stream.
 *
 * Reads the stream from infp.  Finds the last complete DRR record
 * before the truncation point and builds a resume token that allows
 * resuming from that record using "zstream resume -t <token> -i <file>".
 */
static int
generate_resume_token(FILE *infp)
{
	dmu_replay_record_t drr;
	int nread;

	/* Read DRR_BEGIN */
	nread = sfread(&drr, sizeof (drr), infp);
	if (nread == 0 || drr.drr_type != DRR_BEGIN) {
		(void) fprintf(stderr,
		    "Error: stream does not start with DRR_BEGIN\n");
		return (1);
	}

	struct drr_begin drrb = drr.drr_u.drr_begin;
	if (drrb.drr_magic != DMU_BACKUP_MAGIC) {
		(void) fprintf(stderr,
		    "Error: invalid send stream magic\n");
		return (1);
	}

	/*
	 * Save BEGIN fields before the scanning loop overwrites drr.
	 */
	uint64_t featureflags = DMU_GET_FEATUREFLAGS(drrb.drr_versioninfo);
	uint64_t toguid = drrb.drr_toguid;
	uint64_t fromguid = drrb.drr_fromguid;
	char toname[MAXNAMELEN];
	(void) strlcpy(toname, drrb.drr_toname, sizeof (toname));

	/* Read the BEGIN payload (nvlist) */
	char *begin_payload = NULL;
	uint64_t begin_payloadlen = drr.drr_payloadlen;
	if (begin_payloadlen > 0) {
		begin_payload = safe_malloc(begin_payloadlen);
		if (fread(begin_payload, begin_payloadlen, 1, infp) != 1) {
			(void) fprintf(stderr,
			    "Error: failed to read BEGIN payload\n");
			free(begin_payload);
			return (1);
		}
	}

	/* Unpack the BEGIN nvlist to extract feature flags */
	nvlist_t *begin_nvl = NULL;
	if (begin_payload != NULL) {
		int err = nvlist_unpack(begin_payload, begin_payloadlen,
		    &begin_nvl, 0);
		free(begin_payload);
		if (err != 0) {
			(void) fprintf(stderr,
			    "Error: failed to unpack BEGIN nvlist\n");
			return (1);
		}
	} else {
		begin_nvl = fnvlist_alloc();
	}

	/* Scan forward to find the last complete record */
	uint64_t resume_obj = 0, resume_off = 0;
	uint64_t resume_stream_offset = 0;
	uint64_t total_bytes = 0;
	int found_resume_point = 0;
	int seen_end = 0;

	/*
	 * Only track stream position for seekable file inputs.
	 * Pipes and other non-seekable streams return -1 from ftell().
	 */
	long probe = ftell(infp);
	int seekable = (probe >= 0);

	for (;;) {
		long record_start = ftell(infp);
		nread = sfread(&drr, sizeof (drr), infp);
		if (nread == 0)
			break;

		uint64_t psize = record_payload_size(&drr);
		if (psize > 0) {
			void *buf = safe_malloc(psize);
			if (fread(buf, psize, 1, infp) != 1) {
				free(buf);
				break;
			}
			free(buf);
		}

		if (drr.drr_type == DRR_END) {
			seen_end = 1;
			break;
		}

		/*
		 * Update resume point.  We want to resume at a
		 * data-carrying record (WRITE, WRITE_EMBEDDED,
		 * WRITE_BYREF, SPILL) or the next OBJECT/OBJECT_RANGE
		 * after the last data record.
		 */
		if (seekable && record_start >= 0)
			resume_stream_offset = (uint64_t)record_start;

		switch (drr.drr_type) {
		case DRR_WRITE: {
			struct drr_write *drrw = &drr.drr_u.drr_write;
			resume_obj = drrw->drr_object;
			resume_off = drrw->drr_offset;
			uint64_t len = DRR_WRITE_PAYLOAD_SIZE(drrw);
			total_bytes = drrw->drr_offset + len;
			found_resume_point = 1;
			break;
		}
		case DRR_WRITE_BYREF: {
			struct drr_write_byref *drrwb =
			    &drr.drr_u.drr_write_byref;
			resume_obj = drrwb->drr_object;
			resume_off = drrwb->drr_offset;
			uint64_t blen = drrwb->drr_length;
			total_bytes = drrwb->drr_offset + blen;
			found_resume_point = 1;
			break;
		}
		case DRR_WRITE_EMBEDDED: {
			struct drr_write_embedded *drrwe =
			    &drr.drr_u.drr_write_embedded;
			resume_obj = drrwe->drr_object;
			resume_off = drrwe->drr_offset;
			uint64_t elen = P2ROUNDUP(drrwe->drr_psize, 8);
			total_bytes = drrwe->drr_offset + elen;
			found_resume_point = 1;
			break;
		}
		case DRR_SPILL: {
			struct drr_spill *drrs = &drr.drr_u.drr_spill;
			resume_obj = drrs->drr_object;
			resume_off = 0;
			found_resume_point = 1;
			break;
		}
		case DRR_OBJECT: {
			struct drr_object *drro = &drr.drr_u.drr_object;
			resume_obj = drro->drr_object;
			resume_off = 0;
			found_resume_point = 1;
			break;
		}
		default:
			break;
		}
	}

	nvlist_free(begin_nvl);

	if (seen_end) {
		(void) fprintf(stderr,
		    "Error: stream is complete (reached DRR_END), "
		    "no resume token needed\n");
		return (2);
	}

	if (!found_resume_point) {
		(void) fprintf(stderr,
		    "Error: no data records found in stream\n");
		return (1);
	}

	/* Build the resume nvlist */
	nvlist_t *resume_nvl = fnvlist_alloc();
	fnvlist_add_uint64(resume_nvl, "object", resume_obj);
	fnvlist_add_uint64(resume_nvl, "offset", resume_off);
	fnvlist_add_uint64(resume_nvl, "bytes", total_bytes);
	fnvlist_add_string(resume_nvl, "toname", toname);
	fnvlist_add_uint64(resume_nvl, "toguid", toguid);
	if (fromguid != 0)
		fnvlist_add_uint64(resume_nvl, "fromguid", fromguid);

	/* Add feature flags */
	if (featureflags & DMU_BACKUP_FEATURE_LARGE_BLOCKS)
		fnvlist_add_boolean_value(resume_nvl, "largeblockok",
		    B_TRUE);
	if (featureflags & DMU_BACKUP_FEATURE_EMBED_DATA)
		fnvlist_add_boolean_value(resume_nvl, "embedok", B_TRUE);
	if (featureflags & DMU_BACKUP_FEATURE_COMPRESSED)
		fnvlist_add_boolean_value(resume_nvl, "compressok", B_TRUE);
	if (featureflags & DMU_BACKUP_FEATURE_RAW)
		fnvlist_add_boolean_value(resume_nvl, "rawok", B_TRUE);

	/* Add stream offset for zstream resume fast-seek */
	if (resume_stream_offset > 0)
		fnvlist_add_uint64(resume_nvl, "stream_offset",
		    resume_stream_offset);

	/* Encode the token */
	char *token = nvlist_to_resume_token(resume_nvl);
	nvlist_free(resume_nvl);

	if (token == NULL)
		return (1);

	(void) printf("%s\n", token);
	free(token);
	return (0);
}

int
zstream_do_token(int argc, char *argv[])
{
	int c;
	int generate = 0;
	char *infile = NULL;

	fletcher_4_init();

	while ((c = getopt(argc, argv, "gi:")) != -1) {
		switch (c) {
		case 'g':
			generate = 1;
			break;
		case 'i':
			infile = optarg;
			break;
		default:
			token_usage();
		}
	}

	argc -= optind;
	argv += optind;

	if (generate) {
		FILE *infp;
		if (infile != NULL) {
			infp = fopen(infile, "rb");
			if (infp == NULL) {
				(void) fprintf(stderr,
				    "Error: cannot open %s: %s\n",
				    infile, strerror(errno));
				return (1);
			}
		} else {
			infp = stdin;
		}

		int ret = generate_resume_token(infp);
		if (infile != NULL)
			fclose(infp);
		return (ret);
	}

	/* Legacy mode: decode an existing resume token */
	if (argc < 1) {
		(void) fprintf(stderr, "Need to pass the resume token\n");
		token_usage();
	}

	char *resume_token = argv[0];
	libzfs_handle_t *hdl;

	if ((hdl = libzfs_init()) == NULL) {
		(void) fprintf(stderr, "%s\n", libzfs_error_init(errno));
		return (1);
	}

	nvlist_t *resume_nvl =
	    zfs_send_resume_token_to_nvlist(hdl, resume_token);

	if (resume_nvl == NULL) {
		(void) fprintf(stderr,
		    "Unable to parse resume token: %s\n",
		    libzfs_error_description(hdl));
		libzfs_fini(hdl);
		return (1);
	}

	dump_nvlist(resume_nvl, 5);
	nvlist_free(resume_nvl);

	libzfs_fini(hdl);
	return (0);
}
