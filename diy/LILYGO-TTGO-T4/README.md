# DIY LILYGO TTGO T4 V1.3


![](./img/T4_1.png)

#

## LILYGO TTGO T4 Specifications


*1. TTGO T4 V1.3 Micro-USB CH343; VendorID:1a86 ProductID:55d4 (same sa retail Jade);
ESP32-D0WDQ6 dual core Tensilica LX6, 240 MHz, supports Secure Boot V2, 4MB SPIFlash; 8MB PSRAM; battery;*

*2. LCD 2.4" ILI9341 240x320 Display*

#

### TTGO T4 Board Pins

| TTGO T4 Pins | ILI9341 LCD Display Pins |
| ------------------ | ----------------------- |
| 12 | MISO |
| 23 | MOSI |
| 18 | CKL |
| 27 | CS |
| 32 | DC |
| 5 | RST |
| 4 |  BCKL |

#

### Button Pins

| TTGO T4 Pin |  Button  |
| ----------- | --------- |
| IO39 | BTN "A" |
| IO37 | BTN "B" |
| IO38 | BTN "SW" |

#

## Some minor code changes

### Full screen UI

```
CONFIG_GUI_DISPLAY_WINDOW_X1=0
CONFIG_GUI_DISPLAY_WINDOW_Y1=0
CONFIG_GUI_DISPLAY_WINDOW_X2=240
CONFIG_GUI_DISPLAY_WINDOW_Y2=240 
```

#

### IOT buttons bluetooth workaround

TTGO T4 hw has three ```iot``` buttons (same as M5Stack-Basic/Fire boards) and A button behaves behaves badly when Bluetooth is active.

```
#if (!defined(CONFIG_BT_ENABLED)) || (!defined(CONFIG_BOARD_TYPE_TTGO_T4) && !defined(CONFIG_BOARD_TYPE_M5_BLACK_GRAY) && !defined(CONFIG_BOARD_TYPE_M5_FIRE))
```

###  Battery and voltage monitoring

```
#elif defined(CONFIG_BOARD_TYPE_M5_BLACK_GRAY) || defined(CONFIG_BOARD_TYPE_M5_FIRE) || defined(CONFIG_BOARD_TYPE_TTGO_T4)
```

### IP5306 PMU

TTGO T4 hw has IP5306 power controller (same as M5Stack-Basic/Fire boards) :
```
#if defined(CONFIG_BOARD_TYPE_JADE) || defined(CONFIG_BOARD_TYPE_JADE_V1_1)                                            \
    || defined(CONFIG_BOARD_TYPE_M5_STICKC_PLUS) || defined(CONFIG_BOARD_TYPE_M5_BLACK_GRAY)                           \
    || defined(CONFIG_BOARD_TYPE_M5_FIRE) || defined(CONFIG_BOARD_TYPE_TTGO_T4)
```

and:

```
 #elif defined(CONFIG_BOARD_TYPE_M5_BLACK_GRAY)                                                                         \
    || defined(CONFIG_BOARD_TYPE_M5_FIRE) || defined(CONFIG_BOARD_TYPE_TTGO_T4) // M5Stack Basic with IP5303 Power PMU
```

## Project settings
### Defaults sdkconfig settings:
[TTGO T4 defaults](./sdkconfig_lilygo_ttgo_t4.defaults)

#

### Build project settings:
[TTGO T4 project build](./dysplay_ttgo_t4_Kconfig.projbuild)
