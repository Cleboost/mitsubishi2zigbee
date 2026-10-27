# Typon pour gravure simple face (Perchlorure de fer)

Ce dossier contient les fichiers prêts pour l'impression de votre typon (transparent / calque) à l'**échelle 1:1 exacte**.

## Fichiers fournis

1. **`typon_echelle1_A4.pdf`** : Page A4 prête à imprimer directement (échelle 100%, noir et blanc).
2. **`typon_echelle1_carte.svg`** : Fichier vectoriel rogné aux dimensions de la carte + repères de coupe.

## Caractéristiques du typon

* **Contour du PCB réel (Edge.Cuts) :** Rectangle intérieur de 30 × 45 mm (trait fin de 0,1 mm) indiquant la taille finale de la carte époxy.
* **Cadre de découpe du papier transparent :** Un grand rectangle extérieur noir (40 × 55 mm, trait de 0,25 mm) situé à **5 mm tout autour du circuit**. C'est le guide de coupe au cutter ou aux ciseaux pour découper le transparent sans risquer d'abîmer le circuit.
* **Mires en croix aux 4 angles :** 4 grandes croix de repérage (6 mm) situées sur chaque coin du cadre de découpe du transparent.
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
