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
 * Copyright (c) 2026 CompEd Software Design srl.
 */

#include <err.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libzfs.h>
#include <libzfs_core.h>
#include <sys/dmu_send.h>
#include <sys/zfs_ioctl.h>
#include <sys/zio_checksum.h>
#include "zstream.h"
#include "zstream_util.h"

static void
resume_usage(void)
{
	(void) fprintf(stderr,
	    "usage: zstream resume -t resume_token [-i infile]\n"
	    "\n"
	    "Read a cached send stream from infile (or stdin),\n"
	    "generate a resume stream at the point specified by\n"
	    "the resume token, and write it to stdout.\n"
	    "\n"
	    "Example:\n"
	    "  zstream resume -t <token> -i cached.zfs | \\\n"
	    "      zfs receive -s pool/fs\n");
	exit(1);
}

/*
 * Return the payload size for a given DRR record type.
 */
static uint64_t
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
 * Extract the {object, offset} for a record, used to compare against
 * the resume point. Records without object/offset semantics return {0, 0}.
 */
static void
record_object_offset(dmu_replay_record_t *drr, uint64_t *obj, uint64_t *off)
{
	*obj = 0;
	*off = 0;
	switch (drr->drr_type) {
	case DRR_OBJECT:
		*obj = drr->drr_u.drr_object.drr_object;
		break;
	case DRR_FREEOBJECTS:
		*obj = drr->drr_u.drr_freeobjects.drr_firstobj;
		break;
	case DRR_WRITE:
		*obj = drr->drr_u.drr_write.drr_object;
		*off = drr->drr_u.drr_write.drr_offset;
		break;
	case DRR_FREE:
		*obj = drr->drr_u.drr_free.drr_object;
		*off = drr->drr_u.drr_free.drr_offset;
		break;
	case DRR_WRITE_BYREF:
		*obj = drr->drr_u.drr_write_byref.drr_object;
		*off = drr->drr_u.drr_write_byref.drr_offset;
		break;
	case DRR_SPILL:
		*obj = drr->drr_u.drr_spill.drr_object;
		break;
	case DRR_WRITE_EMBEDDED:
		*obj = drr->drr_u.drr_write_embedded.drr_object;
		*off = drr->drr_u.drr_write_embedded.drr_offset;
		break;
	case DRR_OBJECT_RANGE:
		*obj = drr->drr_u.drr_object_range.drr_firstobj;
		break;
	case DRR_REDACT:
		*obj = drr->drr_u.drr_redact.drr_object;
		*off = drr->drr_u.drr_redact.drr_offset;
		break;
	default:
		break;
	}
}

/*
 * Returns B_TRUE if this record's {object, offset} is strictly before
 * the resume point.
 *
 * OBJECT, FREEOBJECTS, and OBJECT_RANGE records are metadata that
 * describe objects without a data offset.  They are only filtered by
 * object number, not offset.  An OBJECT record for resume_obj must
 * be included even when resume_off > 0 so the receive side knows
 * the object's metadata.
 */
static boolean_t
record_before_resume(dmu_replay_record_t *drr, uint64_t resume_obj,
    uint64_t resume_off)
{
	uint64_t obj, off;
	record_object_offset(drr, &obj, &off);

	switch (drr->drr_type) {
	case DRR_OBJECT:
	case DRR_OBJECT_RANGE:
	case DRR_FREEOBJECTS:
		return (obj < resume_obj);
	default:
		break;
	}

	return (obj < resume_obj ||
	    (obj == resume_obj && off < resume_off));
}

/*
 * Read DRR_BEGIN from the input stream, modify it to include resume
 * metadata, and write it to output. Also initializes zc with the
 * checksum of the modified header + payload.
 *
 * Returns 0 on success, nonzero on error.
 */
static int
modify_and_write_begin(FILE *infp, int outfd, uint64_t resume_obj,
    uint64_t resume_off, zio_cksum_t *zc)
{
	dmu_replay_record_t drr;
	if (sfread(&drr, sizeof (drr), infp) == 0) {
		(void) fprintf(stderr, "Error: empty or truncated stream\n");
		return (EINVAL);
	}
	if (drr.drr_type != DRR_BEGIN) {
		(void) fprintf(stderr, "Error: stream does not start with "
		    "DRR_BEGIN record\n");
		return (EINVAL);
	}

	struct drr_begin *drrb = &drr.drr_u.drr_begin;
	if (drrb->drr_magic != DMU_BACKUP_MAGIC) {
		(void) fprintf(stderr,
		    "Error: invalid send stream magic\n");
		return (EINVAL);
	}

	/* Read the nvlist payload from the input stream */
	uint64_t orig_payloadlen = drr.drr_payloadlen;
	void *payload = NULL;
	if (orig_payloadlen > 0) {
		payload = safe_malloc(orig_payloadlen);
		if (fread(payload, orig_payloadlen, 1, infp) != 1) {
			(void) fprintf(stderr,
			    "Error: failed to read BEGIN payload\n");
			free(payload);
			return (EINVAL);
		}
	}

	/* Unpack the nvlist */
	nvlist_t *nvl = NULL;
	int err;
	if (payload != NULL) {
		err = nvlist_unpack(payload, orig_payloadlen, &nvl, 0);
		free(payload);
		if (err != 0) {
			(void) fprintf(stderr,
			    "Error: failed to unpack BEGIN nvlist\n");
			return (EINVAL);
		}
	} else {
		nvl = fnvlist_alloc();
	}

	/* Set the RESUME feature flag */
	uint64_t featureflags = DMU_GET_FEATUREFLAGS(drrb->drr_versioninfo);
	featureflags |= DMU_BACKUP_FEATURE_RESUMING;
	DMU_SET_FEATUREFLAGS(drrb->drr_versioninfo, featureflags);

	/* Add resume point to nvlist */
	fnvlist_add_uint64(nvl, BEGINNV_RESUME_OBJECT, resume_obj);
	fnvlist_add_uint64(nvl, BEGINNV_RESUME_OFFSET, resume_off);

	/* Repack the nvlist */
	size_t new_payloadlen;
	char *new_payload = fnvlist_pack(nvl, &new_payloadlen);
	fnvlist_free(nvl);

	drr.drr_payloadlen = (uint32_t)new_payloadlen;

	/* Zero out checksum (DRR_BEGIN has zero checksum in the stream) */
	bzero(&drr.drr_u.drr_checksum.drr_checksum, sizeof (zio_cksum_t));

	/* Write the modified DRR_BEGIN */
	err = dump_record(&drr, new_payload, new_payloadlen, zc, outfd);
	fnvlist_pack_free(new_payload, new_payloadlen);

	if (err != 0) {
		(void) fprintf(stderr,
		    "Error: failed to write resume BEGIN record\n");
	}
	return (err);
}

int
zstream_do_resume(int argc, char *argv[])
{
	char *token = NULL;
	char *infile = NULL;
	int c;

	while ((c = getopt(argc, argv, "t:i:")) != -1) {
		switch (c) {
		case 't':
			token = optarg;
			break;
		case 'i':
			infile = optarg;
			break;
		default:
			resume_usage();
		}
	}

	if (token == NULL) {
		(void) fprintf(stderr,
		    "zstream resume: -t resume_token is required\n");
		resume_usage();
	}

	/* Parse the resume token to extract resume_object and resume_offset */
	libzfs_handle_t *hdl = libzfs_init();
	if (hdl == NULL) {
		(void) fprintf(stderr, "%s\n", libzfs_error_init(errno));
		return (1);
	}

	nvlist_t *resume_nvl = zfs_send_resume_token_to_nvlist(hdl, token);
	if (resume_nvl == NULL) {
		(void) fprintf(stderr, "Error: unable to parse resume "
		    "token: %s\n", libzfs_error_description(hdl));
		libzfs_fini(hdl);
		return (1);
	}

	uint64_t resume_obj, resume_off;
	if (nvlist_lookup_uint64(resume_nvl, "object", &resume_obj) != 0 ||
	    nvlist_lookup_uint64(resume_nvl, "offset", &resume_off) != 0) {
		(void) fprintf(stderr,
		    "Error: resume token missing object/offset fields\n");
		nvlist_free(resume_nvl);
		libzfs_fini(hdl);
		return (1);
	}
	nvlist_free(resume_nvl);

	/* Open the input stream */
	FILE *infp;
	if (infile != NULL) {
		infp = fopen(infile, "r");
		if (infp == NULL) {
			(void) fprintf(stderr,
			    "Error: cannot open %s: %s\n",
			    infile, strerror(errno));
			libzfs_fini(hdl);
			return (1);
		}
	} else {
		infp = stdin;
	}

	zio_cksum_t zc;
	bzero(&zc, sizeof (zc));
	int err = modify_and_write_begin(infp, STDOUT_FILENO,
	    resume_obj, resume_off, &zc);
	if (err != 0) {
		libzfs_fini(hdl);
		if (infile != NULL)
			fclose(infp);
		return (1);
	}

	/*
	 * Scan forward through the cached stream.  Skip records whose
	 * {object, offset} is before the resume point.  For records at
	 * or after the resume point, recompute checksums and write them
	 * to stdout.
	 */
	dmu_replay_record_t drr;
	int outfd = STDOUT_FILENO;

	for (;;) {
		/* Read one record header */
		int nread = sfread(&drr, sizeof (drr), infp);
		if (nread == 0)
			break;

		uint64_t psize = record_payload_size(&drr);
		void *buf = NULL;
		if (psize > 0) {
			buf = safe_malloc(psize);
			if (fread(buf, psize, 1, infp) != 1) {
				(void) fprintf(stderr,
				    "Error: failed to read record payload\n");
				free(buf);
				err = EINVAL;
				break;
			}
		}

		if (drr.drr_type == DRR_END) {
			/*
			 * Always write DRR_END regardless of position.
			 *
			 * The END record has two checksum fields at
			 * different offsets within the union:
			 * 1. drr_end.drr_checksum (offset 0 in union)
			 *    — running stream checksum of everything
			 *    before this record.  Compared against
			 *    drc_prev_cksum by the receive side.
			 * 2. drr_checksum.drr_checksum (at the end of
			 *    the union, via drr_pad[])
			 *    — per-record checksum including this END
			 *    header.  Verified by standard per-record
			 *    checksum logic.
			 *
			 * Both must be recomputed for our resume stream.
			 */
			drr.drr_u.drr_end.drr_checksum = zc;
			bzero(&drr.drr_u.drr_checksum.drr_checksum,
			    sizeof (zio_cksum_t));
			err = dump_record(&drr, buf, psize, &zc, outfd);
			free(buf);
			break;
		}

		if (!record_before_resume(&drr,
		    resume_obj, resume_off)) {
			/*
			 * Zero out the old checksum so dump_record writes the
			 * new one based on our running checksum context.
			 */
			bzero(&drr.drr_u.drr_checksum.drr_checksum,
			    sizeof (zio_cksum_t));
			err = dump_record(&drr, buf, psize, &zc, outfd);
			if (err != 0) {
				(void) fprintf(stderr,
				    "Error: failed to write record\n");
				free(buf);
				break;
			}
		}
		free(buf);
	}

	libzfs_fini(hdl);
	if (infile != NULL)
		fclose(infp);
	return (err != 0 ? 1 : 0);
}
