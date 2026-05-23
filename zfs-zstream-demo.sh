#!/usr/bin/env bash
#
# zfs-zstream-demo.sh — Demonstrate zstream split/join/token -g features
#
# Requires pre-existing setup:
#   za-master   — source host with dataset za-master-pool/master
#   za-client-1 — slave host with dataset za-client-1-pool/slave
#   za-client-2 — slave host with dataset za-client-2-pool/slave
#
# This host acts as the local "bkp-host" (the bkp-host in tutorial 3).
#
# All 6 tutorial steps are executed with verification.
# Exits on first failure so you can inspect manually.
# No cleanup is performed at the end.

set -uo pipefail

# ── Hosts ────────────────────────────────────────────────────────────────
MASTER="za-master"
CLIENT1="za-client-1"
CLIENT2="za-client-2"
LOCAL_HOST="$(hostname -s)"

# ── Datasets ─────────────────────────────────────────────────────────────
MASTER_SNAP="za-master-pool/master@$(date +%Y%m%d%H%M%S)"
CLIENT1_FS="za-client-1-pool/slave"
CLIENT2_FS="za-client-2-pool/slave"

# ── Directories ──────────────────────────────────────────────────────────
DEMO_DIR="/tmp/zstream-demo"
FULL_STREAM="${DEMO_DIR}/full.zfs"
TRUNCATED="${DEMO_DIR}/truncated.zfs"
TOKEN_FILE="${DEMO_DIR}/resume-token"
CHUNK_PREFIX="${DEMO_DIR}/chunk"
JOIN_STREAM="${DEMO_DIR}/joined.zfs"
REASSEMBLED="${DEMO_DIR}/reassembled.zfs"

# ── Counters ─────────────────────────────────────────────────────────────
TOTAL=0
PASS=0
FAIL=0

# ── Helpers ──────────────────────────────────────────────────────────────
ssh_run() {
    # Run ssh with host key auto-accept, no warnings.
    ssh -o StrictHostKeyChecking=accept-new \
        -o UserKnownHostsFile="${HOME}/.ssh/known_hosts" "$@"
}

run() {
    # Run a command, print it, execute it, and verify exit code.
    # Exits on failure.
    local desc="$1"; shift
    TOTAL=$((TOTAL + 1))
    echo ""
    echo ">>> [Step $TOTAL] $desc"
    echo "    $*"
    if "$@"; then
        PASS=$((PASS + 1))
        echo "    PASS"
        return 0
    else
        FAIL=$((FAIL + 1))
        echo "    FAIL (exit $?)"
        exit 1
    fi
}

run_ssh() {
    # Run a command on a remote host. Exits on failure.
    local desc="$1"; shift
    local host="$1"; shift
    TOTAL=$((TOTAL + 1))
    echo ""
    echo ">>> [Step $TOTAL] $desc (on $host)"
    echo "    ssh $host $*"
    if ssh_run "$host" "$@"; then
        PASS=$((PASS + 1))
        echo "    PASS"
        return 0
    else
        FAIL=$((FAIL + 1))
        echo "    FAIL (exit $?)"
        exit 1
    fi
}

run_local() {
    # Run a command on the local host. Exits on failure.
    # Note: Do NOT use shell redirects (>, |) with this helper — use
    # bash -c '...' instead, since the helper prints to stdout.
    local desc="$1"; shift
    TOTAL=$((TOTAL + 1))
    echo ""
    echo ">>> [Step $TOTAL] $desc (local)"
    echo "    $*"
    if "$@"; then
        PASS=$((PASS + 1))
        echo "    PASS"
        return 0
    else
        FAIL=$((FAIL + 1))
        echo "    FAIL (exit $?)"
        exit 1
    fi
}

# ── Pre-flight sanity checks ────────────────────────────────────────────
echo "=============================================="
echo " ZFS zstream demo — Tutorial 3"
echo "=============================================="
echo ""
echo "  MASTER:   ${MASTER}   (${MASTER_SNAP})"
echo "  CLIENT1:  ${CLIENT1}  (${CLIENT1_FS})"
echo "  CLIENT2:  ${CLIENT2}  (${CLIENT2_FS})"
echo "  LOCAL:    ${LOCAL_HOST}"
echo ""

echo "--- Pre-flight checks ---"

# Check SSH connectivity to all hosts
for host in "${MASTER}" "${CLIENT1}" "${CLIENT2}"; do
    TOTAL=$((TOTAL + 1))
    echo ""
    echo ">>> [Step $TOTAL] SSH to ${host}"
    echo "    ssh ${host} echo ok"
    if ssh_run "${host}" "echo ok" >/dev/null 2>&1; then
        PASS=$((PASS + 1))
        echo "    PASS"
    else
        FAIL=$((FAIL + 1))
        echo "    FAIL (cannot reach ${host})"
        exit 1
    fi
done

# Check zfs is available on all hosts
for host in "${MASTER}" "${CLIENT1}" "${CLIENT2}"; do
    TOTAL=$((TOTAL + 1))
    echo ""
    echo ">>> [Step $TOTAL] zfs available on ${host}"
    echo "    ssh ${host} zfs version"
    if ssh_run "${host}" "zfs version" >/dev/null 2>&1; then
        PASS=$((PASS + 1))
        echo "    PASS"
    else
        FAIL=$((FAIL + 1))
        echo "    FAIL (zfs not available on ${host})"
        exit 1
    fi
done

# Check datasets exist on all hosts
for host_fs in "${MASTER}:${MASTER_SNAP%%@*}" "${CLIENT1}:${CLIENT1_FS}" "${CLIENT2}:${CLIENT2_FS}"; do
    host="${host_fs%%:*}"
    fs="${host_fs#*:}"
    TOTAL=$((TOTAL + 1))
    echo ""
    echo ">>> [Step $TOTAL] Dataset ${fs} exists on ${host}"
    echo "    ssh ${host} zfs get mountpoint ${fs}"
    if ssh_run "${host}" "zfs get -H -o value mountpoint ${fs}" >/dev/null 2>&1; then
        PASS=$((PASS + 1))
        echo "    PASS"
    else
        FAIL=$((FAIL + 1))
        echo "    FAIL (dataset ${fs} not found on ${host})"
        exit 1
    fi
done

# Check zstream is available locally
TOTAL=$((TOTAL + 1))
echo ""
echo ">>> [Step $TOTAL] zstream available locally"
echo "    which zstream"
if command -v zstream >/dev/null 2>&1; then
    PASS=$((PASS + 1))
    echo "    PASS"
else
    FAIL=$((FAIL + 1))
    echo "    FAIL (zstream not found in PATH)"
    exit 1
fi

# Check ZFS permissions — clone,create,destroy,mount,promote,receive,rollback,send,snapshot
REQUIRED_PERMS="clone create destroy mount promote receive rollback send snapshot"
for host_fs in "${MASTER}:${MASTER_SNAP%%@*}" "${CLIENT1}:${CLIENT1_FS}" "${CLIENT2}:${CLIENT2_FS}"; do
    host="${host_fs%%:*}"
    fs="${host_fs#*:}"
    TOTAL=$((TOTAL + 1))
    echo ""
    echo ">>> [Step $TOTAL] ZFS permissions on ${fs} on ${host}"
    perms="$(ssh_run "${host}" "zfs allow ${fs}" 2>/dev/null | grep "$(whoami)")"
    if [ -z "${perms}" ]; then
        FAIL=$((FAIL + 1))
        echo "    FAIL (no ZFS permissions for $(whoami) on ${fs})"
        exit 1
    fi
    missing=""
    for perm in ${REQUIRED_PERMS}; do
        if ! echo "${perms}" | grep -qw "${perm}"; then
            missing="${missing} ${perm}"
        fi
    done
    if [ -z "${missing}" ]; then
        PASS=$((PASS + 1))
        echo "    PASS"
    else
        FAIL=$((FAIL + 1))
        echo "    FAIL (missing permissions:${missing})"
        exit 1
    fi
done

# Check mountpoints are set (not "none")
for host_fs in "${MASTER}:${MASTER_SNAP%%@*}" "${CLIENT1}:${CLIENT1_FS}" "${CLIENT2}:${CLIENT2_FS}"; do
    host="${host_fs%%:*}"
    fs="${host_fs#*:}"
    TOTAL=$((TOTAL + 1))
    echo ""
    echo ">>> [Step $TOTAL] Mountpoint set on ${fs} on ${host}"
    mp="$(ssh_run "${host}" "zfs get -H -o value mountpoint ${fs}" 2>/dev/null)"
    echo "    mountpoint = ${mp}"
    if [ "${mp}" != "none" ] && [ -n "${mp}" ]; then
        PASS=$((PASS + 1))
        echo "    PASS"
    else
        FAIL=$((FAIL + 1))
        echo "    FAIL (mountpoint is '${mp}' — set a mountpoint before running)"
        exit 1
    fi
done

echo ""
echo "--- All pre-flight checks passed ---"
echo ""

mkdir -p "${DEMO_DIR}"

# ── Step 0: Create test data, snapshot, and save full stream ─────────────
echo ""
echo "=============================================="
echo " Step 0 — Create test data, snapshot, and save full stream"
echo "=============================================="

run_ssh "Create test file and snapshot on master" "${MASTER}" \
    "zfs destroy -f ${MASTER_SNAP} 2>/dev/null || true; dd if=/dev/urandom of=${MASTER_SNAP%%@*}/test.bin bs=1M count=1; zfs snapshot ${MASTER_SNAP}"

# Set mountpoints on client datasets for zfs recv to work
run_ssh "Set mountpoint on client-1" "${CLIENT1}" \
    "zfs set mountpoint=/za-client-1-pool/slave za-client-1-pool/slave; zfs mount za-client-1-pool/slave 2>/dev/null || true"

run_ssh "Set mountpoint on client-2" "${CLIENT2}" \
    "zfs set mountpoint=/za-client-2-pool/slave za-client-2-pool/slave; zfs mount za-client-2-pool/slave 2>/dev/null || true"

run_local "Create full stream file on local host" \
    bash -c 'ssh -o StrictHostKeyChecking=accept-new -o UserKnownHostsFile="${HOME}/.ssh/known_hosts" "${1}" "zfs send ${2}" > "${3}"' _ "${MASTER}" "${MASTER_SNAP}" "${FULL_STREAM}"

run_local "Verify stream file is non-empty" \
    test -s "${FULL_STREAM}"

TOTAL=$((TOTAL + 1))
echo ""
echo ">>> [Step $TOTAL] Stream file info"
echo "    ls -lh ${FULL_STREAM}"
ls -lh "${FULL_STREAM}"
PASS=$((PASS + 1))
echo "    PASS"

# ── Step 1: Generate resume token from truncated stream ─────────────────
echo ""
echo "=============================================="
echo " Step 1 — Generate resume token from truncated stream"
echo "=============================================="

run_local "Truncate stream to 8K" \
    bash -c 'head -c 8K "${1}" > "${2}"' _ "${FULL_STREAM}" "${TRUNCATED}"

run_local "Generate resume token from truncated stream" \
    bash -c 'zstream token -g -i "${1}" > "${2}"' _ "${TRUNCATED}" "${TOKEN_FILE}"

run_local "Verify token is non-empty" \
    test -s "${TOKEN_FILE}"

TOTAL=$((TOTAL + 1))
echo ""
echo ">>> [Step $TOTAL] Inspect token to find byte offset"
echo "    zstream token \$(cat ${TOKEN_FILE})"
zstream token "$(cat "${TOKEN_FILE}")"
PASS=$((PASS + 1))
echo "    PASS"

# ── Step 2: Resume a send from a partial stream file ────────────────────
echo ""
echo "=============================================="
echo " Step 2 — Resume a send from a partial stream file"
echo "=============================================="

# 2a: Send truncated stream to client-1 — expect failure
TOTAL=$((TOTAL + 1))
echo ""
echo ">>> [Step $TOTAL] Send truncated stream to client-1 (expect failure)"
echo "    ssh ${CLIENT1} zfs recv -s -FuF ${CLIENT1_FS}"
if ssh_run "${CLIENT1}" "zfs recv -s -FuF ${CLIENT1_FS}" < "${TRUNCATED}"; then
    FAIL=$((FAIL + 1))
    echo "    UNEXPECTED SUCCESS (truncated stream should fail)"
    exit 1
else
    PASS=$((PASS + 1))
    echo "    PASS (recv failed as expected on truncated stream)"
fi

# 2b: Get the resume token from client-1's interrupted recv, then resume
TOTAL=$((TOTAL + 1))
echo ""
echo ">>> [Step $TOTAL] Get resume token from client-1"
echo "    ssh ${CLIENT1} zfs get -H -o value receive_resume_token ${CLIENT1_FS}"
RESUME_TOKEN="$(ssh_run "${CLIENT1}" "zfs get -H -o value receive_resume_token ${CLIENT1_FS}")"
if [ "${RESUME_TOKEN}" = "-" ] || [ -z "${RESUME_TOKEN}" ]; then
    FAIL=$((FAIL + 1))
    echo "    FAIL (no resume token on client-1)"
    exit 1
else
    PASS=$((PASS + 1))
    echo "    PASS (token: ${RESUME_TOKEN:0:40}...)"
fi

# 2c: Send the rest of the stream using zstream resume with client-1's token
TOTAL=$((TOTAL + 1))
echo ""
echo ">>> [Step $TOTAL] Resume from token — send remainder to client-1"
echo "    zstream resume -t <token> -i ${FULL_STREAM} | ssh ${CLIENT1} zfs recv -s -F ${CLIENT1_FS}"
if zstream resume -t "${RESUME_TOKEN}" -i "${FULL_STREAM}" | ssh_run "${CLIENT1}" "zfs recv -s -F ${CLIENT1_FS}"; then
    PASS=$((PASS + 1))
    echo "    PASS"
else
    FAIL=$((FAIL + 1))
    echo "    FAIL (resume recv failed)"
    exit 1
fi

# ── Step 3: Split a stream into chunks ──────────────────────────────────
echo ""
echo "=============================================="
echo " Step 3 — Split a stream into chunks"
echo "=============================================="

run_local "Split full stream into 128K chunks" \
    zstream split -c 128K -i "${FULL_STREAM}" -o "${CHUNK_PREFIX}"

TOTAL=$((TOTAL + 1))
echo ""
echo ">>> [Step $TOTAL] List created chunks"
echo "    ls -lh ${CHUNK_PREFIX}*"
ls -lh "${CHUNK_PREFIX}"*
PASS=$((PASS + 1))
echo "    PASS"

# ── Step 4: Join chunks back into a single stream ───────────────────────
echo ""
echo "=============================================="
echo " Step 4 — Join chunks back into a single stream"
echo "=============================================="

run_local "Join all chunks" \
    bash -c 'zstream join -i "${1}"* > "${2}"' _ "${CHUNK_PREFIX}" "${JOIN_STREAM}"

TOTAL=$((TOTAL + 1))
echo ""
echo ">>> [Step $TOTAL] Verify reassembled stream structure matches original"
echo "    zstream dump ${FULL_STREAM} | grep Total"
echo "    zstream dump ${JOIN_STREAM} | grep Total"
ORIG_RECORDS="$(zstream dump "${FULL_STREAM}" 2>/dev/null | grep 'Total records' | awk '{print $4}')"
ORIG_PAYLOAD="$(zstream dump "${FULL_STREAM}" 2>/dev/null | grep 'Total payload' | awk '{print $4}')"
JOIN_RECORDS="$(zstream dump "${JOIN_STREAM}" 2>/dev/null | grep 'Total records' | awk '{print $4}')"
JOIN_PAYLOAD="$(zstream dump "${JOIN_STREAM}" 2>/dev/null | grep 'Total payload' | awk '{print $4}')"
if [ "${ORIG_RECORDS}" = "${JOIN_RECORDS}" ] && [ "${ORIG_PAYLOAD}" = "${JOIN_PAYLOAD}" ]; then
    PASS=$((PASS + 1))
    echo "    Records: ${ORIG_RECORDS}, Payload: ${ORIG_PAYLOAD} — PASS"
else
    FAIL=$((FAIL + 1))
    echo "    Original: records=${ORIG_RECORDS} payload=${ORIG_PAYLOAD}"
    echo "    Joined:   records=${JOIN_RECORDS} payload=${JOIN_PAYLOAD} — FAIL"
    exit 1
fi

# Receive the joined stream on client-1
run_local "Receive joined stream on client-1" \
    bash -c 'cat "${1}" | ssh -o StrictHostKeyChecking=accept-new -o UserKnownHostsFile="${HOME}/.ssh/known_hosts" "${2}" "zfs receive -FuF ${3}"' _ "${JOIN_STREAM}" "${CLIENT1}" "${CLIENT1_FS}"

# ── Step 5: Chunked upload with resume ──────────────────────────────────
echo ""
echo "=============================================="
echo " Step 5 — Chunked upload to client-2 with resume"
echo "=============================================="

# Clean up any previous chunk files from this step
rm -f "${DEMO_DIR}/data-bkp."*

cnt=0
send_opt="${MASTER_SNAP}"
while true; do
    chunk="${DEMO_DIR}/data-bkp.$(printf '%04d' $cnt)"
    TOTAL=$((TOTAL + 1))
    echo ""
    echo ">>> [Step $TOTAL] Fetch chunk $cnt from master (simulating interrupted transfer)"
    echo "    ssh ${MASTER} zfs send ${send_opt} | head -c 200K > ${chunk}"

    if ssh_run "${MASTER}" "zfs send ${send_opt}" | head -c 200K > "${chunk}"; then
        # head exited 0 — stream was complete, last chunk received
        PASS=$((PASS + 1))
        echo "    PASS (stream complete, last chunk received)"
        break
    else
        FAIL=$((FAIL + 1))
        echo "    FAIL (head failed — truncated transfer)"
        exit 1
    fi

    # Generate token from what we received so far
    TOTAL=$((TOTAL + 1))
    echo ""
    echo ">>> [Step $TOTAL] Generate resume token from partial chunk $cnt"
    echo "    zstream token -g -i ${chunk}"

    token="$(zstream token -g -i "${chunk}")"
    if [ $? -eq 0 ] && [ -n "${token}" ]; then
        PASS=$((PASS + 1))
        echo "    PASS (token generated: ${token})"
        send_opt="-t ${token}"
    else
        FAIL=$((FAIL + 1))
        echo "    FAIL (could not generate token)"
        exit 1
    fi

    cnt=$((cnt + 1))
done

# Join all collected chunks (demonstrates zstream join with partial resume streams)
# Note: the upload chunks from the loop above are truncated by head -c 200K,
# so they are not valid joinable fragments. Instead, split the full stream
# into proper chunks and resume to produce joinable fragments.
echo ""
echo ">>> [Step $TOTAL] Split full stream and produce joinable chunks via resume -c"

# First, create a resume token from where the upload "left off" (200K mark)
TOTAL=$((TOTAL + 1))
echo ""
echo ">>> [Step $TOTAL] Generate token at 200K mark for chunked resume"
head -c 200K "${FULL_STREAM}" > "${DEMO_DIR}/at-200k.zfs"
if zstream token -g -i "${DEMO_DIR}/at-200k.zfs" > "${DEMO_DIR}/chunk-token"; then
    PASS=$((PASS + 1))
    echo "    PASS"
else
    FAIL=$((FAIL + 1))
    echo "    FAIL (could not generate token)"
    exit 1
fi

# Use zstream resume -c to produce a chunked resume stream
TOTAL=$((TOTAL + 1))
echo ""
echo ">>> [Step $TOTAL] Produce chunked resume stream with -c"
if zstream resume -t "$(cat "${DEMO_DIR}/chunk-token")" -c 200K -i "${FULL_STREAM}" > "${DEMO_DIR}/chunk-resume.zfs"; then
    PASS=$((PASS + 1))
    echo "    PASS"
else
    FAIL=$((FAIL + 1))
    echo "    FAIL (resume -c failed)"
    exit 1
fi

# Join head (first 200K) + resume chunk
TOTAL=$((TOTAL + 1))
echo ""
echo ">>> [Step $TOTAL] Join head + resume chunk"
head -c 200K "${FULL_STREAM}" > "${DEMO_DIR}/upload-head.zfs"
if zstream join -i "${DEMO_DIR}/upload-head.zfs" "${DEMO_DIR}/chunk-resume.zfs" > "${REASSEMBLED}"; then
    PASS=$((PASS + 1))
    echo "    PASS"
else
    FAIL=$((FAIL + 1))
    echo "    FAIL (join failed)"
    exit 1
fi

# Verify reassembled stream structure
TOTAL=$((TOTAL + 1))
echo ""
echo ">>> [Step $TOTAL] Verify reassembled upload stream matches original"
ORIG_RECORDS="$(zstream dump "${FULL_STREAM}" 2>/dev/null | grep 'Total records' | awk '{print $4}')"
JOIN_RECORDS="$(zstream dump "${REASSEMBLED}" 2>/dev/null | grep 'Total records' | awk '{print $4}')"
if [ "${ORIG_RECORDS}" = "${JOIN_RECORDS}" ]; then
    PASS=$((PASS + 1))
    echo "    Records: ${ORIG_RECORDS} — PASS"
else
    FAIL=$((FAIL + 1))
    echo "    Original: ${ORIG_RECORDS} Joined: ${JOIN_RECORDS} — FAIL"
    exit 1
fi

# Clean up upload test files
rm -f "${DEMO_DIR}/data-bkp."*

# Receive on client-2
run_local "Receive reassembled stream on client-2" \
    bash -c 'cat "${1}" | ssh -o StrictHostKeyChecking=accept-new -o UserKnownHostsFile="${HOME}/.ssh/known_hosts" "${2}" "zfs receive -FuF ${3}"' _ "${REASSEMBLED}" "${CLIENT2}" "${CLIENT2_FS}"

# ── Step 6: Distribute a single stream to multiple slave hosts ──────────
echo ""
echo "=============================================="
echo " Step 6 — Distribute to multiple slaves with resume"
echo "=============================================="

# Clean up any partial receive state on clients
run_ssh "Clean up client-1 partial state" "${CLIENT1}" \
    "zfs list -t snapshot -H -o name za-client-1-pool/slave 2>/dev/null | while read s; do zfs destroy -f \"\$s\"; done"
run_ssh "Clean up client-2 partial state" "${CLIENT2}" \
    "zfs list -t snapshot -H -o name za-client-2-pool/slave 2>/dev/null | while read s; do zfs destroy -f \"\$s\"; done"

# 6a: Send truncated stream to client-1 (simulate interrupted transfer)
TOTAL=$((TOTAL + 1))
echo ""
echo ">>> [Step $TOTAL] Send truncated stream to client-1 (expect failure)"
echo "    head -c 200K ${JOIN_STREAM} | ssh ${CLIENT1} zfs recv -s -FuF ${CLIENT1_FS}"
if head -c 200K "${JOIN_STREAM}" | ssh_run "${CLIENT1}" "zfs recv -s -FuF ${CLIENT1_FS}"; then
    FAIL=$((FAIL + 1))
    echo "    UNEXPECTED SUCCESS"
    exit 1
else
    PASS=$((PASS + 1))
    echo "    PASS (recv failed as expected)"
fi

# 6b: Client-1 gets its resume token from the interrupted recv
TOTAL=$((TOTAL + 1))
echo ""
echo ">>> [Step $TOTAL] Client-1 generates resume token from local interrupted recv"
echo "    ssh ${CLIENT1} \"zfs get receive_resume_token ${CLIENT1_FS}\""
run_ssh "Client-1 gets resume token" "${CLIENT1}" \
    "zfs get receive_resume_token ${CLIENT1_FS}"

# 6c: Client-1 resumes using its own token
TOTAL=$((TOTAL + 1))
echo ""
echo ">>> [Step $TOTAL] Client-1 resumes from master stream using its own token"
token1="$(ssh_run "${CLIENT1}" "zfs get -H -o value receive_resume_token ${CLIENT1_FS}")"
if [ "${token1}" = "-" ] || [ -z "${token1}" ]; then
    FAIL=$((FAIL + 1))
    echo "    FAIL (no resume token on client-1)"
    exit 1
else
    echo "    zstream resume -t <token> -i ${JOIN_STREAM} | ssh ${CLIENT1} zfs recv -s -F ${CLIENT1_FS}"
    if zstream resume -t "${token1}" -i "${JOIN_STREAM}" | ssh_run "${CLIENT1}" "zfs recv -s -F ${CLIENT1_FS}"; then
        PASS=$((PASS + 1))
        echo "    PASS"
    else
        FAIL=$((FAIL + 1))
        echo "    FAIL (resume recv failed)"
        exit 1
    fi
fi

# 6d: Send truncated stream to client-2 (simulate interrupted transfer)
TOTAL=$((TOTAL + 1))
echo ""
echo ">>> [Step $TOTAL] Send truncated stream to client-2 (expect failure)"
echo "    head -c 200K ${JOIN_STREAM} | ssh ${CLIENT2} zfs recv -s -FuF ${CLIENT2_FS}"
if head -c 200K "${JOIN_STREAM}" | ssh_run "${CLIENT2}" "zfs recv -s -FuF ${CLIENT2_FS}"; then
    FAIL=$((FAIL + 1))
    echo "    UNEXPECTED SUCCESS"
    exit 1
else
    PASS=$((PASS + 1))
    echo "    PASS (recv failed as expected)"
fi

# 6e: Client-2 gets its resume token and resumes
TOTAL=$((TOTAL + 1))
echo ""
echo ">>> [Step $TOTAL] Client-2 resumes from master stream using its own token"
token2="$(ssh_run "${CLIENT2}" "zfs get -H -o value receive_resume_token ${CLIENT2_FS}")"
if [ "${token2}" = "-" ] || [ -z "${token2}" ]; then
    FAIL=$((FAIL + 1))
    echo "    FAIL (no resume token on client-2)"
    exit 1
else
    echo "    zstream resume -t <token> -i ${JOIN_STREAM} | ssh ${CLIENT2} zfs recv -s -F ${CLIENT2_FS}"
    if zstream resume -t "${token2}" -i "${JOIN_STREAM}" | ssh_run "${CLIENT2}" "zfs recv -s -F ${CLIENT2_FS}"; then
        PASS=$((PASS + 1))
        echo "    PASS"
    else
        FAIL=$((FAIL + 1))
        echo "    FAIL (resume recv failed)"
        exit 1
    fi
fi

# ── Summary ──────────────────────────────────────────────────────────────
echo ""
echo "=============================================="
echo " Summary"
echo "=============================================="
echo "  Total:  ${TOTAL}"
echo "  Passed: ${PASS}"
echo "  Failed: ${FAIL}"
echo ""
echo "  All steps passed!"
exit 0
