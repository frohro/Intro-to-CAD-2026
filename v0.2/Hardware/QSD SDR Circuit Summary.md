# Technical Design Summary: High-Sensitivity Parallel Summation QSD SDR

## 1. Project Overview
This design is a high-performance Direct Conversion Software Defined Radio (SDR) front-end optimized for the HF spectrum (3.5 MHz – 30 MHz). The primary design goal was to achieve a sensitivity of **0.1 µV** while maintaining superior image rejection and a ultra-low noise floor, suitable for weak-signal reception in crowded amateur radio bands.

## 2. RF Front-End & Mixing
### Architecture: Balanced Parallel Summation
*   **Transformer:** A center-tapped 1:1+1 (or 1:2 depending on jumper configurations) transformer (**H1102NLT**) provides galvanic isolation and produces balanced anti-phase signals ($0^\circ$ and $180^\circ$).
*   **Mixer:** The **SN74CBT3253** high-speed bus switch acts as a Tayloe Mixer. Unlike standard designs that use *separate* capacitors, this design employs a **Parallel Current Summation** technique. Each I/Q channel samples the transformer twice per cycle, effectively doubling the sampling duty cycle to 50%. This increases the conversion gain and energy collection efficiency.
*   **Sampling Capacitors:** **10 nF C0G (NP0)** capacitors were selected. While simulation showed 1 nF allowed for faster tracking, 10 nF was chosen for the final design to provide:
    1.  **Superior Image Rejection:** 10 nF "swamps" out tiny parasitic PCB and switch capacitances.
    2.  **Built-in Anti-Aliasing:** In combination with the $100\Omega$ effective source resistance, it creates a hardware cutoff at **159 kHz**, perfectly protecting the 96 kHz sample rate.
    3.  **Linearity:** C0G material is mandatory to prevent the voltage-coefficient distortion inherent in X7R types, which would otherwise destroy quadrature balance.

## 3. Analog Baseband (The "Buffer-First" Strategy)
### Topology: 2-Op-Amp Instrumentation Amplifier
To solve the "coupling" issue found in standard inverting-amplifier SDRs (where sampling capacitors drain into the gain resistors), this design uses two **OPA1612** dual packages.
*   **High-Impedance Buffering:** Each of the four mixer outputs ($0^\circ, 90^\circ, 180^\circ, 270^\circ$) feeds directly into a non-inverting op-amp input. This ensures the sampling capacitors "hold" their peak voltage without being loaded, preserving perfect 90° phase separation even at 30 MHz.
*   **Symmetry:** Signal Path A is buffered at unity gain, while Path B performs the subtraction and provides the primary system gain (**~ 430x**). This gain was intentionally reduced from the original 1300x to provide an extra **10 dB of dynamic range headroom**, preventing the ADC from clipping on strong nearby signals.
*   **Stability:** $10\Omega$ resistors are placed at the op-amp inputs to prevent parasitic VHF oscillations, while $100\text{ pF}$ feedback capacitors provide a secondary $150\text{ kHz}$ low-pass filter.

## 4. Clocking and Frequency Control
*   **Unified Reference:** A single **24.576 MHz CMOS Oscillator** drives the entire system.
*   **Synchronization:** The oscillator feeds both the **MS5351M** Local Oscillator and the **PCM1808 ADC** (Master Mode). This ensures that the RF mixing and digital sampling are frequency-locked, eliminating the "frequency crawl" seen in dual-clock designs.
*   **Signal Integrity:** **33$\Omega$ star-termination resistors** are used at the oscillator and the Si5351 outputs. These resistors dampen reflections and "soften" square-wave edges, reducing harmonic RFI that could desensitize the 0.1 µV front-end.

## 5. Power and Grounding
*   **Dual-Rail Supply:** The system runs on a bipolar **+/- 2.25V** rail. This allows the signal path to be centered at **0V (GNDA)**, maximizing headroom and simplifying the interface between the mixer and the ADC.
*   **SGM2205 Analog Regulator:** A 20V-rated LDO steps down the input to a clean 4.5V. It utilizes a **100 nF Feed-Forward capacitor ($C_{ff}$)** to achieve a near-silent noise floor at audio frequencies.
*   **Filtering Bank:** The analog supply features a **1$\Omega$ series resistor** followed by **300 µF to 500 µF of Tantalum capacitance**. This "Brute Force" filter acts as a local battery, swallowing switching noise from the Pi Pico.
*   **Power Protection:** A **PMOSFET-based "Ideal Diode"** circuit automatically detects the external 12V supply. It prioritizes the clean 12V rail when plugged in but falls back to USB power with **zero voltage drop**, ensuring the 4.5V regulator always has enough headroom to function.

## 6. Layout Philosophy
*   **Star Grounding:** Analog and Digital grounds are kept distinct on the schematic, but the PCB (a 4-layer stackup) is a solid plane on layer two, as was advised by the AI, where is said this was the most recent best practice.
*   **RF Isolation:** The high-speed 24 MHz clock traces are kept strictly on the top layer, away from the OPA1612 inputs, and are routed with matched lengths for the I and Q paths to preserve quadrature.
*   **DC Blocking:** 100 nF input capacitors protect the mixer, while **1 µF - 10 µF Tantalum** capacitors provide snappy DC-settling times at the output, preventing the "waterfall thump" during frequency changes.
*   The I and Q channels are kept as symmetrical and identical as possible, with trace length matching on the PCB.


## Further Information
* There are LTSpice simulations in the Frohne/Docs/ folder, and a README.md with a different description of this receiver.