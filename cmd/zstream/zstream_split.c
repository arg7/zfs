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

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/zfs_ioctl.h>
#include <sys/zio_checksum.h>
#include <zfs_fletcher.h>
#include "zstream.h"
#include "zstream_util.h"

static void
split_usage(void)
{
	(void) fprintf(stderr,
	    "usage: zstream split [-i infile] -c SIZE [-o prefix]\n"
	    "\n"
	    "Split a send stream (from infile or stdin) into fixed-size\n"
	    "chunks that can be reassembled with 'zstream join'.\n"
	    "\n"
	    "Options:\n"
	    "  -i FILE    input stream (stdin if omitted)\n"
	    "  -c SIZE    max bytes per chunk, supports K/M/G/T/P/E suffixes\n"
	    "  -o PREFIX  output file prefix (default \"chunk\")\n"
	    "\n"
	    "Pipe example:\n"
	    "  zfs send pool/fs@s1 | zstream split -c 5M -o part\n"
	    "  zstream join -i part.000 part.001 | zfs receive pool/dst\n");
	exit(1);
}

/*
 * Read one record (header + payload) from infp into *drr_out / *buf_out.
 * On EOF returns 0, on error returns -1, on success returns 1.
 * Caller must free *buf_out on success.
 */
static int
read_one_record(FILE *infp, dmu_replay_record_t *drr_out,
    void **buf_out, uint64_t *psize_out)
{
	int nread = sfread(drr_out, sizeof (*drr_out), infp);
	if (nread == 0)
		return (0);

	uint64_t psize = record_payload_size(drr_out);
	*buf_out = NULL;
	*psize_out = psize;

	if (psize > 0) {
		*buf_out = safe_malloc(psize);
		if (fread(*buf_out, psize, 1, infp) != 1) {
			(void) fprintf(stderr,
			    "Error: failed to read record payload\n");
			free(*buf_out);
			*buf_out = NULL;
			return (-1);
		}
	}
	return (1);
}

/*
 * Write a single record to an output chunk.  The per-record checksum
 * is zeroed and recomputed into the running checksum zc.  The DRR_END
 * record is handled specially (its stream checksum field is updated).
 */
static int
write_one_record(dmu_replay_record_t *drr, void *buf, uint64_t psize,
    zio_cksum_t *zc, int outfd)
{
	if (drr->drr_type == DRR_END) {
		drr->drr_u.drr_end.drr_checksum = *zc;
		bzero(&drr->drr_u.drr_checksum.drr_checksum,
		    sizeof (zio_cksum_t));
	} else {
		bzero(&drr->drr_u.drr_checksum.drr_checksum,
		    sizeof (zio_cksum_t));
	}
	return (dump_record(drr, buf, psize, zc, outfd));
}

int
zstream_do_split(int argc, char *argv[])
{
	char *infile = NULL;
	uint64_t chunk_size = 0;
	const char *prefix = "chunk";
	int c;

	while ((c = getopt(argc, argv, "i:c:o:h")) != -1) {
		switch (c) {
		case 'i':
			infile = optarg;
			break;
		case 'c':
			chunk_size = parse_size_suffix(optarg);
			if (chunk_size == 0 && optarg[0] != '\0') {
				(void) fprintf(stderr,
				    "Error: invalid chunk size: %s\n",
				    optarg);
				split_usage();
			}
			break;
		case 'o':
			prefix = optarg;
			break;
		default:
			split_usage();
		}
	}

	if (chunk_size == 0) {
		(void) fprintf(stderr, "Error: -c SIZE is required\n");
		split_usage();
	}

	fletcher_4_init();

	FILE *fullfp;
	if (infile != NULL) {
		fullfp = fopen(infile, "rb");
		if (fullfp == NULL) {
			(void) fprintf(stderr,
			    "Error: cannot open %s: %s\n",
			    infile, strerror(errno));
			return (1);
		}
	} else {
		fullfp = stdin;
	}

	/* Read and save the DRR_BEGIN record (+ nvlist payload). */
	dmu_replay_record_t begin_drr;
	if (sfread(&begin_drr, sizeof (begin_drr), fullfp) == 0) {
		(void) fprintf(stderr,
		    "Error: empty or truncated stream\n");
		if (infile != NULL)
			fclose(fullfp);
		return (1);
	}
	if (begin_drr.drr_type != DRR_BEGIN) {
		(void) fprintf(stderr,
		    "Error: stream does not start with DRR_BEGIN\n");
		if (infile != NULL)
			fclose(fullfp);
		return (1);
	}
	if (begin_drr.drr_u.drr_begin.drr_magic != DMU_BACKUP_MAGIC) {
		(void) fprintf(stderr,
		    "Error: invalid send stream magic\n");
		if (infile != NULL)
			fclose(fullfp);
		return (1);
	}

	void *begin_payload = NULL;
	uint64_t begin_plen = begin_drr.drr_payloadlen;
	if (begin_plen > 0) {
		begin_payload = safe_malloc(begin_plen);
		if (fread(begin_payload, begin_plen, 1, fullfp) != 1) {
			(void) fprintf(stderr,
			    "Error: failed to read BEGIN payload\n");
			free(begin_payload);
			if (infile != NULL)
				fclose(fullfp);
			return (1);
		}
	}
	uint64_t begin_size = sizeof (begin_drr) + begin_plen;

	/*
	 * Every chunk gets a copy of the saved DRR_BEGIN at its
	 * start.  zstream join copies the first BEGIN (head) and
	 * skips subsequent ones (partials with skip_begin=1).
	 *
	 * Overflow buffer: on pipe input we can't fseek(SEEK_CUR)
	 * backwards when a record exceeds the chunk limit, so we
	 * stash it here and process it first in the next chunk.
	 */
	int have_stashed = 0;
	dmu_replay_record_t stashed_drr;
	void *stashed_buf = NULL;
	uint64_t stashed_psize = 0;

	int chunk_idx = 0;
	int seen_end = 0;

	while (!seen_end) {
		char outname[PATH_MAX];
		snprintf(outname, sizeof (outname), "%s.%03d",
		    prefix, chunk_idx);
		FILE *outfp = fopen(outname, "wb");
		if (outfp == NULL) {
			(void) fprintf(stderr,
			    "Error: cannot create %s: %s\n",
			    outname, strerror(errno));
			free(begin_payload);
			free(stashed_buf);
			if (infile != NULL)
				fclose(fullfp);
			return (1);
		}

		zio_cksum_t zc;
		bzero(&zc, sizeof (zc));
		uint64_t bytes_written = 0;
		int outfd = fileno(outfp);

		/* Write the saved DRR_BEGIN at the start of every chunk. */
		int ret = dump_record(&begin_drr, begin_payload,
		    begin_plen, &zc, outfd);
		if (ret != 0) {
			(void) fprintf(stderr,
			    "Error: failed to write BEGIN record\n");
			free(begin_payload);
			free(stashed_buf);
			if (infile != NULL)
				fclose(fullfp);
			fclose(outfp);
			return (1);
		}
		bytes_written += begin_size;

		/*
		 * If we stashed an overflowing record from the previous
		 * chunk, write it first.
		 */
		if (have_stashed) {
			have_stashed = 0;

			if (stashed_drr.drr_type == DRR_END) {
				ret = write_one_record(&stashed_drr, stashed_buf,
				    stashed_psize, &zc, outfd);
				free(stashed_buf);
				stashed_buf = NULL;
				fclose(outfp);
				if (ret != 0) {
					(void) fprintf(stderr,
					    "Error: failed to write END record\n");
					free(begin_payload);
					if (infile != NULL)
						fclose(fullfp);
					return (1);
				}
				seen_end = 1;
				break;
			}

			/*
			 * Always write the stashed record, even if it
			 * exceeds the chunk size.  Otherwise records
			 * larger than chunk_size would be stashed
			 * forever and never make progress.
			 */
			ret = write_one_record(&stashed_drr, stashed_buf,
			    stashed_psize, &zc, outfd);
			free(stashed_buf);
			stashed_buf = NULL;
			if (ret != 0) {
				(void) fprintf(stderr,
				    "Error: failed to write stashed record\n");
				free(begin_payload);
				if (infile != NULL)
					fclose(fullfp);
				fclose(outfp);
				return (1);
			}
			bytes_written += sizeof (stashed_drr) + stashed_psize;
		}

		/* Copy records until the size limit or end of stream. */
		for (;;) {
			dmu_replay_record_t drr;
			void *buf = NULL;
			uint64_t psize;

			int rc = read_one_record(fullfp, &drr, &buf, &psize);
			if (rc == 0) {
				seen_end = 1;
				break; /* EOF */
			}
			if (rc < 0) {
				free(begin_payload);
				free(stashed_buf);
				if (infile != NULL)
					fclose(fullfp);
				fclose(outfp);
				return (1);
			}

			if (drr.drr_type == DRR_END) {
				write_one_record(&drr, buf, psize,
				    &zc, outfd);
				free(buf);
				seen_end = 1;
				break;
			}

			uint64_t rec_size = sizeof (drr) + psize;

			/*
			 * Enforce chunk size.  When this is the first
			 * record in a chunk (only BEGIN written so far),
			 * write it unconditionally — otherwise records
			 * larger than chunk_size would never be written.
			 */
			if (bytes_written > begin_size &&
			    bytes_written + rec_size > chunk_size) {
				stashed_drr = drr;
				stashed_buf = buf;
				stashed_psize = psize;
				have_stashed = 1;
				break;
			}

			ret = write_one_record(&drr, buf, psize, &zc, outfd);
			free(buf);
			if (ret != 0) {
				(void) fprintf(stderr,
				    "Error: failed to write record\n");
				free(begin_payload);
				free(stashed_buf);
				if (infile != NULL)
					fclose(fullfp);
				fclose(outfp);
				return (1);
			}
			bytes_written += rec_size;
		}

		fclose(outfp);
		chunk_idx++;
	}

	free(begin_payload);
	free(stashed_buf);
	if (infile != NULL)
		fclose(fullfp);

	(void) fprintf(stderr,
	    "Split complete: %d chunk(s) written\n", chunk_idx);
	return (0);
}
