# ESP32-S3 Super Mini — BOARD.md

Spécification de carte pour développement firmware **ESP-IDF**, **Arduino**, **PlatformIO** ou **ESPHome** sur module **ESP32-S3 Super Mini**.

Cette fiche est volontairement stricte : elle privilégie la stabilité, la reproductibilité, la récupération après flash raté et l'absence de surprises au boot.

---

## 1. Identification matérielle

| Champ | Valeur |
|---|---|
| Carte | ESP32-S3 Super Mini |
| Source principale carte | ESPBoards — `esp32-s3-super-mini` |
| Type de carte | Carte de développement compacte ESP32-S3, USB-C, antenne PCB |
| SoC | Espressif ESP32-S3 |
| CPU | 2 cœurs Xtensa LX7 32 bits |
| Fréquence CPU | jusqu'à 240 MHz |
| SRAM interne | 512 KB |
| ROM | 384 KB |
| Flash annoncée pour cette carte | 4 MB |
| PSRAM | non annoncée ; considérer absente tant qu'elle n'est pas vérifiée |
| Connectivité | Wi-Fi 2,4 GHz 802.11 b/g/n ; Bluetooth 5 Low Energy |
| Bluetooth Classic | **non supporté par l'ESP32-S3** ; ne pas concevoir de firmware BR/EDR ou A2DP Classic |
| USB | USB-C relié à l'USB natif ESP32-S3 selon variante ; USB Serial/JTAG + USB OTG full-speed possibles au niveau SoC |
| Tension logique GPIO | 3,3 V uniquement |
| Alimentation carte | USB-C 5 V ; broche 5V ; régulateur 3,3 V embarqué selon variante |
| LED embarquée | WS2812 RGB sur GPIO48 selon le pinout ESPBoards ; parfois LED rouge partagée avec GPIO48 selon révision |
| Dimensions annoncées | environ 18 × 23,5 mm ; certaines descriptions indiquent 22,52 × 18 mm : vérifier au pied à coulisse avant boîtier |
| Frameworks | ESP-IDF, Arduino, PlatformIO, ESPHome, MicroPython possible |

### Points à vérifier sur la carte réelle

Les cartes vendues sous le nom **ESP32-S3 Super Mini** ne sont pas toutes strictement identiques. Avant figer un design matériel ou un firmware de production, vérifier :

- la taille de flash réelle avec `esptool.py flash_id` ; ne pas supposer 4 MB sans test ;
- la présence ou non de PSRAM avec `esptool.py flash_id` et les logs de boot ESP-IDF ;
- le type exact de puce ou module : `ESP32-S3`, `ESP32-S3FH4R2`, variante avec flash/PSRAM intégrée, clone sans module blindé, etc. ;
- le mapping réel des broches `TX`/`RX` : sur ESP32-S3, UART0 par défaut est `GPIO43`/`GPIO44`, tandis que `GPIO19`/`GPIO20` sont les lignes USB ;
- la polarité et le type de la LED utilisateur : WS2812 sur GPIO48, LED simple, ou LED partagée ;
- la présence réelle de pads batterie `B+`, `B-` et `BOOST`, et le courant admissible du chargeur/boost ;
- le courant maximal réaliste du régulateur 3,3 V ;
- l'orientation du pinout sur la sérigraphie de la carte reçue.

---

## 2. Sources retenues

| Source | Utilisation | URL |
|---|---|---|
| ESPBoards — ESP32-S3 Super Mini | caractéristiques carte, pinout, LED GPIO48, flash annoncée, dimensions, exemples Arduino/PlatformIO/ESPHome | https://www.espboards.dev/esp32/esp32-s3-super-mini/ |
| Espressif — ESP32-S3 SoC | caractéristiques SoC : Xtensa LX7 dual-core, 240 MHz, 512 KB SRAM, Wi-Fi 4, Bluetooth LE 5, GPIO | https://www.espressif.com/en/products/socs/esp32-s3 |
| ESP-IDF — GPIO & RTC GPIO ESP32-S3 | mapping GPIO, ADC, restrictions flash/PSRAM, USB-JTAG GPIO19/GPIO20 | https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/gpio.html |
| Espressif — ESP32-S3 Hardware Design Guidelines | alimentation, découplage, strapping pins, GPIO, USB, UART, ADC, RF | https://docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32s3/schematic-checklist.html |
| ESP-IDF — Establish Serial Connection with ESP32-S3 | flash USB natif, GPIO20 D+, GPIO19 D-, procédure BOOT/RESET | https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/establish-serial-connection.html |
| Espressif FAQ USB | limites USB Serial/JTAG, USB OTG, CDC/RNDIS/NCM, récupération si les pins USB sont réutilisés | https://docs.espressif.com/projects/esp-faq/en/latest/software-framework/peripherals/usb.html |

---

## 3. Politique de conception

### Objectif

Le firmware doit rester récupérable par USB, stable au reset et prévisible après deep sleep. Les GPIO critiques doivent être explicitement documentés dans le code.

### Règles projet

- Ne jamais utiliser un GPIO sans le déclarer dans un fichier `board_pins.h` ou équivalent.
- Ne jamais câbler de périphérique qui force un niveau au reset sur `GPIO0`, `GPIO3`, `GPIO45` ou `GPIO46`.
- Ne pas utiliser `GPIO19`/`GPIO20` pour autre chose si l'USB est l'interface unique de flash, log ou debug.
- Désactiver la LED RGB `GPIO48` si elle n'est pas explicitement nécessaire.
- Désactiver la PSRAM dans la configuration tant qu'elle n'est pas détectée et testée.
- Ne pas supposer que les labels Arduino `TX`, `RX`, `SDA`, `SCL`, `SS`, `MOSI`, `MISO`, `SCK` sont universels : l'ESP32-S3 permet le remapping par GPIO matrix.
- Préférer des pull-up/pull-down externes pour les signaux qui doivent avoir un état défini avant l'initialisation du firmware.

---

## 4. Contraintes électriques impératives

### 4.1 Logique GPIO

- Tous les GPIO sont en logique **3,3 V**.
- Ne jamais appliquer 5 V sur un GPIO.
- Tout périphérique 5 V doit passer par un convertisseur de niveau ou une interface explicitement compatible 3,3 V.
- Les GPIO ne doivent pas alimenter directement des charges importantes : utiliser transistor, MOSFET, driver ou relais adapté.
- Les entrées flottantes sont interdites en production : pull-up, pull-down ou configuration logicielle documentée.

### 4.2 Alimentation

- Le port USB-C alimente la carte en 5 V.
- La broche `5V` peut être entrée ou sortie selon la topologie de la carte ; ne pas injecter simultanément USB et 5 V externe tant que le schéma réel n'est pas confirmé.
- La broche `3V3` provient du régulateur embarqué. Elle doit alimenter seulement des charges faibles sauf vérification du régulateur.
- Pour capteurs/radios/actionneurs externes, préférer une alimentation 3,3 V externe propre, masse commune, découplage local.
- Si des pads batterie existent, ne pas supposer un circuit de charge fiable ou protégé sans identifier l'IC de charge.

### 4.3 Découplage recommandé

Pour tout montage stable :

- 100 nF au plus près de chaque capteur/module externe ;
- 10 µF à 47 µF sur le rail 3,3 V si périphériques externes ;
- alimentation externe obligatoire si moteur, relais, LED haute puissance, servo, radio LoRa ou charge inductive ;
- masse commune courte et propre entre carte et périphériques ;
- ne pas placer de plan cuivre ou de métal sous l'antenne PCB.

### 4.4 RF

- Garder une zone libre autour de l'antenne PCB.
- Éviter boîtier métallique, batterie ou câble collé contre l'antenne.
- Ne pas initialiser Wi-Fi/BLE si le produit n'en a pas besoin : gain de consommation et réduction de bruit RF.

---

## 5. Pinout pratique de l'ESP32-S3 Super Mini

> Orientation : USB-C vers le haut, carte vue de face. Le pinout ci-dessous suit la page ESPBoards et son diagramme. Vérifier avec la sérigraphie de la carte reçue.

### 5.1 Rangée gauche, face avant, de haut en bas

| Broche carte | GPIO SoC probable | Fonctions usuelles | Remarques fiabilité |
|---|---:|---|---|
| `TX` | GPIO43 probable | UART0 TX | Logs UART0 possibles. Confirmer mapping exact. Ne pas confondre avec USB. |
| `RX` | GPIO44 probable | UART0 RX | UART0 RX. Confirmer mapping exact. |
| `GP1` | GPIO1 | ADC1_CH0, Touch1, GPIO, PWM | Bon GPIO général. Bon choix ADC. |
| `GP2` | GPIO2 | ADC1_CH1, Touch2, GPIO, PWM | Bon GPIO général. |
| `GP3` | GPIO3 | ADC1_CH2, Touch3, strapping JTAG | **Strapping pin**. Éviter si un niveau externe peut être imposé au reset. |
| `GP4` | GPIO4 | ADC1_CH3, Touch4, GPIO, PWM | Bon GPIO général. |
| `GP5` | GPIO5 | ADC1_CH4, Touch5, GPIO, PWM | Bon GPIO général. |
| `GP6` | GPIO6 | ADC1_CH5, Touch6, GPIO, PWM | Bon GPIO général. |
| `GP7` | GPIO7 | ADC1_CH6, Touch7, GPIO, PWM | Bon GPIO général. |

### 5.2 Rangée droite, face avant, de haut en bas

| Broche carte | GPIO SoC | Fonctions usuelles | Remarques fiabilité |
|---|---:|---|---|
| `5V` | — | alimentation 5 V | Prudence si USB + 5 V externe simultanés. |
| `GND` | — | masse | Masse commune obligatoire avec modules externes. |
| `3V3` / `3VOUT` | — | sortie 3,3 V | Courant disponible inconnu ; limiter les charges. |
| `GP13` | GPIO13 | ADC2_CH2, Touch13, GPIO, PWM, SPI possible | GPIO général ; éviter ADC2 si précision critique. |
| `GP12` | GPIO12 | ADC2_CH1, Touch12, GPIO, PWM, SPI possible | GPIO général ; bon SCK possible. |
| `GP11` | GPIO11 | ADC2_CH0, Touch11, GPIO, PWM, SPI possible | GPIO général ; bon MOSI possible. |
| `GP10` | GPIO10 | ADC1_CH9, Touch10, GPIO, PWM, SPI CS possible | Bon GPIO général ; bon CS. |
| `GP9` | GPIO9 | ADC1_CH8, Touch9, GPIO, PWM, I2C possible | Bon GPIO général ; SCL recommandé si bus court. |
| `GP8` | GPIO8 | ADC1_CH7, Touch8, GPIO, PWM, I2C possible | Bon GPIO général ; SDA recommandé si bus court. |

### 5.3 Pads arrière / pads additionnels selon variante

Certaines cartes exposent des pads additionnels sur la face arrière. Ils sont utiles pour un design compact mais moins pratiques pour le prototypage breadboard.

| Pad | GPIO SoC | Statut conseillé | Remarques |
|---|---:|---|---|
| `GP14` | GPIO14 | utilisable | ADC2_CH3, Touch14. |
| `GP15` | GPIO15 | utilisable | ADC2_CH4. |
| `GP16` | GPIO16 | utilisable | ADC2_CH5. |
| `GP17` | GPIO17 | utilisable | ADC2_CH6 ; bon UART1 RX/TX selon mapping. |
| `GP18` | GPIO18 | utilisable | ADC2_CH7 ; bon UART1 RX/TX selon mapping. |
| `GP21` | GPIO21 | utilisable | GPIO général ; pas ADC. |
| `GP33` | GPIO33 | prudence | Peut être réservé sur variantes avec mémoire octal/PSRAM. |
| `GP34` | GPIO34 | prudence | Peut être réservé sur variantes avec mémoire octal/PSRAM. |
| `GP35` | GPIO35 | prudence | Peut être réservé sur variantes avec mémoire octal/PSRAM. |
| `GP36` | GPIO36 | prudence | Peut être réservé sur variantes avec mémoire octal/PSRAM. |
| `GP37` | GPIO37 | prudence | Peut être réservé sur variantes avec mémoire octal/PSRAM. |
| `GP38` | GPIO38 | utilisable avec prudence | Peut avoir un état particulier en USB-OTG download mode. |
| `GP39` | GPIO39 | réserver si JTAG externe requis | MTCK / JTAG traditionnel selon configuration. |
| `GP40` | GPIO40 | réserver si JTAG externe requis | MTDO / JTAG traditionnel selon configuration. |
| `GP41` | GPIO41 | réserver si JTAG externe requis | MTDI / JTAG traditionnel selon configuration. |
| `GP42` | GPIO42 | réserver si JTAG externe requis | MTMS / JTAG traditionnel selon configuration. |
| `GP45` | GPIO45 | à éviter | **Strapping pin** lié à VDD_SPI. Ne pas forcer au reset. |
| `GP46` | GPIO46 | à éviter | **Strapping pin** lié boot/ROM messages. Ne pas forcer au reset. |
| `GP47` | GPIO47 | utilisable avec prudence | Haut numéro ; vérifier variante/mémoire. |
| `GP48` | GPIO48 | réservé LED | WS2812 RGB/LED selon variante ; ne pas utiliser comme GPIO externe. |

### 5.4 Broches USB internes

| GPIO | Fonction | Statut |
|---:|---|---|
| GPIO19 | USB D- / USB Serial-JTAG / USB OTG | Relié au port USB-C sur les cartes USB natives. Ne pas réutiliser si USB nécessaire. |
| GPIO20 | USB D+ / USB Serial-JTAG / USB OTG | Relié au port USB-C sur les cartes USB natives. Ne pas réutiliser si USB nécessaire. |

---

## 6. Classification GPIO pour ce projet

### 6.1 GPIO recommandés en priorité

À privilégier pour GPIO numériques, I2C, SPI, PWM, interruptions :

```text
GPIO1, GPIO2, GPIO4, GPIO5, GPIO6, GPIO7, GPIO8, GPIO9, GPIO10, GPIO11, GPIO12, GPIO13,
GPIO14, GPIO15, GPIO16, GPIO17, GPIO18, GPIO21
```

Notes :

- `GPIO1` à `GPIO10` sont les meilleurs candidats analogiques car ils correspondent à ADC1.
- `GPIO11` à `GPIO20` correspondent à ADC2 ; éviter `GPIO19`/`GPIO20` si USB utilisé.
- `GPIO14` à `GPIO18` sont souvent sur pads arrière ; tenir compte de la mécanique.

### 6.2 GPIO utilisables avec conditions

```text
GPIO33, GPIO34, GPIO35, GPIO36, GPIO37, GPIO38, GPIO39, GPIO40, GPIO41, GPIO42, GPIO47
```

Conditions :

- vérifier l'absence de PSRAM/flash octal qui réserve `GPIO33` à `GPIO37` ;
- réserver `GPIO39` à `GPIO42` si le debug JTAG traditionnel est nécessaire ;
- éviter les signaux critiques au boot sur les pads arrière peu accessibles ;
- documenter tout usage dans `board_pins.h`.

### 6.3 GPIO à éviter

```text
GPIO0, GPIO3, GPIO19, GPIO20, GPIO45, GPIO46, GPIO48
```

Raisons :

- `GPIO0` : BOOT / download mode ;
- `GPIO3` : strapping lié au choix de la source JTAG ;
- `GPIO19` / `GPIO20` : USB D- / D+ ;
- `GPIO45` : strapping VDD_SPI ;
- `GPIO46` : strapping boot/ROM messages ;
- `GPIO48` : LED WS2812/LED embarquée, et prudence sur variantes mémoire.

---

## 7. Strapping pins et boot

L'ESP32-S3 échantillonne des broches de strapping au reset. Après reset, elles redeviennent des GPIO normales, mais un mauvais câblage peut empêcher le boot ou rendre le flash impossible.

| GPIO | Rôle au boot | Règle projet |
|---:|---|---|
| GPIO0 | boot normal vs download mode | Doit rester haut pour boot depuis flash. BOOT le tire bas pour flasher. |
| GPIO3 | sélection source JTAG selon eFuses | Éviter périphérique qui force un niveau au reset. |
| GPIO45 | sélection tension VDD_SPI selon configuration | Ne pas utiliser en production sauf justification forte. |
| GPIO46 | participe au boot/download et messages ROM | Ne pas utiliser en production sauf justification forte. |

### Règles strictes

- **GPIO0 bas au reset** : mode download.
- **GPIO0 haut au reset** : boot normal depuis la flash si les autres conditions sont correctes.
- Ne jamais ajouter une capacité élevée sur GPIO0 : risque d'entrée involontaire en mode download.
- Tout périphérique connecté à GPIO0/GPIO3/GPIO45/GPIO46 doit être haute impédance pendant reset.
- En design produit, garder des accès physiques à `BOOT`, `RESET`, `GND` et `3V3`.

---

## 8. USB Serial/JTAG, USB OTG et récupération

L'ESP32-S3 est plus riche que l'ESP32-C3 côté USB : il dispose d'un contrôleur **USB Serial/JTAG** et d'un périphérique **USB OTG full-speed**. Les deux fonctions s'appuient sur les lignes USB natives `GPIO19` et `GPIO20`.

### 8.1 Mapping USB

| Signal | GPIO ESP32-S3 | Remarque |
|---|---:|---|
| USB D- | GPIO19 | Ne pas utiliser comme GPIO si USB requis. |
| USB D+ | GPIO20 | Ne pas utiliser comme GPIO si USB requis. |

### 8.2 USB Serial/JTAG

Fonctions :

- flash via USB sans pont USB-UART externe ;
- console série USB ;
- JTAG via USB ;
- debug ESP-IDF/OpenOCD sans adaptateur externe.

Règles :

- Garder `GPIO19`/`GPIO20` intacts si l'USB-C est le seul moyen de flash/debug.
- Prévoir BOOT/RESET accessibles pour le premier flash ou après flash effacé.
- Si le firmware passe en deep sleep, le port USB peut disparaître puis réapparaître au réveil.
- Si l'application reconfigure l'USB en TinyUSB CDC/ECM/NCM/RNDIS, la séquence d'auto-upload peut ne plus fonctionner comme avec USB Serial/JTAG.

### 8.3 USB OTG / TinyUSB

Usages possibles :

- CDC ACM custom ;
- HID device ;
- MIDI ;
- MSC ;
- USB host full-speed selon stack ESP-IDF ;
- CDC-ECM/RNDIS/NCM selon exemples et composants disponibles.

Contraintes :

- L'ESP32-S3 n'est pas USB high-speed 480 Mbit/s ; il faut raisonner en **full-speed**.
- USB OTG et USB Serial/JTAG partagent les ressources USB internes ; ne pas supposer que flash/debug USB et device USB custom fonctionneront simultanément.
- Si l'USB devient l'interface applicative principale après boot, garder un chemin de récupération : BOOT/RESET, UART0, ou firmware OTA fiable.

---

## 9. LED embarquée et signaux locaux

### 9.1 WS2812 RGB sur GPIO48

Le diagramme ESPBoards indique une LED WS2812 RGB sur `GPIO48`.

Règles projet :

- réserver `GPIO48` à la LED embarquée ;
- ne pas câbler de périphérique externe sur `GPIO48` ;
- initialiser la LED seulement si le firmware en a besoin ;
- au boot, mettre l'état LED dans un module séparé pour éviter les interférences timing avec Wi-Fi/BLE ;
- vérifier l'ordre couleur réel : GRB probable, mais pas garanti.

### 9.2 LED power / batterie

Certaines variantes indiquent :

- une LED d'alimentation ;
- une LED batterie/charge ;
- des pads `B+`, `B-`, `BOOST`.

Ces éléments sont **hors contrôle firmware** sauf preuve contraire. Ne pas baser un état applicatif critique sur ces LEDs.

---

## 10. Mapping périphériques recommandé

### 10.1 Fichier `board_pins.h` minimal

```c
#pragma once

#include "driver/gpio.h"

// Board identity
#define BOARD_NAME "ESP32-S3-SuperMini"

// On-board LED
#define BOARD_LED_RGB_GPIO        GPIO_NUM_48
#define BOARD_LED_RGB_COUNT       1

// I2C - conforme au pinout ESPBoards, sans utiliser de strapping pin
#define BOARD_I2C_SDA_GPIO        GPIO_NUM_8
#define BOARD_I2C_SCL_GPIO        GPIO_NUM_9
#define BOARD_I2C_FREQ_HZ         400000

// SPI - choix conservateur sur broches de face avant
#define BOARD_SPI_MOSI_GPIO       GPIO_NUM_11
#define BOARD_SPI_MISO_GPIO       GPIO_NUM_13
#define BOARD_SPI_SCLK_GPIO       GPIO_NUM_12
#define BOARD_SPI_CS0_GPIO        GPIO_NUM_10

// UART applicatif - éviter UART0 si la console de boot/log l'utilise
#define BOARD_UART1_TX_GPIO       GPIO_NUM_17
#define BOARD_UART1_RX_GPIO       GPIO_NUM_18
#define BOARD_UART_BAUDRATE       115200

// Analog inputs - préférer ADC1
#define BOARD_ADC0_GPIO           GPIO_NUM_1
#define BOARD_ADC1_GPIO           GPIO_NUM_2
#define BOARD_ADC2_GPIO           GPIO_NUM_4
#define BOARD_ADC3_GPIO           GPIO_NUM_5

// Reserved / do-not-use
#define BOARD_BOOT_GPIO           GPIO_NUM_0
#define BOARD_USB_DM_GPIO         GPIO_NUM_19
#define BOARD_USB_DP_GPIO         GPIO_NUM_20
#define BOARD_STRAP_GPIO3         GPIO_NUM_3
#define BOARD_STRAP_GPIO45        GPIO_NUM_45
#define BOARD_STRAP_GPIO46        GPIO_NUM_46
```

### 10.2 I2C

Mapping recommandé :

```c
#define BOARD_I2C_SDA_GPIO GPIO_NUM_8
#define BOARD_I2C_SCL_GPIO GPIO_NUM_9
```

Règles :

- pull-up externes 4,7 kΩ vers 3,3 V pour un bus fiable ;
- 2,2 kΩ possible si bus court et plusieurs modules, mais surveiller consommation ;
- ne jamais tirer vers 5 V ;
- réduire à 100 kHz si câbles longs ou environnement bruité ;
- si deep sleep ultra basse consommation, vérifier la fuite via pull-up externes.

### 10.3 SPI

Mapping recommandé :

```c
#define BOARD_SPI_MOSI_GPIO GPIO_NUM_11
#define BOARD_SPI_MISO_GPIO GPIO_NUM_13
#define BOARD_SPI_SCLK_GPIO GPIO_NUM_12
#define BOARD_SPI_CS0_GPIO  GPIO_NUM_10
```

Règles :

- garder un pull-up externe sur CS si le périphérique ne doit jamais être sélectionné au boot ;
- ne pas utiliser `GPIO45`/`GPIO46` comme CS ;
- ne pas utiliser `GPIO19`/`GPIO20` comme SPI si USB nécessaire ;
- baisser la fréquence SPI si fils volants, breadboard ou nappe longue.

### 10.4 UART

UART0 :

```c
// UART0 matériel ESP32-S3 par défaut : GPIO43 TX, GPIO44 RX
// Les broches sérigraphiées TX/RX de la carte correspondent probablement à UART0.
```

UART applicatif recommandé :

```c
#define BOARD_UART1_TX_GPIO GPIO_NUM_17
#define BOARD_UART1_RX_GPIO GPIO_NUM_18
```

Règles :

- garder UART0 pour logs de secours si l'application USB devient instable ;
- ne pas connecter un périphérique sensible aux logs de boot sur UART0 ;
- documenter baudrate, inversion éventuelle et niveau logique ;
- pour RS485, utiliser un GPIO dédié pour DE/RE et prévoir état par défaut désactivé.

### 10.5 ADC

Mapping ADC utile :

| GPIO | ADC | Recommandation |
|---:|---|---|
| GPIO1 | ADC1_CH0 | recommandé |
| GPIO2 | ADC1_CH1 | recommandé |
| GPIO3 | ADC1_CH2 | éviter : strapping |
| GPIO4 | ADC1_CH3 | recommandé |
| GPIO5 | ADC1_CH4 | recommandé |
| GPIO6 | ADC1_CH5 | recommandé |
| GPIO7 | ADC1_CH6 | recommandé |
| GPIO8 | ADC1_CH7 | recommandé si pas I2C |
| GPIO9 | ADC1_CH8 | recommandé si pas I2C |
| GPIO10 | ADC1_CH9 | recommandé |
| GPIO11-20 | ADC2_CH0-9 | possible mais éviter si précision critique ; GPIO19/20 réservés USB |

Règles :

- ADC ESP32-S3 non idéal pour mesure de précision absolue ; calibrer ;
- ajouter 100 nF à la masse près de l'entrée si signal lent ;
- protéger toute entrée issue d'un diviseur batterie ;
- ne jamais dépasser 3,3 V ;
- préférer ADC1 pour stabilité et simplicité.

### 10.6 PWM / LEDC

- La plupart des GPIO libres peuvent sortir un PWM via LEDC.
- Éviter PWM sur strapping pins et USB pins.
- Pour LED externe, utiliser résistance série ou driver.
- Pour moteur ou relais, utiliser driver/MOSFET + diode de roue libre si charge inductive.

### 10.7 Touch

- Touch disponible notamment sur GPIO1 à GPIO14.
- Éviter les touch pads sur fils longs ou environnement bruité.
- Ne pas utiliser un touch pad sur strapping pin `GPIO3` pour une fonction critique.
- Prévoir calibration logicielle et seuils adaptatifs.

---

## 11. Configuration ESP-IDF recommandée

### 11.1 Création projet

```bash
idf.py set-target esp32s3
idf.py menuconfig
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

Si `/dev/ttyACM0` change après reset USB, utiliser :

```bash
idf.py -p $(ls /dev/ttyACM* | head -n1) flash monitor
```

### 11.2 `sdkconfig.defaults` minimal

```ini
CONFIG_IDF_TARGET="esp32s3"

# Console stable via USB Serial/JTAG tant que l'application ne prend pas l'USB OTG.
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y
CONFIG_ESP_CONSOLE_SECONDARY_NONE=y

# Logs : adapter au projet.
CONFIG_LOG_DEFAULT_LEVEL_INFO=y

# PSRAM désactivée par défaut : ne l'activer qu'après vérification matérielle.
CONFIG_SPIRAM=n
```

### 11.3 CMake minimal

`main/CMakeLists.txt` :

```cmake
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_driver_gpio esp_driver_i2c esp_driver_spi
)
```

Selon version ESP-IDF, les dépendances exactes peuvent varier. Ne pas figer un `REQUIRES` inutile si le projet n'utilise pas le périphérique.

### 11.4 Vérification GPIO au boot

Ajouter temporairement pendant le bring-up :

```c
#include "driver/gpio.h"
#include "soc/soc_caps.h"

void dump_board_gpios(void) {
    gpio_dump_io_configuration(stdout, SOC_GPIO_VALID_GPIO_MASK);
}
```

But : repérer rapidement les pins `RESERVED`, les pulls actifs et les réaffectations inattendues par le bootloader ou les drivers.

---

## 12. Configuration Arduino recommandée

Dans Arduino IDE avec le package Espressif :

| Réglage | Valeur recommandée |
|---|---|
| Board | `ESP32S3 Dev Module` ou `Esp32s3 Dev` selon version du package |
| USB CDC On Boot | Enabled si console USB souhaitée |
| USB Mode | Hardware CDC and JTAG si disponible |
| CPU Frequency | 240 MHz |
| Flash Size | 4 MB après vérification |
| PSRAM | Disabled sauf preuve de présence |
| Upload Speed | 921600 si stable ; sinon 460800 ou 115200 |
| Partition Scheme | selon application ; OTA si firmware terrain |

Exemple de définitions :

```cpp
#define BOARD_LED_RGB_PIN 48
#define BOARD_I2C_SDA    8
#define BOARD_I2C_SCL    9
#define BOARD_SPI_MOSI   11
#define BOARD_SPI_MISO   13
#define BOARD_SPI_SCK    12
#define BOARD_SPI_CS     10
```

### Blink WS2812 minimal avec FastLED

```cpp
#include <FastLED.h>

#define LED_PIN 48
#define NUM_LEDS 1

CRGB leds[NUM_LEDS];

void setup() {
  FastLED.addLeds<NEOPIXEL, LED_PIN>(leds, NUM_LEDS);
  leds[0] = CRGB::Black;
  FastLED.show();
}

void loop() {
  leds[0] = CRGB::Blue;
  FastLED.show();
  delay(250);
  leds[0] = CRGB::Black;
  FastLED.show();
  delay(750);
}
```

Si la LED ne répond pas : vérifier ordre couleur, variante de carte, conflit GPIO48 et configuration mémoire.

---

## 13. Configuration PlatformIO recommandée

### ESP-IDF

```ini
[env:esp32-s3-super-mini-idf]
platform = espressif32
board = esp32-s3-devkitm-1
framework = espidf

board_build.mcu = esp32s3
board_build.flash_size = 4MB
board_upload.flash_size = 4MB

monitor_speed = 115200
upload_speed = 921600

build_flags =
  -DBOARD_NAME=\"ESP32-S3-SuperMini\"
```

### Arduino

```ini
[env:esp32-s3-super-mini-arduino]
platform = espressif32
board = esp32-s3-devkitm-1
framework = arduino

board_build.mcu = esp32s3
board_build.flash_size = 4MB
board_upload.flash_size = 4MB

monitor_speed = 115200
upload_speed = 921600

build_flags =
  -DARDUINO_USB_MODE=1
  -DARDUINO_USB_CDC_ON_BOOT=1
  -DBOARD_NAME=\"ESP32-S3-SuperMini\"
```

Si l'upload devient instable, réduire `upload_speed` à `460800` ou `115200` et flasher en maintenant BOOT.

---

## 14. ESPHome minimal

```yaml
esp32:
  board: esp32-s3-devkitc-1
  framework:
    type: esp-idf

logger:
  hardware_uart: USB_SERIAL_JTAG

# Exemple I2C
# i2c:
#   sda: GPIO8
#   scl: GPIO9
#   scan: true
#   frequency: 400kHz

# Exemple LED RGB embarquée selon variante
# light:
#   - platform: esp32_rmt_led_strip
#     rgb_order: GRB
#     pin: GPIO48
#     num_leds: 1
#     chipset: ws2812
#     name: "Onboard RGB LED"
```

Règles ESPHome :

- désactiver la LED si elle provoque des resets ou des logs RMT ;
- ne pas activer PSRAM sans vérification ;
- éviter `GPIO19`/`GPIO20` hors USB ;
- utiliser `esp-idf` plutôt qu'Arduino si USB, sleep ou Wi-Fi doivent être finement maîtrisés.

---

## 15. Procédure de récupération après flash raté

### 15.1 Symptômes courants

- `/dev/ttyACM0` apparaît/disparaît en boucle ;
- `idf.py flash` reste sur `Connecting...` ;
- le firmware démarre puis coupe l'USB ;
- la LED clignote mais aucun port série stable n'apparaît ;
- après activation TinyUSB/CDC/RNDIS/NCM, l'auto-reset ne fonctionne plus.

### 15.2 Séquence de récupération

1. Débrancher USB.
2. Maintenir `BOOT`.
3. Brancher USB ou appuyer brièvement sur `RESET`.
4. Relâcher `BOOT` après 1 seconde.
5. Lancer :

```bash
esptool.py --chip esp32s3 -p /dev/ttyACM0 flash_id
```

6. Effacer seulement si nécessaire :

```bash
esptool.py --chip esp32s3 -p /dev/ttyACM0 erase_flash
```

7. Reflasher firmware minimal connu bon.

### 15.3 Firmware minimal de secours

Maintenir un binaire de secours qui :

- n'active pas TinyUSB custom ;
- n'active pas deep sleep ;
- n'utilise pas `GPIO19`/`GPIO20` ;
- initialise la LED GPIO48 en noir/off ;
- imprime les infos chip/flash/PSRAM au boot.

---

## 16. Flash et partitions

### 16.1 Flash 4 MB

Pour 4 MB, éviter les partitions trop ambitieuses. Exemple OTA raisonnable :

```csv
# Name,   Type, SubType, Offset,  Size, Flags
nvs,      data, nvs,     0x9000,  0x6000,
otadata,  data, ota,     0xf000,  0x2000,
phy_init, data, phy,     0x11000, 0x1000,
ota_0,    app,  ota_0,   0x20000, 0x1E0000,
ota_1,    app,  ota_1,           0x1E0000,
```

Vérifier avec :

```bash
idf.py size
idf.py partition-table
```

### 16.2 Sans OTA

Pour firmware local ou prototype :

```csv
# Name,   Type, SubType, Offset,  Size, Flags
nvs,      data, nvs,     0x9000,  0x6000,
phy_init, data, phy,     0xf000,  0x1000,
factory,  app,  factory, 0x10000, 0x300000,
spiffs,   data, spiffs,          0xF0000,
```

---

## 17. Sécurité firmware

### 17.1 Développement

- Garder logs USB actifs.
- Garder BOOT/RESET accessibles.
- Ne pas activer Secure Boot ou Flash Encryption tant que le pinout et la procédure OTA ne sont pas validés.
- Ne pas brûler d'eFuse lié au JTAG/USB avant validation finale.

### 17.2 Production

À envisager seulement quand le firmware est stabilisé :

- Secure Boot v2 ;
- Flash Encryption ;
- NVS encryption si secrets locaux ;
- OTA signé ;
- partition `nvs_keys` ;
- désactivation des logs sensibles ;
- décision explicite sur JTAG/USB debug.

Ne jamais activer une sécurité irréversible sans procédure de récupération, logs de fabrication et firmware OTA validé.

---

## 18. Gestion basse consommation

### 18.1 Deep sleep

- L'USB disparaît en deep sleep.
- Les pins avec pull-up externes consomment si tirées au mauvais niveau.
- Préférer les GPIO RTC pour wake-up.
- Désactiver Wi-Fi/BLE proprement avant sleep.
- Éteindre WS2812 GPIO48 avant sleep, car une LED RGB peut consommer même quand elle paraît inactive selon son état.

### 18.2 Wake sources

À privilégier :

- timer wake ;
- GPIO RTC en niveau stable ;
- bouton avec pull-up/pull-down externe ;
- capteur basse consommation avec sortie open-drain.

À éviter :

- wake sur pins flottantes ;
- wake sur USB pins ;
- wake sur strapping pins ;
- wake sur ligne I2C partagée avec modules mal alimentés.

---

## 19. Checklist bring-up

### 19.1 Première mise sous tension

- [ ] Inspecter soudure USB-C et régulateur.
- [ ] Mesurer 3,3 V à vide.
- [ ] Brancher USB via câble data connu bon.
- [ ] Vérifier port `/dev/ttyACM*`.
- [ ] Lire `flash_id`.
- [ ] Flasher firmware minimal.
- [ ] Confirmer logs USB.
- [ ] Confirmer LED GPIO48 ou la désactiver.
- [ ] Scanner I2C si périphériques.
- [ ] Vérifier consommation idle.

### 19.2 Avant câblage externe

- [ ] Identifier tous les GPIO utilisés dans `board_pins.h`.
- [ ] Confirmer aucun usage de `GPIO19`/`GPIO20` hors USB.
- [ ] Confirmer aucun périphérique ne force `GPIO0`, `GPIO3`, `GPIO45`, `GPIO46` au reset.
- [ ] Ajouter pull-up/pull-down sur entrées externes.
- [ ] Ajouter résistances série ou drivers pour charges.
- [ ] Confirmer masse commune.

### 19.3 Avant intégration mécanique

- [ ] Vérifier dimensions réelles.
- [ ] Laisser espace libre devant antenne.
- [ ] Garder accès BOOT/RESET ou pads de programmation.
- [ ] Éviter boîtier métallique devant antenne.
- [ ] Prévoir dissipation si Wi-Fi continu.

---

## 20. Tests de validation recommandés

### 20.1 Test flash/USB

```bash
for i in $(seq 1 10); do
  idf.py -p /dev/ttyACM0 flash || exit 1
  sleep 2
done
```

Objectif : valider stabilité upload/reset.

### 20.2 Test Wi-Fi + GPIO

- Démarrer Wi-Fi STA.
- Faire ping continu.
- Basculer GPIO critiques.
- Vérifier absence de reset brownout.
- Vérifier que l'ADC reste dans la tolérance attendue.

### 20.3 Test sleep

- Mesurer courant actif Wi-Fi off.
- Mesurer courant deep sleep LED off.
- Vérifier wake timer.
- Vérifier wake GPIO.
- Vérifier reconnexion USB après réveil si nécessaire.

### 20.4 Test USB applicatif

Si CDC/ECM/NCM/RNDIS ou TinyUSB :

- flasher firmware minimal de secours ;
- flasher firmware USB applicatif ;
- vérifier enumeration USB ;
- vérifier récupération par BOOT/RESET ;
- vérifier que l'OTA ou le chemin UART de secours fonctionne.

---

## 21. Erreurs fréquentes à éviter

- Activer PSRAM sans qu'elle existe réellement.
- Utiliser `GPIO19`/`GPIO20` pour un périphérique externe puis perdre le flash USB.
- Confondre `TX/RX` de la carte avec `GPIO19/GPIO20`.
- Utiliser `GPIO0` comme entrée bouton externe sans pull-up propre.
- Utiliser `GPIO45` ou `GPIO46` parce qu'ils sont exposés sur pads arrière.
- Alimenter un capteur 5 V et envoyer son signal directement vers un GPIO.
- Mettre un périphérique SPI qui maintient CS actif au reset.
- Tirer I2C vers 5 V.
- Coller une batterie LiPo ou une masse métallique sous l'antenne PCB.
- Considérer la LED WS2812 comme une LED classique sans timing ni consommation.

---

## 22. Décisions par défaut pour ce projet

| Sujet | Décision |
|---|---|
| Target ESP-IDF | `esp32s3` |
| Flash | 4 MB par défaut, à vérifier |
| PSRAM | désactivée par défaut |
| Console dev | USB Serial/JTAG |
| USB pins | réservés, non disponibles pour GPIO |
| LED | GPIO48 réservé, désactivé si inutile |
| I2C | GPIO8 SDA, GPIO9 SCL |
| SPI | MOSI GPIO11, MISO GPIO13, SCK GPIO12, CS GPIO10 |
| UART applicatif | GPIO17 TX, GPIO18 RX, ou mapping explicite projet |
| ADC | préférer ADC1 : GPIO1,2,4,5,6,7,8,9,10 |
| Strapping pins | évités en production |
| JTAG | USB Serial/JTAG par défaut ; GPIO39-42 libres seulement si JTAG externe non requis |

---

## 23. Notes finales

Cette carte est puissante pour son format, mais sa compacité impose de la discipline : USB natif, pads arrière, LED WS2812, strapping pins et variantes de clones peuvent provoquer des pannes difficiles à diagnostiquer. La bonne pratique consiste à figer un `board_pins.h`, tester la carte réelle, puis interdire tout changement de GPIO non documenté.

Pour un produit ou une installation longue durée, ne pas se contenter du nom commercial `ESP32-S3 Super Mini` : archiver photos recto/verso, résultat `esptool.py flash_id`, logs de boot, courant mesuré, pinout validé et version du firmware.
