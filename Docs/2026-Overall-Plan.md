# Overall Plan for the Intro-to-CAD-2026 Board
The 2026 board is a prototype receiver and we would like this to enable a new version of the SDR-TRX.
## Things We Have Now
* USB Commands
* SDR appears as a sound card for 48 kHz and 96 kHz
* A good hardware design
* A platform that allows us to figure out what frequencies for the Si5351a have spurs, noise, etc.

## Possible Things to Add and Figure Out
* What to do when?
* Does it make sense to have them all available at once?
* Commands to switch between different modes 
* WiFi Connection so we don't need a USB connection but we are using a PC SDR software
	- What protocol for WiFi?
	- What SDR Software?
		- Quisk
		- SDR++
		- Soapy
		- OpenWebRX+
			+ Does OpenWebRX+ allow transmitting?
	* What new commands are needed for WiFi?
	* We need to make WiFi work and run it on clean DC and compare spurs to USB.
* Local Only
	* OLED interference
	* DSP
	* What to display
	* Digital Decoding
		- FT8
		- FT4
		- WSPR
		- etc.
	* Use the DAC?
+ Phasing Multiple Receivers using one Si5351a
	* Using the JLCPCB 5 board order
	* All on one board
+ Creating a receiver that could monitor multiple bands at once
	* Using the JLCPCB 5 board order
	* All on one board

## Specific Questions to Answer
* Does SoapySDR have a Protocol for UDP transfer we could easily use for WiFi?
* What are the advantages and disadvantages of using a SoapySDR protocol for WiFi as opposed to a custom one or whatever our other options are?
	- What are our other options for the I/Q protocol?
* What embedded software is there:
	* For DSP?
	* Which would allow sending and sending/receiving FT8 and other digital protocols?
	* Should we use the DAC?

## Paper on the Image Rejection and Spur Reduction of the QSD SDR
* What theories can we come up with to explain the image rejection improvements?
	* It seems that it is a second order effect caused by the non-ideal op amps and the low impedance of the inverting inputs.  It seems that matched components does not guarantee us perfect image rejection without ideal op amps.
		- Why?
	* What simulations can we do to verify the theories?
* Do we do one or two papers?
* Where do we publish them
## Assymmetry with the Softrock Tayloe Detector
When the impedance is low, the second switch (90 degrees late) has an unsymmetry that ruins the image rejection when you only use two of the four outputs (regardless of the transformer recharging the capacitors).  If the switches were perfect this would be okay, but they probably are not.  If I change Vt to 0.5 the results are bad.  If I change it to 0.99 it looks pretty good.  If I change the amplifiers to high input impedance all is okay too.  Look at the softrock to see what it looks like.  Then look at the switches to see if the switch model is realistic.  Using both sides of the transformer and high Z gives 2.99 mV vs using only one side of the tronsformer and inverting inputs gives 1.16 mV peak to peak.

## Ideas for QSD SDR Receiver Hints Paper
* Use buffer amplifiers for good sampling voltage waveforms and image rejection.
* Be careful of cutoff frequencies anywhere in your passband.  The high frequencies near the cutoff will show more disturbance to the image rejection.
* Program the Si5351a using integer dividers.
* Use 1% components (probably less important)
* Use low noise op amps
* Don't use large resistors that add noise
* Make sure your switches can switch quickly enough
* Match the length of I_LO and Q_LO
* How to calculate cutoff frequencies
* Design filter for high Z output impedance and 50 Ohm input impedance
* Could you figure out a way to sum voltages on the sampling capacitors?
	- Maybe use transformers on the outputs?  Check this out in LTspice.
* Use 4:1 switches for BPF.
* Use PCM1808 instead of cheap sound card or CJC5340
	- Measure R-L vs R+L to characterize sound card
	- Look at the noise output of the sound card or ADC as a function of frequency to make sure spectral display looks good.
* Be careful to eliminate ground loops.
* Be very careful to make sure USB power is well filtered for both 4.5V and 3.3 clean voltage.
	- Put an option for running on a clean 12V power supply.
* Bypass all ICs and VA/2 well.
* Use a switched 3.5 mm jack so when you connect to it, you disconnect the SDR.
* Use a separate 24.576 MHz crystal for the PCM1808 and Si5351a.
* Use AI to review your design and to discuss it with an "expert"
* Think about the mechanical issues.  
* Include datasheets in Kicad schematic.
* Use CDFER JLCPCB library if you are getting it built.
* Put in the 3D parts and make the silkscreen perfect.
* If in doubt, make it so you can try both ways using jumpers.
