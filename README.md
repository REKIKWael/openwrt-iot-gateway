# Passerelle IoT OpenWrt et Matter

Projet personnel de montée en compétence sur OpenWrt (novembre 2026).
Image OpenWrt 25.12 construite depuis les sources pour une cible x86_64,
avec un paquet C maison qui publie l'état réseau de la passerelle en MQTT.
Testée sous QEMU.

## Ce que fait le projet

- **Image firmware** construite depuis les sources (`v25.12.6`, cible x86_64)
- **Paquet `iotd`** : recette OpenWrt, démon C, configuration UCI et service procd
- **Configuration réseau au premier démarrage** : script `uci-defaults` qui crée l'interface WAN
- **Résultat** : l'image démarre directement en passerelle opérationnelle, sans configuration manuelle

```
+---------------- OpenWrt (QEMU) ----------------+           +---- Hôte (Ubuntu / WSL2) ----+
| procd -> iotd                                  |   MQTT    |                              |
|   lit /etc/config/iotd (libuci)                | --------> | broker mosquitto             |
|   publie l'état réseau en JSON (libmosquitto)  |  TCP 1883 | mosquitto_sub 'gateway/#'    |
|   WAN eth1 (DHCP) 10.0.2.15                    |           | 10.0.2.2                     |
+------------------------------------------------+           +------------------------------+
```

## Le démon `iotd`

- Lit sa configuration UCI avec **libuci** : valeurs par défaut, contrôle des types et des plages
- Se connecte au broker avec **libmosquitto**, dans une boucle unique sans thread
- Publie toutes les N secondes l'état de la passerelle en JSON (nom d'hôte, uptime, interfaces IPv4)
- **Reprise automatique** si le broker ou le réseau tombe (y compris au démarrage, avant le DHCP)
- **Disponibilité** : message retenu `online` / `offline`, avec un testament MQTT (LWT) en cas de plantage
- **Arrêt propre** sur SIGTERM / SIGINT
- Service **procd** : démarrage au boot, relance automatique, redémarrage si la configuration change

Exemple de messages reçus :

```
gateway/status/availability online
gateway/status {"hostname":"OpenWrt","uptime":76,"interfaces":[{"name":"eth1","ipv4":"10.0.2.15"},{"name":"br-lan","ipv4":"192.168.1.1"}]}
```

Configuration (`/etc/config/iotd`) :

```
config iotd 'main'
    option enabled '1'
    option broker '10.0.2.2'
    option port '1883'
    option topic 'gateway/status'
    option interval '10'
```

## Structure du dépôt

```
package/iotd/
├── Makefile              recette du paquet OpenWrt
├── src/iotd.c            démon C
└── files/
    ├── iotd.config       configuration UCI par défaut (conffiles)
    └── iotd.init         script d'init procd
image-files/etc/uci-defaults/
└── 99-gateway-network    configuration réseau au premier démarrage
configs/diffconfig        configuration minimale du build
scripts/build.sh          reconstruction complète de l'image
```

## Reproduire

Sous Linux (testé sur Ubuntu 24.04 dans WSL2) :

```sh
./scripts/build.sh                      # clone, feeds, configuration, build
sudo apt install qemu-system-x86 mosquitto mosquitto-clients
mosquitto_sub -h localhost -t 'gateway/#' -v &

cd ~/openwrt/bin/targets/x86/64
gunzip -k openwrt-x86-64-generic-ext4-combined.img.gz
qemu-system-x86_64 -m 256 -nographic \
  -drive file=openwrt-x86-64-generic-ext4-combined.img,format=raw \
  -netdev user,id=lan -device e1000,netdev=lan \
  -netdev user,id=wan -device e1000,netdev=wan
```

Dans le réseau `user` de QEMU, l'hôte est joignable à l'adresse `10.0.2.2`.

## Tests réalisés

| Test | Résultat |
|---|---|
| Configuration invalide (`port=abc`, `port=99999`) | valeur par défaut conservée, ou refus de démarrer avec un code d'erreur |
| Arrêt du broker puis redémarrage | reconnexion automatique, sans plantage |
| Démarrage avant que le WAN ait son adresse DHCP | `Network unreachable`, puis connexion 6 s plus tard |
| `kill -9` du démon | `offline` publié par le broker (testament), relance par procd |
| Modification de la configuration + `reload_config` | redémarrage du démon avec la nouvelle configuration |
| Image neuve, premier démarrage | WAN créé, service actif, publication MQTT sans intervention |

## Ce que j'ai appris

- **OpenWrt et Buildroot** : un build system issu de Buildroot, plus les feeds,
  un gestionnaire de paquets sur la cible (apk depuis la 25.12), UCI, procd, ubus et netifd
- **Recette de paquet** : `Build/Prepare`, `Build/Compile`, `Package/install`, `conffiles`, dépendances
- **Toolchain croisée musl** et options de durcissement ajoutées par OpenWrt
  (`-fstack-protector`, `_FORTIFY_SOURCE`, RELRO)
- **UCI** : zone de préparation, `commit`, sections nommées et anonymes, lecture en C avec libuci
- **procd** : respawn, triggers de rechargement, journal système, ordre `START` / `STOP`
- **Mise à jour d'un paquet** : `conffiles` préserve la configuration du terrain (`.apk-new`) ;
  une mise à jour ne réactive pas un service désactivé
- **Personnalisation de l'image** : dossier `files/` et scripts `uci-defaults` idempotents
- **MQTT** : QoS 1, messages retenus, testament (LWT), keepalive

## Limites et pistes d'amélioration

- Connexion MQTT **sans TLS** ni authentification (à ajouter : `mosquitto_tls_set`, certificats X.509)
- Paquet **non signé** (`apk add --allow-untrusted`) : sur un produit, signer les paquets et les images
- Règle de pare-feu dédiée limitant les sorties au broker
- Lecture de l'état réseau via **ubus** (netifd) plutôt que `getifaddrs`
- Délai de reconnexion fixe : passer à un délai croissant (backoff exponentiel)
- Testé sous QEMU uniquement, pas sur du matériel réel

## Extension : pont MQTT ↔ Matter

Prise en main du SDK Matter officiel (`connectedhomeip` v1.6.1) sous Linux :

- compilation de l'exemple d'ampoule (`lighting-app`) et du contrôleur `chip-tool` ;
- mise en service sur le réseau IP : PASE (SPAKE2+), fail-safe, attestation DAC/PAI,
  CSR, certificat opérationnel (NOC), ACL, puis sessions CASE ;
- pilotage des clusters On/Off et Level Control, lecture du modèle de données (Descriptor).

Le script `matter-bridge/matter_mqtt_bridge.py` relie MQTT et Matter :

```
mosquitto_pub -t home/light/set -m ON
   -> broker MQTT -> pont Python -> chip-tool -> Matter (CASE) -> ampoule
   <- home/light/state {"on": true, "level": 50}
```

| Topic | Rôle |
|---|---|
| `home/light/set` | `ON`, `OFF`, `TOGGLE` ou un niveau de 0 à 254 |
| `home/light/state` | état JSON, message retenu |
| `home/light/availability` | `online` / `offline`, avec testament MQTT |

Choix de conception :

- file de commandes et **fil de travail unique** : chip-tool n'est jamais lancé en parallèle ;
- **validation stricte** des messages, et appel de chip-tool **sans shell** : protection contre
  l'injection de commande, testée avec le message `ON; touch /tmp/pirate` ;
- relecture périodique de l'état (un autre contrôleur peut agir sur l'ampoule : multi-admin).

### Reproduire

```sh
git clone --depth 1 --branch v1.6.1.0 https://github.com/project-chip/connectedhomeip.git
cd connectedhomeip
./scripts/checkout_submodules.py --shallow --platform linux
source scripts/bootstrap.sh
./scripts/examples/gn_build_example.sh examples/lighting-app/linux out/lighting
./scripts/examples/gn_build_example.sh examples/chip-tool out/chip-tool

./out/lighting/chip-lighting-app &                        # l'ampoule
./out/chip-tool/chip-tool pairing onnetwork 1 20202021    # mise en service
python3 ../openwrt-iot-gateway/matter-bridge/matter_mqtt_bridge.py
```

Dépendances Ubuntu supplémentaires : `libevent-dev`, `avahi-daemon`, `python3-paho-mqtt`.

### Limites

- Mise en service par le réseau IP : pas de BLE sous WSL2, et pas de Thread (pas de radio 802.15.4)
- Le pont lance chip-tool à chaque commande, donc une session CASE par commande. Une version de
  production garderait la session ouverte et s'abonnerait aux changements d'attributs.
- Le pont tourne sur le PC hôte, pas encore dans l'image OpenWrt

## Licence

MIT
