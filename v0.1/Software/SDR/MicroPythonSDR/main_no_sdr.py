# Test version without Si5351 - just test the command interface
print("Starting SDR test (no Si5351)...")

try:
    print("Importing modules...")
    import machine
    import time
    import config
    print("Modules imported successfully")
    
    # Mock Si5351 class
    class MockSi5351:
        def __init__(self):
            self._crystal_freq = config.CRYSTAL_FREQ
            print("Mock Si5351 created")
            
        def set_frequency(self, freq, clock, pll, mult, half_bw):
            print(f"Mock: Set frequency to {freq}Hz")
            return mult
            
        def set_phase(self, clock, pll, phase):
            print(f"Mock: Set phase to {phase}")
            
        @property
        def outputs_enabled(self):
            return True
            
        @outputs_enabled.setter
        def outputs_enabled(self, val):
            print(f"Mock: Outputs {'enabled' if val else 'disabled'}")
            
        @property
        def clock_0(self):
            return MockClock()
            
        @property
        def clock_1(self):
            return MockClock()
            
        @property
        def pll_a(self):
            return MockPLL()
            
        @property
        def _last_pll_type(self):
            return 'G'
            
        @property
        def _last_quality(self):
            return 0
    
    class MockClock:
        def drive_strength(self, strength):
            print(f"Mock: Clock drive strength set to {strength}")
    
    class MockPLL:
        pass
    
    # Mock Radio class (simplified)
    class MockRadio:
        def __init__(self, synth, frequency):
            self._si5351 = synth
            self._frequency = frequency
            print(f"Mock Radio created with frequency {frequency}Hz")
            
        def setFrequency(self):
            print(f"Mock: Setting frequency to {self._frequency}Hz")
            self._si5351.outputs_enabled = True
            
        def run(self):
            self.setFrequency()
            print("SDR ready - waiting for commands...")
            while True:
                try:
                    user_input = input()
                    if user_input == "FREQ,":
                        print(self._frequency)
                        self.setFrequency()
                        print("OK,G,0")
                    elif user_input.startswith("FREQ,"):
                        try:
                            self._frequency = int(user_input[5:])
                            print(self._frequency)
                            self.setFrequency()
                            print("OK,G,0")
                        except ValueError:
                            print("Error: Invalid frequency")
                    elif user_input.startswith("VER"):
                        print("SDR Pi Pico version 0.1")
                        print("OK")
                    elif user_input.startswith("XTAL"):
                        print(self._si5351._crystal_freq)
                        print("OK")
                    else:
                        print(f"Unknown command: {user_input}")
                    time.sleep(0.05)
                except Exception as e:
                    print(f"Error in main loop: {e}")
                    time.sleep(0.1)
    
    # Create mock instances
    print("Creating mock Si5351 instance...")
    synth = MockSi5351()
    
    print("Creating mock Radio instance...")
    r = MockRadio(synth, config.DEFAULT_FREQ)
    
    print("Starting main loop...")
    r.run()
    
except Exception as e:
    print(f"Error: {e}")
    import sys
    sys.print_exception(e)
