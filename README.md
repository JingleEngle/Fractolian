# Fractolian

VCV Fractal Oscillator by JingleEngle for VCV Rack 2.

Fractolian 2.3.6 is a 28 HP oscillator with up to 16 voices. Blend four seeded fractal sources on the XY pad, choose from ten formulas, and save up to 16 tones. Connect a pitch cable to 1V/OCT and patch OUT to hear it.

## Downloads

[Windows x64 plugin](https://raw.githubusercontent.com/JingleEngle/Fractolian/main/builds/Fractolian-2.3.6-win-x64.vcvplugin)

## Build

With the Rack SDK 2.6.6 for your platform and the Rack build tools installed, run this from the repository root:

```sh
make -j2 RACK_DIR=/absolute/path/to/Rack-SDK dist
```

The installable `.vcvplugin` package appears in `dist/`. On macOS you can also run `Build Fractolian.command`. The macOS workflow in GitHub Actions builds separate packages for Apple Silicon and Intel Macs.

Existing patches use the plugin ID `JingleEngleFractal` and module ID `FractalXY`.

## License

Fractolian is licensed under the GNU General Public License, version 3 or any later version (`GPL-3.0-or-later`). See [LICENSE](LICENSE).
