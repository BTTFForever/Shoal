# Shoal 3D (experimental)

A copy of the Linux port with a third dimension. The original 2D version in `../ShoalLinux` is untouched.

- **Tank:** the bounding box of all monitors, with a depth equal to its width (`--depthscale=100` is 100% of the width).
  Fish swim in all three axes, avoid all six walls, flock, seek and speed-mood as before. Rocks, the old depth-rank
  shadows and desktop capture are removed in this version.
- **Camera:** perspective, sitting in front of the screen at `--camera=100` (percent of the tank depth). The front plane
  (z = 0) maps to the screen one to one; deeper things shrink and fade into the dark water.
- **Fish:** two spheroids per fish (body and tail), ray-cast per pixel in the fragment shader with a light from above left,
  a specular highlight, the head-to-tail colour gradient and depth fog. They write real depth, so fish occlude each other.
- **Background:** a ray-cast room (floor, ceiling, side and back walls) with moving caustics and distance fog.
- **Multi-monitor:** one shared tank; each monitor shows its part of the same camera view.

Build: `make` (needs SDL2, GLEW, OpenGL 4.3, xxd). Run: `./shoal3d`, or `./shoal3d --windowed` (Esc quits).
Options and `~/.config/shoal/settings.conf` are shared with the 2D version, plus `--camera=N` and `--depthscale=N`.
`--boids=N --size=P --sizevar=N --showfps=1 --seconds=N` are handy for testing.
