#! /bin/ksh -p
# SPDX-License-Identifier: CDDL-1.0
#
# CDDL HEADER START
#
# The contents of this file are subject to the terms of the
# Common Development and Distribution License (the "License").
# You may not use this file except in compliance with the License.
#
# You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
# or https://opensource.org/licenses/CDDL-1.0.
# See the License for the specific terms and conditions.
#
# When distributing Covered Code, include this CDDL HEADER in each
# file and include the License file at usr/src/OPENSOLARIS.LICENSE.
# If applicable, add the following below this CDDL HEADER, with the
# fields enclosed in brackets "[]" replaced by your own identifying
# information: Portions Copyright [yyyy] [name of copyright owner]
#
# CDDL HEADER END
#

#
# Copyright (c) 2026, CompEd Software Design srl.
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/snapshot/snapshot.cfg

#
# DESCRIPTION:
# Verify 'zfs snapshot -g <properties>' outputs CSV with requested
# properties for each created snapshot.
#

verify_runnable "both"

function cleanup
{
	log_must zfs destroy -r $TESTPOOL/$TESTFS
}

log_assert "zfs snapshot -g outputs CSV of requested property values only"

log_onexit cleanup

log_must zfs create $TESTPOOL/$TESTFS
log_must zfs set com:test:mytag=hello $TESTPOOL/$TESTFS
log_must zfs create $TESTPOOL/$TESTFS/child

# Test 1: single snapshot with guid and creation (no name prefix)
log_note "Test 1: single snapshot with guid,creation"
output=$(zfs snapshot -g guid,creation $SNAPFS 2>&1)
ret=$?
if [[ $ret -ne 0 ]]; then
	log_fail "zfs snapshot -g failed with exit code $ret"
fi

# Verify CSV format: guid,creation (no name)
IFS=',' read -r guid creation <<< "$output"
if [[ -z "$guid" || -z "$creation" ]]; then
	log_fail "CSV output missing fields: '$output'"
fi
if [[ "$guid" == "-" ]]; then
	log_fail "guid is '-': '$output'"
fi
if [[ "$creation" == "-" ]]; then
	log_fail "creation is '-': '$output'"
fi
log_note "Test 1 passed: '$output'"

# Test 2: recursive snapshots
log_note "Test 2: recursive -r -g guid,creation"
output=$(zfs snapshot -r -g guid,creation $TESTPOOL/$TESTFS@rsnap 2>&1)
ret=$?
if [[ $ret -ne 0 ]]; then
	log_fail "zfs snapshot -r -g failed with exit code $ret"
fi

lines=$(echo "$output" | wc -l)
if [[ $lines -ne 3 ]]; then
	log_fail "Expected 3 lines for recursive, got $lines: '$output'"
fi

# Verify each line has correct format (no name, just guid,creation)
while IFS= read -r line; do
	IFS=',' read -r guid creation <<< "$line"
	if [[ -z "$guid" || "$guid" == "-" || "$creation" == "-" ]]; then
		log_fail "Invalid line in recursive output: '$line'"
	fi
done <<< "$output"
log_note "Test 2 passed: recursive output has $lines valid lines"

# Test 3: user property
log_note "Test 3: user property com:test:mytag"
output=$(zfs snapshot -g com:test:mytag $TESTPOOL/$TESTFS@usnap 2>&1)
ret=$?
if [[ $ret -ne 0 ]]; then
	log_fail "zfs snapshot -g with user prop failed: $ret"
fi

IFS=',' read -r userval <<< "$output"
if [[ "$userval" != "hello" ]]; then
	log_fail "User property value mismatch: expected 'hello', got '$userval'"
fi
log_note "Test 3 passed: '$output'"

# Test 4: invalid property returns '-'
log_note "Test 4: invalid property returns '-'"
output=$(zfs snapshot -g guid,nonexistentprop $TESTPOOL/$TESTFS@inv 2>&1)
ret=$?
if [[ $ret -ne 0 ]]; then
	log_fail "zfs snapshot -g with invalid prop failed: $ret"
fi

IFS=',' read -r guid invprop <<< "$output"
if [[ "$guid" == "-" ]]; then
	log_fail "Valid property guid is '-': '$output'"
fi
if [[ "$invprop" != "-" ]]; then
	log_fail "Invalid property should be '-', got '$invprop'"
fi
log_note "Test 4 passed: '$output'"

# Test 5: no -g flag produces no output
log_note "Test 5: no -g flag produces no stdout"
output=$(zfs snapshot $TESTPOOL/$TESTFS@nogo 2>&1)
ret=$?
if [[ -n "$output" ]]; then
	log_fail "Expected no output without -g, got: '$output'"
fi
log_note "Test 5 passed: no output without -g"

# Test 6: combined -g with -o
log_note "Test 6: combined -g and -o"
output=$(zfs snapshot -o com:test:otherval=world -g guid $TESTPOOL/$TESTFS@combo 2>&1)
ret=$?
if [[ $ret -ne 0 ]]; then
	log_fail "Combined -g and -o failed: $ret"
fi

IFS=',' read -r guidval <<< "$output"
if [[ "$guidval" == "-" ]]; then
	log_fail "guid is '-' in combined test: '$output'"
fi

# Verify the -o property was actually set
snapval=$(zfs get -H -o value com:test:otherval $TESTPOOL/$TESTFS@combo 2>&1)
if [[ "$snapval" != "world" ]]; then
	log_fail "-o property not set on snapshot: expected 'world', got '$snapval'"
fi
log_note "Test 6 passed: '$output'"

# Test 7: name property included when requested
log_note "Test 7: name property in -g"
output=$(zfs snapshot -g name,guid $TESTPOOL/$TESTFS@nametest 2>&1)
ret=$?
if [[ $ret -ne 0 ]]; then
	log_fail "zfs snapshot -g name failed: $ret"
fi

IFS=',' read -r name guidval <<< "$output"
if [[ "$name" != "$TESTPOOL/$TESTFS@nametest" ]]; then
	log_fail "Name mismatch: expected '$TESTPOOL/$TESTFS@nametest', got '$name'"
fi
if [[ "$guidval" == "-" ]]; then
	log_fail "guid is '-' in name test: '$output'"
fi
log_note "Test 7 passed: '$output'"

# Test 8: -g guid must return the snapshot's own guid, not the parent's
# This catches bugs where the parent filesystem handle is used instead of
# the snapshot handle, causing snapshot-specific properties to return
# the parent's values instead of the snapshot's.
log_note "Test 8: -g guid matches snapshot guid from zfs get"
parent_guid=$(zfs get -H -o value guid $TESTPOOL/$TESTFS 2>&1)
if [[ -z "$parent_guid" ]]; then
	log_fail "Could not get parent guid: '$parent_guid'"
fi

output=$(zfs snapshot -g guid $TESTPOOL/$TESTFS@crosscheck 2>&1)
ret=$?
if [[ $ret -ne 0 ]]; then
	log_fail "zfs snapshot -g guid failed: $ret"
fi

IFS=',' read -r snap_guid <<< "$output"
if [[ "$snap_guid" == "$parent_guid" ]]; then
	log_fail "Snapshot guid '$snap_guid' matches parent guid '$parent_guid' " \
	    "- likely reading from wrong handle (parent instead of snapshot)"
fi

snap_guid_get=$(zfs get -H -o value guid $TESTPOOL/$TESTFS@crosscheck 2>&1)
if [[ -z "$snap_guid_get" ]]; then
	log_fail "Could not get snapshot guid via zfs get: '$snap_guid_get'"
fi
if [[ "$snap_guid" != "$snap_guid_get" ]]; then
	log_fail "Snapshot guid mismatch: -g returned '$snap_guid', " \
	    "zfs get returned '$snap_guid_get'"
fi
log_note "Test 8 passed: -g guid='$snap_guid' != parent guid='$parent_guid'"

log_pass "zfs snapshot -g works correctly"
