# Controlled media trials after explicit reboot authorization

On November 17, 2026, controlled trials were authorized on the test host
with persistent logging across reboots.

The production codec guards and installed revision-6 kext remain unchanged.
New experiments use a separate, process-local encoder registration, the
reviewed revision-8 initialization repair, a witnessed constructor, and an
exact UUID/function-hash-checked HEVC height override.

## Evidence and recovery

The durable experiment directory is:

```text
$HOME/Library/Application Support/NootedRedDiagnostics/media-20261117/
```

`active.json` identifies the latest trial, exact command, binary hash, boot
time and state. Each timestamped trial directory retains:

* `state.json`, `probe.log`, `kernel-live.log`, and sampled `health.jsonl`;
* `before-api-submit.json`, written before the public encode call;
* `packets.bin`, command/texture descriptions and
  `before-driver-submit.json`, synced before returning to the submitting caller;
* `callback.hevc` when an encode callback succeeds, saved before teardown;
* `reports/` and, after a reboot, `recovered-<boot-time>/`.

The probe requires regular-file logging and successful `fsync`/`F_FULLFSYNC`
for submission checkpoints. The logging observer records a stall after
20 seconds and does not launch another frame. It does not claim that killing
a process can cancel GPU work.

A local user LaunchAgent runs the
read-only recovery collector after login and at one-minute intervals. It
never submits GPU work. Recovery collection is bounded to three passes per
new boot to allow delayed crash reports. Remove this temporary LaunchAgent
after completing the investigation.

## Resume after interruption

1. Read `active.json` and the corresponding `state.json` and `probe.log`.
   A pending state is not a successful encode.
2. Run the collector if needed:

   ```sh
   python3 "$HOME/Library/Application Support/NootedRedDiagnostics/media-20261117/evidence.py" recover --force
   ```

3. Compare `before-driver-submit.json` and `packets.bin` with the successful
   control. Inspect reset timing and distinguish pre-reset from post-reset
   faults. Preserve the exact first-trial configuration.
4. Validate successful output with independent CPU decoding and the existing
   source-pattern checks. An encoder callback alone is not acceptance.
5. Review the result before starting a new, separately identified trial.
   No GPU trial is automatically replayed after reboot.

## Initial sequence

1. Dry square capture: initialization repaired, native circular bitstream
   mode/size and native slice settings; exit before submission.
2. One-frame 4096×2304 control with the same instrumentation and independent
   CPU reconstruction.
3. One-frame 4096×4096 HEVC Main experiment with the same settings.

Results are pending at creation of this note. Revision 8's existing report
and archive remain the prior bounded candidate, not proof of these trials.
