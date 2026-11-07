"""Pre-script de PlatformIO: mantiene sincronizados los recursos web embebidos."""

import sys
from pathlib import Path

Import("env")  # type: ignore[name-defined]  # Proporcionado por PlatformIO/SCons.

SCRIPT_DIR = Path(env["PROJECT_DIR"]) / "scripts"  # type: ignore[name-defined]
sys.path.insert(0, str(SCRIPT_DIR))

# bb_epaper incluye SPI.h desde un .inl y no declara SPI como dependencia en
# library.properties. PlatformIO no inspecciona ese .inl, así que hacemos
# visible el SPI que ya forma parte del framework Arduino-ESP32.
FRAMEWORK_DIR = Path(env.PioPlatform().get_package_dir("framework-arduinoespressif32"))  # type: ignore[name-defined]
env.Append(CPPPATH=[str(FRAMEWORK_DIR / "libraries" / "SPI" / "src")])  # type: ignore[name-defined]

from embed_web import generate  # noqa: E402

generate()
