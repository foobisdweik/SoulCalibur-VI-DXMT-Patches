# Direct input A/B controls

Launch the game normally, then connect directly to the running mod.
The client sends an explicit command. The game acknowledges it on the next engine frame.
No file watcher starts or schedules the recording.

The updated DLL must be installed before that game launch.
The client cannot add this endpoint to an older DLL that is already loaded.
The endpoint requires the existing frame changes and an input hook to be enabled.
The installed configuration already enables them.

## Quick comparison

Run these commands from the repository directory.
The default bottle is `SC6-FEX` in CrossOver Preview.
Set `SC6_BOTTLE` if the running game uses another bottle.

1. Launch your usual SC6-FEX app and enter an offline Training battle.
2. Run `tools/input-control/control.sh status` to confirm the connection and current settings.
3. Run `tools/input-control/control.sh a` to disable background polling and start recording A.
4. Perform a repeatable sequence for about 30 seconds.
5. Run `tools/input-control/control.sh stop /tmp/sc6-input-A.tsv` to stop recording and save A.
6. Run `tools/input-control/control.sh b` to enable 125 Hz background polling and start recording B.
7. Repeat the same sequence for about 30 seconds.
8. Run `tools/input-control/control.sh stop /tmp/sc6-input-B.tsv` to stop recording and save B.
9. Run `tools/input-control/control.sh restore` to restore the polling rate from game startup.

`a` and `b` each discard the preceding capture, so save it before switching conditions.
Neither command changes the latch, early steps, GPU waits, or the rendering limit.
`stop` ends recording but leaves the selected polling condition active.
`restore` ends recording and restores the polling rate. Save the capture before restoring.
Normal engine reads continue in both conditions.
A reader iteration already in progress can finish near a switch. Exclude the first second when comparing steady conditions.

The client never launches SC6. If no endpoint exists, it reports an error.
If multiple games expose endpoints, select one with `--pid PID`.
Commands time out after two seconds if the game cannot acknowledge them.
A paused process may apply a queued command when it resumes, so check `status` afterward.

## Recorded data

The client saves a TSV file, which is a text table with tabs between columns.
The DLL retains records in memory until the client saves them.
Therefore, the game does not write a file for each captured frame.
The buffer holds 16,384 frame samples, about 136 seconds at 120 frames/s.
When it fills, the game counts dropped samples instead of overwriting records.

Each row describes the interval between two successive engine frames:

- `seconds` is elapsed time since the first sample.
- `frame_ms` is the measured frame interval in milliseconds.
- `battle_steps` is the unsigned difference between battle-counter values.
- `poll_cycles` counts completed background reader cycles.
- `engine_reads` counts XInput calls observed on the game thread.
- `presses` counts reads that delivered at least one new or retained button press.
- `early_steps` counts steps released early by input.

A cycle considers four controller slots. Empty slots retry less often.
The first interval can contain a reader iteration that was already running when the condition changed.
If the battle counter resets, its unsigned difference can become very large. Exclude that interval from a step-rate calculation.
Use a continuous Training battle because menu and loading transitions make these comparisons harder to interpret.
Keep actions, scene, and other settings alike in A and B.

These records measure software behavior. They do not measure the delay from a physical button press to visible pixels.
The achieved polling rate and its effect in SC6 remain unverified until an in-game comparison is authorized and recorded.

## Build and checks

Run `tools/input-control/build.sh` to build the console client.
Run `tests/check-measurements.sh --wine` for the existing checks and standalone Wine tests.
Those tests do not launch SC6.
The input test exercises the endpoint and runs the actual client against the test process.
