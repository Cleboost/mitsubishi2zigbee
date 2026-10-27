# Typon pour gravure simple face (Perchlorure de fer)

Ce dossier contient les fichiers prêts pour l'impression de votre typon (transparent / calque) à l'**échelle 1:1 exacte**.

## Fichiers fournis

1. **`typon_echelle1_A4.pdf`** : Page A4 prête à imprimer directement (échelle 100%, noir et blanc).
2. **`typon_echelle1_carte.svg`** : Fichier vectoriel rogné aux dimensions de la carte + repères de coupe.

## Caractéristiques du typon

* **Cadre de détourage :** Tracé noir net de 30 × 45 mm (épaisseur 0,2 mm) matérialisant les contours exacts du circuit.
* **Croix de repérage aux 4 angles :** 4 mires en croix (longueur 4 mm) situées à chaque angle extérieur pour aligner précisément votre découpe ou superposition.
* **Témoin de sens de lecture « F.Cu » :**
  * Gravé directement en cuivre en haut à droite ($x = 137{,}5\text{ mm}$, $y = 33\text{ mm}$).
  * **Règle absolue d'insolation :**
    * Regardez votre typon imprimé : vous devez pouvoir lire le texte **« F.Cu » à l'endroit**.
    * Plaquez ensuite la face encrée de votre transparent directement contre la couche de résine photosensible cuivrée.
    * Après insolation, révélation et gravure au perchlo, les écritures et pistes seront dans le sens parfait sans risque d'inversion.
* **Pointages de perçage Dremel :**
  * Toutes les pastilles traversantes sont imprimées avec un petit point central non exposé (`drill-shape-opt 1`), ce qui crée une amorce de centrage automatique pour la pointe du foret.

## Paramètres d'impression recommandés

* **Mise à l'échelle :** 100 % (désactiver impérativement « Ajuster à la page » ou « Réduire les pages hors format »).
* **Qualité :** Résolution maximale (600 ou 1200 DPI), noir monochrome pur (haute densité d'encre pour bloquer les UV).
