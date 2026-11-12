# 7.13 — fixes from GitHub issues #10 and #15

Client only: rebuild `w3dproto.3dsx`; no conversion is needed.

**Caves (#15).** The 7.12–7.12d fixes from `release/7.12` are now on `main` as well (they were only on the release
branch): the slope limit no longer walls off a cave entrance, the camera stays under the terrain in caves and crypts, a
gap in a cave's floor no longer lifts the walker onto the hill above, a gap in its wall is a wall, and a step into a
"held spot" inside the rock is refused. The tester's `perf_events.txt` from Shadowthread Cave and Fel Rock Cave are
exactly these cases (Fel Rock: feet on the cave floor at 1328.48 with the terrain 1481.40 above, then thrown onto that
hill; Shadowthread: `no floor ... height kept` around 10783, 937 and the held spot inside the rock).

- **Charge no longer leaves the model behind.** The server moves a charging player along an `SMSG_MONSTER_MOVE` spline, not
  with the movement packets we send. The client now follows a running spline on its own object (the same path a taxi uses),
  sends no movement meanwhile, and puts the mover at the exact end of it. Before, the server's hitbox charged while the
  drawn character stayed where it was.
- **Cannibalize works on a corpse again.** A dead target was treated as "no living target": the soft-target pass replaced
  it with a living enemy, and the cast carried that enemy as its unit target. Cannibalize (20577) now keeps the corpse (it
  can also be cast with no target), faces it, and goes out without a unit target - vMaNGOS finds an edible corpse near the
  caster itself.
- **Feed Pet finds food in the bags.** The 1.12 server sends subclass 0 for *every* consumable ("client known only 0
  subclass" in vMaNGOS), so Food & Drink could never match subclass 5. Every consumable is offered now (the server checks
  the pet's diet), together with Trade Goods / Meat. The list also asks for the item data of bag items it has not seen yet.
- **Ranged abilities keep their own icons.** Only Attack, Auto Shot, Shoot and Throw draw the weapon in hand; the server
  marks every ranged ability (Arcane Shot, Serpent Sting, Multi-Shot ...) as ranged for the shooting animation, and the old
  fallback gave all of them the bow icon. A spell whose picture is missing from an old pack still falls back to the weapon.

Tests on the PC: `net/tests/run_713.sh` (`weaponicon_test`, `feed_test`, `cannibalize_test`, `selfmove_test`), plus the
existing `run_pet.sh` (`pet_test`, `selfcast_test`) and a full devkitARM build of `w3dproto.3dsx` / `w3dproto.cia`.
Not checked on the console yet.
