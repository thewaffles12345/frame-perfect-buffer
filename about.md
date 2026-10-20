# Frame Perfect Buffer

Holds an early jump press and fires it on the last viable 240 Hz physics tick.

Click too early for a frame perfect and the mod keeps the press, watches the player every tick, and lets it through at the last moment it can still work: the last tick you're on the ground, the last tick you're touching the orb, or the tick before you'd hit a hazard. If none of those shows up within **Max delay** ticks the press fires anyway.

**Safe mode** (on by default): when a press in an attempt was actually delayed, that attempt's progress is not saved and a completion runs as test mode.

Keep **Max delay** small. Every grounded jump inside the window is moved to its last viable tick, which is only what you want on inputs that would otherwise be late.
