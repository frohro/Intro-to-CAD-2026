# Project Documentation: Parallel Summation QSD SDR 

## 1. System Architecture
This receiver is a high-performance Direct Conversion SDR utilizing a **Quadrature Sampling Detector (QSD)** architecture. The design is optimized for high conversion gain and low noise by prioritizing signal symmetry and high-impedance buffering.

## 2. RF Front-End & Mixer
*   **Parallel Summation Mixer:** A modified Tayloe mixer design using a center-tapped 1:1+1 transformer and a dual 1:4 bus switch (**SN74CBT3253**). 
*   **Sampling Logic:** Each I/Q channel samples the transformer secondary twice per LO cycle (I samples at 0° and 180°; Q samples at 90° and 270°). This architecture increases sampling efficiency compared to standard single-sample designs.
*   **Sampling Capacitors:** Utilizes **C0G/NP0** dielectrics (10nF) to ensure linear tracking of the RF signal and to minimize parasitic effects from the CMOS switches.

## 3. Analog Baseband (Instrumentation Amplifier)
*   **Topology:** Each channel (I and Q) utilizes one **OPA1612** dual op-amp package configured as a **2-Op-Amp Instrumentation Amplifier**.
*   **High-Z Buffering:** The sampling capacitors feed directly into non-inverting op-amp inputs. This high-impedance interface prevents "charge sharing" or coupling between phases, preserving the 90° quadrature relationship at high frequencies.
*   **Differential Summation:** The first op-amp acts as a buffer for the negative phase, while the second op-amp performs the subtraction and provides the primary system gain.
*   **Virtual Ground:** A 2.25V bias is provided to the transformer center-tap. Because of the balanced In-Amp design, current demand on this virtual ground is minimized, reducing crosstalk.

## 4. Clocking and Synchronization
*   **Synchronous Clocking:** A single **24.576 MHz CMOS oscillator** serves as the master reference for the entire system.
*   **Local Oscillator:** The clock drives an **MS5351M** (Si5351A) synthesizer to generate the quadrature LO signals.
*   **ADC Clocking:** The same 24.576 MHz source drives the **PCM1808 ADC** (SCKI). This frequency-locks the RF mixing to the digital sampling, eliminating relative frequency drift.

## 5. Power Management and Noise Isolation
*   **Dual Power Path:** A **PMOSFET-based "Ideal Diode"** circuit automatically switches between USB power and an external clean 12V supply. The PMOS prevents high-voltage back-feeding into the USB port while maintaining maximum headroom for the regulators when on USB power.
*   **Analog Regulation:** An **SGM2205** high-voltage LDO provides a quiet 4.5V rail. It features a large **Feed-Forward capacitor ($C_{ff}$)** and a **bulk tantalum reservoir** (isolated by 1$\Omega$) to suppress low-frequency supply noise.
*   **EMI Suppression:** Digital and analog zones are isolated using **ferrite beads** in a star configuration. I2C lines to the LO include series damping resistors to "soften" digital edges and reduce harmonic interference.

## 6. Digital Interface
*   **ADC:** **PCM1808** 24-bit Sigma-Delta converter operating in **Master Mode**.
*   **MCU:** **Raspberry Pi Pico** operating as an **I2S Slave**. Data is captured via PIO state machines and streamed over USB or WiFi (UDP) for processing.
*   **DAC:**  **CJC4334H** a 24 bit DAC with a line level output, and an amplified (suitable for headphones) output.

