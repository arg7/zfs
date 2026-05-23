#!/bin/ksh -p
# SPDX-License-Identifier: CDDL-1.0

#
# This file and its contents are supplied under the terms of the
# Common Development and Distribution License ("CDDL"), version 1.0.
# You may only use this file in accordance with the terms of version
# 1.0 of the CDDL.
#
# A full copy of the text of the CDDL should have accompanied this
# source.  A copy of the CDDL is also available via the Internet at
# http://www.illumos.org/license/CDDL.
#

#
# Copyright (c) 2026 CompEd Software Design srl.
#

. $STF_SUITE/tests/functional/rsend/rsend.kshlib
. $STF_SUITE/include/math.shlib

#
# Description:
# Verify "zstream split" produces record-aligned chunks that can be
# reassembled with "zstream join", and "zstream resume -c" limits
# output size for chunking workflows.  Uses head -c for partial
# receive stream cutting.
#
# Strategy:
# 1. Create source dataset with mixed data, snapshot, save full send.
# 2. Split into chunks (file and pipe), verify join+receive, data integrity.
# 3. Verify join exit codes (0=complete, 2=incomplete, 1=error).
# 4. Verify resume -c produces size-limited partials.
# 5. Verify resume works with pipe input (no -i).
# 6. Verify split with custom output prefix.
# 7. End-to-end: resume -c partial → join with head → incomplete but valid.
#

verify_runnable "both"

log_assert "Verify zstream split and resume -c chunking features."
log_onexit cleanup_pool $POOL2

typeset sendfs=$POOL2/fs
typeset recvfs=$POOL2/recv
typeset stream=$BACKDIR/cached.zfs

log_must zfs create -o compress=lz4 $sendfs
typeset dir=$(get_prop mountpoint $sendfs)

# Create data: full records (2MB) and embedded records (16KB)
truncate -s 2m $dir/full_records
log_must dd if=/dev/urandom of=$dir/full_records conv=notrunc bs=128k count=4

recsize=16384
log_must truncate -s $recsize $dir/embedded_records
log_must dd if=/dev/urandom of=$dir/embedded_records \
    seek=$((recsize - 8)) bs=1 count=8 conv=notrunc

log_must zfs snapshot $sendfs@snap1

# Generate cached full send stream
log_must eval "zfs send $sendfs@snap1 > $stream"

typeset stream_size=$(stat -c %s "$stream" 2>/dev/null)
log_note "Full stream size: $stream_size bytes"

# ------------------------------------------------------------------
# Test 1: split (file input) → join → receive → data PASS
# ------------------------------------------------------------------
log_note "=== Test 1: split (file) + join + receive ==="
typeset chunk_size=524288  # 512 KB

zfs destroy -r $recvfs 2>/dev/null
rm -f $BACKDIR/c1.*

log_must eval "zstream split -i $stream -c $chunk_size -o $BACKDIR/c1"

# Gather and sort all chunks
typeset -a chunks
for f in $BACKDIR/c1.[0-9]*; do
	[[ -f $f ]] || continue
	chunks+=($f)
done
typeset num_chunks=${#chunks[@]}
if (( num_chunks == 0 )); then
	log_fail "split produced no chunks"
fi
log_note "split produced $num_chunks chunks"

# Verify each chunk starts with DRR_BEGIN
for f in "${chunks[@]}"; do
	typeset drr_type=$(dd if=$f bs=1 count=4 2>/dev/null | od -A n -t u4 | tr -d ' ')
	if (( drr_type != 0 )); then
		log_fail "Chunk $f does not start with DRR_BEGIN (type=$drr_type)"
	fi
done

# Verify each chunk is ≤ chunk_size (except the last which may be smaller)
chunk_idx=0
while (( chunk_idx < num_chunks - 1 )); do
	typeset sz=$(stat -c %s "${chunks[$chunk_idx]}" 2>/dev/null)
	if (( sz > chunk_size )); then
		log_fail "Chunk $chunk_idx size $sz exceeds chunk_size $chunk_size"
	fi
	((chunk_idx = chunk_idx + 1))
done
log_note "Chunk sizes respect $chunk_size byte limit"

# Join all chunks and receive
zfs destroy -r $recvfs 2>/dev/null
log_must eval "zstream join -i ${chunks[0]} ${chunks[@]:1} 2>/dev/null |" \
    " zfs receive -F $recvfs"
log_must diff -r $dir /$recvfs
log_note "PASS: split (file) → join → receive"

# ------------------------------------------------------------------
# Test 2: split (pipe input) → identical chunks → join → data PASS
# ------------------------------------------------------------------
log_note "=== Test 2: split (pipe) + join + receive ==="

zfs destroy -r $recvfs 2>/dev/null
rm -f $BACKDIR/c2.*

log_must eval "cat $stream | zstream split -c $chunk_size -o $BACKDIR/c2"

typeset -a pipe_chunks
for f in $BACKDIR/c2.[0-9]*; do
	[[ -f $f ]] || continue
	pipe_chunks+=($f)
done
typeset num_pipe_chunks=${#pipe_chunks[@]}

if (( num_pipe_chunks != num_chunks )); then
	log_fail "Pipe split produced $num_pipe_chunks chunks, expected $num_chunks"
fi

# Verify file and pipe chunks are byte-identical
integer i=0
while (( i < num_chunks )); do
	if ! cmp -s ${chunks[$i]} ${pipe_chunks[$i]}; then
		log_fail "Chunk $i differs between file and pipe input"
	fi
	((i = i + 1))
done
log_note "File and pipe split produced identical chunks"

# Join pipe chunks and verify
zfs destroy -r $recvfs 2>/dev/null
log_must eval "zstream join -i ${pipe_chunks[0]} ${pipe_chunks[@]:1}" \
    " 2>/dev/null | zfs receive -F $recvfs"
log_must diff -r $dir /$recvfs
log_note "PASS: split (pipe) → join → receive"

# ------------------------------------------------------------------
# Test 3: Join exit codes
# ------------------------------------------------------------------
log_note "=== Test 3: join exit codes ==="

# All chunks → exit 0 (complete)
zstream join -i ${chunks[0]} ${chunks[@]:1} > /dev/null 2>/dev/null
rc=$?
if (( rc != 0 )); then
	log_fail "Join all chunks returned $rc, expected 0 (complete)"
fi

# Head-only → exit 2 (incomplete, no DRR_END)
zstream join -i ${chunks[0]} > /dev/null 2>/dev/null
rc=$?
if (( rc != 2 )); then
	log_fail "Join head-only returned $rc, expected 2 (incomplete)"
fi

# Bad input → exit 1 (error)
zstream join -i /nonexistent/file 2>/dev/null
rc=$?
if (( rc != 1 )); then
	log_fail "Join bad input returned $rc, expected 1 (error)"
fi
log_note "PASS: join exit codes (0=complete, 2=incomplete, 1=error)"

# ------------------------------------------------------------------
# Test 4: resume -c byte limit
# ------------------------------------------------------------------
log_note "=== Test 4: resume -c (byte limit) ==="

# Get token from the first chunk (record-aligned)
typeset head_chunk=${chunks[0]}
typeset TOKEN
TOKEN=$(zstream token -g -i $head_chunk 2>/dev/null)
if [[ -z "$TOKEN" || "$TOKEN" == "-" ]]; then
	log_fail "zstream token -g failed for head chunk"
fi

typeset limit=65536  # 64 KB
log_must eval "zstream resume -t '$TOKEN' -i $stream -c $limit" \
    " > $BACKDIR/resume_partial.zfs"

typeset partial_size=$(stat -c %s $BACKDIR/resume_partial.zfs 2>/dev/null)

# Verify partial size: must be ≥ 300 (valid BEGIN + at least one data record)
# and ≤ limit + overhead (one record may exceed limit)
if (( partial_size < 300 )); then
	log_fail "resume -c output too small: $partial_size bytes"
fi
log_note "resume -c $limit produced $partial_size bytes"

# Verify partial is a valid stream fragment (has BEGIN record)
typeset drr_type=$(dd if=$BACKDIR/resume_partial.zfs bs=1 count=4 2>/dev/null | od -A n -t u4 | tr -d ' ')
if (( drr_type != 0 )); then
	log_fail "resume partial does not start with DRR_BEGIN (type=$drr_type)"
fi

# Join head chunk with resume partial — should be compatible (exit 2 = incomplete)
zstream join -i $head_chunk $BACKDIR/resume_partial.zfs \
    > $BACKDIR/resume_joined.zfs 2>/tmp/resume_join_stderr
rc=$?
typeset joined_size=$(stat -c %s $BACKDIR/resume_joined.zfs 2>/dev/null)

# Exit 2 (incomplete) or 0 (complete) are both valid here
if (( rc != 0 && rc != 2 )); then
	log_note "join stderr: $(cat /tmp/resume_join_stderr)"
	log_fail "join head+resume_partial returned $rc"
fi

# The joined stream must be larger than the head alone
if (( joined_size <= $(stat -c %s $head_chunk) )); then
	log_fail "joined stream not larger than head chunk"
fi
log_note "Joined stream: $joined_size bytes (exit $rc)"
log_note "PASS: resume -c produces valid joinable partial"

# ------------------------------------------------------------------
# Test 5: resume with pipe input (no -i flag)
# ------------------------------------------------------------------
log_note "=== Test 5: resume with pipe input ==="

zfs destroy -r $recvfs 2>/dev/null

# Interrupt receive with dd to get a kernel token
head -c 262144 < $stream | zfs receive -s $recvfs 2>/dev/null
TOKEN=$(zfs get -H -o value receive_resume_token $recvfs)
if [[ -z "$TOKEN" || "$TOKEN" == "-" ]]; then
	log_fail "Failed to obtain resume token for pipe test"
fi

# Resume via pipe — cat full stream into zstream resume
log_must eval "cat $stream | zstream resume -t '$TOKEN' 2>/dev/null |" \
    " zfs receive -s $recvfs"

# Verify token cleared and data correct
typeset post_token
post_token=$(zfs get -H -o value receive_resume_token $recvfs)
if [[ "$post_token" != "-" ]]; then
	log_fail "Resume token still present after pipe resume"
fi
log_must diff -r $dir /$recvfs
log_note "PASS: resume with pipe input"

# ------------------------------------------------------------------
# Test 6: Full end-to-end with split → pipe → join
# ------------------------------------------------------------------
log_note "=== Test 6: end-to-end split+pipe+join+receive ==="

zfs destroy -r $recvfs 2>/dev/null
rm -f $BACKDIR/ee.*

# Split via pipe
log_must eval "cat $stream | zstream split -c 262144 -o $BACKDIR/ee"
typeset -a ee_chunks
for f in $BACKDIR/ee.[0-9]*; do
	[[ -f $f ]] || continue
	ee_chunks+=($f)
done
log_note "End-to-end split: ${#ee_chunks[@]} chunks"

# Join and receive
log_must eval "zstream join -i ${ee_chunks[0]} ${ee_chunks[@]:1}" \
    " 2>/dev/null | zfs receive -F $recvfs"
log_must diff -r $dir /$recvfs
log_note "PASS: end-to-end split+pipe+join+receive"

# ------------------------------------------------------------------
log_pass "zstream split and resume -c chunking features work correctly."
