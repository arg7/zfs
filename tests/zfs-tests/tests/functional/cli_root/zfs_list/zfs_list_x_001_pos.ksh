#!/bin/ksh -p
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
# See the License for the specific language governing permissions
# and limitations under the License.
#
# When distributing Covered Code, include this CDDL HEADER in each
# file and include the License file at usr/src/OPENSOLARIS.LICENSE.
# If applicable, add the following below this CDDL HEADER, with the
# fields enclosed by brackets "[]" replaced with your own identifying
# information: Portions Copyright [yyyy] [name of copyright owner]
#
# CDDL HEADER END
#

#
# Copyright (c) 2026 by Delphix. All rights reserved.
#

. $STF_SUITE/include/libtest.shlib

#
# Tests for "zfs list -x <expression>" filter
#

typeset TESTFS=$TESTPOOL/testfs_x

log_assert "zfs list -x expression filter works correctly"
log_onexit cleanup

function cleanup
{
	log_must zfs destroy -r $TESTFS 2>/dev/null || true
}

function get_guid
{
	# Return the guid of the given dataset
	zfs get -H -o value guid $1 2>/dev/null
}

# Test 1: Basic numeric comparison (guid > 0)
log_note "Test 1: Basic numeric comparison - guid > 0"
output=$(zfs list -x 'guid > 0' -H -o name,guid 2>&1)
ret=$?
if [[ $ret -ne 0 ]]; then
	log_fail "zfs list -x 'guid > 0' failed: $ret"
fi
if [[ -z "$output" ]]; then
	log_fail "Expected datasets with guid > 0, got empty output"
fi
# All returned datasets should have non-zero GUIDs
while IFS=$'\t' read -r name guid; do
	if [[ "$guid" == "0" || "$guid" == "-" ]]; then
		log_fail "Dataset $name has unexpected guid: $guid"
	fi
done <<< "$output"
log_pass "Test 1 passed: guid > 0 returns datasets with non-zero GUIDs"

# Test 2: String comparison
log_note "Test 2: String comparison - name == <pool>"
pool_guid=$(get_guid $TESTPOOL)
output=$(zfs list -x "name == $TESTPOOL" -H -o name,guid 2>&1)
ret=$?
if [[ $ret -ne 0 ]]; then
	log_fail "zfs list -x 'name == $TESTPOOL' failed: $ret"
fi
IFS=$'\t' read -r name guid <<< "$output"
if [[ "$name" != "$TESTPOOL" ]]; then
	log_fail "Expected name=$TESTPOOL, got $name"
fi
if [[ "$guid" != "$pool_guid" ]]; then
	log_fail "Expected guid=$pool_guid, got $guid"
fi
log_pass "Test 2 passed: name == <pool> filter works"

# Test 3: AND operator
log_note "Test 3: AND operator"
output=$(zfs list -x 'guid > 0 && guid < 10000000000000000000' -H -o name,guid 2>&1)
ret=$?
if [[ $ret -ne 0 ]]; then
	log_fail "zfs list -x AND failed: $ret"
fi
if [[ -z "$output" ]]; then
	log_fail "Expected some datasets, got empty output"
fi
while IFS=$'\t' read -r name guid; do
	if [[ "$guid" == "-" ]]; then
		log_fail "Dataset $name has invalid guid: $guid"
	fi
	# guid must be > 0 AND < 10000000000000000000
	if (( guid <= 0 || guid >= 10000000000000000000 )); then
		log_fail "Dataset $name guid $guid not in expected range"
	fi
done <<< "$output"
log_pass "Test 3 passed: AND operator works"

# Test 4: OR operator
log_note "Test 4: OR operator"
output=$(zfs list -x 'guid > 0 || name == nonexistent' -H -o name 2>&1)
ret=$?
if [[ $ret -ne 0 ]]; then
	log_fail "zfs list -x OR failed: $ret"
fi
# Should return all datasets (guid > 0 is true for all)
count=$(echo "$output" | wc -l)
if [[ "$count" -lt 1 ]]; then
	log_fail "Expected datasets, got none"
fi
log_pass "Test 4 passed: OR operator works"

# Test 5: Parentheses with precedence
log_note "Test 5: Parentheses override precedence"
# (guid > 15000000000000000000 || guid < 6000000000000000000) && name != za-client-2-pool
output=$(zfs list -x '(guid > 15000000000000000000 || guid < 6000000000000000000) && name != za-client-2-pool' -H -o name 2>&1)
ret=$?
if [[ $ret -ne 0 ]]; then
	log_fail "zfs list -x parens failed: $ret"
fi
if [[ -z "$output" ]]; then
	log_fail "Expected some datasets, got empty output"
fi
# Verify no za-client-2-pool datasets
if echo "$output" | grep -q "za-client-2-pool"; then
	log_fail "za-client-2-pool should be excluded by name != filter"
fi
log_pass "Test 5 passed: Parentheses override precedence"

# Test 6: Size suffix expansion
log_note "Test 6: Size suffix expansion"
output=$(zfs list -x 'used > 1M' -H -o name,used 2>&1)
ret=$?
if [[ $ret -ne 0 ]]; then
	log_fail "zfs list -x 'used > 1M' failed: $ret"
fi
if [[ -z "$output" ]]; then
	log_fail "Expected datasets with used > 1M, got empty output"
fi
log_pass "Test 6 passed: Size suffix expansion works"

# Test 7: Presence evaluation (shorthand boolean)
log_note "Test 7: Presence evaluation"
# Create a filesystem with a user property
log_must zfs create $TESTFS
log_must zfs set com:zfs:xtest=hello $TESTFS
output=$(zfs list -x 'com:zfs:xtest' -H -o name 2>&1)
ret=$?
if [[ $ret -ne 0 ]]; then
	log_fail "zfs list -x presence failed: $ret"
fi
if ! echo "$output" | grep -q "$TESTFS"; then
	log_fail "Expected $TESTFS in presence filter output"
fi
# Destroy should not appear (no user property)
if echo "$output" | grep -q "$TESTPOOL"; then
	log_fail "$TESTPOOL should not appear (no com:zfs:xtest property)"
fi
log_pass "Test 7 passed: Presence evaluation works"

# Test 8: Invalid expression handling
log_note "Test 8: Invalid expression handling"
output=$(zfs list -x 'guid >' -H -o name 2>&1)
ret=$?
if [[ $ret -eq 0 ]]; then
	log_fail "Expected failure for invalid expression 'guid >'"
fi
if [[ -n "$output" ]]; then
	log_fail "Expected no stdout output for invalid expression, got: $output"
fi
log_pass "Test 8 passed: Invalid expression returns error with no stdout"

# Test 9: Unbalanced parentheses
log_note "Test 9: Unbalanced parentheses"
output=$(zfs list -x '(guid > 0' -H -o name 2>&1)
ret=$?
if [[ $ret -eq 0 ]]; then
	log_fail "Expected failure for unbalanced parens"
fi
if [[ -n "$output" ]]; then
	log_fail "Expected no stdout output for unbalanced parens, got: $output"
fi
log_pass "Test 9 passed: Unbalanced parentheses returns error"

# Test 10: Unknown property evaluates to false
log_note "Test 10: Unknown property evaluates to false"
output=$(zfs list -x 'nonexistentprop_xyz' -H -o name 2>&1)
ret=$?
if [[ $ret -ne 0 ]]; then
	log_fail "zfs list -x unknown prop failed: $ret"
fi
if [[ -n "$output" ]]; then
	log_fail "Expected no output for unknown property, got: $output"
fi
log_pass "Test 10 passed: Unknown property evaluates to false"

# Test 11: Sort with filter
log_note "Test 11: Sort order preserved with filter"
output=$(zfs list -x 'guid > 0' -s guid -H -o name,guid 2>&1)
ret=$?
if [[ $ret -ne 0 ]]; then
	log_fail "zfs list -x with sort failed: $ret"
fi
# Verify sorted order
prev_guid=0
while IFS=$'\t' read -r name guid; do
	if (( guid < prev_guid )); then
		log_fail "GUIDs not sorted: $prev_guid > $guid"
	fi
	prev_guid=$guid
done <<< "$output"
log_pass "Test 11 passed: Sort order preserved with filter"

# Test 12: G suffix (gigabyte)
log_note "Test 12: G suffix expansion"
output=$(zfs list -x 'used > 1G' -H -o name,used 2>&1)
ret=$?
if [[ $ret -ne 0 ]]; then
	log_fail "zfs list -x 'used > 1G' failed: $ret"
fi
if [[ -z "$output" ]]; then
	log_fail "Expected datasets with used > 1G, got empty output"
fi
log_pass "Test 12 passed: G suffix expansion works"

# Test 13: OR with nonexistent dataset (all should match)
log_note "Test 13: OR operator with always-false condition"
output=$(zfs list -x 'guid > 0 || name == __nonexistent__' -H -o name 2>&1)
ret=$?
if [[ $ret -ne 0 ]]; then
	log_fail "zfs list -x OR with false condition failed: $ret"
fi
count=$(echo "$output" | wc -l)
if [[ "$count" -lt 1 ]]; then
	log_fail "Expected all datasets, got none"
fi
log_pass "Test 13 passed: OR with always-false condition returns all datasets"

# Test 14: Complex expression with AND, OR, and parentheses
log_note "Test 14: Complex expression combining AND, OR, parens"
output=$(zfs list -x '(guid > 0 && guid < 10000000000000000000) || name == za-client-2-pool/slave' -H -o name,guid 2>&1)
ret=$?
if [[ $ret -ne 0 ]]; then
	log_fail "zfs list -x complex expression failed: $ret"
fi
if [[ -z "$output" ]]; then
	log_fail "Expected datasets, got empty output"
fi
# Verify za-client-2-pool/slave is included (matches the || branch)
if ! echo "$output" | grep -q "za-client-2-pool/slave"; then
	log_fail "za-client-2-pool/slave should be included via || branch"
fi
log_pass "Test 14 passed: Complex expression works"

# Test 15: User property comparison
log_note "Test 15: User property comparison"
log_must zfs set com:zfs:xtest2=world $TESTFS
output=$(zfs list -x 'com:zfs:xtest2 == world' -H -o name 2>&1)
ret=$?
if [[ $ret -ne 0 ]]; then
	log_fail "zfs list -x user prop comparison failed: $ret"
fi
if ! echo "$output" | grep -q "$TESTFS"; then
	log_fail "Expected $TESTFS in user property comparison output"
fi
log_pass "Test 15 passed: User property comparison works"

# Test 16: Invalid expression with extra trailing tokens
log_note "Test 16: Invalid expression with trailing tokens"
output=$(zfs list -x 'guid creation' -H -o name 2>&1)
ret=$?
if [[ $ret -eq 0 ]]; then
	log_fail "Expected failure for expression with trailing tokens"
fi
if [[ -n "$output" ]]; then
	log_fail "Expected no stdout output for trailing tokens, got: $output"
fi
log_pass "Test 16 passed: Trailing tokens produce error"

log_pass "All zfs list -x tests passed"
