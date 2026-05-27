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
#include <zfs_fletcher.h>
#include "zstream.h"
#include "zstream_util.h"

static void
resume_usage(void)
{
	(void) fprintf(stderr,
	    "usage: zstream resume -t resume_token [-i infile] "
	    "[-o offset] [-c size] [-S] [-H header]\n"
	    "\n"
	    "Read a cached send stream from infile (or stdin),\n"
	    "generate a resume stream at the point specified by\n"
	    "the resume token, and write it to stdout.\n"
	    "\n"
	    "Options:\n"
	    "  -t TOKEN   resume token (required)\n"
	    "  -i FILE    input stream file (default: stdin)\n"
	    "  -o OFFSET  explicit stream byte offset (supports K/M/G/T/P/E)\n"
	    "  -c SIZE    cut output after SIZE bytes (supports K/M/G/T/P/E)\n"
	    "  -S         skip-seek: do not use stream_offset from token\n"
	    "  -H HEADER  DRR_BEGIN header source (file path or\n"
	    "             data:application/octet-stream;base64,<b64>)\n");
	exit(1);
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
 * Read a DRR_BEGIN record from a header source.
 * HEADER can be a file path or "data:application/octet-stream;base64,<b64>".
 * Returns 0 on success, nonzero on error.
 */
static int
read_header(const char *header, dmu_replay_record_t *drr_out,
    void **buf_out, uint64_t *psize_out)
{
	if (header == NULL)
		return (-1);

	/* Check for base64 data: URI */
	if (strncmp(header, "data:application/octet-stream;base64,",
	    sizeof("data:application/octet-stream;base64,") - 1) == 0) {
		const char *b64 = header +
		    (sizeof("data:application/octet-stream;base64,") - 1);

		/* Decode base64 to find output size */
		size_t b64_len = strlen(b64);
		unsigned char *decoded = safe_malloc(b64_len);
		size_t dec_len = 0;

		/* Simple base64 decode */
		static const char b64_table[256] = {
			['A']=0,['B']=1,['C']=2,['D']=3,['E']=4,['F']=5,['G']=6,['H']=7,
			['I']=8,['J']=9,['K']=10,['L']=11,['M']=12,['N']=13,['O']=14,['P']=15,
			['Q']=16,['R']=17,['S']=18,['T']=19,['U']=20,['V']=21,['W']=22,['X']=23,
			['Y']=24,['Z']=25,['a']=26,['b']=27,['c']=28,['d']=29,['e']=30,['f']=31,
			['g']=32,['h']=33,['i']=34,['j']=35,['k']=36,['l']=37,['m']=38,['n']=39,
			['o']=40,['p']=41,['q']=42,['r']=43,['s']=44,['t']=45,['u']=46,['v']=47,
			['w']=48,['x']=49,['y']=50,['z']=51,['0']=52,['1']=53,['2']=54,['3']=55,
			['4']=56,['5']=57,['6']=58,['7']=59,['8']=60,['9']=61,['+']=62,['/']=63
		};

		size_t i = 0;
		while (b64[i] && b64[i] != '=') {
			uint32_t val = 0;
			int valid = 0;
			for (int j = 0; j < 4 && b64[i] && b64[i] != '='; j++, i++) {
				val = (val << 6) | (b64_table[(unsigned char)b64[i]] & 0x3f);
				valid++;
			}
			if (valid >= 2) {
				decoded[dec_len++] = (val >> 16) & 0xff;
			}
			if (valid >= 3) {
				decoded[dec_len++] = (val >> 8) & 0xff;
			}
			if (valid >= 4) {
				decoded[dec_len++] = val & 0xff;
			}
		}

		/* Read the DRR_BEGIN from decoded data */
		memcpy(drr_out, decoded, sizeof(*drr_out));
		*psize_out = record_payload_size(drr_out);
		if (*psize_out > 0) {
			*buf_out = safe_malloc(*psize_out);
			if (dec_len < sizeof(*drr_out) + *psize_out) {
				uint64_t payload_start = sizeof(*drr_out);
				uint64_t payload_avail = dec_len > payload_start ?
				    dec_len - payload_start : 0;
				if (payload_avail > *psize_out)
					payload_avail = *psize_out;
				memcpy(*buf_out, decoded + payload_start, payload_avail);
				bzero((char*)*buf_out + payload_avail,
				    *psize_out - payload_avail);
			} else {
				memcpy(*buf_out, decoded + sizeof(*drr_out),
				    *psize_out);
			}
		} else {
			*buf_out = NULL;
		}
		free(decoded);
		return (0);
	}

	/* Treat as file path */
	FILE *hf = fopen(header, "rb");
	if (hf == NULL) {
		(void) fprintf(stderr,
		    "Error: cannot open header file %s: %s\n",
		    header, strerror(errno));
		return (-1);
	}

	size_t nread = fread(drr_out, sizeof(*drr_out), 1, hf);
	fclose(hf);
	if (nread != 1) {
		(void) fprintf(stderr,
		    "Error: failed to read DRR_BEGIN from %s\n", header);
		return (-1);
	}

	*psize_out = record_payload_size(drr_out);
	if (*psize_out > 0) {
		*buf_out = safe_malloc(*psize_out);
		hf = fopen(header, "rb");
		fseek(hf, sizeof(*drr_out), SEEK_SET);
		nread = fread(*buf_out, *psize_out, 1, hf);
		fclose(hf);
		if (nread != 1) {
			(void) fprintf(stderr,
			    "Error: failed to read header payload from %s\n",
			    header);
			free(*buf_out);
			*buf_out = NULL;
			*psize_out = 0;
			return (-1);
		}
	} else {
		*buf_out = NULL;
	}

	return (0);
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
	char *offset_opt = NULL;
	char *cut_opt = NULL;
	int c;
	int skip_seek = 0;
	char *header_file = NULL;
	int err = 0;

	fletcher_4_init();

	while ((c = getopt(argc, argv, "t:i:o:c:SH:")) != -1) {
		switch (c) {
		case 't':
			token = optarg;
			break;
		case 'i':
			infile = optarg;
			break;
		case 'o':
			offset_opt = optarg;
			break;
		case 'c':
			cut_opt = optarg;
			break;
		case 'S':
			skip_seek = 1;
			break;
		case 'H':
			header_file = optarg;
			break;
		default:
			resume_usage();
		}
	}

	/* Use global token from zstream -t if not provided locally */
	if (token == NULL)
		token = (char *)zstream_resume_token;

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

	uint64_t stream_offset = 0;

	if (offset_opt != NULL) {
		stream_offset = parse_size_suffix(offset_opt);
		if (stream_offset == 0 && offset_opt[0] != '\0') {
			(void) fprintf(stderr,
			    "Error: invalid offset value: %s\n", offset_opt);
			libzfs_fini(hdl);
			return (1);
		}
	} else {
		stream_offset = token_stream_offset(token);
	}

	/* Parse cut limit (-c SIZE) if provided */
	uint64_t cut_bytes = 0;
	if (cut_opt != NULL) {
		cut_bytes = parse_size_suffix(cut_opt);
		if (cut_bytes == 0 && cut_opt[0] != '\0') {
			(void) fprintf(stderr,
			    "Error: invalid cut size: %s\n", cut_opt);
			libzfs_fini(hdl);
			return (1);
		}
	}

	/* Open the input stream */
	FILE *infp;
	if (infile != NULL) {
		infp = fopen(infile, "rb");
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

	/*
	 * Write the DRR_BEGIN record.  If -H is given, read the header
	 * from the header source (file or base64 data URI) and write it
	 * with resume metadata patched in.  Otherwise read it from the
	 * input stream as usual.
	 */
	if (header_file != NULL) {
		/* -H given: read header from header_file, write with resume */
		dmu_replay_record_t hdrr;
		void *hdr_buf = NULL;
		uint64_t hdr_psize = 0;
		if (read_header(header_file, &hdrr, &hdr_buf, &hdr_psize) != 0) {
			(void) fprintf(stderr,
			    "Error: failed to read header from %s\n",
			    header_file);
			libzfs_fini(hdl);
			if (infile != NULL)
				fclose(infp);
			return (1);
		}
		if (hdrr.drr_type != DRR_BEGIN) {
			(void) fprintf(stderr,
			    "Error: header does not contain DRR_BEGIN\n");
			free(hdr_buf);
			libzfs_fini(hdl);
			if (infile != NULL)
				fclose(infp);
			return (1);
		}
		/* Unpack the nvlist payload from the header */
		nvlist_t *nvl = NULL;
		if (hdr_psize > 0) {
			err = nvlist_unpack(hdr_buf, hdr_psize, &nvl, 0);
			if (err != 0) {
				(void) fprintf(stderr,
				    "Error: failed to unpack "
				    "BEGIN nvlist from header\n");
				free(hdr_buf);
				libzfs_fini(hdl);
				if (infile != NULL)
					fclose(infp);
				return (1);
			}
		} else {
			nvl = fnvlist_alloc();
		}
		/* Set the RESUME feature flag */
		struct drr_begin *drrb = &hdrr.drr_u.drr_begin;
		uint64_t featureflags =
		    DMU_GET_FEATUREFLAGS(drrb->drr_versioninfo);
		featureflags |= DMU_BACKUP_FEATURE_RESUMING;
		DMU_SET_FEATUREFLAGS(drrb->drr_versioninfo, featureflags);
		/* Add resume point to nvlist */
		fnvlist_add_uint64(nvl, BEGINNV_RESUME_OBJECT, resume_obj);
		fnvlist_add_uint64(nvl, BEGINNV_RESUME_OFFSET, resume_off);
		/* Repack the nvlist */
		size_t new_nvlist_len;
		char *new_nvlist = fnvlist_pack(nvl, &new_nvlist_len);
		fnvlist_free(nvl);
		/* Update header payload length */
		hdrr.drr_payloadlen = new_nvlist_len;
		/* Zero out checksum (DRR_BEGIN has zero checksum in stream) */
		bzero(&hdrr.drr_u.drr_checksum.drr_checksum,
		    sizeof (zio_cksum_t));
		/* Update running checksum like dump_record does for DRR_BEGIN */
		fletcher_4_incremental_native(&hdrr,
		    offsetof(dmu_replay_record_t,
		    drr_u.drr_checksum.drr_checksum), &zc);
		fletcher_4_incremental_native(&hdrr.drr_u.drr_checksum.drr_checksum,
		    sizeof (zio_cksum_t), &zc);
		/* Write header */
		if (write(STDOUT_FILENO, &hdrr, sizeof (hdrr)) == -1) {
			(void) fprintf(stderr, "Error: write failed\n");
			fnvlist_pack_free(new_nvlist, new_nvlist_len);
			free(hdr_buf);
			libzfs_fini(hdl);
			if (infile != NULL)
				fclose(infp);
			return (1);
		}
		/* Write nvlist payload */
		if (new_nvlist_len > 0) {
			if (write(STDOUT_FILENO, new_nvlist,
			    new_nvlist_len) == -1) {
				(void) fprintf(stderr, "Error: write failed\n");
				fnvlist_pack_free(new_nvlist, new_nvlist_len);
				free(hdr_buf);
				libzfs_fini(hdl);
				if (infile != NULL)
					fclose(infp);
				return (1);
			}
			fletcher_4_incremental_native(new_nvlist,
			    new_nvlist_len, &zc);
		}
		fnvlist_pack_free(new_nvlist, new_nvlist_len);
		free(hdr_buf);
	} else {
		/* No -H: read header from input stream as usual */
		err = modify_and_write_begin(infp, STDOUT_FILENO,
		    resume_obj, resume_off, &zc);
		if (err != 0) {
			libzfs_fini(hdl);
			if (infile != NULL)
				fclose(infp);
			return (1);
		}
	}

	/*
	 * Scan forward through the cached stream.  Skip records whose
	 * {object, offset} is before the resume point.  For records at
	 * or after the resume point, recompute checksums and write them
	 * to stdout.
	 */
	dmu_replay_record_t drr;
	int outfd = STDOUT_FILENO;
	uint64_t bytes_written = 0;

	/*
	 * If -S (skip-seek) is not set and the token contains a stream
	 * byte offset, seek directly to that position in the cached
	 * stream.  The offset points to the start of the resume-point
	 * record's header.  Verify the record at that position matches
	 * resume_obj/resume_off; fall back to a full scan on mismatch.
	 */
	if (!skip_seek && stream_offset > 0 && infile != NULL) {
		off_t curpos = ftello(infp);
		if (curpos >= 0 && (uint64_t)curpos < stream_offset) {
			(void) fseeko(infp, (off_t)stream_offset, SEEK_SET);

			dmu_replay_record_t probe;
			if (sfread(&probe, sizeof (probe), infp) != 0) {
				uint64_t obj, off;
				record_object_offset(&probe, &obj, &off);
				uint64_t probe_payload = record_payload_size(&probe);
				if (probe_payload > 0)
					fseeko(infp, (off_t)probe_payload,
					    SEEK_CUR);
				if (obj != resume_obj || off != resume_off) {
					(void) fprintf(stderr,
					    "Warning: stream offset 0x%llx "
					    "does not match resume point "
					    "(object=%llu offset=%llu), "
					    "falling back to full scan\n",
					    (unsigned long long)stream_offset,
					    (unsigned long long)resume_obj,
					    (unsigned long long)resume_off);
					(void) fseeko(infp, curpos, SEEK_SET);
				} else {
					(void) fseeko(infp,
					    (off_t)stream_offset, SEEK_SET);
				}
			} else {
				(void) fprintf(stderr,
				    "Warning: cannot read record at stream "
				    "offset 0x%llx, "
				    "falling back to full scan\n",
				    (unsigned long long)stream_offset);
				(void) fseeko(infp, curpos, SEEK_SET);
			}
		}
	}

	for (;;) {
		/*
		 * If a byte limit is set (-c), stop before reading the next
		 * record once we've reached it.  Don't write DRR_END here —
		 * the caller will continue with another resume partial.
		 */
		if (cut_bytes > 0 && bytes_written >= cut_bytes)
			break;
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
			bytes_written += sizeof (drr) + psize;
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
			bytes_written += sizeof (drr) + psize;
		}
		free(buf);
	}

	libzfs_fini(hdl);
	if (infile != NULL)
		fclose(infp);
	return (err != 0 ? 1 : 0);
}
