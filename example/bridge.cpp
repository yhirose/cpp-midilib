// Pass what a MIDI input plays to another program, one line a message, for
// a player that reads standard input:
//
//   on <note> <velocity>   velocity 0 to 16383 (with CC 88 when sent)
//   off <note>
//   breath <value>         CC 2 with CC 34: 0 to 16383, once a read when it moved
//   bend <value>           -8192 to 8191
//   range <semitones>      the bend's range, as RPN 0 sets it
//
// Usage: bridge [name-part] [seconds]   (the first input whose name has it;
// until it is stopped, or for `seconds`)
#include <midilib.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

int main(int argc, char **argv) {
  std::string want = argc > 1 ? argv[1] : "";
  int64_t seconds = argc > 2 ? std::atoll(argv[2]) : 0;
  const midilib::Port *port = nullptr;
  auto ports = midilib::input_ports();
  for (auto &p : ports) {
    if (!port && p.name.find(want) != std::string::npos) { port = &p; }
  }
  if (!port) {
    std::fprintf(stderr, "bridge: no input named like '%s'\n", want.c_str());
    return 1;
  }
  std::fprintf(stderr, "bridge: reading %s\n", port->name.c_str());

  midilib::Input in(port->id);
  midilib::Controls controls;
  std::vector<midilib::Message> block;
  block.reserve(4096);
  int breath = -1;
  int rpn_msb = 127, rpn_lsb = 127;
  auto end = midilib::now_ns() + seconds * 1000000000;
  while (seconds == 0 || midilib::now_ns() < end) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    block.clear();
    in.drain(block, midilib::now_ns());
    bool wrote = false;
    for (auto &m : block) {
      controls.apply(m);
      switch (m.kind()) {
      case midilib::Kind::note_on:
        std::printf("on %d %d\n", m.note(), controls.velocity14(m.channel()));
        wrote = true;
        break;
      case midilib::Kind::note_off:
        std::printf("off %d\n", m.note());
        wrote = true;
        break;
      case midilib::Kind::bend:
        std::printf("bend %d\n", m.bend());
        wrote = true;
        break;
      case midilib::Kind::cc:
        if (m.cc() == 101) { rpn_msb = m.value(); }
        if (m.cc() == 100) { rpn_lsb = m.value(); }
        if (m.cc() == 6 && rpn_msb == 0 && rpn_lsb == 0) {
          std::printf("range %d\n", m.value());
          wrote = true;
        }
        break;
      default: break;
      }
    }
    // The breath once both halves of its changes are in.
    auto b = controls.value14(0, 2);
    if (b != breath) {
      std::printf("breath %d\n", b);
      breath = b;
      wrote = true;
    }
    if (wrote) { std::fflush(stdout); }
  }
}
