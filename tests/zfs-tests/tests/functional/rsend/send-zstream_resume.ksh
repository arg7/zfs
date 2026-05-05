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
# Verify that "zstream resume" produces a valid resume stream from a
# cached full send stream and a resume token.
#
# Strategy:
# 1. Create source dataset with data of mixed record types (full and
#    embedded records), then snapshot.
# 2. Generate a cached full send stream.
# 3. Start zfs receive -s but interrupt it partway through using zpipe -c
#    to generate a resume token.
# 4. Use zstream resume -t <token> -i <cached> to produce a resume stream
#    directly on the receiving side (no master node contact).
# 5. Pipe the resume stream to zfs receive -s to complete the receive.
# 6. Verify the receive completes cleanly (no resume token, snapshot
#    expected to be present).
#

verify_runnable "both"

log_assert "Verify zstream resume produces a valid resume stream."
log_onexit cleanup_pool $POOL2

typeset sendfs=$POOL2/fs
typeset recvfs=$POOL2/recv
typeset stream=$BACKDIR/cached.zfs
typeset resume_stream=$BACKDIR/resume.zfs
typeset cut_bytes=$((4 * 1024 * 1024))  # cut at 4 MB

log_must zfs create -o compress=lz4 $sendfs
typeset dir=$(get_prop mountpoint $sendfs)

# Create data: full-size records and an embedded record
truncate -s 2m $dir/full_records
log_must dd if=/dev/urandom of=$dir/full_records conv=notrunc bs=128k count=4

recsize=16384
log_must truncate -s $recsize $dir/embedded_records
log_must dd if=/dev/urandom of=$dir/embedded_records \
    seek=$((recsize - 8)) bs=1 count=8 conv=notrunc

log_must zfs snapshot $sendfs@snap1

# Generate cached full send stream
log_must eval "zfs send $sendfs@snap1 > $stream"

# Interrupted receive to get a resume token.  zpipe -c exits with
# code 142 after the byte limit, causing the pipeline to fail, but
# zfs receive -s saves the resume state on the dataset.
zfs send $sendfs@snap1 | zpipe -c $cut_bytes | zfs receive -s $recvfs
# ignore the pipeline exit code; verify we got a token instead
typeset TOKEN
TOKEN=$(zfs get -H -o value receive_resume_token $recvfs)
if [[ -z "$TOKEN" || "$TOKEN" == "-" ]]; then
    log_fail "Failed to obtain resume token after interrupted receive"
fi

log_note "Got resume token: ${TOKEN:0:40}..."

# Generate resume stream from cached send + token
log_must eval "zstream resume -t '$TOKEN' -i $stream > $resume_stream"

# Complete the receive
log_must eval "zfs receive -s $recvfs < $resume_stream"

# Verify resume token cleared (receive completed)
typeset post_token
post_token=$(zfs get -H -o value receive_resume_token $recvfs)
if [[ "$post_token" != "-" ]]; then
    log_fail "Resume token still present after completed receive"
fi

# Verify snapshot exists on destination
zfs get -H -o value type $recvfs@snap1 > /dev/null 2>&1 ||
    log_fail "Snapshot $recvfs@snap1 not found after resume receive"

log_pass "zstream resume produces a valid resume stream."
