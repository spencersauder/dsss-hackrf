# Windows packaging (portable + installer)

This project is Linux-first, but you can build a portable Windows bundle and an installer using MSYS2/MinGW and Inno Setup.

## 1. Build in MSYS2 (mingw64 shell)
```bash
pacman -S --needed ^
  mingw-w64-x86_64-toolchain mingw-w64-x86_64-pkg-config ^
  mingw-w64-x86_64-gtk3 ^
  mingw-w64-x86_64-soapysdr ^
  mingw-w64-x86_64-liquid-dsp ^
  mingw-w64-x86_64-libhackrf

# then in mingw64 shell:
./configure
make -j4
```
The binary will be at `src/dsss-transfer-gui.exe`.

## 2. Collect runtime DLLs into `dist/`
In the mingw64 shell:
```bash
mkdir -p dist
cp src/dsss-transfer-gui.exe dist/

for f in $(ldd src/dsss-transfer-gui.exe | awk '/mingw64/ {print $3}'); do
  cp "$f" dist/
done

# SoapySDR plugins (HackRF, etc.)
mkdir -p dist/share/SoapySDR/modules0.8
cp /mingw64/share/SoapySDR/modules0.8/*.dll dist/share/SoapySDR/modules0.8/

# (Optional) GLib schemas, if required by your build
mkdir -p dist/share/glib-2.0/schemas
cp /mingw64/share/glib-2.0/schemas/*.xml dist/share/glib-2.0/schemas/
glib-compile-schemas dist/share/glib-2.0/schemas
```

Typical DLLs you need (version names may differ):
- GTK stack: libgtk-3-0.dll, libgdk-3-0.dll, libpangocairo-1.0-0.dll, libpango-1.0-0.dll, libharfbuzz-0.dll, libcairo-2.dll, libgdk_pixbuf-2.0-0.dll, libgio-2.0-0.dll, libgobject-2.0-0.dll, libglib-2.0-0.dll, libintl-8.dll, libiconv-2.dll, zlib1.dll, libepoxy-0.dll, libpangoft2-1.0-0.dll, libfontconfig-1.dll, libfreetype-6.dll, libpng16-16.dll, libpixman-1-0.dll, plus image loaders (jpeg/tiff/webp) if pulled in.
- SDR: libSoapySDR.dll, libhackrf.dll, plugins in share/SoapySDR/modules0.8/.
- DSP: libliquid.dll.
- Runtimes: libstdc++-6.dll, libwinpthread-1.dll, libgcc_s_seh-1.dll.

Test on a clean Windows VM (no MSYS2/GTK installed): copy `dist/`, run `dsss-transfer-gui.exe`. If it starts, your DLL set is good.

## 3. Build installer (Inno Setup)
We provide `windows/installer.iss`. Install Inno Setup and run:
```bash
iscc windows/installer.iss
```
This will produce `dsss-transfer-setup.exe` using the contents of `dist/`.

## 4. Distribute
- Portable ZIP: zip the `dist/` folder.
- Installer: use the generated `dsss-transfer-setup.exe`.

## Notes
- If you change dependencies or GTK version, repeat the ldd copy step.
- Make sure HackRF driver/SoapySDR plugin DLLs are present (libhackrf, Soapy HackRF plugin).
- For a cleaner cross-platform build, consider moving to CMake + cpack (NSIS/Inno), but for quick packaging this flow is simpler.


