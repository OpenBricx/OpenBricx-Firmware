# OpenBricx Deck — post-build packaging.
#
# After every `pio run`, drop two clearly named images into deck/dist/:
#   openbricx-deck-ota-v<ver>.bin       — app-only image; upload THIS over Wi-Fi OTA.
#   openbricx-deck-v<ver>-firmware.bin  — merged bootloader + table + app; USB cold-flash @ 0x0.
#
# The version is read from src/config.h, the single source of truth.

Import("env")  # noqa: F821  (injected by PlatformIO)
import os
import re
import shutil

platform = env.PioPlatform()


def fw_version():
    cfg = os.path.join(env.subst("$PROJECT_DIR"), "src", "config.h")
    try:
        with open(cfg, "r", encoding="utf-8") as f:
            m = re.search(r'#define\s+OBX_FW_VERSION\s+"([^"]+)"', f.read())
            if m:
                return m.group(1)
    except OSError:
        pass
    return "0.0.0"


def package_bins(source, target, env):
    ver = fw_version()
    build_dir = env.subst("$BUILD_DIR")
    dist = os.path.join(env.subst("$PROJECT_DIR"), "dist")
    os.makedirs(dist, exist_ok=True)

    app_bin = os.path.join(build_dir, "firmware.bin")

    # 1) OTA app image — this is what the Console uploads over Wi-Fi.
    ota_out = os.path.join(dist, "openbricx-deck-ota-v%s.bin" % ver)
    shutil.copyfile(app_bin, ota_out)

    # 2) Merged full-flash image — bootloader + partition table + otadata + app,
    #    laid out at the real partition offsets for a USB cold flash at 0x0.
    esptool = os.path.join(platform.get_package_dir("tool-esptoolpy"), "esptool.py")
    flash_size = env.BoardConfig().get("upload.flash_size", "4MB")
    merged_out = os.path.join(dist, "openbricx-deck-v%s-firmware.bin" % ver)

    cmd = (
        '"{py}" "{tool}" --chip esp32s3 merge_bin -o "{out}" '
        "--flash_mode dio --flash_freq 80m --flash_size {fs} "
        '0x0 "{boot}" 0x8000 "{parts}" 0xf000 "{otad}" 0x20000 "{app}"'
    ).format(
        py=env.subst("$PYTHONEXE"),
        tool=esptool,
        out=merged_out,
        fs=flash_size,
        boot=os.path.join(build_dir, "bootloader.bin"),
        parts=os.path.join(build_dir, "partitions.bin"),
        otad=os.path.join(build_dir, "ota_data_initial.bin"),
        app=app_bin,
    )
    env.Execute(cmd)

    print("")
    print("  Packaged firmware v%s -> deck/dist/" % ver)
    print("    OTA  (Wi-Fi): openbricx-deck-ota-v%s.bin" % ver)
    print("    Full (USB) :  openbricx-deck-v%s-firmware.bin" % ver)
    print("")


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", package_bins)
