# Input handling and polling research, 2026-11-01

## Implementation update

The user set the priorities after this review: fix retained input, remove online checks, and increase controller polling.
The user identified CrossOver Preview as using CodeWeavers Wine 11.17.
The upstream source below provides background. It does not verify that fork's exact behavior.

The implementation now does the following:

1. It clears retained input when a controller disconnects, including a reconnect before the next engine read.
2. It removes the mod's online-status checks because this installation is for offline play.
3. It adds an independent reader with a default target of 250 reads/s for each of four controller slots.
4. It leaves GPU waits unchanged because their effect on input latency remains unmeasured and has lower priority.

`SC6VRR_INPUT_POLL_HZ` selects the target rate. Use `120` for 120 reads/s, or `0` for engine reads only.
The maximum is 1,000 reads/s. The OS can delay wakeups, so these values are targets.
The log reports completed polling cycles each second through `poll_cycles`.
One cycle checks four slots, but disconnected slots retry only once per second to avoid repeated device enumeration.
Engine reads continue and can discover reconnects sooner. They do not count toward that field.
The reader starts on the first engine frame after the input hook becomes available.
It requires `SC6VRR_FRAME_MODS=1` and an installed input hook from the latch, early-step option, or input trace.

The reader retains observed presses under a lock. The game thread consumes them and changes the battle gate.
A short observed press therefore survives until an engine read, even if the physical button has already been released.
The latch then retains that press until a battle step, as before.
This keeps the existing set of button bits. It does not preserve the order of multiple transitions between steps.
No game launch or in-game test is authorized for this change.

## Original review before implementation

The original review below describes the code at `3b01edf`.
Its online-check proposal is superseded by the user's offline-only instruction.
Its measurement-first priority is superseded by the implementation order above.

The best next changes concern correctness and measurement before increasing polling frequency.
The current code has several places where retained input or delayed checks could affect play.
These are source findings and proposed experiments. No live latency improvement has been measured.

## Reviewed state

- `HEAD` and fetched `origin/native-metal-capture` both point to `3b01edf`.
- Recent commits add the latch, early steps, input traces, gate counts, and `pad-watch`.
- `33ce55b` gates the Training input display. Its final explanation supersedes earlier notes about doubled movement.
- `711142a` enables the latch and early steps by default, but both require the gate.
- The repository's sample configuration disables `SC6VRR_FRAME_MODS`, `SC6VRR_UNCAP`, and `SC6VRR_GATE`.
- The installed configuration enables those three settings, both input options, and `SC6VRR_BLOCK_QUERY_WAIT`.
- `local.sc6vrr.install-dll` is loaded. Building `dll/build/dinput8.dll` can therefore replace the installed mod automatically.

## 1. Correct the latch lifecycle

`hook_xinput_get_state` returns on an error without clearing `xi_last[user]` or `xi_sticky[user]`.
Therefore, a disconnected pad retains its previous button state inside the mod.
A replacement pad in that slot can inherit a pending press if no intervening step cleared it.
If the replacement holds the same button, the retained previous state can suppress detection of a new press.
These paths are visible in source. Their frequency during actual reconnects is unverified.

Clear both words when a valid slot reports `ERROR_DEVICE_NOT_CONNECTED`.
Do not treat every error as a disconnect because an invalid call should not erase valid state.
Test reconnects with a released button, the same held button, and a different held button.
Test all four slots independently.

The latch currently applies in menus and online sessions whenever its hook is installed.
Only early steps check `input_in_battle` and `input_online`.
Decide explicitly whether retaining short taps online is intended before changing that behavior.

## 2. Check online state immediately before an early step

`input_frame` refreshes `input_online` at most once every 30 engine frames.
At 120 frames/s, 30 frames take 250 ms. At 60 frames/s, they take 500 ms.
An online session can therefore begin while the cached value still says offline.
The initial value is zero, which means offline.

Refresh the online check immediately before granting an early step.
Keep the existing requirement that the game must already be running a battle.
Measure the check's cost because this function enters game code.
Test the transition from offline play to online play, rather than only a session that is already online.

## 3. Preserve the order of short input sequences

`xi_sticky` stores a set of button bits. A set cannot retain event order.
For example, a short left press followed by a short right press can retain both directions simultaneously.
The current latch preserves taps that an engine read observed, but it cannot recover taps between engine reads.
It cannot represent two presses of the same button before a battle step as two separate events.

First measure these cases with the existing trace and the Training input display.
Then consider retaining action buttons separately from directions.
An ordered queue could retain transitions, but replaying them over later steps could change command timing.
Do not introduce that queue without defining how transitions map to the game's 60 Hz input history.
Test quarter circles, double taps, opposing directions, simultaneous buttons, and release-sensitive moves for both players.

## 4. Improve the GPU wait before adding a controller polling thread

`hook_query_wait` waits in 1 ms intervals and checks GPU completion after each timeout.
This is GPU polling, rather than controller polling.
If the GPU finishes just after a check and its event arrives late, completion waits until another timeout.
With ideal 1 ms wakeups, that adds up to roughly 1 ms. OS scheduling can add more.
If this delays the next engine frame, the next controller read is delayed too.
That link is plausible, but it needs a live measurement.

The source records an earlier observation of events arriving more than 20 ms late.
This review did not reproduce that observation.

The loop counts requested wait time with `waited += 1`, rather than measuring elapsed time.
Therefore, its stated 500 ms ceiling is not a strict elapsed-time ceiling when wakeups arrive late.
Use a deadline from `QueryPerformanceCounter` to bound the hook's own wait.
The original function can still wait afterward, so this does not bound the entire call.

Compare the current loop with an experiment that briefly checks completion before blocking.
Keep any active checking bounded because otherwise it can consume a CPU core.
Prefer correcting delivery of the DXMT event if that delay is reproducible.
Measure CPU use and frame delays together because reducing CPU use can increase latency.

## 5. Separate device reports from XInput reads

Current upstream Wine reads controller reports on an update thread.
`XInputGetState` then returns the current stored state.
Therefore, more calls can observe stored changes sooner, but they do not necessarily request new device reports.
The installed CrossOver fork may differ. Its exact path remains unverified.

Sources: [Wine XInput implementation](https://github.com/wine-mirror/wine/blob/master/dlls/xinput1_3/main.c).
Relevant functions are `hid_update_thread_proc`, `set_current_state`, and `xinput_get_state`.

A separate polling thread could capture a press and release between engine reads.
However, the current hook ignores reads from other threads, so a thread alone would not feed the existing latch.
It would require a safe transfer of events to the game thread.
Keep UE4 calls and changes to `gate_acc` on the game thread.

Before implementing this, compare 120, 250, 500, and 1,000 reads/s in a separate diagnostic program.
Record actual intervals, changed states, CPU use, and missed short taps.
Do not interpret an unchanged state as proof that no device report arrived.

## 6. Measure timers rather than applying Windows timer advice directly

Microsoft documents that `timeBeginPeriod` can improve timeout accuracy on Windows.
It does not improve the accuracy of `QueryPerformanceCounter`.
Source: [Microsoft timer documentation](https://learn.microsoft.com/en-us/windows/win32/api/timeapi/nf-timeapi-timebeginperiod).

Upstream Wine's `NtSetTimerResolution` reports a fixed 1 ms value and does not apply requested resolution changes.
This return value does not establish the actual accuracy of short waits.
Source: [Wine synchronization implementation](https://github.com/wine-mirror/wine/blob/master/dlls/ntdll/unix/sync.c).
Therefore, adding `timeBeginPeriod(1)` is not an evidence-backed fix for this CrossOver setup.

Windows supports `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` for short time-sensitive waits.
Its documented Windows behavior does not establish the behavior of CrossOver on macOS.
Source: [Microsoft waitable timer documentation](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-createwaitabletimerexw).

Benchmark `Sleep(1)`, an event timeout of 1 ms, and supported waitable timers in the actual bottle.
Measure each wait with `QueryPerformanceCounter` under idle conditions and during a battle.
Report the median, the 95th percentile, the 99th percentile, and the maximum.
Compare those delays with the 8.333 ms engine-frame budget at 120 frames/s.

## 7. Account for early-step timing and slow rendering

Early steps retain accumulator debt, so moving a step earlier does not create a free extra step.
However, retaining the long-run step count does not preserve the real-time interval between every pair of steps.
At ideal 120 frames/s, an isolated early step can change adjacent 16.667 ms intervals to 8.333 ms and 25 ms.
Thus, game windows keep their step counts, but their real-time placement can change.
This should be tested for defensive actions and for both players.

The gate releases at most one step per engine frame.
Therefore, rendering below 60 frames/s cannot sustain 60 battle steps/s through this gate.
A numerical reproduction of its accumulator scheduled 450 steps during 10 seconds at 45 frames/s.
The same model scheduled 600 at 60, 80, 120, and 240 frames/s.
At 144 frames/s, floating-point boundary rounding produced 599 in that exact 10-second model.
These are model results, rather than measurements of completed game steps.
Changing a polling timer cannot remove the one-step-per-frame limit.
Any catch-up design must run the connected battle work and advance input history consistently.

## Measurement plan

1. Record event receipt and observation times on a shared clock.
2. Record XInput reads, input-history writes, and completed battle steps with timestamps.
3. Record GPU waits and engine-frame intervals in the same run.
4. Compare stock input, latch only, and latch with early steps in matching offline scenes.
5. Exercise short taps, releases, motion commands, reconnects, and both players.
6. Verify online transitions with early steps disabled immediately when the session becomes online.
7. Repeat timer comparisons with the same scene and the same actions.

`pad-watch` currently records HID event timestamps, but its GameController handlers record callback receipt time.
Those timestamps describe different points, so their difference is not automatically device latency.
Its two streams start relative to the logger's own start, while the game's trace has frame numbers without timestamps.
Add a shared time reference before subtracting times across those logs.

`log_msg` flushes its file after every line.
Therefore, detailed input tracing adds synchronous file work on the game thread.
Use a bounded buffer and write batches for latency experiments. Report dropped records explicitly.
Software timestamps cannot measure the delay from a physical press to visible pixels by themselves.
That measurement needs an external reference, such as a suitable high-speed recording.

## Recommended implementation order

1. Reset disconnected slots and verify online state before early steps.
2. Add timestamps and buffered diagnostics without changing input behavior.
3. Measure actual waits and GPU event delivery in SC6-FEX.
4. Test an elapsed-time deadline and bounded GPU polling.
5. Consider independent input capture only if traces show missed transitions between engine reads.

The original research did not change runtime code. See the implementation update above for the subsequent changes.

## Validation of the implementation

The DLL built successfully with the repository's normal compiler flags.
`tests/check-measurements.sh --wine` passed all 27 Python tests and the existing standalone Wine tests.
After adding the retry delay for disconnected slots, the DLL and `test-clock-cave.exe` were rebuilt.
The final standalone input test passed under CrossOver Preview in `SC6-Rosetta`.
It checks captured taps, reconnects, slot isolation, independent reader operation, and retries for empty slots.
These tests do not launch SoulCalibur VI.
The game's achieved polling rate, CPU cost, and input latency remain unverified.
The existing installer copied the rebuilt DLL into the game folder automatically.
