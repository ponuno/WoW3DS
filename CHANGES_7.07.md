# 7.07 — fixes from a tester's report (cumulative: includes the 7.06 patch)

Copy the `wow3ds/` folder over your source tree (same paths), then `make` in proto3ds/ as usual.

1. **Beeps instead of spell sounds.** With a sound pack the fight's events (spell cast / fail, blows, spell damage, falls, swings)
   played built-in sine blips, because the pack never had files for them. Now: the client's own file or nothing, as in the original.
   Spell damage no longer plays a second sound on top of the spell's own impact; the damage school is read as 1.12's index.
   The client patch alone removes the beeps. Optional: rerun the sound conversion with the new w3dconv.exe (sound only) to get a
   real whoosh for misses, a fizzle for failed casts and a flesh hit where a blow has no sound of its own.
2. **Hunter's pet.** The pet window offered only "Abandon" (deletes the pet on the server for good). Now: Dismiss (Dismiss Pet spell,
   Call Pet brings it back) and Abandon separately (Abandon asks "FOR GOOD?"). Red warnings when the pet is unhappy (an unfed,
   freshly tamed pet runs away for good after ~20 min) and when it runs away; Call Pet's answers are shown ("no pet to call",
   "your pet is dead: use Revive Pet"). A pet that already ran away / was abandoned is gone server-side: tame a new one.
3. **"Item not found" on items that begin a quest.** Using such an item now asks it for its quest (as the original), the quest
   window opens and can be accepted.

Tests: pet_test, useitem_test, sound_test, sound_world_test, run_ui (62 scenes), run_stage5, ARM compile + link check.
Not yet tried on the console.
