# Sparkium JSON scenes

Each scene is stored in its own directory with `scene.json` as the entry point. Asset paths are resolved relative to
the JSON file, so every scene directory is self-contained and can be moved or archived on its own. Absolute paths and
parent-relative paths are also accepted.

Version 1 contains `film`, `renderer`, `camera`, named `materials`, named `geometries`, and an `entities` array. It
supports Lambertian, specular, Principled, and emissive materials; sphere, OBJ, and inline mesh geometry; mesh instances
and point lights. Mesh transforms can use translation/rotation/scale, a 16-value column-major matrix, or `look_at`.

The original Sparks demos are represented by these scene directories:

- `area_light`
- `cornell_box`
- `point_light`
- `principled`
- `specular`
- `texture`

Render a scene without a window:

```bash
./build-release/demo/sparkium_cli/demo_sparkium_cli assets/scenes/cornell_box/scene.json \
  --frames 32 --output cornell_box.png
```

Browse all scenes interactively:

```bash
./build-release/demo/sparkium_gui/demo_sparkium_gui assets/scenes
```

See `scene.schema.json` for the machine-readable format.
