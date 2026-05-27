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
 * Copyright (c) 2020 by Delphix. All rights reserved.
 * Copyright (c) 2020 by Datto Inc. All rights reserved.
 * Copyright (c) 2026 CompEd Software Design srl.
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libintl.h>
#include <stddef.h>
#include <libzfs.h>
#include "zstream.h"
#include "zstream_util.h"

const char *zstream_resume_token = NULL;

void
zstream_usage(void)
{
	(void) fprintf(stderr,
	    "usage: zstream command args ...\n"
	    "Available commands are:\n"
	    "\n"
	    "\tzstream dump [-vCd] FILE\n"
	    "\t... | zstream dump [-vCd]\n"
	    "\n"
	    "\tzstream decompress [-v] [OBJECT,OFFSET[,TYPE]] ...\n"
	    "\n"
	    "\tzstream drop_record [-v] [OBJECT,OFFSET] ...\n"
	    "\n"
	    "\tzstream recompress [ -l level] TYPE\n"
	    "\n"
	    "\tzstream token resume_token\n"
	    "\tzstream token -g [-i FILE]\n"
	    "\n"
	    "\tzstream join [-i head] partial1 [partial2 ...]\n"
	    "\n"
	    "\tzstream resume -t resume_token [-i FILE]\n"
	    "\n"
	    "\tzstream split -i FILE -c SIZE [-o prefix]\n"
	    "\n"
	    "\tzstream redup [-v] FILE | ...\n"
	    "\n"
	    "Global flags (before subcommand):\n"
	    "  -t resume_token  fast-rewind input to resume point\n"
	    "  -i FILE          input file (required with -t)\n");
	exit(1);
}

int
main(int argc, char *argv[])
{
	char *basename = strrchr(argv[0], '/');
	basename = basename ? (basename + 1) : argv[0];
	if (argc >= 1 && strcmp(basename, "zstreamdump") == 0)
		return (zstream_do_dump(argc, argv));

	if (argc < 2)
		zstream_usage();

	/*
	 * Parse optional global -t/-i flags before the subcommand name.
	 * These set the shared zstream_resume_token for subcommands
	 * that support fast-forwarding to a resume point.
	 */
	char *token = NULL;
	char *infile = NULL;
	int ci = 1;
	while (ci < argc) {
		if (strcmp(argv[ci], "--") == 0) {
			ci++;
			break;
		}
		if (argv[ci][0] != '-')
			break;
		if (strcmp(argv[ci], "-t") == 0 && ci + 1 < argc) {
			token = argv[ci + 1];
			ci += 2;
		} else if (strcmp(argv[ci], "-i") == 0 && ci + 1 < argc) {
			infile = argv[ci + 1];
			ci += 2;
		} else {
			break;
		}
	}
	if (ci >= argc)
		zstream_usage();

	zstream_resume_token = token;
	(void) infile;

	char *subcommand = argv[ci];
	int sub_argc = argc - ci;
	char **sub_argv = argv + ci;

	if (strcmp(subcommand, "dump") == 0) {
		return (zstream_do_dump(sub_argc, sub_argv));
	} else if (strcmp(subcommand, "decompress") == 0) {
		return (zstream_do_decompress(sub_argc, sub_argv));
	} else if (strcmp(subcommand, "drop_record") == 0) {
		return (zstream_do_drop_record(sub_argc, sub_argv));
	} else if (strcmp(subcommand, "recompress") == 0) {
		return (zstream_do_recompress(sub_argc, sub_argv));
	} else if (strcmp(subcommand, "token") == 0) {
		return (zstream_do_token(sub_argc, sub_argv));
	} else if (strcmp(subcommand, "join") == 0) {
		return (zstream_do_join(sub_argc, sub_argv));
	} else if (strcmp(subcommand, "resume") == 0) {
		return (zstream_do_resume(sub_argc, sub_argv));
	} else if (strcmp(subcommand, "split") == 0) {
		return (zstream_do_split(sub_argc, sub_argv));
	} else if (strcmp(subcommand, "redup") == 0) {
		return (zstream_do_redup(sub_argc, sub_argv));
	} else {
		zstream_usage();
	}
}
