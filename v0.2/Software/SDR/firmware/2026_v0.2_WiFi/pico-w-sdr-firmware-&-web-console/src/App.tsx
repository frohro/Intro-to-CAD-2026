import React, { useState, useMemo } from 'react';
import { 
  Radio, 
  Wifi, 
  Usb, 
  Sliders, 
  FileCode, 
  Cpu, 
  HelpCircle, 
  CheckCircle2, 
  Copy, 
  Activity, 
  RefreshCw,
  Terminal,
  Settings,
  Layers,
  ArrowRight,
  Sparkles
} from 'lucide-react';

interface FirmwareFile {
  name: string;
  path: string;
  description: string;
  category: 'core' | 'driver' | 'network' | 'usb' | 'build';
}

const FIRMWARE_FILES: FirmwareFile[] = [
  { name: 'main.c', path: '/firmware/main.c', description: 'Dual-core orchestration, USB/WiFi stream router, command parser', category: 'core' },
  { name: 'openhpsdr.c', path: '/firmware/openhpsdr.c', description: 'OpenHPSDR Protocol 1 UDP discovery & 24-bit I/Q packet formatter', category: 'network' },
  { name: 'openhpsdr.h', path: '/firmware/openhpsdr.h', description: 'Protocol 1 framing structures and packet definitions', category: 'network' },
  { name: 'si5351.c', path: '/firmware/si5351.c', description: 'On-board Golden Integer LO calculation engine & I2C register driver', category: 'driver' },
  { name: 'si5351.h', path: '/firmware/si5351.h', description: 'Si5351a constants, register maps, and candidate types', category: 'driver' },
  { name: 'i2s_rx.pio', path: '/firmware/i2s_rx.pio', description: 'RP2040 PIO assembly program for PCM1808 I2S slave receive', category: 'driver' },
  { name: 'usb_descriptors.c', path: '/firmware/usb_descriptors.c', description: 'UAC1 (48k/96k 24-bit S24_3LE) & CDC composite descriptors', category: 'usb' },
  { name: 'tusb_config.h', path: '/firmware/tusb_config.h', description: 'TinyUSB audio buffers and endpoint sizing', category: 'usb' },
  { name: 'wifi_config.h', path: '/firmware/wifi_config.h', description: 'Network credentials and TCP control port definitions', category: 'network' },
  { name: 'CMakeLists.txt', path: '/firmware/CMakeLists.txt', description: 'Pico SDK + CYW43 lwIP + TinyUSB build manifest', category: 'build' },
  { name: 'quisk_conf_unified.py', path: '/firmware/quisk_conf_unified.py', description: 'Host-side Quisk hardware driver supporting direct frequency tuning', category: 'build' },
];

function gcd(a: number, b: number): number {
  while (b !== 0) {
    const t = b;
    b = a % b;
    a = t;
  }
  return a;
}

export default function App() {
  const [activeTab, setActiveTab] = useState<'calculator' | 'architecture' | 'files' | 'setup'>('calculator');
  const [testFreq, setTestFreq] = useState<number>(7050000);
  const [sampleRate, setSampleRate] = useState<number>(48000);
  const [isJohnson, setIsJohnson] = useState<boolean>(false);
  const [copiedPath, setCopiedPath] = useState<string | null>(null);

  // Client-side simulation of the RP2040 on-board Golden Integer LO algorithm
  const loCandidates = useMemo(() => {
    const crystal = 24576000;
    const mult = isJohnson ? 4 : 1;
    const nStep = isJohnson ? 1 : 2;
    const halfBw = sampleRate / 2;
    const targetSi = testFreq * mult;

    const nMin = Math.max(isJohnson ? 4 : 6, Math.ceil(600000000 / targetSi));
    const nMax = Math.min(isJohnson ? 127 : 126, Math.floor(900000000 / targetSi));
    const adjustedNMin = (!isJohnson && nMin % 2 !== 0) ? nMin + 1 : nMin;

    const list: Array<{
      n: number;
      m: number;
      lo: number;
      offset: number;
      rankQ: number;
      p1: number;
      p2: number;
      p3: number;
      isGolden: boolean;
    }> = [];

    for (let n = adjustedNMin; n <= nMax; n += nStep) {
      const prod = targetSi * n;
      const m = Math.round(prod / crystal);
      if (m <= 14 || m >= 91) continue;

      const loLogical = (crystal * m) / (n * mult);
      const offset = loLogical - testFreq;

      if (Math.abs(offset) < halfBw) {
        const g = gcd(m, n);
        const rankQ = Math.floor(n / g);
        const p1 = 128 * m - 512;
        list.push({
          n,
          m,
          lo: Math.round(loLogical),
          offset: Math.round(offset),
          rankQ,
          p1,
          p2: 0,
          p3: 1,
          isGolden: true,
        });
      }
    }

    list.sort((a, b) => {
      if (b.rankQ !== a.rankQ) return b.rankQ - a.rankQ; // High q = highest quality
      if (Math.abs(a.offset) !== Math.abs(b.offset)) return Math.abs(a.offset) - Math.abs(b.offset);
      return a.n - b.n;
    });

    return list;
  }, [testFreq, sampleRate, isJohnson]);

  const copyToClipboard = (text: string, path: string) => {
    navigator.clipboard.writeText(text);
    setCopiedPath(path);
    setTimeout(() => setCopiedPath(null), 2000);
  };

  return (
    <div className="min-h-screen bg-slate-950 text-slate-100 flex flex-col font-sans selection:bg-cyan-500 selection:text-slate-950">
      {/* Header */}
      <header className="border-b border-slate-800/80 bg-slate-900/60 backdrop-blur-md sticky top-0 z-50">
        <div className="max-w-7xl mx-auto px-4 sm:px-6 lg:px-8 h-16 flex items-center justify-between">
          <div className="flex items-center space-x-3">
            <div className="w-10 h-10 rounded-xl bg-gradient-to-tr from-cyan-500 to-blue-600 flex items-center justify-center shadow-lg shadow-cyan-500/20 ring-1 ring-white/20">
              <Radio className="w-5 h-5 text-white" />
            </div>
            <div>
              <div className="flex items-center space-x-2">
                <h1 className="font-semibold text-lg tracking-tight text-white">Pico W SDR Unified Firmware</h1>
                <span className="px-2 py-0.5 text-xs font-medium rounded-full bg-cyan-500/10 text-cyan-400 border border-cyan-500/30">v0.2 Board</span>
              </div>
              <p className="text-xs text-slate-400">OpenHPSDR Protocol 1 (WiFi) + UAC1 24-bit Audio (USB)</p>
            </div>
          </div>

          <div className="flex items-center space-x-2">
            <div className="flex items-center space-x-1 px-3 py-1.5 rounded-lg bg-slate-800/80 border border-slate-700/60 text-xs font-medium text-slate-300">
              <Cpu className="w-3.5 h-3.5 text-cyan-400 mr-1" />
              <span>RP2040 @ 250 MHz</span>
            </div>
            <div className="flex items-center space-x-1 px-3 py-1.5 rounded-lg bg-slate-800/80 border border-slate-700/60 text-xs font-medium text-slate-300">
              <Wifi className="w-3.5 h-3.5 text-emerald-400 mr-1" />
              <span>CYW43439</span>
            </div>
          </div>
        </div>
      </header>

      {/* Navigation Tabs */}
      <div className="border-b border-slate-800/80 bg-slate-900/30">
        <div className="max-w-7xl mx-auto px-4 sm:px-6 lg:px-8 flex space-x-1">
          <button
            id="tab-calculator"
            onClick={() => setActiveTab('calculator')}
            className={`px-4 py-3 text-sm font-medium border-b-2 flex items-center space-x-2 transition-colors ${
              activeTab === 'calculator'
                ? 'border-cyan-500 text-cyan-400 bg-cyan-500/5'
                : 'border-transparent text-slate-400 hover:text-slate-200 hover:border-slate-700'
            }`}
          >
            <Sliders className="w-4 h-4" />
            <span>On-Board LO Calculator</span>
          </button>
          <button
            id="tab-architecture"
            onClick={() => setActiveTab('architecture')}
            className={`px-4 py-3 text-sm font-medium border-b-2 flex items-center space-x-2 transition-colors ${
              activeTab === 'architecture'
                ? 'border-cyan-500 text-cyan-400 bg-cyan-500/5'
                : 'border-transparent text-slate-400 hover:text-slate-200 hover:border-slate-700'
            }`}
          >
            <Layers className="w-4 h-4" />
            <span>Dual-Mode Architecture</span>
          </button>
          <button
            id="tab-files"
            onClick={() => setActiveTab('files')}
            className={`px-4 py-3 text-sm font-medium border-b-2 flex items-center space-x-2 transition-colors ${
              activeTab === 'files'
                ? 'border-cyan-500 text-cyan-400 bg-cyan-500/5'
                : 'border-transparent text-slate-400 hover:text-slate-200 hover:border-slate-700'
            }`}
          >
            <FileCode className="w-4 h-4" />
            <span>Source Code Inventory</span>
          </button>
          <button
            id="tab-setup"
            onClick={() => setActiveTab('setup')}
            className={`px-4 py-3 text-sm font-medium border-b-2 flex items-center space-x-2 transition-colors ${
              activeTab === 'setup'
                ? 'border-cyan-500 text-cyan-400 bg-cyan-500/5'
                : 'border-transparent text-slate-400 hover:text-slate-200 hover:border-slate-700'
            }`}
          >
            <Terminal className="w-4 h-4" />
            <span>Build & Connect Guide</span>
          </button>
        </div>
      </div>

      {/* Content Area */}
      <main className="max-w-7xl mx-auto px-4 sm:px-6 lg:px-8 py-8 flex-1 w-full">
        {activeTab === 'calculator' && (
          <div className="space-y-6">
            <div className="bg-slate-900/60 border border-slate-800 rounded-2xl p-6 backdrop-blur-sm">
              <div className="flex flex-col md:flex-row md:items-center justify-between gap-4 pb-6 border-b border-slate-800">
                <div>
                  <h2 className="text-xl font-semibold text-white flex items-center space-x-2">
                    <span>RP2040 On-Board Golden LO Calculator</span>
                    <span className="text-xs px-2 py-0.5 bg-emerald-500/10 text-emerald-400 border border-emerald-500/30 rounded-full">Active in Firmware</span>
                  </h2>
                  <p className="text-sm text-slate-400 mt-1">
                    When standard SDR software sends <code className="text-cyan-300 bg-slate-800 px-1.5 py-0.5 rounded">FREQ,&lt;hz&gt;</code>, the Cortex-M0+ calculates integer dividers on-chip in ~8 µs.
                  </p>
                </div>

                <div className="flex flex-wrap items-center gap-3">
                  {/* Preset Bands */}
                  <span className="text-xs text-slate-400 font-medium">Quick Tune:</span>
                  {[
                    { label: '80m (3.65M)', freq: 3650000 },
                    { label: '40m (7.05M)', freq: 7050000 },
                    { label: '20m (14.10M)', freq: 14100000 },
                    { label: '10m (28.40M)', freq: 28400000 },
                  ].map((b) => (
                    <button
                      key={b.label}
                      onClick={() => setTestFreq(b.freq)}
                      className="px-3 py-1 text-xs font-medium bg-slate-800 hover:bg-slate-700 text-slate-300 rounded-lg border border-slate-700 transition"
                    >
                      {b.label}
                    </button>
                  ))}
                </div>
              </div>

              {/* Parameter Controls */}
              <div className="grid grid-cols-1 md:grid-cols-3 gap-6 pt-6">
                <div>
                  <label className="block text-xs font-semibold text-slate-400 uppercase tracking-wider mb-2">
                    Target RF Frequency (Hz)
                  </label>
                  <div className="relative">
                    <input
                      type="number"
                      value={testFreq}
                      onChange={(e) => setTestFreq(Number(e.target.value))}
                      step={1000}
                      min={1000000}
                      max={60000000}
                      className="w-full bg-slate-950 border border-slate-700 rounded-xl px-4 py-2.5 text-cyan-300 font-mono text-base focus:outline-none focus:ring-2 focus:ring-cyan-500 focus:border-transparent"
                    />
                    <span className="absolute right-3 top-3 text-xs text-slate-500 font-mono">Hz</span>
                  </div>
                  <span className="text-xs text-slate-500 mt-1 block">{(testFreq / 1000000).toFixed(6)} MHz</span>
                </div>

                <div>
                  <label className="block text-xs font-semibold text-slate-400 uppercase tracking-wider mb-2">
                    ADC Sample Rate
                  </label>
                  <div className="grid grid-cols-2 gap-2">
                    <button
                      onClick={() => setSampleRate(48000)}
                      className={`py-2.5 px-4 rounded-xl text-sm font-medium border transition ${
                        sampleRate === 48000
                          ? 'bg-cyan-500/10 border-cyan-500 text-cyan-400'
                          : 'bg-slate-950 border-slate-800 text-slate-400 hover:border-slate-700'
                      }`}
                    >
                      48 kHz (M1=0)
                    </button>
                    <button
                      onClick={() => setSampleRate(96000)}
                      className={`py-2.5 px-4 rounded-xl text-sm font-medium border transition ${
                        sampleRate === 96000
                          ? 'bg-cyan-500/10 border-cyan-500 text-cyan-400'
                          : 'bg-slate-950 border-slate-800 text-slate-400 hover:border-slate-700'
                      }`}
                    >
                      96 kHz (M1=1)
                    </button>
                  </div>
                </div>

                <div>
                  <label className="block text-xs font-semibold text-slate-400 uppercase tracking-wider mb-2">
                    LO Mixer Topology
                  </label>
                  <div className="grid grid-cols-2 gap-2">
                    <button
                      onClick={() => setIsJohnson(false)}
                      className={`py-2.5 px-4 rounded-xl text-sm font-medium border transition ${
                        !isJohnson
                          ? 'bg-cyan-500/10 border-cyan-500 text-cyan-400'
                          : 'bg-slate-950 border-slate-800 text-slate-400 hover:border-slate-700'
                      }`}
                    >
                      DIRECT (90° Phase)
                    </button>
                    <button
                      onClick={() => setIsJohnson(true)}
                      className={`py-2.5 px-4 rounded-xl text-sm font-medium border transition ${
                        isJohnson
                          ? 'bg-cyan-500/10 border-cyan-500 text-cyan-400'
                          : 'bg-slate-950 border-slate-800 text-slate-400 hover:border-slate-700'
                      }`}
                    >
                      JOHNSON (÷4)
                    </button>
                  </div>
                </div>
              </div>
            </div>

            {/* Results Table */}
            <div className="bg-slate-900/60 border border-slate-800 rounded-2xl overflow-hidden backdrop-blur-sm">
              <div className="p-6 border-b border-slate-800 flex items-center justify-between">
                <div>
                  <h3 className="text-base font-semibold text-white">Computed Integer Candidates</h3>
                  <p className="text-xs text-slate-400">Ranked by lowest-term spur harmonic decoupling (<span className="text-emerald-400 font-mono">q = n/gcd(m,n)</span>)</p>
                </div>
                <div className="text-xs font-mono text-slate-400 bg-slate-950 px-3 py-1.5 rounded-lg border border-slate-800">
                  XTAL: 24.576000 MHz (Fixed PCM1808 SCKI)
                </div>
              </div>

              {loCandidates.length > 0 ? (
                <div className="overflow-x-auto">
                  <table className="w-full text-left text-sm">
                    <thead className="bg-slate-950/80 text-xs uppercase font-semibold text-slate-400 border-b border-slate-800">
                      <tr>
                        <th className="py-3.5 px-6">Quality Tier</th>
                        <th className="py-3.5 px-4 font-mono">LO Frequency</th>
                        <th className="py-3.5 px-4 font-mono">Offset</th>
                        <th className="py-3.5 px-4 font-mono">Divider N</th>
                        <th className="py-3.5 px-4 font-mono">Mult M</th>
                        <th className="py-3.5 px-4 font-mono">Rank (q)</th>
                        <th className="py-3.5 px-4 font-mono">P1 Parameter</th>
                        <th className="py-3.5 px-6">Generated CDC/TCP Command</th>
                      </tr>
                    </thead>
                    <tbody className="divide-y divide-slate-800/60 font-mono text-xs">
                      {loCandidates.map((c, idx) => {
                        const isBest = idx === 0;
                        const isMid = idx === Math.floor(loCandidates.length / 2) && loCandidates.length >= 3;
                        const isWorst = idx === loCandidates.length - 1 && loCandidates.length >= 2;
                        const cmd = `FREQ,${testFreq}`;
                        const fullCmd = `FREQ,${c.lo},${c.n},${c.m},0,1,${c.p1},0,1`;

                        return (
                          <tr key={`${c.n}-${c.m}`} className={isBest ? 'bg-emerald-950/20' : 'hover:bg-slate-800/30'}>
                            <td className="py-3 px-6 font-sans">
                              {isBest ? (
                                <span className="inline-flex items-center px-2.5 py-0.5 rounded-full text-xs font-medium bg-emerald-500/10 text-emerald-400 border border-emerald-500/30">
                                  BEST (Default)
                                </span>
                              ) : isMid ? (
                                <span className="inline-flex items-center px-2.5 py-0.5 rounded-full text-xs font-medium bg-amber-500/10 text-amber-400 border border-amber-500/30">
                                  MIDDLE
                                </span>
                              ) : isWorst ? (
                                <span className="inline-flex items-center px-2.5 py-0.5 rounded-full text-xs font-medium bg-rose-500/10 text-rose-400 border border-rose-500/30">
                                  WORST
                                </span>
                              ) : (
                                <span className="text-slate-500">Tier #{idx + 1}</span>
                              )}
                            </td>
                            <td className="py-3 px-4 text-cyan-300 font-bold">{c.lo.toLocaleString()} Hz</td>
                            <td className="py-3 px-4 text-slate-300">{c.offset > 0 ? `+${c.offset}` : c.offset} Hz</td>
                            <td className="py-3 px-4 text-slate-300">{c.n}</td>
                            <td className="py-3 px-4 text-slate-300">{c.m}</td>
                            <td className="py-3 px-4 text-emerald-400 font-bold">{c.rankQ}</td>
                            <td className="py-3 px-4 text-slate-400">{c.p1}</td>
                            <td className="py-3 px-6">
                              <div className="flex items-center space-x-2">
                                <span className="text-slate-400 bg-slate-950 px-2 py-1 rounded border border-slate-800">{cmd}</span>
                                <span className="text-slate-600">or</span>
                                <span className="text-slate-500 truncate max-w-[150px]">{fullCmd}</span>
                              </div>
                            </td>
                          </tr>
                        );
                      })}
                    </tbody>
                  </table>
                </div>
              ) : (
                <div className="p-8 text-center text-slate-400">
                  <p>No exact integer dividers within passband (±{sampleRate / 2} Hz). Firmware will use high-precision fractional PLL synthesis for exact frequency match.</p>
                </div>
              )}
            </div>
          </div>
        )}

        {activeTab === 'architecture' && (
          <div className="space-y-6">
            <div className="grid grid-cols-1 md:grid-cols-2 gap-6">
              {/* USB Architecture Card */}
              <div className="bg-slate-900/60 border border-slate-800 rounded-2xl p-6 backdrop-blur-sm">
                <div className="flex items-center space-x-3 mb-4">
                  <div className="w-10 h-10 rounded-xl bg-blue-500/10 border border-blue-500/30 flex items-center justify-center text-blue-400">
                    <Usb className="w-5 h-5" />
                  </div>
                  <div>
                    <h3 className="font-semibold text-white">USB Mode (UAC1 + CDC)</h3>
                    <p className="text-xs text-slate-400">Direct PC Connection via USB Cable</p>
                  </div>
                </div>
                <div className="space-y-3 text-sm text-slate-300">
                  <div className="p-3 bg-slate-950 rounded-xl border border-slate-800/80">
                    <div className="font-medium text-xs text-slate-400 uppercase tracking-wider mb-1">Audio Class 1.0 (UAC1)</div>
                    <p className="text-xs text-slate-300">24-bit stereo (<code className="text-cyan-300">S24_3LE</code>) with dynamic alternate settings: Alt 1 (48 kHz, 294B) & Alt 2 (96 kHz, 582B).</p>
                  </div>
                  <div className="p-3 bg-slate-950 rounded-xl border border-slate-800/80">
                    <div className="font-medium text-xs text-slate-400 uppercase tracking-wider mb-1">CDC Serial Protocol</div>
                    <p className="text-xs text-slate-300">Accepts <code className="text-cyan-300">FREQ,&lt;hz&gt;</code> or full 8-register dumps. Also handles <code className="text-cyan-300">RATE</code>, <code className="text-cyan-300">VER</code>, <code className="text-cyan-300">XTAL</code>.</p>
                  </div>
                  <div className="p-3 bg-slate-950 rounded-xl border border-slate-800/80">
                    <div className="font-medium text-xs text-slate-400 uppercase tracking-wider mb-1">Host Compatibility</div>
                    <p className="text-xs text-slate-300">Works with Quisk, ALSA arecord, PulseAudio, PipeWire, and GNU Radio.</p>
                  </div>
                </div>
              </div>

              {/* WiFi Architecture Card */}
              <div className="bg-slate-900/60 border border-slate-800 rounded-2xl p-6 backdrop-blur-sm">
                <div className="flex items-center space-x-3 mb-4">
                  <div className="w-10 h-10 rounded-xl bg-emerald-500/10 border border-emerald-500/30 flex items-center justify-center text-emerald-400">
                    <Wifi className="w-5 h-5" />
                  </div>
                  <div>
                    <h3 className="font-semibold text-white">WiFi Mode (OpenHPSDR Protocol 1)</h3>
                    <p className="text-xs text-slate-400">Wireless LAN Operation via CYW43439</p>
                  </div>
                </div>
                <div className="space-y-3 text-sm text-slate-300">
                  <div className="p-3 bg-slate-950 rounded-xl border border-slate-800/80">
                    <div className="font-medium text-xs text-slate-400 uppercase tracking-wider mb-1">Protocol 1 UDP Streaming</div>
                    <p className="text-xs text-slate-300">Port 1024 auto-discovery (<code className="text-emerald-300">0xEFFE 0x02</code>) and 1032-byte 24-bit I/Q packet framing (<code className="text-emerald-300">EP6</code>).</p>
                  </div>
                  <div className="p-3 bg-slate-950 rounded-xl border border-slate-800/80">
                    <div className="font-medium text-xs text-slate-400 uppercase tracking-wider mb-1">Embedded Command & Control</div>
                    <p className="text-xs text-slate-300">Decodes 5-byte C&C headers for frequency and sample rate changes directly from network packets.</p>
                  </div>
                  <div className="p-3 bg-slate-950 rounded-xl border border-slate-800/80">
                    <div className="font-medium text-xs text-slate-400 uppercase tracking-wider mb-1">Host Compatibility</div>
                    <p className="text-xs text-slate-300">SDR++, LinHPSDR, Thetis, Quisk (Hermes module), and SparkSDR.</p>
                  </div>
                </div>
              </div>
            </div>

            {/* Core Distribution Flow */}
            <div className="bg-slate-900/60 border border-slate-800 rounded-2xl p-6 backdrop-blur-sm">
              <h3 className="font-semibold text-white mb-4">RP2040 Dual-Core Task Allocation</h3>
              <div className="grid grid-cols-1 md:grid-cols-2 gap-4">
                <div className="p-4 bg-slate-950/80 rounded-xl border border-slate-800">
                  <div className="flex items-center space-x-2 text-cyan-400 font-semibold text-sm mb-2">
                    <Cpu className="w-4 h-4" />
                    <span>Core 1: Real-Time Audio Engine</span>
                  </div>
                  <ul className="text-xs text-slate-400 space-y-1.5 list-disc list-inside">
                    <li>Drives DMA ping-pong IRQ without jitter</li>
                    <li>Samples PCM1808 via PIO state machine (GPIO 9, 10, 11)</li>
                    <li>Transfers 24/32-bit audio buffers to Core 0 via Inter-Core FIFO</li>
                    <li>Handles dynamic rate reconfiguration sentinels (48k/96k)</li>
                  </ul>
                </div>

                <div className="p-4 bg-slate-950/80 rounded-xl border border-slate-800">
                  <div className="flex items-center space-x-2 text-emerald-400 font-semibold text-sm mb-2">
                    <Activity className="w-4 h-4" />
                    <span>Core 0: Dispatch & Network Stack</span>
                  </div>
                  <ul className="text-xs text-slate-400 space-y-1.5 list-disc list-inside">
                    <li>Runs CYW43 WiFi driver and lwIP TCP/UDP stack</li>
                    <li>Formats OpenHPSDR Protocol 1 packets (1032 bytes)</li>
                    <li>Packs S24_3LE audio frames for TinyUSB UAC1 endpoint</li>
                    <li>Executes on-chip Si5351a Golden LO calculation and I2C tuning</li>
                  </ul>
                </div>
              </div>
            </div>
          </div>
        )}

        {activeTab === 'files' && (
          <div className="space-y-4">
            <div className="flex items-center justify-between mb-2">
              <div>
                <h3 className="text-base font-semibold text-white">Firmware Source Inventory</h3>
                <p className="text-xs text-slate-400">All files generated and ready in the <code className="text-cyan-300">/firmware</code> directory</p>
              </div>
            </div>

            <div className="grid grid-cols-1 gap-3">
              {FIRMWARE_FILES.map((f) => (
                <div key={f.path} className="p-4 bg-slate-900/60 border border-slate-800 rounded-xl flex items-center justify-between hover:border-slate-700 transition">
                  <div className="flex items-center space-x-3">
                    <div className="w-8 h-8 rounded-lg bg-slate-800 flex items-center justify-center text-cyan-400">
                      <FileCode className="w-4 h-4" />
                    </div>
                    <div>
                      <div className="flex items-center space-x-2">
                        <span className="font-mono text-sm font-semibold text-white">{f.name}</span>
                        <span className="text-xs font-mono text-slate-500">{f.path}</span>
                      </div>
                      <p className="text-xs text-slate-400 mt-0.5">{f.description}</p>
                    </div>
                  </div>

                  <button
                    onClick={() => copyToClipboard(f.path, f.path)}
                    className="px-3 py-1.5 bg-slate-800 hover:bg-slate-700 text-slate-300 text-xs font-medium rounded-lg border border-slate-700 flex items-center space-x-1.5 transition"
                  >
                    {copiedPath === f.path ? <CheckCircle2 className="w-3.5 h-3.5 text-emerald-400" /> : <Copy className="w-3.5 h-3.5" />}
                    <span>{copiedPath === f.path ? 'Copied' : 'Copy Path'}</span>
                  </button>
                </div>
              ))}
            </div>
          </div>
        )}

        {activeTab === 'setup' && (
          <div className="space-y-6">
            <div className="bg-slate-900/60 border border-slate-800 rounded-2xl p-6 backdrop-blur-sm space-y-4">
              <h3 className="text-base font-semibold text-white">How to Compile for Raspberry Pi Pico W</h3>
              <p className="text-sm text-slate-400">
                To generate the <code className="text-cyan-300 font-mono">sdr_pico_w_unified.uf2</code> file, run CMake with the <code className="text-cyan-300 font-mono">-DPICO_BOARD=pico_w</code> flag:
              </p>

              <div className="bg-slate-950 p-4 rounded-xl border border-slate-800 font-mono text-xs text-slate-300 space-y-2">
                <p className="text-slate-500"># 1. Ensure TinyUSB (&gt;= 0.19) is available</p>
                <p className="text-cyan-400">git clone https://github.com/hathach/tinyusb ~/tinyusb</p>
                <p className="text-cyan-400">export PICO_TINYUSB_PATH=$HOME/tinyusb</p>
                <p className="text-slate-500 mt-2"># 2. Build the firmware for Pico W</p>
                <p className="text-cyan-400">cd firmware</p>
                <p className="text-cyan-400">mkdir -p build && cd build</p>
                <p className="text-cyan-400">cmake -DPICO_BOARD=pico_w ..</p>
                <p className="text-cyan-400">make -j4</p>
              </div>
            </div>

            <div className="bg-slate-900/60 border border-slate-800 rounded-2xl p-6 backdrop-blur-sm space-y-4">
              <h3 className="text-base font-semibold text-white">Connecting with SDR++ (WiFi OpenHPSDR Mode)</h3>
              <ol className="list-decimal list-inside text-sm text-slate-300 space-y-2">
                <li>Set your WiFi SSID and password in <code className="text-cyan-300 font-mono">wifi_config.h</code> and flash the Pico W.</li>
                <li>Connect your PC to the same WiFi network.</li>
                <li>Open **SDR++**, set the **Source** dropdown to **OpenHPSDR**.</li>
                <li>Click **Discover** — your Pico W SDR will appear with its MAC address and board ID.</li>
                <li>Click **Play** to start receiving live 24-bit stereo I/Q audio over WiFi!</li>
              </ol>
            </div>
          </div>
        )}
      </main>

      {/* Footer */}
      <footer className="border-t border-slate-800 bg-slate-950 py-4 text-center text-xs text-slate-500">
        Intro-to-CAD-2026 v0.2 Board • RP2040 + CYW43439 + PCM1808 + Si5351a • OpenHPSDR Protocol 1 & UAC1
      </footer>
    </div>
  );
}
