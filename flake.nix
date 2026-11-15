{
  description = "Development shell for mitsubishi2zigbee (firmware, KiCad, Z2M converter)";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = {nixpkgs, ...}: let
    inherit (nixpkgs) lib;
  in {
    # NixOS: imports = [ inputs.mitsubishi2zigbee.nixosModules.platformio-udev ];
    nixosModules.platformio-udev = {pkgs, ...}: {
      services.udev.packages = [pkgs.platformio-core.udev];
    };

    devShells = lib.genAttrs [
      "x86_64-linux"
      "aarch64-linux"
      "x86_64-darwin"
      "aarch64-darwin"
    ] (system: let
      pkgs = nixpkgs.legacyPackages.${system};
    in {
      default = pkgs.mkShell {
        name = "mitsubishi2zigbee-dev-shell";

        packages = with pkgs; [
          platformio
          platformio-core
          esptool
          python3
          python3Packages.pip
          python3Packages.virtualenv

          kicad

          nodejs_22

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
          if [ "$(uname -s)" = Linux ]; then
            if [ ! -e /etc/udev/rules.d/99-platformio-udev.rules ]; then
              echo ""
              echo " NixOS: services.udev.packages = [ platformio-core.udev ]; then nixos-rebuild switch"
              echo ""
            fi
            export PLATFORMIO_SETTING_DISABLE_UDEV_RULES_CHECK=true
          fi
        '';
      };
    });
  };
}