# Test the CJC4334H DAC
I want to use the Raspberry Pi Pico SDK to test the CJC4334H DAC.  It is connected tho the YD-RP2040 I2S using the following pins:

 GP16 (SCK), GP17 (WS/LRCK), and GP18 (SD).
 The MCLK can be connected to a 24.576 MHz oscillator or GP15 (selected by jumper).

 I need to test the DAC by outputing a sine wave to the audio output jack, or something more interesting if you like.  The output of the DAC feeds a PAM8908 headphone amplifier.  I need to test this too, because I have another board that I wish to use this circuit on, and it has not yet been tested.  The DAC has been tested, but to drive the PAM8908 I need it to work, and it has not been tested using the Pico SDK.