<!--- Please fill out the following template, which will help other contributors review your Pull Request. -->

<!--- Provide a general summary of your changes in the Title above -->

### Motivation and Context
When `zfs send` and `zfs recv` communicate directly over SSH, the
recv side generates a resume token on an interrupted receive, and the
send side uses it with `zfs send -t <token>` to continue. However, in
workflows where the stream is stored as a file — for distribution to
multiple slave hosts, staged uploads over unreliable links, or chunked
logistics — there was no way to resume an interrupted receive or
continue an interrupted send from a partial stream file without
involving the original snapshot host.

This change fills that gap by adding stream-level tooling that
operates on raw send files:

- **Resume without the origin host**: `zstream token -g -i partial-stream`
  generates a resume token from a truncated stream (the equivalent of
  what `zfs recv` produces on interruption). `zstream resume -t <token>
  -i stream-file | zfs recv -s <pool>` lets any slave resume from a
  locally-saved token. A master generates one snapshot stream; slaves
  consume it with full resume support — no need to reach back to the
  master.

- **Chunked upload with resume**: On an incomplete upload, generate a
  resume token from the partial stream on the consumer side, pass it to
  the sender for `zfs send -t <token>`, and continue. Once all chunks
  arrive, join them with `zstream join`.

- **Splitting for logistics**: `zstream split -c <size>` breaks a stream
  into record-aligned chunks for transport over size-limited channels;
  `zstream join` reassembles them for receive.

The kernel change (stream byte offset in the resume token) makes
resume O(1) instead of O(n) on stream size, critical for multi-terabyte
streams.

### Additional Features

This PR also includes two new CLI query extensions for `zfs snapshot` and `zfs list`:

**Feature 1: Snapshot Property Extraction (`-g`)** — `zfs snapshot -g <props>`
- Immediate CSV output of requested properties at snapshot creation time
- No separate `zfs get` lookup needed; eliminates the need for post-creation property queries
- Supports built-in properties (`guid`, `creation`) and user properties (`com:foo:bar`)
- `-g guid,creation` outputs: `13682857927658284279,1716382100` (no snapshot name by default; add `name` to the list if needed)
- Invalid/unknown properties output `-`; silent on snapshot failure
- Recursive (`-r`) support: one CSV line per created snapshot
- Test: `tests/zfs-tests/tests/functional/snapshot/snapshot_g_001_pos.ksh` (7 tests)

**Feature 2: Conditional Expression Filtering (`-x`)** — `zfs list -x "<expr>"`
- Native logical evaluation engine inside `zfs list`; no external `grep`/`awk` piping needed
- Recursive descent parser supporting:
  - Comparison operators: `==`, `!=`, `>`, `<`, `>=`, `<=`
  - Logical operators: `&&` (AND), `||` (OR) with correct precedence (`&&` binds tighter)
  - Parentheses `( )` for precedence override (including nested)
  - Presence evaluation: bare property name evaluates to true if it has a non-empty value
  - Size suffix expansion: `K`, `M`, `G`, `T`, `P`, `E` (base-2)
  - Time suffix expansion: `d`, `h`, `m`, `s`
- Filter applied during iteration (pre-AVL tree), so non-matching datasets are discarded immediately — sort order with `-s` is preserved
- Unknown/invalid properties evaluate to false (no crash)
- Malformed expressions produce stderr error with no stdout output
- Examples:
  ```bash
  zfs list -x 'guid > 0' -H -o name,guid
  zfs list -x 'used > 1G && name != temp*' -H -o name,used
  zfs list -x '(guid > 15T || guid < 6T) && com:backup == yes' -H -o name
  zfs list -t snap -x 'com:company:retention'
  zfs list -x 'guid > 0' -s guid -H -o name,guid   # sorted output
  ```
- Unit tests: `tests/zfs-tests/cmd/xexpr_test.c` (38 tests)
- E2E tests: `tests/zfs-tests/tests/functional/cli_root/zfs_list/zfs_list_x_001_pos.ksh` (16 tests)

### Description
**Kernel** (`module/zfs/`):
- `DS_FIELD_RESUME_STREAM_OFFSET` — new ZAP field in the dataset
- `dmu_recv.c` — captures `stream_offset` from `drc_rrd->bytes_read` /
  `rrd->header_offset` (including a write-batch fix where
  `rwa->stream_offset` was overwritten before `save_resume_state()`)
- `dsl_dataset.c` — stores `"stream_offset"` in the resume token nvlist

**Userspace** — new and enhanced `zstream` subcommands (`cmd/zstream/`):
- `zstream resume` — **new subcommand**
  - `-o <hex>` — explicit seek offset for O(1) resume; validated via
    probe record
  - `-c <size>` — byte limit for chunking (no DRR_END written)
  - `-S` — skip-seek mode: no seeking and no resume-point filtering;
    caller pre-positions stdin at the resume point
  - `-H <header_file>` — file with original DRR_BEGIN+nvlist for use
    with `-S` when piped input lacks a BEGIN header
  - Works with pipe input (stdin), falls back to linear scan if fseek
    unavailable or `stream_offset` missing
- `zstream token -g` — **new subcommand**; generate resume token from
  truncated stream
- `zstream split -c <size> [-o prefix]` — **new subcommand**; splits full
  send into record-aligned chunks each with DRR_BEGIN prepended;
  pipe-compatible via stashed record overflow buffering (no fseek/rewind)
- `zstream join [-i head] partial...` — **new subcommand**; reassemble
  head + partial fragments
- Global `-t <token>` flag for fast-rewind to resume point
- `record_payload_size()` deduplicated into shared `zstream_util.c`

**Backward compatibility**: unknown nvlist keys are ignored by stock
ZFS 2.3.4. Stock `zstream resume` works on token from patched kernel;
patched zstream falls back to linear scan on stock token (no
`stream_offset` key).

### How Has This Been Tested?
- Ran `sudo -u ar ./scripts/zfs-tests.sh -T rsend` — all 6 new tests pass:
  - `send-zstream_resume.ksh` — basic resume, global -t, token -g,
    bogus offset
  - `send-zstream_split.ksh` — split file/pipe, join exit codes (0/1/2),
    resume -c, pipe resume, E2E
  - `send-resume_token_offset.ksh` — stream_offset at 5 cut points
    (4KB, 16KB, 64KB, 128KB, 512KB), kernel token check
- Backward compat validated on stock kernel 2.3.4: cross-token resume
  succeeds, data md5sum matches
- Tested with pipe input (stdin) for split and resume — no fseek required

### Types of changes
<!--- What types of changes does your code introduce? Put an `x` in all the boxes that apply: -->
- [ ] Bug fix (non-breaking change which fixes an issue)
- [x] New feature (non-breaking change which adds functionality)
- [x] Performance enhancement (non-breaking change which improves efficiency)
- [ ] Code cleanup (non-breaking change which makes code smaller or more readable)
- [ ] Quality assurance (non-breaking change which makes the code more robust against bugs)
- [ ] Breaking change (fix or feature that would cause existing functionality to change)
- [ ] Library ABI change (libzfs, libzfs\_core, libnvpair, libuutil and libzfsbootenv)
- [ ] Documentation (a change to man pages or other documentation)

### Tutorial 3: Working with ZFS send streams as local files

`zfs-howto.md` contains Tutorial 3, a step-by-step guide covering the new
`zstream` features:

- **Step 0** — Save a full send stream to a file
- **Step 1** — Generate a resume token from a partial stream (`zstream token -g`)
- **Step 2** — Resume a send from a partial stream file (`zstream resume -t <token>`)
- **Step 3** — Split a stream into chunks (`zstream split -c <size>`)
- **Step 4** — Join chunks back into a single stream (`zstream join`)
- **Step 5** — Chunked upload to remote file server with resume
- **Step 6** — Distribute a single stream to multiple slave hosts with resume

Each step includes copy-pasteable commands and explains the expected output.

### Demo script

`zfs-zstream-demo.sh` is a complete demo/test script that exercises all 6
tutorial steps across 3 hosts (za-master, za-client-1, za-client-2). It:

- Runs pre-flight sanity checks (SSH, zpools, permissions, mountpoints)
- Exits on first failure for manual inspection
- Uses random chunk sizes for interrupted transfers to stress-test resume
- Validates truncated streams before using them (`zstream dump -C`)
- Demonstrates cross-host resume workflows with `zstream resume` and `zfs recv`

### Checklist:
<!--- Go over all the following points, and put an `x` in all the boxes that apply. -->
<!--- If you're unsure about any of these, don't hesitate to ask. We're here to help! -->
- [x] My code follows the OpenZFS [code style requirements](https://github.com/openzfs/zfs/blob/master/.github/CONTRIBUTING.md#coding-conventions).
- [x] I have updated the documentation accordingly.
- [x] I have read the [**contributing** document](https://github.com/openzfs/zfs/blob/master/.github/CONTRIBUTING.md).
- [x] I have added [tests](https://github.com/openzfs/zfs/tree/master/tests) to cover my changes.
- [x] I have run the ZFS Test Suite with this change applied.
- [x] All commit messages are properly formatted and contain [`Signed-off-by`](https://github.com/openzfs/zfs/blob/master/.github/CONTRIBUTING.md#signed-off-by).

