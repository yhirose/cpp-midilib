# cpp-midilib

MIDI input for C++17 in one header, for a program that makes sound as it is
played: the inputs, one opened by its id, and its messages in the order they
came, each stamped on the clock the audio device keeps.

macOS (CoreMIDI) only for now.

```cpp
#include <midilib.h>

auto ports = midilib::input_ports();          // [{id, name}]
midilib::Input in(ports[0].id);
midilib::Controls controls;

// In the audio callback: everything that came by the end of this block.
std::vector<midilib::Message> block;          // reserve() it once
in.drain(block, block_end_ns);
for (auto &m : block) {
  controls.apply(m);
  auto at = m.time_ns;                        // where in the block it lands
  switch (m.kind()) {
  case midilib::Kind::note_on: /* m.note(), controls.velocity14(m.channel()) */ break;
  case midilib::Kind::note_off: /* m.note() */ break;
  case midilib::Kind::bend: /* m.bend(): -8192 to 8191 */ break;
  default: break;
  }
}
double breath = controls.value(0, 2);         // CC 2 with CC 34: 0.0 to 1.0
```

- `input_ports()` lists the inputs. An id stays the same while a device is
  plugged in.
- `Input` takes the messages on CoreMIDI's thread and keeps them in a queue
  that needs no lock; `poll()` takes one and `drain()` takes all that came by a
  time. Read them from one thread. When the queue (4096 messages unless you
  say otherwise) is full, what comes is dropped and counted by `dropped()`.
- Times are in ns on the host clock, `now_ns()`, the one CoreAudio stamps its
  buffers with.
- A `Message` is one channel message (note, pressure, controller, program,
  bend) as it came. A note on at velocity 0 is a note off to `kind()`.
  Messages that came together (a wind controller's legato: the new note on,
  then the old note off) keep their order. System messages and sysex are
  left out.
- `Controls` reads 14-bit values: controllers 0 to 31 with 32 to 63, and a
  note on's velocity with the High Resolution Velocity Prefix (CC 88).

## Example and test

```sh
cd example
make            # builds both and runs the test (it needs the CoreMIDI server)
./example EWI   # prints what the first input named like "EWI" plays
./bridge EWI    # passes it on to another program, one line a message
```

## License

MIT
