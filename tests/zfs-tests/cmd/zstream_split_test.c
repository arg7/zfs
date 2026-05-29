// SPDX-License-Identifier: CDDL-1.0
/*
 * Unit tests for zstream split/join round-trip integrity.
 *
 * Constructs synthetic send streams, splits them into chunks
 * using the zstream binary, joins them back, and verifies
 * byte-identical determinism across two passes.
 * No ZFS pool or root privileges required.
 *
 * Strategy:
 *  1. Build a synthetic send stream S (BEGIN + n*WRITE + END).
 *  2. Split S -> chunks, then Join chunks -> N (first pass,
 *     normalizes checksums).
 *  3. Split N -> chunks, then Join chunks -> T (second pass).
 *  4. Assert N == T (split/join is deterministic).
 *  5. Assert each chunk starts with DRR_BEGIN (record type 0).
 *
 * The test uses system() to invoke "zstream" for split and join;
 * it only uses ZFS headers for record structures.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>
#include <sys/zfs_ioctl.h>

/* ---------- test infrastructure ---------- */

static int tests_run = 0;
static int tests_failed = 0;
static char testdir[] = "/tmp/zsttest_XXXXXX";

#define	TEST(name) \
	do { \
		tests_run++; \
		printf("  %-50s", name); \
		fflush(stdout); \
	} while (0)

#define	FAIL(fmt, ...) \
	do { \
		printf("  FAIL\n    " fmt "\n", ##__VA_ARGS__); \
		tests_failed++; \
		return; \
	} while (0)

static void *
xm(size_t n)
{
	void *p = malloc(n);
	if (p == NULL) {
		(void) fprintf(stderr, "FATAL: malloc(%zu) failed\n", n);
		exit(1);
	}
	return (p);
}

/* ---------- synthetic send stream ---------- */

#define	DUMMY_PAYLOAD_SIZE 128
static const unsigned char dummy_payload[DUMMY_PAYLOAD_SIZE] = {
	0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static void
make_stream(const char *path, int nrecs, int rec_size, size_t *out_size)
{
	FILE *fp = fopen(path, "wb");
	if (fp == NULL) {
		(void) fprintf(stderr, "FATAL: fopen(%s): %s\n",
		    path, strerror(errno));
		exit(1);
	}

	dmu_replay_record_t drr;
	bzero(&drr, sizeof (drr));
	drr.drr_type = DRR_BEGIN;
	drr.drr_u.drr_begin.drr_magic = DMU_BACKUP_MAGIC;
	drr.drr_u.drr_begin.drr_versioninfo = 0;
	drr.drr_u.drr_begin.drr_creation_time = 0;
	drr.drr_u.drr_begin.drr_type = DMU_OST_ZFS;
	drr.drr_u.drr_begin.drr_flags = 0;
	drr.drr_u.drr_begin.drr_toguid = 1;
	drr.drr_u.drr_begin.drr_fromguid = 0;
	drr.drr_payloadlen = DUMMY_PAYLOAD_SIZE;
	strncpy(drr.drr_u.drr_begin.drr_toname, "pool/test@snap",
	    sizeof (drr.drr_u.drr_begin.drr_toname) - 1);

	(void) fwrite(&drr, sizeof (drr), 1, fp);
	(void) fwrite(dummy_payload, DUMMY_PAYLOAD_SIZE, 1, fp);
	size_t total = sizeof (drr) + DUMMY_PAYLOAD_SIZE;

	for (int i = 0; i < nrecs; i++) {
		bzero(&drr, sizeof (drr));
		drr.drr_type = DRR_WRITE;
		drr.drr_u.drr_write.drr_object = 1;
		drr.drr_u.drr_write.drr_type = DMU_OT_PLAIN_FILE_CONTENTS;
		drr.drr_u.drr_write.drr_offset =
		    (uint64_t)i * (uint64_t)rec_size;
		drr.drr_u.drr_write.drr_logical_size = (uint64_t)rec_size;
		drr.drr_u.drr_write.drr_toguid = 1;
		drr.drr_u.drr_write.drr_checksumtype = ZIO_CHECKSUM_OFF;
		drr.drr_u.drr_write.drr_compressiontype = 0;

		(void) fwrite(&drr, sizeof (drr), 1, fp);
		total += sizeof (drr);

		if (rec_size > 0) {
			char *data = xm(rec_size);
			memset(data, (int)((i & 0x3F) ^ 0x5A), rec_size);
			(void) fwrite(data, rec_size, 1, fp);
			total += (size_t)rec_size;
			free(data);
		}
	}

	bzero(&drr, sizeof (drr));
	drr.drr_type = DRR_END;
	(void) fwrite(&drr, sizeof (drr), 1, fp);
	total += sizeof (drr);

	(void) fclose(fp);
	*out_size = total;
}

/* ---------- helpers ---------- */

static int
count_chunks(const char *prefix)
{
	for (int i = 0; i < 9999; i++) {
		char name[1024];
		snprintf(name, sizeof (name), "%s.%03d", prefix, i);
		if (access(name, F_OK) != 0)
			return (i);
	}
	return (-1);
}

static void
cleanup_chunks(const char *prefix, int nchunks)
{
	for (int i = 0; i < nchunks; i++) {
		char name[1024];
		snprintf(name, sizeof (name), "%s.%03d", prefix, i);
		(void) unlink(name);
	}
}

static int
files_equal(const char *a, const char *b)
{
	struct stat sa, sb;
	if (stat(a, &sa) != 0 || stat(b, &sb) != 0)
		return (0);
	if (sa.st_size != sb.st_size)
		return (0);

	FILE *fa = fopen(a, "rb");
	FILE *fb = fopen(b, "rb");
	if (fa == NULL || fb == NULL) {
		if (fa != NULL) (void) fclose(fa);
		if (fb != NULL) (void) fclose(fb);
		return (0);
	}

	char bufa[65536], bufb[65536];
	int match = 1;
	size_t na;
	while ((na = fread(bufa, 1, sizeof (bufa), fa)) > 0) {
		size_t nb = fread(bufb, 1, sizeof (bufb), fb);
		if (na != nb || memcmp(bufa, bufb, na) != 0) {
			match = 0;
			break;
		}
	}
	(void) fclose(fa);
	(void) fclose(fb);
	return (match);
}

static const char *
zstream_bin(void)
{
	static const char *cached = NULL;
	if (cached != NULL)
		return (cached);

	const char *env = getenv("ZSTREAM_BIN");
	if (env != NULL) {
		cached = env;
		return (cached);
	}
	if (access(".libs/zstream", X_OK) == 0) {
		cached = ".libs/zstream";
		return (cached);
	}
	cached = "zstream";
	return (cached);
}

static int
run_split(const char *infile, uint64_t chunk_size, const char *prefix)
{
	char cmd[4096];
	snprintf(cmd, sizeof (cmd),
	    "%s split -i '%s' -c %lu -o '%s' 2>/dev/null",
	    zstream_bin(), infile, (unsigned long)chunk_size, prefix);
	int rc = system(cmd);
	if (rc == -1)
		return (-1);
	if (WIFEXITED(rc))
		return (WEXITSTATUS(rc));
	return (-1);
}

static int
run_join(const char *outfile, int nchunks, const char *prefix)
{
	char cmd[16384];
	int off = snprintf(cmd, sizeof (cmd),
	    "%s join -i '%s.%03d'", zstream_bin(), prefix, 0);
	for (int i = 1; i < nchunks; i++)
		off += snprintf(cmd + off, sizeof (cmd) - off,
		    " '%s.%03d'", prefix, i);
	snprintf(cmd + off, sizeof (cmd) - off,
	    " > '%s' 2>/dev/null", outfile);
	int rc = system(cmd);
	if (rc == -1)
		return (-1);
	if (WIFEXITED(rc))
		return (WEXITSTATUS(rc));
	return (-1);
}

static int
split_and_join(const char *infile, const char *outfile,
    const char *prefix, uint64_t chunk_size)
{
	int rc = run_split(infile, chunk_size, prefix);
	if (rc != 0) {
		(void) fprintf(stderr, "  split returned %d\n", rc);
		return (-1);
	}
	int nchunks = count_chunks(prefix);
	if (nchunks <= 0) {
		(void) fprintf(stderr, "  no chunks produced\n");
		return (-1);
	}
	rc = run_join(outfile, nchunks, prefix);
	cleanup_chunks(prefix, nchunks);
	if (rc != 0) {
		(void) fprintf(stderr, "  join returned %d\n", rc);
		return (-1);
	}
	return (nchunks);
}

static int
chunks_start_with_begin(const char *prefix, int nchunks)
{
	for (int i = 0; i < nchunks; i++) {
		char name[1024];
		snprintf(name, sizeof (name), "%s.%03d", prefix, i);

		FILE *fp = fopen(name, "rb");
		if (fp == NULL)
			return (0);

		unsigned char t[4];
		if (fread(t, 1, 4, fp) != 4) {
			(void) fclose(fp);
			return (0);
		}
		(void) fclose(fp);

		uint32_t rt = ((uint32_t)t[3] << 24) |
		    ((uint32_t)t[2] << 16) | ((uint32_t)t[1] << 8) |
		    (uint32_t)t[0];
		if (rt != 0)
			return (0);
	}
	return (1);
}

/* ---------- merge helpers ---------- */

static int
run_merge(int *exitcode, const char *headfile, int npartials,
    const char **partials)
{
	char cmd[16384];
	int off = snprintf(cmd, sizeof (cmd),
	    "%s join -m '%s'", zstream_bin(), headfile);
	for (int i = 0; i < npartials; i++)
		off += snprintf(cmd + off, sizeof (cmd) - off,
		    " '%s'", partials[i]);
	snprintf(cmd + off, sizeof (cmd) - off, " 2>/dev/null");
	int rc = system(cmd);
	if (rc == -1)
		return (-1);
	if (WIFEXITED(rc)) {
		*exitcode = WEXITSTATUS(rc);
		return (0);
	}
	return (-1);
}

/*
 * Like make_stream but WITHOUT a trailing DRR_END.
 * Produces a clean-truncated stream that ends on a record boundary.
 */
static void
make_stream_noend(const char *path, int nrecs, int rec_size,
    size_t *out_size)
{
	FILE *fp = fopen(path, "wb");
	if (fp == NULL) {
		(void) fprintf(stderr, "FATAL: fopen(%s): %s\n",
		    path, strerror(errno));
		exit(1);
	}

	dmu_replay_record_t drr;
	bzero(&drr, sizeof (drr));
	drr.drr_type = DRR_BEGIN;
	drr.drr_u.drr_begin.drr_magic = DMU_BACKUP_MAGIC;
	drr.drr_u.drr_begin.drr_versioninfo = 0;
	drr.drr_u.drr_begin.drr_creation_time = 0;
	drr.drr_u.drr_begin.drr_type = DMU_OST_ZFS;
	drr.drr_u.drr_begin.drr_flags = 0;
	drr.drr_u.drr_begin.drr_toguid = 1;
	drr.drr_u.drr_begin.drr_fromguid = 0;
	drr.drr_payloadlen = DUMMY_PAYLOAD_SIZE;
	strncpy(drr.drr_u.drr_begin.drr_toname, "pool/test@snap",
	    sizeof (drr.drr_u.drr_begin.drr_toname) - 1);

	(void) fwrite(&drr, sizeof (drr), 1, fp);
	(void) fwrite(dummy_payload, DUMMY_PAYLOAD_SIZE, 1, fp);
	size_t total = sizeof (drr) + DUMMY_PAYLOAD_SIZE;

	for (int i = 0; i < nrecs; i++) {
		bzero(&drr, sizeof (drr));
		drr.drr_type = DRR_WRITE;
		drr.drr_u.drr_write.drr_object = 1;
		drr.drr_u.drr_write.drr_type = DMU_OT_PLAIN_FILE_CONTENTS;
		drr.drr_u.drr_write.drr_offset =
		    (uint64_t)i * (uint64_t)rec_size;
		drr.drr_u.drr_write.drr_logical_size = (uint64_t)rec_size;
		drr.drr_u.drr_write.drr_toguid = 1;
		drr.drr_u.drr_write.drr_checksumtype = ZIO_CHECKSUM_OFF;
		drr.drr_u.drr_write.drr_compressiontype = 0;

		(void) fwrite(&drr, sizeof (drr), 1, fp);
		total += sizeof (drr);

		if (rec_size > 0) {
			char *data = xm(rec_size);
			memset(data, (int)((i & 0x3F) ^ 0x5A), rec_size);
			(void) fwrite(data, rec_size, 1, fp);
			total += (size_t)rec_size;
			free(data);
		}
	}

	(void) fclose(fp);
	*out_size = total;
}

/* ---------- test cases ---------- */

static void
do_roundtrip(int nrecs, int rec_size, uint64_t chunk_size, const char *desc)
{
	TEST(desc);

	char syn[1024], norm[1024], test[1024];
	char p1[1024], p2[1024];

	snprintf(syn, sizeof (syn), "%s/syn", testdir);
	snprintf(norm, sizeof (norm), "%s/norm", testdir);
	snprintf(test, sizeof (test), "%s/test", testdir);
	snprintf(p1, sizeof (p1), "%s/p1", testdir);
	snprintf(p2, sizeof (p2), "%s/p2", testdir);

	size_t stream_size;
	make_stream(syn, nrecs, rec_size, &stream_size);

	int n1 = split_and_join(syn, norm, p1, chunk_size);
	if (n1 < 0) {
		(void) unlink(syn);
		(void) unlink(norm);
		FAIL("first pass split/join failed");
	}

	int n2 = split_and_join(norm, test, p2, chunk_size);
	if (n2 < 0) {
		(void) unlink(syn);
		(void) unlink(norm);
		(void) unlink(test);
		FAIL("second pass split/join failed");
	}

	if (!files_equal(norm, test)) {
		(void) unlink(syn);
		(void) unlink(norm);
		(void) unlink(test);
		FAIL("second pass produced different output");
	}

	int rc = run_split(norm, chunk_size, p2);
	if (rc != 0) {
		(void) unlink(syn);
		(void) unlink(norm);
		(void) unlink(test);
		cleanup_chunks(p2, 0);
		FAIL("re-split failed (exit %d)", rc);
	}
	int nc = count_chunks(p2);
	if (nc <= 0) {
		(void) unlink(syn);
		(void) unlink(norm);
		(void) unlink(test);
		FAIL("re-split produced no chunks");
	}
	if (!chunks_start_with_begin(p2, nc)) {
		(void) unlink(syn);
		(void) unlink(norm);
		(void) unlink(test);
		cleanup_chunks(p2, nc);
		FAIL("chunk missing DRR_BEGIN");
	}
	cleanup_chunks(p2, nc);

	(void) unlink(syn);
	(void) unlink(norm);
	(void) unlink(test);
	printf("  PASS\n");
}

static void test_empty_stream(void)
{
	do_roundtrip(0, 0, 4096, "empty stream (BEGIN+END only)");
}

static void test_single_chunk(void)
{
	do_roundtrip(4, 4096, 1048576, "4x4K records, 1M chunk (fits in 1)");
}

static void test_many_small_records(void)
{
	do_roundtrip(100, 8192, 65536, "100x8K records, 64K chunks");
}

static void test_oversized_records(void)
{
	do_roundtrip(5, 131072, 65536,
	    "5x128K records, 64K chunks (oversized)");
}

static void test_zero_length_records(void)
{
	do_roundtrip(200, 0, 1024, "200 zero-size records, 1K chunks");
}

static void test_many_chunks(void)
{
	do_roundtrip(200, 4096, 32768, "200x4K records, 32K chunks (many)");
}

static void test_exact_boundary(void)
{
	do_roundtrip(30, 16384, 131072,
	    "30x16K records, 128K chunks (exact)");
}

static void test_single_large_record(void)
{
	do_roundtrip(1, 262144, 65536,
	    "1x256K record, 64K chunks (single oversize)");
}

/* ---------- merge test cases ---------- */

static void test_merge_complete_head(void)
{
	TEST("merge on complete stream (has DRR_END), no partials");

	char syn[1024], norm[1024], head[1024];
	char p1[1024];

	snprintf(syn, sizeof (syn), "%s/syn", testdir);
	snprintf(norm, sizeof (norm), "%s/norm", testdir);
	snprintf(head, sizeof (head), "%s/head", testdir);
	snprintf(p1, sizeof (p1), "%s/p1", testdir);

	size_t sz;
	make_stream(syn, 10, 4096, &sz);
	int n = split_and_join(syn, norm, p1, 65536);
	(void) unlink(syn);
	if (n < 0)
		FAIL("normalization failed");

	/* Copy norm as the merge head (complete stream with DRR_END) */
	char cp[2048];
	snprintf(cp, sizeof (cp), "cp '%s' '%s'", norm, head);
	if (system(cp) != 0) {
		(void) unlink(norm);
		(void) unlink(head);
		FAIL("cp failed");
	}

	int exitcode;
	int rc = run_merge(&exitcode, head, 0, NULL);
	(void) unlink(norm);
	(void) unlink(head);

	if (rc != 0)
		FAIL("merge command failed");
	if (exitcode != 0)
		FAIL("expected exit 0 (head complete), got %d", exitcode);

	printf("  PASS\n");
}

static void test_merge_dirty_tail(void)
{
	TEST("merge rejects dirty tail (mid-record truncation)");

	char dirty[1024];
	snprintf(dirty, sizeof (dirty), "%s/dirty", testdir);

	FILE *fp = fopen(dirty, "wb");
	if (fp == NULL)
		FAIL("fopen failed");

	dmu_replay_record_t drr;
	bzero(&drr, sizeof (drr));

	/* DRR_BEGIN */
	drr.drr_type = DRR_BEGIN;
	drr.drr_u.drr_begin.drr_magic = DMU_BACKUP_MAGIC;
	drr.drr_payloadlen = DUMMY_PAYLOAD_SIZE;
	(void) fwrite(&drr, sizeof (drr), 1, fp);
	(void) fwrite(dummy_payload, DUMMY_PAYLOAD_SIZE, 1, fp);

	/* DRR_WRITE with 4096-byte payload, but only 2048 written */
	bzero(&drr, sizeof (drr));
	drr.drr_type = DRR_WRITE;
	drr.drr_u.drr_write.drr_object = 1;
	drr.drr_u.drr_write.drr_type = DMU_OT_PLAIN_FILE_CONTENTS;
	drr.drr_u.drr_write.drr_offset = 0;
	drr.drr_u.drr_write.drr_logical_size = 4096;
	drr.drr_u.drr_write.drr_checksumtype = ZIO_CHECKSUM_OFF;
	(void) fwrite(&drr, sizeof (drr), 1, fp);

	char payload[2048];
	memset(payload, 0xAA, sizeof (payload));
	(void) fwrite(payload, sizeof (payload), 1, fp);
	(void) fclose(fp);

	int exitcode;
	int rc = run_merge(&exitcode, dirty, 0, NULL);
	(void) unlink(dirty);

	if (rc != 0)
		FAIL("merge command failed");
	if (exitcode != 1)
		FAIL("expected exit 1 (dirty tail), got %d", exitcode);

	printf("  PASS\n");
}

static void test_merge_clean_noend_no_partials(void)
{
	TEST("merge on clean incomplete head, no partials (exit 2)");

	char head[1024];
	snprintf(head, sizeof (head), "%s/head", testdir);

	size_t sz;
	make_stream_noend(head, 20, 4096, &sz);

	int exitcode;
	int rc = run_merge(&exitcode, head, 0, NULL);
	(void) unlink(head);

	if (rc != 0)
		FAIL("merge command failed");
	if (exitcode != 2)
		FAIL("expected exit 2 (incomplete), got %d", exitcode);

	printf("  PASS\n");
}

static void test_merge_vs_join(void)
{
	TEST("merge vs join produce byte-identical output");

	char syn[1024], norm[1024];
	char partial[1024], join_out[1024], merge_head[1024];
	char p1[1024];

	snprintf(syn, sizeof (syn), "%s/syn", testdir);
	snprintf(norm, sizeof (norm), "%s/nrm", testdir);
	snprintf(partial, sizeof (partial), "%s/pt", testdir);
	snprintf(join_out, sizeof (join_out), "%s/jn", testdir);
	snprintf(merge_head, sizeof (merge_head), "%s/mg", testdir);
	snprintf(p1, sizeof (p1), "%s/p1", testdir);

	/*
	 * Build a complete stream and normalize it through split+join
	 * so all records have valid embedded checksums.  Then strip
	 * DRR_END to obtain a clean-tailed head for merge testing.
	 */
	size_t sz;
	make_stream(syn, 8, 4096, &sz);
	int n = split_and_join(syn, norm, p1, 65536);
	(void) unlink(syn);
	if (n < 0)
		FAIL("normalization failed");

	/*
	 * Strip trailing DRR_END from norm to create a clean, incomplete
	 * head.  make_stream writes DRR_END as a full dmu_replay_record_t
	 * followed by no payload.
	 */
	struct stat st;
	if (stat(norm, &st) != 0) {
		(void) unlink(norm);
		FAIL("stat norm failed");
	}
	if (truncate(norm, st.st_size - sizeof (dmu_replay_record_t)) != 0) {
		(void) unlink(norm);
		FAIL("truncate norm failed");
	}
	/* norm is now the head (clean tail, no DRR_END, valid checksums) */

	/* Build a smaller complete stream to use as the partial */
	make_stream(partial, 3, 4096, &sz);

	/* Standard join: zstream join -i norm partial > join_out */
	char join_cmd[4096];
	snprintf(join_cmd, sizeof (join_cmd),
	    "%s join -i '%s' '%s' > '%s' 2>/dev/null",
	    zstream_bin(), norm, partial, join_out);
	int jrc = system(join_cmd);
	if (jrc != 0) {
		(void) unlink(norm);
		(void) unlink(partial);
		FAIL("standard join failed");
	}

	/* Merge: copy norm to merge_head, then merge partial into it */
	char cp_cmd[2048];
	snprintf(cp_cmd, sizeof (cp_cmd),
	    "cp '%s' '%s'", norm, merge_head);
	if (system(cp_cmd) != 0) {
		(void) unlink(norm);
		(void) unlink(partial);
		(void) unlink(join_out);
		FAIL("cp failed");
	}

	const char *partials[] = { partial };
	int m_exit;
	int mrc = run_merge(&m_exit, merge_head, 1, partials);
	(void) unlink(norm);
	(void) unlink(partial);

	if (mrc != 0)
		FAIL("merge command failed");

	if (!files_equal(merge_head, join_out)) {
		(void) unlink(merge_head);
		(void) unlink(join_out);
		FAIL("merge output differs from join output");
	}

	(void) unlink(merge_head);
	(void) unlink(join_out);
	printf("  PASS\n");
}

/* ---------- main ---------- */

int
main(int argc __attribute__((unused)), char **argv __attribute__((unused)))
{
	if (mkdtemp(testdir) == NULL) {
		(void) fprintf(stderr, "FATAL: mkdtemp: %s\n",
		    strerror(errno));
		return (1);
	}

	printf("Running zstream split/join round-trip unit tests...\n\n");

	test_empty_stream();
	test_single_chunk();
	test_many_small_records();
	test_oversized_records();
	test_zero_length_records();
	test_many_chunks();
	test_exact_boundary();
	test_single_large_record();

	test_merge_complete_head();
	test_merge_dirty_tail();
	test_merge_clean_noend_no_partials();
	test_merge_vs_join();

	printf("\n%d tests run, %d failed\n", tests_run, tests_failed);
	(void) rmdir(testdir);
	return (tests_failed > 0 ? 1 : 0);
}
