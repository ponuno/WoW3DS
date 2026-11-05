# 7.08 (cumulative: 7.06 + 7.07 + this)

1. **Intro cinematics: the camera no longer jumps around.** The converter read the camera path with a guessed key layout and guessed
   tangents. It now detects the key size from the data and draws a smooth curve through the keys.
   **Needs the new w3dconv.exe and the sound conversion again** (sound.bat) - cinematics.txt is made there.
2. **A / B skip the intro** (the walking controls swallowed the press).
3. **Zeppelins / ships carry you.** Standing on the ship's own floor puts you on board (it used to need a hand-measured deck box,
   which the zeppelin did not match: you stayed bound to the ground and slid off). Check perf_events.txt for "ride: on transport".
4. **The spirit healer answers.** Its menu opened hidden behind the death page; now its offer comes up at once (Accept / Decline).
