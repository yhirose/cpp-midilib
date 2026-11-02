// Print what a MIDI input plays: notes with their 14-bit velocity, and the
// breath (CC 2, at 14 bits when the device sends them so) as it changes,
// read every 5 ms as an audio callback would.
// Usage: example [name-part] [seconds]   (the first input whose name has it)
#include <midilib.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

int main(int argc, char **argv) {
  std::string want = argc > 1 ? argv[1] : "";
  int seconds = argc > 2 ? std::atoi(argv[2]) : 20;

  auto ports = midilib::input_ports();
  for (auto &p : ports) { std::printf("input %s: %s\n", p.id.c_str(), p.name.c_str()); }
  const midilib::Port *port = nullptr;
  for (auto &p : ports) {
    if (!port && p.name.find(want) != std::string::npos) { port = &p; }
  }
  if (!port) {
    std::printf("no input named like '%s'\n", want.c_str());
    return 1;
  }
  std::printf("reading %s for %d s\n", port->name.c_str(), seconds);

  midilib::Input in(port->id);
  midilib::Controls controls;
  std::vector<midilib::Message> block;
  block.reserve(1024);
  int last_breath = -1;
  auto start = midilib::now_ns();
  auto end = start + int64_t(seconds) * 1000000000;
  while (midilib::now_ns() < end) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    block.clear();
    in.drain(block, midilib::now_ns());
    for (auto &m : block) {
      controls.apply(m);
      auto at = (m.time_ns - start) / 1e6;
      switch (m.kind()) {
      case midilib::Kind::note_on:
        std::printf("%9.1f ms  note on  %3d  velocity %5d / 16383\n", at, m.note(), controls.velocity14(m.channel()));
        break;
      case midilib::Kind::note_off: std::printf("%9.1f ms  note off %3d\n", at, m.note()); break;
      case midilib::Kind::bend: std::printf("%9.1f ms  bend %d\n", at, m.bend()); break;
      default: break;
      }
    }
    auto breath = controls.value14(0, 2);
    if (breath != last_breath && breath / 512 != last_breath / 512) {
      std::printf("            breath %5d / 16383  %s\n", breath, std::string(breath / 512, '#').c_str());
      last_breath = breath;
    }
  }
  std::printf("dropped %llu\n", (unsigned long long)in.dropped());
}
