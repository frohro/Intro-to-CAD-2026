

### Intro-to-CAD-2026 (v0.2) - Pico/YD-RP2040 Pinout

| Pin | RP2040 Pin Name | Signal (Net Name) | Target Peripheral (Deduced) |
|:---:|:---|:---|:---|
| **1** | GP0 | `/MISO` | MicroSD Card Slot (SPI MISO) |
| **2** | GP1 | `/CS` | MicroSD Card Slot (SPI Chip Select) |
| **3** | GND | `GND` | Ground |
| **4** | GP2 | `/SPI_CK` | MicroSD Card Slot (SPI Clock) |
| **5** | GP3 | `/MOSI` | MicroSD Card Slot (SPI MOSI) |
| **6** | GP4 | `/SD_CD` | MicroSD Card Slot (Card Detect) |
| **7** | GP5 | `/SW_C` | Rotary Encoder (Push Button Switch) |
| **8** | GND | `GND` | Ground |
| **9** | GP6 | `/SD2` | Audio ADC/DAC (I2S Data 2) |
| **10** | GP7 | `/BCK2` | Audio ADC/DAC (I2S Bit Clock 2) |
| **11** | GP8 | `/WS2` | Audio ADC/DAC (I2S Word Select 2) |
| **12** | GP9 | `/DATA` | Audio ADC/DAC (I2S Data) |
| **13** | GND | `GND` | Ground |
| **14** | GP10 | `/BCK` | Audio ADC/DAC (I2S Bit Clock) |
| **15** | GP11 | `/WS` | Audio ADC/DAC (I2S Word Select) |
| **16** | GP12 | `/OSC_SDA` | Si5351a Clock Generator (I2C SDA) |
| **17** | GP13 | `/OSC_SCL` | Si5351a Clock Generator (I2C SCL) |
| **18** | GND | `GND` | Ground |
| **19** | GP14 | `/BTN` | Tactile Push Button (S2) |
| **20** | GP15 | `/PICO_MCLK` | Master Clock (I2S/Audio Clock) |
| **21** | GP16 | `/SCLK` | Audio DAC U1 (I2S Serial Clock) |
| **22** | GP17 | `/LRCK` | Audio DAC U1 (I2S Left/Right Clock) |
| **23** | GND | `GND` | Ground |
| **24** | GP18 | `/SDIN` | Audio DAC U1 (I2S Data In) |
| **25** | GP19 | `/RGB_DIN` | WS2812/SK6812 RGB LEDs (Data In) |
| **26** | GP20 | `/ANG_B` | Rotary Encoder (Channel B) |
| **27** | GP21 | `/ANG_A` | Rotary Encoder (Channel A) |
| **28** | GND | `GND` | Ground |
| **29** | GP22 | `/TRIG` | Ultrasonic Sensor J6 (Trigger) |
| **30** | RUN | `/Raspberry_Pi_Pico/RST` | Reset Switch (S1) |
| **31** | GP26 (A0) | `/ECHO` | Ultrasonic Sensor J6 (Echo) |
| **32** | GP27 (A1) | `/INA` | Motor Driver U10 (Input A) |
| **33** | GND (AGND)| `GND` | Ground |
| **34** | GP28 (A2) | `/INB` | Motor Driver U10 (Input B) |
| **35** | ADC_VREF | *Unconnected (NC)* | *(Routed to header J3 but no active signal)* |
| **36** | 3V3_OUT | `+3.3V` | 3.3V Board Power Bus |
| **37** | 3V3_EN | *Unconnected (NC)* | *(Routed to header J3 but no active signal)* |
| **38** | GND | `GND` | Ground |
| **39** | VSYS | `+4.7V` | Main Input / VSYS Power Bus |
| **40** | VBUS | `+4.7V` | Tied to 4.7V Bus |

### A Few Notes For Your Code:
* **MicroSD SPI Bus**: Uses `SPI0` on the Pico (GP0, GP2, GP3) with GP1 as the CS pin.
* **I2C Bus**: The Si5351a generator sits on `I2C0` (GP12 = SDA, GP13 = SCL).
* **Audio (I2S)**: You have a few different I2S devices split across the pins. A main I2S DAC uses pins GP16 (SCK), GP17 (WS/LRCK), and GP18 (SD). Other ADCs/DACs use the GP6-GP11 block. 
* **Rotary Encoder**: Make sure to enable internal pull-ups in your code for `/SW_C` (GP5), `/ANG_A` (GP21), and `/ANG_B` (GP20) unless they are hardwired on the board.