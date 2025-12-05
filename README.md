# PicoSynth

A lightweight software synthesizer designed for resource-constrained environments.

## Features

- Q15 fixed-point arithmetic (no floating point required)
- Modular node-based architecture
- Multiple voice polyphony
- ADSR envelope generators
- Waveform generators: sine, saw, square, triangle, noise
- Low-pass and high-pass filters
- Soft clipper for output limiting

## Architecture

Each voice contains up to 8 nodes that can be:
- Oscillators (with waveform function pointer)
- ADSR envelopes
- LP/HP filters
- Mixers (up to 3 inputs)

Nodes are wired together via pointers, allowing flexible signal routing.

## Usage

### Building and Running

```shell
make           # Build the example program
make run       # Build and run (produces output.wav)
make check     # Run unit tests
make clean     # Remove generated files
```

### Example Program

The example program (`tests/example.c`) plays "Happy Birthday" with a piano-like timbre and writes `output.wav` (11025Hz, 16-bit mono):

```shell
make run
# Produces: output.wav (piano melody)
```

You can select different melodies:
```shell
make MELODY=twinkle run    # Play "Twinkle Twinkle"
make list-melodies         # Show available melodies
```

### Unit Tests

Run the test suite to verify the synthesizer is working correctly:
```shell
make check
# Output: === Test Summary ===
#         Passed: 626
#         Failed: 0
```

Tests cover Q15 arithmetic, waveform generators, envelope processing, and synthesizer core functionality.

### Integrating the Synth

The PicoSynth provides a C API for integration into your projects:

```c
#include "picosynth.h"

/* Create synthesizer with 2 voices, 8 nodes per voice */
picosynth_t *s = picosynth_create(2, 8);

/* Get voice and nodes */
picosynth_voice_t *v = picosynth_get_voice(s, 0);
picosynth_node_t *env = picosynth_voice_get_node(v, 0);
picosynth_node_t *osc = picosynth_voice_get_node(v, 1);

/* Initialize nodes */
picosynth_init_env(env, NULL,
    &(picosynth_env_params_t){.attack=5000, .decay=500, .sustain=Q15_MAX/2,
                              .release=500,
                             });
picosynth_init_osc(osc, &env->out, picosynth_voice_freq_ptr(v),
                   picosynth_wave_sine);
picosynth_voice_set_out(v, 1);

/* Play notes */
picosynth_note_on(s, 0, 60);   /* Middle C */
q15_t sample = picosynth_process(s);
picosynth_note_off(s, 0);

/* Cleanup */
picosynth_destroy(s);
```

#### Key Functions

- `picosynth_create(voices, nodes)`: Create synthesizer
- `picosynth_destroy(s)`: Free resources
- `picosynth_note_on(s, voice, midi_note)`: Trigger note
- `picosynth_note_off(s, voice)`: Release note
- `picosynth_process(s)`: Generate one sample
- `picosynth_init_osc(node, gain, freq, wave)`: Initialize oscillator
- `picosynth_init_env(node, gain, atk, dec, sus, rel)`: Initialize envelope
- `picosynth_init_lp(node, gain, input, coeff)`: Initialize low-pass filter
- `picosynth_init_hp(node, gain, input, coeff)`: Initialize high-pass filter
- `picosynth_init_mix(node, gain, in1, in2, in3)`: Initialize mixer

#### Waveform Generators

- `picosynth_wave_sine` - Sine wave
- `picosynth_wave_saw` - Rising sawtooth
- `picosynth_wave_square` - Square wave
- `picosynth_wave_triangle` - Triangle wave
- `picosynth_wave_falling` - Falling ramp
- `picosynth_wave_exp` - Exponential decay
- `picosynth_wave_noise` - White noise

For a complete example, see `tests/example.c` which demonstrates a piano-like timbre.

## License
`picosynth` is available under a permissive MIT-style license.
Use of this source code is governed by a MIT license that can be found in the [LICENSE](LICENSE) file.
