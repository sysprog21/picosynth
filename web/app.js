/* PicoSynth - Web UI */

let audioCtx = null;
let scriptNode = null;
let wasmReady = false;
let isPlaying = false;
let currentSource = null;
let melodyNotes = [];
let melodyBeats = [];
let currentNote = 0;

const NOTE_NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];

/* Status display */
function setStatus(msg, type = '') {
    const el = document.getElementById('status');
    el.textContent = msg;
    el.className = type;
}

/* Initialize Web Audio */
function initAudio() {
    if (audioCtx) return audioCtx;
    if (!wasmReady || typeof Module._picosynth_wasm_get_sample_rate !== 'function') {
        setStatus('Synth not ready', 'error');
        return null;
    }
    try {
        const sampleRate = Module._picosynth_wasm_get_sample_rate();
        audioCtx = new (window.AudioContext || window.webkitAudioContext)({ sampleRate });

        /* ScriptProcessor for real-time keyboard playback.
         * NOTE: createScriptProcessor is deprecated and may cause audio glitches.
         * AudioWorklet is the recommended modern approach.
         */
        const bufferSize = 2048;
        scriptNode = audioCtx.createScriptProcessor(bufferSize, 0, 1);
        scriptNode.onaudioprocess = (e) => {
            const output = e.outputBuffer.getChannelData(0);
            if (!wasmReady || currentNote === 0) {
                output.fill(0);
                return;
            }
            const ptr = Module._picosynth_wasm_render(bufferSize);
            if (!ptr) { output.fill(0); return; }
            const samples = new Int16Array(Module.HEAP16.buffer, ptr, bufferSize);
            for (let i = 0; i < bufferSize; i++) {
                output[i] = samples[i] / 32768.0;
            }
        };
        scriptNode.connect(audioCtx.destination);
    } catch (e) {
        setStatus('Audio not supported', 'error');
        return null;
    }
    return audioCtx;
}

/* WASM ready handler */
document.addEventListener('wasmReady', () => {
    if (Module._picosynth_wasm_init()) {
        wasmReady = true;
        setStatus('Ready', 'ready');
        buildKeyboard();
        buildMelodyButtons();
        initControls();
    } else {
        setStatus('Failed to initialize synth', 'error');
    }
});

/* Build piano keyboard */
function buildKeyboard() {
    const kbd = document.getElementById('keyboard');
    if (!kbd) return;
    kbd.innerHTML = '';

    const startNote = 48; /* C3 */
    const endNote = 72;   /* C5 */

    for (let midi = startNote; midi <= endNote; midi++) {
        const noteName = NOTE_NAMES[midi % 12];
        const isBlack = noteName.includes('#');

        if (isBlack) continue;

        const key = document.createElement('div');
        key.className = 'key white';
        key.dataset.midi = midi;

        const label = document.createElement('span');
        label.className = 'key-label';
        label.textContent = noteName + (Math.floor(midi / 12) - 1);
        key.appendChild(label);

        /* Check if next note is black */
        const nextMidi = midi + 1;
        if (nextMidi <= endNote && NOTE_NAMES[nextMidi % 12].includes('#')) {
            const black = document.createElement('div');
            black.className = 'key black';
            black.dataset.midi = nextMidi;
            key.appendChild(black);
        }

        kbd.appendChild(key);
    }

    /* Accessibility */
    kbd.tabIndex = 0;
    kbd.setAttribute('role', 'application');
    kbd.setAttribute('aria-label', 'Piano keyboard');

    /* Pointer events for mouse and touch */
    kbd.addEventListener('pointerdown', (e) => {
        const key = e.target.closest('.key');
        if (!key) return;
        kbd.setPointerCapture(e.pointerId);
        playNote(parseInt(key.dataset.midi));
    });
    kbd.addEventListener('pointerup', () => stopNote());
    kbd.addEventListener('pointercancel', () => stopNote());
}

/* Play/stop note */
function playNote(midi) {
    if (!wasmReady || midi === currentNote) return;
    initAudio();
    if (audioCtx && audioCtx.state === 'suspended') audioCtx.resume();

    if (currentNote !== 0) {
        const prev = document.querySelector(`.key[data-midi="${currentNote}"]`);
        if (prev) prev.classList.remove('active');
    }

    currentNote = midi;
    Module._picosynth_wasm_note_on(midi);

    const key = document.querySelector(`.key[data-midi="${midi}"]`);
    if (key) key.classList.add('active');
}

function stopNote() {
    if (!wasmReady || currentNote === 0) return;

    const key = document.querySelector(`.key[data-midi="${currentNote}"]`);
    if (key) key.classList.remove('active');

    Module._picosynth_wasm_note_off();
    currentNote = 0;
}

/* Keyboard shortcuts */
const keyMap = { a:60, w:61, s:62, e:63, d:64, f:65, t:66, g:67, y:68, h:69, u:70, j:71, k:72 };
document.addEventListener('keydown', (e) => {
    if (e.repeat || e.target.tagName === 'INPUT' || e.target.tagName === 'SELECT') return;
    const midi = keyMap[e.key.toLowerCase()];
    if (midi) playNote(midi);
});
document.addEventListener('keyup', (e) => {
    const midi = keyMap[e.key.toLowerCase()];
    if (midi && midi === currentNote) stopNote();
});

/* PicoSynth controls */
function initControls() {
    document.getElementById('wave1').addEventListener('change', (e) => {
        Module._picosynth_wasm_set_wave(0, 0, parseInt(e.target.value));
    });
    document.getElementById('wave2').addEventListener('change', (e) => {
        Module._picosynth_wasm_set_wave(0, 1, parseInt(e.target.value));
    });
    document.getElementById('filter').addEventListener('input', (e) => {
        Module._picosynth_wasm_set_filter_coeff(0, parseInt(e.target.value));
    });
}

/* Melody loading */
function parseMelody(text) {
    const notes = [], beats = [];
    for (const line of text.split('\n')) {
        const trimmed = line.trim();
        if (!trimmed || trimmed.startsWith('#')) continue;
        const parts = trimmed.split(/\s+/);
        if (parts.length < 2) continue;

        const noteStr = parts[0];
        const beat = parseInt(parts[1]);
        let midi = 0;

        if (noteStr !== '-' && noteStr.toUpperCase() !== 'R') {
            const match = noteStr.match(/^([A-Ga-g])([#b]?)(-?\d+)$/);
            if (match) {
                const noteMap = {C:0, D:2, E:4, F:5, G:7, A:9, B:11};
                let semitone = noteMap[match[1].toUpperCase()];
                if (match[2] === '#') semitone = (semitone + 1) % 12;
                else if (match[2] === 'b') semitone = (semitone + 11) % 12;
                midi = (parseInt(match[3]) + 1) * 12 + semitone;
            }
        }
        notes.push(midi);
        beats.push(beat);
    }
    return { notes, beats };
}

function displayMelody() {
    const el = document.getElementById('melody-display');
    if (melodyNotes.length === 0) { el.textContent = ''; return; }

    let text = '';
    for (let i = 0; i < melodyNotes.length; i++) {
        const n = melodyNotes[i];
        const name = n === 0 ? '-' : NOTE_NAMES[n % 12] + (Math.floor(n / 12) - 1);
        text += `${name}(${melodyBeats[i]}) `;
        if ((i + 1) % 8 === 0) text += '\n';
    }
    el.textContent = text.trim();
}

function loadMelody(text, name) {
    const { notes, beats } = parseMelody(text);
    melodyNotes = notes;
    melodyBeats = beats;
    displayMelody();
    document.getElementById('play-btn').disabled = false;
    document.getElementById('download-btn').disabled = false;
    setStatus(`Loaded: ${name} (${notes.length} notes)`, 'ready');
}

/* Melody list - discovered from assets/melodies/ */
const MELODY_PATH = 'assets/melodies/';
const MELODIES = [
    { id: 'happy_birthday', name: 'Happy Birthday' },
    { id: 'twinkle', name: 'Twinkle' },
    { id: 'ode_to_joy', name: 'Ode to Joy' },
    { id: 'mary_lamb', name: "Mary's Lamb" },
    { id: 'jingle_bells', name: 'Jingle Bells' },
    { id: 'frere_jacques', name: 'Frere Jacques' },
    { id: 'london_bridge', name: 'London Bridge' }
];

/* Build melody buttons dynamically */
function buildMelodyButtons() {
    const container = document.querySelector('.examples');
    if (!container) return;
    container.innerHTML = '';
    for (const m of MELODIES) {
        const btn = document.createElement('button');
        btn.dataset.melody = m.id;
        btn.textContent = m.name;
        container.appendChild(btn);
    }
}

/* Example melodies */
document.querySelector('.examples').addEventListener('click', async (e) => {
    const btn = e.target.closest('button');
    if (!btn || !btn.dataset.melody) return;

    const name = btn.dataset.melody;
    setStatus(`Loading ${name}...`);
    try {
        const res = await fetch(`${MELODY_PATH}${name}.txt`);
        if (!res.ok) throw new Error(`HTTP ${res.status}`);
        loadMelody(await res.text(), name);
    } catch (err) {
        setStatus(`Failed: ${err.message}`, 'error');
    }
});

/* File loader */
document.getElementById('load-btn').addEventListener('click', () => {
    document.getElementById('file-input').click();
});

document.getElementById('file-input').addEventListener('change', (e) => {
    const file = e.target.files[0];
    if (!file) return;
    const reader = new FileReader();
    reader.onload = (evt) => loadMelody(evt.target.result, file.name);
    reader.onerror = () => setStatus('Error reading file', 'error');
    reader.readAsText(file);
});

/* Playback */
function renderMelody() {
    if (melodyNotes.length === 0) return null;

    const notesPtr = Module._malloc(melodyNotes.length);
    const beatsPtr = Module._malloc(melodyBeats.length);
    Module.HEAPU8.set(new Uint8Array(melodyNotes), notesPtr);
    Module.HEAPU8.set(new Uint8Array(melodyBeats), beatsPtr);

    const outPtrPtr = Module._malloc(4);
    const numSamples = Module._picosynth_wasm_render_melody(notesPtr, beatsPtr, melodyNotes.length, outPtrPtr);

    let samples = null;
    if (numSamples > 0) {
        const bufferPtr = Module.HEAP32[outPtrPtr >> 2];
        samples = new Int16Array(Module.HEAP16.buffer, bufferPtr, numSamples).slice();
        Module._free(bufferPtr);
    }

    Module._free(notesPtr);
    Module._free(beatsPtr);
    Module._free(outPtrPtr);

    return samples;
}

document.getElementById('play-btn').addEventListener('click', () => {
    if (isPlaying || !wasmReady) return;

    initAudio();
    if (!audioCtx) return;
    if (audioCtx.state === 'suspended') audioCtx.resume();

    const samples = renderMelody();
    if (!samples) { setStatus('No melody', 'error'); return; }

    /* Convert to float */
    const float32 = new Float32Array(samples.length);
    for (let i = 0; i < samples.length; i++) {
        float32[i] = samples[i] / 32768.0;
    }

    const buffer = audioCtx.createBuffer(1, samples.length, audioCtx.sampleRate);
    buffer.getChannelData(0).set(float32);

    currentSource = audioCtx.createBufferSource();
    currentSource.buffer = buffer;
    currentSource.connect(audioCtx.destination);

    isPlaying = true;
    document.getElementById('play-btn').disabled = true;
    document.getElementById('stop-btn').disabled = false;
    setStatus('Playing...', 'playing');

    currentSource.onended = () => {
        isPlaying = false;
        currentSource = null;
        document.getElementById('play-btn').disabled = false;
        document.getElementById('stop-btn').disabled = true;
        setStatus('Ready', 'ready');
    };
    currentSource.start();
});

document.getElementById('stop-btn').addEventListener('click', () => {
    if (currentSource) currentSource.stop();
});

/* Download WAV */
document.getElementById('download-btn').addEventListener('click', () => {
    const samples = renderMelody();
    const sampleRate = Module._picosynth_wasm_get_sample_rate();
    const buffer = new ArrayBuffer(44 + samples.length * 2);
    const view = new DataView(buffer);

    const writeStr = (off, str) => { for (let i = 0; i < str.length; i++) view.setUint8(off + i, str.charCodeAt(i)); };
    writeStr(0, 'RIFF');
    view.setUint32(4, 36 + samples.length * 2, true);
    writeStr(8, 'WAVE');
    writeStr(12, 'fmt ');
    view.setUint32(16, 16, true);
    view.setUint16(20, 1, true);
    view.setUint16(22, 1, true);
    view.setUint32(24, sampleRate, true);
    view.setUint32(28, sampleRate * 2, true);
    view.setUint16(32, 2, true);
    view.setUint16(34, 16, true);
    writeStr(36, 'data');
    view.setUint32(40, samples.length * 2, true);
    for (let i = 0; i < samples.length; i++) {
        view.setInt16(44 + i * 2, samples[i], true);
    }

    const blob = new Blob([buffer], { type: 'audio/wav' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = 'picosynth_output.wav';
    a.click();
    URL.revokeObjectURL(url);
    setStatus('Downloaded WAV', 'ready');
});
