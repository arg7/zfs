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
#include <fcntl.h>
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
join_usage(void)
{
	(void) fprintf(stderr,
	    "usage: zstream join [-i head] [-m] partial1 [partial2 ...]\n"
	    "\n"
	    "Join a send stream head fragment with one or more partial\n"
	    "resume streams into a single output stream.\n"
	    "\n"
	    "Options:\n"
	    "  -i head    head fragment (required unless first positional arg)\n"
	    "  -m         merge partials in-place into the head file\n"
	    "             (head must have a clean tail, i.e. no mid-record\n"
	    "             truncation; incompatible with -i)\n"
	    "\n"
	    "Exit codes:\n"
	    "  0  stream is complete (DRR_END written)\n"
	    "  1  error\n"
	    "  2  stream is still incomplete (no DRR_END)\n"
	    "\n"
	    "Example:\n"
	    "  zstream join -i head.zfs resume1.zfs resume2.zfs \\\n"
	    "      > full_stream.zfs\n"
	    "  zstream join --merge 0000.zfs 0001.zfs 0002.zfs\n");
	exit(1);
}

/*
 * Write a fresh DRR_END record with the final checksum zc to outfd.
 */
static int
write_end_record(zio_cksum_t *zc, int outfd)
{
	dmu_replay_record_t drr;
	bzero(&drr, sizeof (drr));
	drr.drr_type = DRR_END;
	drr.drr_u.drr_end.drr_checksum = *zc;
	bzero(&drr.drr_u.drr_checksum.drr_checksum,
	    sizeof (zio_cksum_t));

	return (dump_record(&drr, NULL, 0, zc, outfd));
}

/* Forward declaration for merge path */
static int zstream_do_join_merge(int argc, char *argv[]);

int
zstream_do_join(int argc, char *argv[])
{
	char *headfile = NULL;
	int merge = 0;
	int c;

	while ((c = getopt(argc, argv, "i:mh")) != -1) {
		switch (c) {
		case 'i':
			headfile = optarg;
			break;
		case 'm':
			merge = 1;
			break;
		default:
			join_usage();
		}
	}

	argc -= optind;
	argv += optind;

	if (merge) {
		if (headfile != NULL) {
			(void) fprintf(stderr,
			    "Error: -i and -m are mutually exclusive\n");
			return (1);
		}
		if (argc < 1) {
			(void) fprintf(stderr,
			    "Error: --merge requires a head file\n");
			return (1);
		}
		return (zstream_do_join_merge(argc, argv));
	}

	fletcher_4_init();

	FILE *headfp;
	if (headfile != NULL) {
		headfp = fopen(headfile, "rb");
		if (headfp == NULL) {
			(void) fprintf(stderr,
			    "Error: cannot open head %s: %s\n",
			    headfile, strerror(errno));
			return (1);
		}
	} else if (argc > 0) {
		headfile = argv[0];
		headfp = fopen(headfile, "rb");
		if (headfp == NULL) {
			(void) fprintf(stderr,
			    "Error: cannot open head %s: %s\n",
			    headfile, strerror(errno));
			return (1);
		}
		argc--;
		argv++;
	} else {
		headfp = stdin;
	}

	zio_cksum_t zc;
	bzero(&zc, sizeof (zc));

	int ret = stream_copy_records(headfp, STDOUT_FILENO, &zc, 0);

	if (ret < 0) {
		if (headfile != NULL)
			fclose(headfp);
		(void) fprintf(stderr,
		    "Error: failed to process head fragment\n");
		return (1);
	}

	/*
	 * Validate the head fragment starts with DRR_BEGIN.
	 * If the file was empty or contained only invalid data,
	 * treat it as an error (not an incomplete stream).
	 */
	if (ret == 0) {
		rewind(headfp);
		dmu_replay_record_t drr;
		if (fread(&drr, sizeof (drr), 1, headfp) != 1) {
			if (headfile != NULL)
				fclose(headfp);
			(void) fprintf(stderr,
			    "Error: head fragment is empty or invalid\n");
			return (1);
		}
		if (drr.drr_type != DRR_BEGIN) {
			if (headfile != NULL)
				fclose(headfp);
			(void) fprintf(stderr,
			    "Error: head fragment does not start with DRR_BEGIN\n");
			return (1);
		}
	}

	if (headfile != NULL)
		fclose(headfp);
	if (ret == 1) {
		if (write_end_record(&zc, STDOUT_FILENO) != 0) {
			(void) fprintf(stderr,
			    "Error: failed to write DRR_END\n");
			return (1);
		}
		return (0);
	}

	/* Process partials */
	int seen_end = 0;

	for (int i = 0; i < argc; i++) {
		FILE *pfp = fopen(argv[i], "rb");
		if (pfp == NULL) {
			(void) fprintf(stderr,
			    "Error: cannot open partial %s: %s\n",
			    argv[i], strerror(errno));
			return (1);
		}

		ret = stream_copy_records(pfp, STDOUT_FILENO, &zc, 1);
		fclose(pfp);

		if (ret < 0) {
			(void) fprintf(stderr,
			    "Error: failed to process partial %s\n",
			    argv[i]);
			return (1);
		}
		if (ret == 1)
			seen_end = 1;
	}

	if (seen_end) {
		if (write_end_record(&zc, STDOUT_FILENO) != 0) {
			(void) fprintf(stderr,
			    "Error: failed to write DRR_END\n");
			return (1);
		}
		return (0);
	}

	(void) fprintf(stderr,
	    "Warning: no DRR_END in fragments, stream is incomplete\n");
	return (2);
}

/*
 * In-place merge: append partials into the head file.
 * The head file is validated for a clean tail first.
 */
static int
zstream_do_join_merge(int argc, char *argv[])
{
	char *headfile = argv[0];

	argc--;
	argv++;

	fletcher_4_init();

	/* Phase 1: validate head and accumulate checksum */
	FILE *headfp = fopen(headfile, "rb");
	if (headfp == NULL) {
		(void) fprintf(stderr,
		    "Error: cannot open head %s: %s\n",
		    headfile, strerror(errno));
		return (1);
	}

	/* Ensure the head starts with DRR_BEGIN */
	dmu_replay_record_t first_drr;
	if (fread(&first_drr, sizeof (first_drr), 1, headfp) != 1 ||
	    first_drr.drr_type != DRR_BEGIN) {
		(void) fprintf(stderr,
		    "Error: head %s does not start with DRR_BEGIN\n",
		    headfile);
		fclose(headfp);
		return (1);
	}

	zio_cksum_t zc;
	bzero(&zc, sizeof (zc));

	/* O(1) tail validation: reads only the last record */
	int ret = stream_validate_tail_fast(headfp, &zc);
	fclose(headfp);

	switch (ret) {
	case -2:
		(void) fprintf(stderr,
		    "Error: head %s has a dirty tail "
		    "(truncated mid-record), cannot merge\n", headfile);
		return (1);
	case -1:
		(void) fprintf(stderr,
		    "Error: failed to validate head %s\n", headfile);
		return (1);
	case 1:
		if (argc == 0) {
			(void) fprintf(stderr,
			    "Info: head %s is already complete "
			    "(DRR_END present), nothing to merge\n",
			    headfile);
			return (0);
		}
		/*
		 * Head is complete but partials were given.
		 * Continue with appending — the tail is clean
		 * (ends on a record boundary), so we can
		 * concatenate.
		 */
		break;
	case 0:
		/* Clean EOF, no DRR_END — proceed with merge. */
		break;
	}

	/* Phase 2: open head for appending and merge partials */
	int head_fd = open(headfile, O_WRONLY | O_APPEND);
	if (head_fd < 0) {
		(void) fprintf(stderr,
		    "Error: cannot open %s for appending: %s\n",
		    headfile, strerror(errno));
		return (1);
	}

	int seen_end = 0;

	for (int i = 0; i < argc; i++) {
		FILE *pfp = fopen(argv[i], "rb");
		if (pfp == NULL) {
			(void) fprintf(stderr,
			    "Error: cannot open partial %s: %s\n",
			    argv[i], strerror(errno));
			close(head_fd);
			return (1);
		}

		ret = stream_copy_records(pfp, head_fd, &zc, 1);
		fclose(pfp);

		if (ret < 0) {
			(void) fprintf(stderr,
			    "Error: failed to process partial %s\n",
			    argv[i]);
			close(head_fd);
			return (1);
		}
		if (ret == 1)
			seen_end = 1;
	}

	close(head_fd);

	if (seen_end) {
		/* Reopen to write final DRR_END */
		head_fd = open(headfile, O_WRONLY | O_APPEND);
		if (head_fd < 0) {
			(void) fprintf(stderr,
			    "Error: cannot reopen %s for writing: %s\n",
			    headfile, strerror(errno));
			return (1);
		}
		if (write_end_record(&zc, head_fd) != 0) {
			(void) fprintf(stderr,
			    "Error: failed to write DRR_END\n");
			close(head_fd);
			return (1);
		}
		close(head_fd);
		return (0);
	}

	(void) fprintf(stderr,
	    "Warning: no DRR_END in fragments, stream is incomplete\n");
	return (2);
}
