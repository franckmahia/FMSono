# FMSono

Application macOS native de sonorisation locale : un micro sur une interface USB (par exemple M-Audio Fast Track) vers la sortie audio du Mac, une enceinte filaire ou une enceinte Bluetooth.

## Lancer

Double-cliquer sur **Lancer FMSono.command**, ou ouvrir **dist/FMSono.app**. Le lanceur construit l’application si nécessaire. macOS 14 minimum ; les Command Line Tools Apple avec Swift 6 ou plus sont nécessaires à la compilation. Le bundle construit ici est destiné à l’architecture du Mac de compilation et porte une signature locale ad hoc, sans notarisation pour distribution publique.

1. Brancher la Fast Track et sélectionner **Micro**, puis son **Canal** (les numéros commencent à 1 dans l’interface).
2. Choisir **Sortie système par défaut** ou une sortie explicite. Pour la Motion Boom, la connecter dans les réglages Bluetooth du Mac et la choisir comme sortie.
3. Régler le volume du Mac et de l’enceinte très bas. Positionner l’enceinte loin du micro et parler près du micro.
4. Cliquer sur **Tester le micro**, puis autoriser l’accès au micro. Ce mode affiche le niveau d’entrée avec une sortie silencieuse. Si l’entrée sature, baisser le gain matériel de la Fast Track.
5. Cliquer sur **Diffuser**. Le gain démarre à **−18 dB** ; l’augmenter progressivement selon le besoin.
6. **Couper**, **Échap** ou **⌘⇧M** arrête la capture et la diffusion. Fermer la dernière fenêtre quitte l’application.

Après une déconnexion, un changement de sortie système, un changement de format/buffer, la mise en veille ou une intervention de coupure automatique, la reprise est manuelle. Le gain revient à −18 dB. Les sélections ne changent pas pendant une session. Seul le dernier identifiant du micro choisi est conservé localement ; aucun gain élevé ni état de diffusion n’est restauré au lancement.

## Protections et limites

- Filtre coupe-bas Butterworth à **90 Hz** et limiteur numérique à **−6 dBFS**, toujours actifs.
- Le profil **Voix parlée** analyse une FFT de 2 048 échantillons tous les 256 échantillons. Une résonance étroite et dominante qui persiste environ 300 ms (ou croît rapidement pendant au moins 120 ms) déclenche un filtre coupe-bande de Q 18 et une baisse supplémentaire de 6 dB du gain. Plusieurs résonances ou une résonance persistante déjà filtrée provoquent une coupure verrouillée jusqu’à une nouvelle session.
- La détection est heuristique : elle ne garantit pas l’absence de larsen, ne couvre pas toutes les situations acoustiques et peut confondre une note musicale tenue avec une résonance. Elle peut être désactivée **avant une session** pour les essais musicaux ; le limiteur, les contrôles de flux et la coupure restent présents.
- Le limiteur plafonne le signal numérique ; il ne garantit pas un niveau acoustique sûr. Le volume de l’enceinte et celui du système sont indépendants.
- Le Bluetooth conserve des buffers hors du contrôle de l’application. Il peut continuer à jouer un court instant après la coupure. La Motion Boom originale A3118 n’a pas d’entrée AUX ; sa latence exacte sur ce Mac n’est pas mesurée. Le délai affiché dans les détails est une **estimation issue des buffers et des informations du pilote**, pas une mesure acoustique et pas une mesure complète de la latence Bluetooth.
- La Fast Track doit être reconnue par macOS. Les anciennes variantes peuvent nécessiter un pilote compatible. Le programme ne remplace pas un pilote USB manquant.

## Architecture

**SwiftUI / AppKit** pour l’interface en français et les notifications de veille. **AVFoundation** pour l’autorisation micro. **Core Audio AUHAL** pour choisir séparément entrée et sortie sans changer les réglages globaux du système et sans installer de périphérique virtuel.

Les callbacks audio sont en C : buffers préalloués, ring buffer mono SPSC de 32 768 échantillons, index/configuration atomiques, aucun accès disque/réseau, aucune allocation ni verrou dans les callbacks. Le canal choisi est recopié vers les deux premières sorties physiques, ou la sortie mono. Les autres canaux de sortie restent silencieux.

Les fréquences d’échantillonnage différentes sont adaptées par interpolation linéaire avec compensation bornée de la dérive des horloges. En réduction de fréquence, un filtre passe-bas de quatrième ordre atténue le repliement spectral. C’est un moteur orienté sonorisation de voix, pas un convertisseur de mastering. Les débordements de buffer, les sous-alimentations répétées et les erreurs Core Audio interrompent la session.

## Construire et vérifier

```sh
./scripts/build-app.sh
./scripts/check.sh
```

La construction utilise Swift Package Manager, puis fabrique le bundle, l’icône et sa signature locale. Le script de vérification crée au besoin un environnement Python **dans ce projet** avec pytest et ruff. Les tests compilent et exécutent le même moteur C que l’application, sans utiliser le micro ni les haut-parleurs.

Les 14 tests couvrent : démarrage silencieux, coupure verrouillée, limiteur, entrées non finies, filtre coupe-bas, résonances persistantes, bruit large bande, adaptation 44,1/48 kHz, réduction 96/48 kHz, dérive des horloges, buffers, flux continu, périphériques invalides et notes brèves.

Validation du 5 octobre 2026 : compilation de production, signature locale, pytest (14 tests), ruff, analyse C avec avertissements traités comme erreurs et contrôle des espaces via Git réussis.

La dernière version a été lancée sur le Mac : la **Fast Track, canal 1**, fournit un signal réel en mode **Tester le micro** (environ −35 dBFS au moment du contrôle). Entrée et sortie MacBook Pro ouvertes à **48 kHz**, zéro coupure de buffer pendant ce contrôle court, sortie numérique silencieuse. L’estimation technique affichée était d’environ **37 ms**, sans mesure acoustique. Le bouton **Couper** et le raccourci **Échap** arrêtent bien la capture ; le bouton de coupure reste visible lorsque les détails sont dépliés.

Le contrôle initial était limité au test silencieux. Un essai utilisateur ultérieur avec la Motion Boom a révélé un écho ou un décalage très important ; la latence acoustique exacte n’a pas été mesurée. Une sortie filaire est préférable pour la sonorisation en direct. Les simulations ne prouvent pas l’efficacité anti-larsen dans une pièce. Poursuivre la validation au casque ou à très faible volume, puis avec l’enceinte dans sa position réelle.
