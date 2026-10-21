# Offline MLO real-function contract fixture

This fixture is an offline test, not a firmware simulator or a hardware result.
It must never be loaded into the production kernel. The runner stages a separate
kernel source overlay and boots User Mode Linux without PCI or Wi-Fi devices.

The initial scope is the real mac80211 active-link core, STA/key wrappers,
hardware-key enable/disable logic, and cfg80211 wiphy workqueue. Driver-boundary
recorders are explicitly test doubles. Full MT7927 callback/MCU integration and
authenticated hardware RX remain separate, unverified gates.

The validator cases compile unchanged current function bodies extracted from
`mt7925/main.c`, plus its actual secure-link reader and counter reader. The
runner snapshots those bodies and all quoted prototype header dependencies.
Clock/sleep, metric policy, log output and request-policy boundaries are
explicitly doubled. Accepted requests execute the verbatim production
`mt7925/mlo_completion.c`, real wiphy work and synchronous outer setter. This
is not full driver linkage or proof of the secure request gate/MCU behavior.

No production lifecycle change is authorized by a fixture result. In particular,
do not reorder/reinstall PTK or interpret an early host mask as completion.

## Running

Use `python3 tests/mt7925/run_mlo_kunit.py --help`. Supply the reviewed kernel
source and a new output directory. The source tree is read only; generated files,
build products, logs, and manifests belong to the output directory. The runner
never calls sudo, modprobe, insmod, iw, rfkill, reboot, or a connection harness.

Example:

```sh
python3 tests/mt7925/run_mlo_kunit.py \
  --kernel-source /path/to/wireless-next \
  --output /path/to/new-output-dir
```

Use a new output directory and add `--strict-completion` to label the required
contract explicitly. Both modes now enforce no premature forward/rollback
confirmation. The runner enables PROVE_LOCKING/LOCKDEP; inspect the complete kernel log as
well as the test result. An intentionally failing STA callback produces a core
WARN because the real core swallows that callback error; it must be documented,
not mistaken for a successful firmware transaction or suppressed.

The reviewed kernel already contains intentional dirty dependency changes;
the overlay uses those exact current files, not upstream HEAD. It refuses a
nonempty output, output inside the kernel source, and generated-file collisions.
The original kernel tree must never be cleaned to make this test run.

## Cases and actual execution

| Case | Actual function path | Controlled boundary / oracle |
| --- | --- | --- |
| All six pair edges | `ieee80211_set_active_links` -> real STA/VIF wrappers -> `ieee80211_key_switch_links` -> `drv_set_key` | Literal survivor masks, four STA callsites but two driver notifications; disable departing GTK before shrink STA, install incoming GTK after grow STA; no post-key grow notification |
| Delayed grow | `ieee80211_set_active_links_async` -> actual wiphy work -> outer setter | Block recorder VIF grow; target mask published while outer return is still pending |
| Refused rollback | Actual outer setter and `drv_can_activate_links` | Refuse target; outer returns `-EINVAL`, unchanged pair, no driver/key callbacks |
| Failing rollback | Actual async setter/wiphy work/VIF wrapper | Block then fail VIF grow `-EIO`; temporary target reverts to survivor |
| Forward completion contract | Current unchanged `mt7925_mlo_steer_switch_validate` body plus real blocked grow | Synthetic WCID/key readiness cannot confirm before actual outer return; no overlapping rollback |
| Refused driver rollback | Actual validator body, explicit request refusal double | `-EBUSY` at policy seam is explicitly refused, not confirmed |
| Rollback completion contract | Actual validator/secure reader plus real blocked grow | Survivor RX and transient target cannot confirm; subsequent grow failure stays unconfirmed |
| Owned completion/key boundary | Verbatim `mlo_completion.c` -> real setter/wrappers/key switching | DONE after incoming GTK SET; actual core errno stored separately from lifecycle status |
| Failure and retry | Real VIF/STA/key wrapper calls with recorder failures | Failed grow returns survivor; retry gets new cookie; swallowed STA and software-fallback key failures recorded separately |
| Supersession/ABA | Real owned work plus completion admission/result functions | Queued ABA rejects old cookies; RUNNING replacement refused; external supersession rejects late publication; predecessor check atomic |
| Stale validation | Actual secure reader plus completion validation | Pre-completion timestamp, survivor-only mask and earlier ticket rejected; synthetic current all-member evidence accepted, not RF proof |
| Teardown | Actual `completion_stop` and validator body in ordinary delayed work | Canceller holds wiphy, closes producers/cancels owned job before joining ordinary worker; no wait for wiphy/core progress |

This fixture exercises per-link GTK callbacks. Shared per-MLD PTK reinstallation,
MT7927 incoming secure-RX checks inside its STA callback and actual BSS/STA/key
MCU failures/retry still need dedicated real-driver integration. Transport
callbacks are doubles, not the complete MT7927 ops. Stale descriptor generation
attribution is not fixed: fresh timestamps alone cannot prove radio/WCID/key
incarnation. Synthetic STA/WCID metadata cannot substitute for those tests.
Running and queued teardown use real locks/work cancellation, but allocator/free,
full-device reset, suspend and TDLS integration still need broader coverage.

## Evidence layers

Keep request submission, outer core return, callback/command outcome, and
authenticated datapath validation distinct. Synthetic key/RX state cannot prove
firmware key residency, over-the-air traffic, or a healthy authenticated baseline.

Tests that characterize a current defect are labeled as such. Passing a
characterization test means the defect was reproduced, not repaired.

## Hardware gate

No hardware run follows this fixture. Full MT7927 callback/firmware contracts
remain unverified, and this task does not establish an authenticated-RX baseline.
Incoming RX/ICV root cause is separate. A green completion fixture is not a green
production pair-cycling result.
