# GAMEPLAY.md — the Gameplay page (port extras)

**Everything here is a port extra.** The original game has none of these options. Every option's first value is the
original game, and that is the default, so a `woodyre.cfg` without these keys plays exactly as before (checked: W1B, W2D,
W3D, W2B and W1A logs identical to master with every option off). They came from a tester's suggestions after a full
run. Related: MENU_OPTIONS.md (page 0x1b), DISPLAY.md §5 (the Graphics page, the same list class).

## 1. The page

Options (page 0x1b) has a ninth item, **Gameplay** (item 8, under Controls; the page now starts at 0.09 instead of 0.17 to
fit it). It opens port page **0x43**, a choice list like 0x40 / 0x42: left / right change a value, Continue applies and
saves at once, back drops the edit. Both work from the title and from the pause menu in a level (the world stands still
as for 0x40 / 0x42). The texts are translated for the Polish, Spanish, Brazilian and Russian CDs (`k_port_tr`, hud.c;
the Russian fonts lack a capital Ж, so that label starts with another word).

| row | label | values (cfg key) | default |
|---|---|---|---|
| 1 | Skip cutscenes | Off / On (`skip_cutscenes=0/1`) | Off |
| 2 | Lasers and blasts | Cost a life / Cost a heart (`soft_hazards=0/1`) | Cost a life |
| 3 | Enemy first attack | At once / 0.5 s / 1.0 s / 1.5 s (`enemy_delay=` tenths, 0..30) | At once |
| 4 | Lives after game over | As saved / 3 / 5 / 9 (`gameover_lives=0/3/5/9`) | As saved |

## 2. Skip cutscenes

The original has no skip key for the real-time cinematics (CINEMATIC.md §3; only the House intro can be left through its
menu). Their scripts time every event on the game clock: a cinematic starts with `1130`, the script writes its own
timeline as `DELAYPOP (T − var)` on the 1/100 s clock (CINEMATIC.md §7), and the fight intros start the fight with such a
delay (WWS object 344: `1600 − var118`). Jumping to the end would lose or reorder those events, so the skip **fast-forwards**:
the pause key (Esc / Start) during a cinematic (`cin_running`, not in the House, no menu open) sets `g_skip`; until the
cinematic hands back to the player, every frame advances **0.1 s** (the clamp the clock already uses on a slow frame),
the frame cap and vsync are off, the 3D picture is black (`g_black_frame`), the sound effects are muted
(`audio_master(0, music)`) and the cinematic's `/Rtc/` voice track stops. At the end the volumes and vsync come back and
the scene's own fade-in shows the player. The pause menu does not open from that key press.

Checked: W1B fight intro skipped at 2.5 s: the boss starts at the same game time (22.0 vs 22.1 s), the same script
messages in the same numbers (only reordered within a 0.1 s step); a real Esc at 4 s puts Woody in the fight at 8 s
(wall clock). Test hook: `WOODY_SKIPAT=T` skips the first cinematic running at game time ≥ T.

## 3. Lasers and blasts

In the original these kill at once: a laser (class 50, OBJECTS.md §2.1) and the storm's lightning are `Kill(2)` / `Kill(9)`,
a bomb, cannonball or rocket going off within 400 / 600 is `Hit(0)` + `Kill(6)` (BOMB.md, ROCKET.md). With **Cost a heart**,
`player_kill` turns kinds 2, 6 and 9 into a 1-heart `player_hit` (hit animation, 0.6 s invulnerability); the last heart
then gives the ordinary death (kind 3). A blast also lets go of the rocket and of a bomb in his hands, as the Kill does.
`Player.soft_t` keeps a laser he stands in from taking more than one heart a second. The respawn invulnerability still
protects him. Races (`race_char`) keep the original rule. Pits, water, crushers and falls are unchanged.

Checked with `WOODY_KILLAT="2 K"`: kinds 2, 6, 9 take health 3 → 2; at health 1 the hit kills (kind 3); off = kind 2 death.

## 4. Enemy first attack

The original enemy attacks the moment it notices Woody: Reset zeroes every timer, the reload timer included (ENEMY.md),
so a shooter aims in the same frame and a melee enemy dashes as soon as it is close enough. With a delay:

* melee and ghosts (`enemy_update`): the notice state 2 sets `Enemy.react_t`; while it runs the chase goes on (turning,
  running), but no dash, dive or ghost shot starts;
* shooters 7..9 (`shooter_update`): entering the reload wait from patrol or wander (`shooter_react`) raises the reload
  timer to the delay, so the aim windup starts that much later.

Checked (W1A melee 282 / 293, S1A shooters 322 / 323 / 325): notice → dash 0.23 / 0.28 s became 1.01 / 1.02 s; notice →
aim 0.01 s became 1.01 s. The bosses (14..16) and the bomb thrower (12) are unchanged.

## 5. Lives after game over

The original game over (page 0x1d) wipes the active save in memory and goes to the title; loading the slot again brings
back the lives it was saved with (GAMEFLOW.md), so a slot saved with 1 life stays at 1. With 3 / 5 / 9 the game over sets
`gameover_pending=1` in woodyre.cfg (so a restart in between keeps it), and the next slot loaded (page 2) gets every
character's lives raised to at least that number, once; then the flag is cleared. The log says
`LIVES after game over: character c has n, at least N`. A New game is unchanged (9 lives).

Checked with a slot patched to 2 lives (block lives at `0x10 + slot·0x14a4 + char·0x6dc` of woodyre.sav): As saved →
carousel 1 (it shows lives − 1), 5 → 4.
