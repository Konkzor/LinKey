# Émulation du firmware LinKey

Ce README décrit la cible d'émulation QEMU du firmware LinKey.

L'émulation est activée par le build via `LINKEY_QEMU_EMULATION=ON`. Elle fournit les dépendances matérielles nécessaires à l'exécution dans QEMU via des mocks au link, tout en exécutant la logique applicative du firmware.

## Conception

Le build d'émulation ajoute le composant `Firmware/emulation/` et wrappe les fonctions liées au matériel au link :

- les lectures de tension retournent une tension de supercondensateur constante ;
- les appels WiFi initialisent QEMU OpenETH ;
- MQTT utilise le chemin `mqtt_manager.c` et se connecte à l'URI du broker QEMU ;
- l'enregistrement du callback ULP est remplacé par la mémorisation de la tâche principale à notifier ;
- une tâche FreeRTOS d'émulation écrit périodiquement une trame TIC (constante) dans les buffers RTC et notifie la tâche principale ;
- les trames TIC de test sont écrites dans les buffers RTC lus par `get_linky_data()`.

La configuration spécifique à QEMU est portée par `emulation/sdkconfig.qemu.defaults` et par l'option CMake `LINKEY_QEMU_EMULATION`.

## Fichiers

```text
Firmware/
  emulation/
    CMakeLists.txt            Composant ESP-IDF d'émulation
    linkey_qemu_mocks.c       Mocks QEMU par wrapping au link
    qemu_test_data.h          Trames TIC déterministes
    sdkconfig.qemu.defaults   Paramètres sdkconfig réservés à QEMU
  run_qemu_linkey.sh          Lanceur QEMU pour les artefacts build-qemu-*
```

## Build

Depuis `Firmware/` :

```bash
mkdir -p build-qemu-base
printf '%s\n' 'CONFIG_LINKEY_TARIFF_BASE=y' > build-qemu-base/sdkconfig.tariff.defaults

idf.py -B build-qemu-base \
  -DSDKCONFIG=build-qemu-base/sdkconfig \
  '-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;emulation/sdkconfig.qemu.defaults;build-qemu-base/sdkconfig.tariff.defaults' \
  -DLINKEY_QEMU_EMULATION=ON \
  -DLINKEY_QEMU_TARIFF_OPTION=BASE \
  build
```

Le lanceur utilise par défaut un build séparé par option tarifaire :
`build-qemu-base/`, `build-qemu-hphc/`, `build-qemu-ejp/` ou `build-qemu-tempo/`.

## Exécution

Après le build :

```bash
./run_qemu_linkey.sh
```

Le lanceur utilise les artefacts du build QEMU correspondant à l'option tarifaire sélectionnée.

Options utiles :

```bash
./run_qemu_linkey.sh --build      # build d'abord, puis lance QEMU
./run_qemu_linkey.sh --monitor    # expose le moniteur QEMU sur 127.0.0.1:55555
./run_qemu_linkey.sh --debug      # écrit les logs de debug QEMU dans qemu_debug.log
./run_qemu_linkey.sh --tariff_option hphc
```

Options tarifaires supportées :

```text
base, hphc, ejp, tempo
```

Sans `--tariff_option`, `base` est utilisée.
L'option sélectionne à la fois la trame TIC mockée et la configuration firmware `CONFIG_LINKEY_TARIFF_*`.
La trame est constante. Les données ne sont pas forcément "électriquement" cohérentes.

## Broker MQTT

MQTT utilise le gestionnaire MQTT du firmware. Démarrer le broker local avant QEMU permet de valider la publication de bout en bout.

```bash
docker compose up -d mosquitto
```

Pour observer les messages publiés par le firmware émulé :

```bash
mosquitto_sub -h localhost -p 1884 -t "linkey/#" -v
```

Cette commande se connecte au broker local exposé sur le port `1884`, s'abonne à tous les topics sous `linkey/` et affiche chaque message reçu avec son topic.

L'URI du broker QEMU est définie dans `emulation/sdkconfig.qemu.defaults`.

## Validation

Commandes de validation :

```bash
mkdir -p build-qemu-base
printf '%s\n' 'CONFIG_LINKEY_TARIFF_BASE=y' > build-qemu-base/sdkconfig.tariff.defaults

idf.py -B build-qemu-base \
  -DSDKCONFIG=build-qemu-base/sdkconfig \
  '-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;emulation/sdkconfig.qemu.defaults;build-qemu-base/sdkconfig.tariff.defaults' \
  -DLINKEY_QEMU_EMULATION=ON \
  -DLINKEY_QEMU_TARIFF_OPTION=BASE \
  build

idf.py build
```

Le premier build valide la cible QEMU. Le second valide le build firmware standard.

## Extension des tests

Les extensions propres à l'émulation se placent dans `Firmware/emulation/`.

Ajouts possibles :

- un générateur de trames TIC "cohérentes" (en puissance, incrémentation des index, dépassement, etc.) ;
- plusieurs scénarios de trames TIC ;
- profils de tension pour tester les transitions basse tension ;
- injection d'échecs de publication MQTT ;
- simulation de timeout ULP.
