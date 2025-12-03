CC ?= gcc
CFLAGS = -Wall -Wextra -Wconversion -I include -I .
LDLIBS =

SRCS = src/picosynth.c
HDRS = include/picosynth.h

# Example program (piano demo)
EXAMPLE_SRC = tests/example.c
EXAMPLE_TARGET = example

# Unit tests
TEST_DIR = tests
TEST_SRCS = $(TEST_DIR)/driver.c $(TEST_DIR)/test-q15.c \
            $(TEST_DIR)/test-waveform.c $(TEST_DIR)/test-envelope.c \
            $(TEST_DIR)/test-synth.c $(TEST_DIR)/test-midi.c
TEST_TARGET = test_runner

# Melody selection: set MELODY to change the song
# Available: happy_birthday, twinkle (default: happy_birthday)
MELODY ?= happy_birthday
MELODY_SRC = web/assets/melodies/$(MELODY).txt
MELODY_HDR = melody.h

# Melody converter tools
MIDI2C = tools/midi2c
MIDIPARSE = tools/midiparse
TXT2MIDI = tools/txt2midi

# MIDI file parser source
MIDI_SRC = src/midifile.c
MIDI_HDR = include/midifile.h

TARGET = example

# WebAssembly build
EMCC ?= emcc
WASM_DIR = web
WASM_SRCS = $(WASM_DIR)/wasm.c src/picosynth.c
WASM_OUT = $(WASM_DIR)/picosynth.js
WASM_SAMPLE_RATE ?= 44100
WASM_FLAGS = -O2 -s WASM=1 \
	-s EXPORTED_RUNTIME_METHODS='["ccall","cwrap","HEAPU8","HEAP16","HEAP32"]' \
	-s EXPORTED_FUNCTIONS='["_malloc","_free"]' \
	-s ALLOW_MEMORY_GROWTH=1 -s MODULARIZE=0 \
	-DSAMPLE_RATE=$(WASM_SAMPLE_RATE) \
	-I include -I .

.PHONY: all clean distclean indent list-melodies wasm wasm-clean serve tools check copy-melodies

all: $(TARGET)

# Build the melody converter tool
$(MIDI2C): tools/midi2c.c
	$(CC) -Wall -Wextra -o $@ $<

# Build the MIDI file parser tool
$(MIDIPARSE): tools/midiparse.c $(MIDI_SRC) $(MIDI_HDR)
	$(CC) $(CFLAGS) tools/midiparse.c $(MIDI_SRC) -o $@

# Build the text-to-MIDI converter tool
$(TXT2MIDI): tools/txt2midi.c
	$(CC) -Wall -Wextra -o $@ $<

# Generate melody.h from selected melody file
$(MELODY_HDR): $(MELODY_SRC) $(MIDI2C)
	$(MIDI2C) $(MELODY_SRC) > $@

$(TARGET): $(EXAMPLE_SRC) $(SRCS) $(HDRS) $(MELODY_HDR)
	$(CC) $(CFLAGS) $(EXAMPLE_SRC) $(SRCS) -o $@ $(LDLIBS)

# Build unit test runner
$(TEST_TARGET): $(TEST_SRCS) $(SRCS) $(MIDI_SRC) $(HDRS) $(MIDI_HDR) $(TEST_DIR)/test.h
	$(CC) $(CFLAGS) -I $(TEST_DIR) $(TEST_SRCS) $(SRCS) $(MIDI_SRC) -o $@ $(LDLIBS)

# Run the example program
run: $(TARGET)
	./$(TARGET)

# Run unit tests
check: $(TEST_TARGET)
	@echo "=== Running unit tests ==="
	./$(TEST_TARGET)

clean:
	$(RM) $(TARGET) $(TEST_TARGET) output.wav $(MELODY_HDR)

# Build tools (explicit target, also built automatically as dependency)
tools: $(MIDI2C) $(MIDIPARSE) $(TXT2MIDI)

# WebAssembly build
wasm: $(WASM_OUT) copy-melodies

$(WASM_OUT): $(WASM_SRCS) $(HDRS)
	$(EMCC) $(WASM_FLAGS) $(WASM_SRCS) -o $@
	@echo "WebAssembly build complete: $(WASM_DIR)/"

# Copy melody files to web directory for deployment
copy-melodies:
	@mkdir -p $(WASM_DIR)/assets/melodies
	@echo "Melody files reside in $(WASM_DIR)/assets/melodies/"

wasm-clean:
	$(RM) $(WASM_DIR)/picosynth.js $(WASM_DIR)/picosynth.wasm
	$(RM) -r $(WASM_DIR)/assets

# Remove all generated files
distclean: clean wasm-clean
	$(RM) $(MIDI2C) $(MIDIPARSE) $(TXT2MIDI)

# Local development server
serve: wasm
	@echo "Starting local server at http://127.0.0.1:8080"
	@cd $(WASM_DIR) && python3 -c 'exec("""import http.server\nimport socketserver\nhandler = http.server.SimpleHTTPRequestHandler\nhttpd = socketserver.TCPServer(("127.0.0.1", 8080), handler)\nprint("Serving at http://127.0.0.1:8080")\nhttpd.serve_forever()""")'

# List available melodies
list-melodies:
	@echo "Available melodies:"
	@ls -1 web/assets/melodies/*.txt 2>/dev/null | sed 's|web/assets/melodies/||;s|\.txt||' | while read m; do echo "  $$m"; done
	@echo ""
	@echo "Usage: make MELODY=<name>"
	@echo "Example: make MELODY=twinkle"

# Format all C source and header files
indent:
	clang-format -i $(SRCS) $(HDRS) $(MIDI_SRC) $(MIDI_HDR) $(EXAMPLE_SRC) $(TEST_SRCS) $(TEST_DIR)/test.h $(WASM_DIR)/wasm.c tools/midi2c.c tools/midiparse.c tools/txt2midi.c
