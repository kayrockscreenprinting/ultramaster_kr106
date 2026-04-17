// KR-106 Web Audio loader
//
// Tries AudioWorklet first (glitch-free, separate thread), falls back to
// ScriptProcessorNode (main thread, may glitch under load).
//
// Usage:
//   const synth = new KR106Audio();
//   await synth.init();
//   synth.noteOn(60, 127);
//   synth.noteOff(60);
//   synth.setParam(10, 0.5);
//   synth.loadPreset(0);
//   await synth.initMidi();

const KR106_CACHE = '1776384977';

class KR106Audio {
  constructor() {
    this.ctx = null;
    this.node = null;
    this.mod = null;
    this.dsp = 0;
    this.ready = false;
    this.ptrL = 0;
    this.ptrR = 0;
    this._useWorklet = false;
    this._presetNames = {};
  }

  async init() {
    this.ctx = new AudioContext();
    const sr = this.ctx.sampleRate;

    // Try AudioWorklet first
    try {
      await this.ctx.audioWorklet.addModule(`dist/kr106-processor.js?v=${KR106_CACHE}`);
      this.node = new AudioWorkletNode(this.ctx, 'kr106-processor', {
        outputChannelCount: [2]
      });

      // Fetch both WASM glue JS and binary on main thread
      // (worklet has no fetch/importScripts)
      const [glueResp, wasmResp] = await Promise.all([
        fetch(`dist/kr106_dsp.js?v=${KR106_CACHE}`),
        fetch(`dist/kr106_dsp.wasm?v=${KR106_CACHE}`)
      ]);
      const wasmText = await glueResp.text();
      const wasmBinary = await wasmResp.arrayBuffer();

      // Wait for WASM to initialize inside the worklet
      await new Promise((resolve, reject) => {
        this.node.port.onmessage = (e) => {
          if (e.data.type === 'ready') resolve();
          else if (e.data.type === 'error') reject(new Error(e.data.message));
          else if (e.data.type === 'presetName') {
            this._presetNames[e.data.index] = e.data.name;
          }
          else if (e.data.type === 'presetValues') {
            if (this.onPresetValues) this.onPresetValues(e.data);
          }
          else if (e.data.type === 'sliderUpdate') {
            if (this.onSliderUpdate) this.onSliderUpdate(e.data);
          }
          else if (e.data.type === 'scope') {
            this._scopeData = e.data;
          }
        };
        this.node.port.postMessage({ type: 'init', wasmText, wasmBinary }, [wasmBinary]);
      });

      this.node.connect(this.ctx.destination);
      this._useWorklet = true;
      this.ready = true;

      // Load a second WASM instance on the main thread for stateless queries
      // (tooltip Hz/ms/dB conversions). No kr106_create -- just the pure functions.
      const script = document.createElement('script');
      script.src = `dist/kr106_dsp.js?v=${KR106_CACHE}`;
      await new Promise((res, rej) => { script.onload = res; script.onerror = rej; document.head.appendChild(script); });
      this.mod = await createKR106();

      // Batch-fetch all preset names from the calculator module
      this._presetNames = {};
      const numPresets = this.mod._kr106_get_num_presets();
      for (let i = 0; i < numPresets; i++) {
        const ptr = this.mod._kr106_get_preset_name(i);
        this._presetNames[i] = this.mod.UTF8ToString(ptr);
      }

      console.log(`KR-106 ready (AudioWorklet): ${sr} Hz, cache=${KR106_CACHE}`);
      return;
    } catch (err) {
      console.warn('AudioWorklet failed, falling back to ScriptProcessor:', err.message);
    }

    // Fallback: ScriptProcessorNode on main thread
    const script = document.createElement('script');
    script.src = `dist/kr106_dsp.js?v=${KR106_CACHE}`;
    await new Promise((resolve, reject) => {
      script.onload = resolve;
      script.onerror = reject;
      document.head.appendChild(script);
    });

    this.mod = await createKR106();
    this.dsp = this.mod._kr106_create(sr);

    const maxFrames = 4096;
    this.ptrL = this.mod._malloc(maxFrames * 4);
    this.ptrR = this.mod._malloc(maxFrames * 4);

    const bufferSize = 2048;
    this.node = this.ctx.createScriptProcessor(bufferSize, 0, 2);
    const self = this;
    this.node.onaudioprocess = (e) => {
      const outL = e.outputBuffer.getChannelData(0);
      const outR = e.outputBuffer.getChannelData(1);
      const n = outL.length;

      self.mod._kr106_process(self.dsp, self.ptrL, self.ptrR, n);

      const heap = self.mod.HEAPF32;
      const offL = self.ptrL >> 2;
      const offR = self.ptrR >> 2;
      outL.set(heap.subarray(offL, offL + n));
      outR.set(heap.subarray(offR, offR + n));
    };

    this.node.connect(this.ctx.destination);
    this._useWorklet = false;
    this.ready = true;

    // Cache preset names
    const numPresets = this.mod._kr106_get_num_presets();
    for (let i = 0; i < numPresets; i++) {
      const ptr = this.mod._kr106_get_preset_name(i);
      this._presetNames[i] = this.mod.UTF8ToString(ptr);
    }

    console.log(`KR-106 ready (ScriptProcessor): ${sr} Hz, cache=${KR106_CACHE}`);
  }

  _fetchPresetNames() {
    // Request all preset names from the worklet
    const n = 256; // max presets
    for (let i = 0; i < n; i++) {
      this.node.port.postMessage({ type: 'preset-name-query', index: i });
    }
  }

  noteOn(note, velocity = 127) {
    if (!this.ready) return;
    if (this._useWorklet) {
      this.node.port.postMessage({ type: 'midi', data: [0x90, note, velocity] });
    } else {
      this.mod._kr106_note_on(this.dsp, note, velocity);
    }
  }

  noteOff(note) {
    if (!this.ready) return;
    if (this._useWorklet) {
      this.node.port.postMessage({ type: 'midi', data: [0x80, note, 0] });
    } else {
      this.mod._kr106_note_off(this.dsp, note);
    }
  }

  forceRelease(note) {
    if (!this.ready) return;
    if (this._useWorklet) {
      this.node.port.postMessage({ type: 'midi', data: [0x80, note, 0] });
    } else {
      this.mod._kr106_force_release(this.dsp, note);
    }
  }

  controlChange(cc, value) {
    if (!this.ready) return;
    if (this._useWorklet) {
      this.node.port.postMessage({ type: 'midi', data: [0xB0, cc, Math.round(value * 127)] });
    } else {
      this.mod._kr106_control_change(this.dsp, cc, value);
    }
  }

  setParam(index, value) {
    if (!this.ready) return;
    if (this._useWorklet) {
      this.node.port.postMessage({ type: 'param', index, value });
    } else {
      this.mod._kr106_set_param(this.dsp, index, value);
    }
  }

  loadPreset(index) {
    if (!this.ready) return;
    if (this._useWorklet) {
      this.node.port.postMessage({ type: 'preset', index });
    } else {
      this.mod._kr106_load_preset(this.dsp, index);
    }
  }

  setBankOffset(offset) {
    if (this._useWorklet) {
      this.node.port.postMessage({ type: 'bankOffset', value: offset });
    }
    // Also update main-thread module so getPresetName returns correct bank
    if (this.mod && this.mod._kr106_set_bank_offset) {
      this.mod._kr106_set_bank_offset(offset);
    }
    this._presetNames = {}; // invalidate cache
  }

  getPresetName(index) {
    if (this._presetNames[index]) return this._presetNames[index];
    if (!this.mod) return '';
    const ptr = this.mod._kr106_get_preset_name(index);
    const name = this.mod.UTF8ToString(ptr);
    this._presetNames[index] = name;
    return name;
  }

  getNumPresets() {
    if (!this.mod) return 0;
    return this.mod._kr106_get_num_presets();
  }

  getPresetValue(presetIdx, paramIdx) {
    if (!this.mod) return 0;
    return this.mod._kr106_get_preset_value(presetIdx, paramIdx);
  }

  getVcfSlider() {
    if (this._useWorklet) return 0;
    return this.ready ? this.mod._kr106_get_vcf_slider(this.dsp) : 0;
  }

  getHpfSlider() {
    if (this._useWorklet) return 0;
    return this.ready ? this.mod._kr106_get_hpf_slider(this.dsp) : 0;
  }

  setVoices(n) {
    if (!this.ready) return;
    if (this._useWorklet) {
      this.node.port.postMessage({ type: 'voices', count: n });
    } else {
      this.mod._kr106_set_voices(this.dsp, n);
    }
  }

  setIgnoreVelocity(b) {
    if (!this.ready) return;
    if (this._useWorklet) {
      this.node.port.postMessage({ type: 'ignoreVel', value: b ? 1 : 0 });
    } else {
      this.mod._kr106_set_ignore_velocity(this.dsp, b ? 1 : 0);
    }
  }

  getScopeData() {
    if (this._useWorklet) {
      // Scope data comes via postMessage from worklet
      if (!this._scopeData) return null;
      const sd = this._scopeData;
      this._scopeData = null;
      return sd;
    }
    if (!this.mod) return null;
    const ringPtr = this.mod._kr106_get_scope_ring();
    const ringRPtr = this.mod._kr106_get_scope_ring_r();
    const syncPtr = this.mod._kr106_get_scope_sync_ring();
    const writePos = this.mod._kr106_get_scope_write_pos();
    const ringSize = this.mod._kr106_get_scope_ring_size();
    const heap = this.mod.HEAPF32;
    const ringOff = ringPtr >> 2;
    const ringROff = ringRPtr >> 2;
    const syncOff = syncPtr >> 2;
    return { heap, ringOff, ringROff, syncOff, writePos, ringSize };
  }

  async initMidi() {
    if (!navigator.requestMIDIAccess) {
      console.warn('Web MIDI not available');
      return;
    }
    const midi = await navigator.requestMIDIAccess();
    const connect = (input) => {
      console.log('MIDI: ' + input.name);
      input.onmidimessage = (e) => {
        const d = e.data;
        const st = d[0] & 0xF0;
        if (st === 0x90 && d[2] > 0) {
          this.noteOn(d[1], d[2]);
          if (this.onMidiNote) this.onMidiNote(d[1], true);
        }
        else if (st === 0x80 || (st === 0x90 && d[2] === 0)) {
          this.noteOff(d[1]);
          if (this.onMidiNote) this.onMidiNote(d[1], false);
        }
        else if (st === 0xE0) {
          const bend = ((d[2] << 7) | d[1]) / 8192 - 1; // -1..+1
          this.setParam(36, bend); // kBender
        }
        else if (st === 0xB0) {
          if (d[1] === 1) this.controlChange(1, d[2] / 127); // mod wheel -> LFO trigger
        }
      };
    };
    for (const input of midi.inputs.values()) connect(input);
    midi.onstatechange = (e) => {
      if (e.port.type === 'input' && e.port.state === 'connected') connect(e.port);
    };
  }

  async resume() {
    if (this.ctx && this.ctx.state === 'suspended') await this.ctx.resume();
  }
}

if (typeof module !== 'undefined') module.exports = KR106Audio;
if (typeof window !== 'undefined') window.KR106Audio = KR106Audio;
