# cpp-midilib

MIDI for C++17 in one header, for a program that makes sound as it is
played: the inputs, one opened by its id, and its messages in the order they
came, each stamped on a clock that only goes forward; and the outputs, to
send channel messages and sysexes to (a device's settings, say).

| Platform | Through | Link |
| --- | --- | --- |
| macOS | CoreMIDI | `-framework CoreMIDI -framework CoreFoundation` |
| Windows | WinMM | `winmm` |
| Linux | the ALSA sequencer, loaded from `libasound.so.2` when the first port is asked for (a machine without it, or without `/dev/snd/seq`, has no ports); build with `<alsa/asoundlib.h>` | `-ldl -lpthread` |

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

```cpp
// A device's settings: ask for them with a sysex, and read the answer.
midilib::Output out(midilib::output_ports()[0].id);
midilib::Input in(midilib::input_ports()[0].id, 4096, /*sysex=*/true);
out.send({0xF0, 0x47, 0x7F, 0x6D, 0x40, 0x00, 0x00, 0xF7});
midilib::Message m;
while (in.wait(std::chrono::seconds(1)) && in.poll(m)) {
  if (m.kind() == midilib::Kind::sysex) { /* m.sysex: F0 ... F7 */ }
}
```

- `input_ports()` lists the inputs and `output_ports()` the outputs. An id
  stays the same while a device is plugged in (a device's input and output
  have ids of their own): CoreMIDI's unique id, the device's name on Windows (with
  ` (2)` and on for a second of the same name), `client:port` on Linux.
- `Input` takes the messages on the system's MIDI thread (on Linux, a
  thread of its own) and keeps them in a queue that needs no lock;
  `poll()` takes one, `drain()` takes all that came by a time, and
  `wait(timeout)` waits for one. Read them from one thread. When the queue
  (4096 messages unless you say otherwise) is full, what comes is dropped
  and counted by `dropped()`.
- Times are in ns on `now_ns()`'s clock: on macOS the host clock, which
  CoreAudio stamps its buffers with; QueryPerformanceCounter on Windows;
  CLOCK_MONOTONIC on Linux. On macOS a message has the time CoreMIDI gives
  it; on Windows and Linux, the time it came to this process.
- A `Message` is one channel message (note, pressure, controller, program,
  bend) as it came. A note on at velocity 0 is a note off to `kind()`.
  Messages that came together (a wind controller's legato: the new note on,
  then the old note off) keep their order. System messages are left out,
  and so are sysexes unless the input asks for them (`Input(id, capacity,
  true)`): then each comes whole as one message, `kind()` sysex and its
  bytes, F0 to F7, in `sysex`, with the time its first piece came. A sysex
  cut short, or longer than 64 KiB, is dropped and counted by `dropped()`.
- `Output` sends with `send(bytes)`: whole channel messages and sysexes, one
  after another, with no running status. Bytes that are anything else are
  refused with a `midilib::Error`, and none of them is sent. On Windows a
  sysex is sent whole before `send()` returns.
- `Controls` reads 14-bit values: controllers 0 to 31 with 32 to 63, and a
  note on's velocity with the High Resolution Velocity Prefix (CC 88).

## Example and test

```sh
cd example
make            # builds them and runs the tests
./example EWI   # prints what the first input named like "EWI" plays
./bridge EWI    # passes it on to another program, one line a message
```

`test_common` checks what is the same everywhere with no device. On macOS,
`test` also sends messages through a virtual CoreMIDI source of its own and
reads them back, and sends to a virtual destination of its own; it needs the
CoreMIDI server (not a sandbox).

## License

MIT
