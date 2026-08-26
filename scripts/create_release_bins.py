Import("env")

from pathlib import Path
import json
import re
import shutil
import subprocess
import sys


PROJECT_DIR = Path(env.subst("$PROJECT_DIR"))
BUILD_DIR = Path(env.subst("$BUILD_DIR"))
RELEASE_DIR = PROJECT_DIR / "release"
MAIN_CPP = PROJECT_DIR / "src" / "main.cpp"


def read_version():
    try:
        text = MAIN_CPP.read_text(encoding="utf-8")
        m = re.search(r'FIRMWARE_VERSION\s*=\s*"([^"]+)"', text)
        return m.group(1) if m else "unknown"
    except Exception:
        return "unknown"


def find_esptool():
    try:
        package_dir = env.PioPlatform().get_package_dir("tool-esptoolpy")
    except Exception:
        package_dir = None

    if package_dir:
        base = Path(package_dir)
        candidates = [
            base / "esptool.py",
            base / "esptool" / "__main__.py",
        ]
        for candidate in candidates:
            if candidate.exists():
                return [sys.executable, str(candidate)]

    return [sys.executable, "-m", "esptool"]


def make_manifest(version, factory_out):
    manifest = {
        "name": "Meinberg UA537TGP Ethernet NTP Server",
        "version": version,
        "new_install_prompt_erase": True,
        "builds": [
            {
                "chipFamily": "ESP32",
                "parts": [
                    {
                        "path": factory_out.name,
                        "offset": 0
                    }
                ]
            }
        ]
    }
    (RELEASE_DIR / "manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n",
        encoding="utf-8"
    )


def make_release_bins(source, target, env):
    version = read_version()
    RELEASE_DIR.mkdir(parents=True, exist_ok=True)

    firmware = BUILD_DIR / "firmware.bin"
    if not firmware.exists():
        try:
            firmware = Path(str(target[0]))
        except Exception:
            pass

    if not firmware.exists():
        print("[release] firmware.bin not found; skipping release packaging")
        return

    ota_out = RELEASE_DIR / f"meinberg-ntp-{version}-ota.bin"
    factory_out = RELEASE_DIR / f"meinberg-ntp-{version}-factory.bin"
    shutil.copy2(firmware, ota_out)
    print(f"[release] OTA: {ota_out}")

    # Some PlatformIO/ESP-IDF combinations already create a merged factory image.
    prebuilt_candidates = [
        BUILD_DIR / "firmware.factory.bin",
        BUILD_DIR / "firmware-factory.bin",
        BUILD_DIR / "merged-binary.bin",
    ]
    for candidate in prebuilt_candidates:
        if candidate.exists():
            shutil.copy2(candidate, factory_out)
            make_manifest(version, factory_out)
            print(f"[release] Factory (prebuilt): {factory_out}")
            print(f"[release] Manifest: {RELEASE_DIR / 'manifest.json'}")
            return

    # PlatformIO's ESP32 builder exposes every non-application flash image here:
    # bootloader, partition table, OTA data etc.  This is more reliable in
    # PlatformIO than depending on ESP-IDF's flasher_args.json being present.
    extra_images = env.get("FLASH_EXTRA_IMAGES", [])
    if not extra_images:
        print("[release] FLASH_EXTRA_IMAGES is empty.")
        print("[release] OTA binary created, factory image not created.")
        return

    flash_parts = []

    # FLASH_EXTRA_IMAGES is normally a list of [offset, path] pairs.
    for item in extra_images:
        if not item or len(item) < 2:
            continue
        offset = env.subst(str(item[0]))
        path = Path(env.subst(str(item[1])))
        if not path.is_absolute():
            path = PROJECT_DIR / path
        if not path.exists():
            print(f"[release] missing extra flash image: {path}")
            return
        flash_parts.append((int(offset, 0), offset, path))

    # Add the application at the exact offset selected by PlatformIO.
    app_offset = env.subst("$ESP32_APP_OFFSET")
    if not app_offset or "$" in app_offset:
        app_offset = "0x10000"
    flash_parts.append((int(app_offset, 0), app_offset, firmware))
    flash_parts.sort(key=lambda x: x[0])

    board = env.BoardConfig()
    chip = board.get("build.mcu", "esp32")
    flash_size = board.get("upload.flash_size", "4MB")
    flash_mode = board.get("build.flash_mode", "dio")

    # PlatformIO's board metadata usually stores frequency as integer Hz.
    freq = board.get("build.f_flash", 40000000)
    try:
        flash_freq = f"{int(freq) // 1000000}m"
    except Exception:
        flash_freq = "40m"

    cmd = find_esptool() + [
        "--chip", str(chip),
        "merge_bin",
        "-o", str(factory_out),
        "--flash_mode", str(flash_mode),
        "--flash_freq", str(flash_freq),
        "--flash_size", str(flash_size),
    ]

    for _, offset, path in flash_parts:
        cmd.extend([str(offset), str(path)])

    print("[release] creating merged factory image from PlatformIO FLASH_EXTRA_IMAGES")
    for _, offset, path in flash_parts:
        print(f"[release]   {offset}  {path}")
    print("[release] " + " ".join(cmd))

    try:
        subprocess.check_call(cmd)
    except Exception as exc:
        print(f"[release] merge_bin failed: {exc}")
        print("[release] OTA binary remains available.")
        return

    if not factory_out.exists():
        print("[release] merge_bin returned without creating factory image.")
        return

    make_manifest(version, factory_out)
    print(f"[release] Factory: {factory_out}")
    print(f"[release] Manifest: {RELEASE_DIR / 'manifest.json'}")


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", make_release_bins)
