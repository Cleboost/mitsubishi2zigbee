{ pkgs ? import <nixpkgs> {} }:

pkgs.mkShell {
  name = "mitsubishi2zigbee-dev-shell";

  packages = with pkgs; [
    # Firmware build and flashing
    platformio
    platformio-core
    esptool
    python3
    python3Packages.pip
    python3Packages.virtualenv

    # Hardware & EDA
    kicad

    # Zigbee2MQTT external converter testing / linting
    nodejs_22

    # Utilities
    git
    gnumake
    just
  ];

  shellHook = ''
    echo "=========================================================="
    echo " ❄️  Environnement de développement mitsubishi2zigbee ❄️"
    echo "=========================================================="
    echo " Outils disponibles :"
    echo "   - platformio (pio) : Compilation et téléversement firmware"
    echo "   - esptool.py       : Flashage direct ESP32"
    echo "   - kicad / kicad-cli: CAO électronique & export typons"
    echo "   - node / npm       : Zigbee2MQTT converter"
    echo "   - python3          : Scripts de release OTA"
    echo "=========================================================="
  '';
}
