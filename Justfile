# Justfile pour mitsubishi2zigbee
# Utilisation : `just <recette>` ou simplement `just` pour afficher l'aide

set dotenv-load := false

# Affiche la liste des commandes disponibles
default:
    @just --list

# Compile le firmware (PlatformIO)
build:
    cd firmware && pio run

# Téléverse le firmware sur l'ESP32 via USB
upload:
    cd firmware && pio run --target upload

# Ouvre le moniteur série à 115200 bauds avec décodage des crashs ESP32
monitor:
    cd firmware && pio device monitor

# Compile, téléverse et ouvre immédiatement le moniteur série
flash: upload monitor

# Nettoie les fichiers de compilation (.pio/)
clean:
    cd firmware && pio run --target clean

# Exécute les vérifications électriques ERC et de routage DRC sur KiCad
check-pcb:
    kicad-cli sch erc hardware/mitsubishi2zigbee.kicad_sch --severity-all
    kicad-cli pcb drc hardware/mitsubishi2zigbee.kicad_pcb --schematic-parity

# Régénère les typons (PDF A4 échelle 1:1 et SVG rogné) pour la gravure
typon:
    @mkdir -p hardware/typon
    kicad-cli pcb export pdf -l F.Cu,Edge.Cuts --black-and-white --drill-shape-opt 1 --scale 1 --mode-single -o hardware/typon/typon_echelle1_A4.pdf hardware/mitsubishi2zigbee.kicad_pcb
    kicad-cli pcb export svg -l F.Cu,Edge.Cuts --black-and-white --drill-shape-opt 1 --scale 1 --mode-single --page-size-mode 2 -o hardware/typon/typon_echelle1_carte.svg hardware/mitsubishi2zigbee.kicad_pcb
    @echo "✅ Typons régénérés dans hardware/typon/"
