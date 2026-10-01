# BBattery

Native BlackBerry Q10 battery measurement and history application.
The GUI remains in its application sandbox. A separate, supervised collector
reads the public BPS battery service and records data even when the GUI exits.
The existing authorized root management context installs the collector;
the collector drops its UID and GID before sampling, retaining only group 1000
to traverse the account directory and this application's sandbox GID.

## Build

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Package
python tests/package_test.py
python tools/device.py install
python tools/device.py launch
python tools/device.py provision
python tools/device.py boot-hook
```

The build uses `INTROOP_SDK_ROOT` from the current process, falling back to the
user environment variable if the terminal has not inherited it yet. The current
SDK is `C:\bbdevtools`; pass `-SdkRoot` to override it. Icons are read from the
neighbouring `BBFile` project, relative to this checkout.

Deployment reuses `%LOCALAPPDATA%\Q10Manager\connection.json`, falling back to
the existing `%LOCALAPPDATA%\Q10Deploy\config.json` profile when the former is
absent, without changing credentials or host-key policy. ARM binaries use the established
BB10 `gcc_ntoarmv7le_cpp` / `libcpp.so.4` runtime.

## Measurement Boundaries

- Missing, out-of-range and sentinel readings become SQL NULL, never zero.
- The displayed current is battery **average** current, not instantaneous current.
- Charger input and charge limits are not measured battery current.
- SOC, health and cycle count remain explicitly system-reported values.
- Raw PPS snapshots are retained and deduplicated independently of timed samples.
- Restart, clock change, battery change, pause and large gaps create new segments.
- mAh and mWh use trapezoidal integration over valid adjacent readings.
- Capacity extrapolation requires a completed discharge segment, at least 30
  percentage points of SOC, 95% integration coverage and stable SOC. It is not
  a calibrated full-capacity measurement.
- Capacity estimates are shown separately for discharge integration, charge
  integration, remaining mAh / SOC and design capacity × reported health.
  Charge estimates use battery current, require a completed interval with the
  same SOC-span/coverage guards, and validate SOC direction in legacy records.
  Remaining/SOC estimates require SOC >= 20%. Reported full capacity and design
  capacity remain distinct reference values. Missing inputs show `--`.
  A session/id index keeps legacy SOC-direction checks bounded on long intervals.
- The initial collector interval is 10 seconds; 5, 30 and 60 seconds are available.
- History is stored in the app's private data directory, not in `/tmp`.
- The GUI shows the latest 200 segments. CSV export includes the full history.
- No history is silently deleted.

## Native History And UI

The four screens are Overview, History, Records and Diagnostics. Their title
bars are removed and the compact two-column layout targets Q10. Overview keeps
SOC, battery average current, temperature, charge/discharge capacity estimates
and the current process visible.
Voltage, reported health/cycles, charger limits, snapshot-based capacity
estimates and detailed data-quality statistics live in Diagnostics.

History supports 1/6/24 hours and rolling 7/30 days, previous/next windows,
calendar browsing, curve visibility and synchronized inspection of raw samples.
The overview always shows the latest hour, independent of historical browsing.
Empty periods remain empty; the chart does not stretch old data to fill them.
All raw records contribute to time-weighted statistics and trapezoidal mAh/mWh.
Only rendering is decimated, retaining extrema with a bounded point count.
Inspection moves a shared native cursor without repainting the chart images.
Drag events are coalesced at 33 ms; indexed raw-sample lookups run off the UI
thread, reuse neighbouring samples and discard stale requests. The readout has
a fixed height so gaps and the first touch do not move the chart beneath a finger.
Read failures are marked incomplete rather than presented as complete statistics.
History statistics run on a worker thread with short, paged read statements
so the UI stays responsive and the collector can commit between pages.
Long live ranges refresh at 30 seconds (24 hours) or 60 seconds (7/30 days);
their actual cutoff time stays visible.

Timed collection also reacts to BPS battery events, coalesced to at most one
extra capture per two seconds without moving the regular timer deadline.
Power transitions, charge/discharge transitions and configurable threshold
alerts are stored in an additive `events` table. Alerts have 5-percentage-point
SOC hysteresis, 3-degree temperature hysteresis and a ten-minute per-kind
persistent cooldown. These are recording/in-app alerts, not OS background
notifications or automatic charge control.

When BPS charger info is unavailable, the native PPS decoder can resolve the
charger state from the retained snapshot. Unknown remains unknown. System
charge current and time estimates are separately labeled in Diagnostics.
Historical source records are never rewritten.

ChargeLimiter informed the interaction design only. No iOS private API,
root HTTP server, thermal spoofing, charging-control code or GPL assets were copied.

## Tests

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/test-capacity.ps1
python tests/package_test.py
python tests/device_tools_test.py
python tests/device_test.py
python tests/startup_test.py
python tests/migration_test.py
python tests/inspection_test.py
```

The collector's `--self-test` executes the actual C++ measurement and SQLite
code against isolated temporary data. Device tests verify the real application,
its own-window screenshots, collector identity, data validity and GUI-independent
collection. Pause and recovery tests never terminate unrelated services.
The startup regression test performs three real launches and switches between
current, voltage and temperature charts. Reports are saved under `build`.

Charts use QPainter only for geometry and images. Cascades labels render all
chart text; QtGui font operations caused a startup segmentation fault on Q10.
Deployment verifies GUI exit from individual `pidin` rows, not its table header,
and prevents multiple GUI instances from racing over diagnostic requests.
Collector executables are read back before execution. Deployment can reuse an
identical verified executable. If the device denies execution on a new inode,
it can retain a retired BBattery executable's existing trust: the file must be
root-only, match its recorded SHA256 and pass native self-tests, and must be
absent from both the running processes and current supervisor. Its original
bytes are backed up before replacement, with rollback on verification failure.
The new executable is verified and renamed to a unique hash-qualified path
before switching the running service. No filesystem-wide trust policy is changed.
The supervisor removes only its verified empty private lock directory; it does
not require the unavailable `rmdir` utility on the device.

The optional boot hook adds a BBattery-only stanza to the existing root startup
source, preserving other applications' stanzas. Installing it is not proof of
cold-boot recovery. No automatic reboot is performed.
