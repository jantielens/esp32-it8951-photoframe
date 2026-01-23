# Wiring

This project has a **single, canonical wiring diagram** below (rendered via **Mermaid** on GitHub). Edit the labels (GPIOs, connector pin names, wire colors) to match the physical build.

> Notes
> - GitHub renders Mermaid diagrams automatically.
> - In VS Code, Mermaid preview depends on your Markdown/Mermaid extensions.
> - This is a *wiring diagram* (connections + labels), not a symbol-accurate electronics schematic.

## Canonical Wiring Diagram

```mermaid
graph LR

BAT[Battery / LiPo]
BOOST[MiniBoost 5V Boost]

ESP[ESP32]
IT[iT8951 Driver Board]
SD[SD Card]

BAT -->|VBAT_PLUS| BOOST
BAT -->|GND| BOOST

BAT -->|VBAT_TO_ESP32| ESP
BAT -->|GND| ESP

ESP -->|BOOST_EN_GPIO16| BOOST

BOOST -->|V5_TO_IT8951_VCC| IT
BOOST -->|GND| IT

ESP -->|SD_PWR_EN_GPIO14| SD

ESP -->|SD_SCK_GPIO12| SD
ESP -->|SD_MOSI_GPIO11| SD
SD -->|SD_MISO_GPIO13| ESP
ESP -->|SD_CS_GPIO10| SD

ESP -->|SPI_SCK_GPIO36| IT
ESP -->|SPI_MOSI_GPIO35| IT
IT -->|SPI_MISO_GPIO37| ESP
ESP -->|SPI_CS_GPIO34| IT

ESP -->|RST_GPIO38| IT
IT -->|BUSY_IRQ_GPIO4| ESP
```

## Canonical Wiring Diagram (With BOOST_EN Pull-Down)

This is the same wiring as the canonical diagram above, with one extra component: a **pull-down resistor** from the MiniBoost enable line to GND so the boost stays off until firmware drives `GPIO16` HIGH.

```mermaid
graph LR

BAT[Battery / LiPo]
BOOST[MiniBoost 5V Boost]

ESP[ESP32]
IT[iT8951 Driver Board]
SD[SD Card]

EN[MiniBoost EN]
RPD[Pulldown resistor]
GND[GND]

BAT -->|VBAT_PLUS| BOOST
BAT -->|GND| BOOST

BAT -->|VBAT_TO_ESP32| ESP
BAT -->|GND| ESP

BAT -->|GND| GND

ESP -->|BOOST_EN_GPIO16| EN
EN -->|EN_TO_BOOST| BOOST
EN ---|RPD_TO_GND| RPD
RPD -->|GND| GND

BOOST -->|V5_TO_IT8951_VCC| IT
BOOST -->|GND| IT

ESP -->|SD_PWR_EN_GPIO14| SD

ESP -->|SD_SCK_GPIO12| SD
ESP -->|SD_MOSI_GPIO11| SD
SD -->|SD_MISO_GPIO13| ESP
ESP -->|SD_CS_GPIO10| SD

ESP -->|SPI_SCK_GPIO36| IT
ESP -->|SPI_MOSI_GPIO35| IT
IT -->|SPI_MISO_GPIO37| ESP
ESP -->|SPI_CS_GPIO34| IT

ESP -->|RST_GPIO38| IT
IT -->|BUSY_IRQ_GPIO4| ESP
```

- Typical values: `100k` (weak) to `10k` (stronger). Start with `100k` unless you have noise/EMI issues.
- Place the resistor physically close to the MiniBoost EN pin if possible.

## How to Update This Diagram

- Prefer updating **this diagram first** when you change wiring.
- Use the same names as the firmware (e.g., “BUSY”, “CS”, “RST”), and include the **GPIO number**.
- If you have consistent wire colors, append them to labels (example: `SPI SCK (GPIO18, green)`).

## Optional: Embed a Photo or Real Schematic

Markdown also supports embedding images. If you export a diagram from KiCad / EasyEDA / draw.io, add it like this:

```md
![Wiring overview](../assets/wiring/wiring-overview.svg)
```

If you want to go this route, I recommend committing an **SVG** (crisp in GitHub) plus the original source file (e.g., `.kicad_sch` or `.drawio`).
