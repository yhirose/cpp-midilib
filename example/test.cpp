// midilib against a virtual MIDI source this program makes: what is sent
// comes back as messages, in order, at the times sent, and the controls read
// 14-bit values from them. Needs the CoreMIDI server (not a sandbox).
#include <midilib.h>

#include <chrono>
#include <cstdio>
#include <functional>
#include <optional>
#include <thread>

static int failures = 0;

#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
      failures++;                                                         \
    }                                                                     \
  } while (0)

static MIDIEndpointRef source;

static uint32_t ump(uint8_t status, uint8_t d1, uint8_t d2 = 0) {
  return (0x2u << 28) | (uint32_t(status) << 16) | (uint32_t(d1) << 8) | d2;
}

// Send `words` as packets at `times` (host ticks; 0 is now), one packet per
// distinct time, words of one time together.
static void send(const std::vector<std::pair<MIDITimeStamp, std::vector<uint32_t>>> &packets) {
  alignas(8) Byte buf[4096];
  auto list = reinterpret_cast<MIDIEventList *>(buf);
  auto p = MIDIEventListInit(list, kMIDIProtocol_1_0);
  for (auto &[time, words] : packets) {
    p = MIDIEventListAdd(list, sizeof buf, p, time, words.size(), words.data());
  }
  MIDIReceivedEventList(source, list);
}

// Wait up to 2 s for `done`.
static bool wait_for(const std::function<bool()> &done) {
  for (int i = 0; i < 400; i++) {
    if (done()) { return true; }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return done();
}

// The next `n` messages, waiting for them.
static std::vector<midilib::Message> take(midilib::Input &in, size_t n) {
  std::vector<midilib::Message> got;
  wait_for([&] {
    midilib::Message m;
    while (got.size() < n && in.poll(m)) { got.push_back(m); }
    return got.size() == n;
  });
  return got;
}

int main() {
  MIDIClientRef client;
  if (MIDIClientCreateWithBlock(CFSTR("midilib test"), &client, ^(const MIDINotification *) {}) != noErr ||
      MIDISourceCreateWithProtocol(client, CFSTR("midilib test source"), kMIDIProtocol_1_0, &source) != noErr) {
    std::printf("FAIL: no CoreMIDI (run outside a sandbox)\n");
    return 1;
  }

  // The source is listed by its name, with an id it opens by.
  std::string id;
  wait_for([&] {
    for (auto &p : midilib::input_ports()) {
      if (p.name == "midilib test source") { id = p.id; }
    }
    return !id.empty();
  });
  CHECK(!id.empty());

  // An id no port has, or one that is not a number, does not open.
  auto refuses = [](const std::string &bad) {
    try {
      midilib::Input in(bad);
      return false;
    } catch (const midilib::Error &) { return true; }
  };
  CHECK(refuses("12345"));
  CHECK(refuses("abc"));
  CHECK(refuses(""));
  CHECK(refuses(id + "x"));

  // now_ns() is the host clock in ns, as the system reads it in ns itself.
  auto ours = midilib::now_ns();
  auto system = int64_t(clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
  CHECK(ours - system < 1000000 && system - ours < 1000000);

  std::optional<midilib::Input> box;
  box.emplace(id);
  auto &in = *box;
  // The connection is made in the background: send until one arrives.
  wait_for([&] {
    send({{0, {ump(0xB0, 127, 0)}}});
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    midilib::Message m;
    bool any = false;
    while (in.poll(m)) { any = true; }
    return any;
  });

  // A note on and off, with the channel, note and velocity.
  send({{0, {ump(0x93, 60, 100)}}});
  auto got = take(in, 1);
  CHECK(got.size() == 1 && got[0].kind() == midilib::Kind::note_on && got[0].channel() == 3 &&
        got[0].note() == 60 && got[0].velocity() == 100 && got[0].size == 3);

  // A note on at velocity 0 is a note off, with its bytes as they came.
  send({{0, {ump(0x90, 60, 0)}}});
  got = take(in, 1);
  CHECK(got.size() == 1 && got[0].kind() == midilib::Kind::note_off && got[0].bytes[0] == 0x90 &&
        got[0].velocity() == 0);

  // A wind controller's legato: the new note on, then the old note off, in
  // one packet, keep their order and time.
  send({{0, {ump(0x90, 62, 90), ump(0x80, 60, 0)}}});
  got = take(in, 2);
  CHECK(got.size() == 2 && got[0].kind() == midilib::Kind::note_on && got[0].note() == 62 &&
        got[1].kind() == midilib::Kind::note_off && got[1].note() == 60 &&
        got[0].time_ns == got[1].time_ns);

  // Bend at rest, at its top and at its bottom; a channel pressure is two
  // bytes, its value the second.
  send({{0, {ump(0xE0, 0, 0x40), ump(0xE0, 0x7F, 0x7F), ump(0xE0, 0, 0), ump(0xD0, 77)}}});
  got = take(in, 4);
  CHECK(got.size() == 4 && got[0].bend() == 0 && got[1].bend() == 8191 && got[2].bend() == -8192 &&
        got[3].kind() == midilib::Kind::channel_pressure && got[3].size == 2 && got[3].value() == 77);

  // System messages and sysex are left out: a clock (UMP type 1) and a short
  // sysex (type 3, two words) between two notes.
  send({{0, {ump(0x90, 64, 1), 0x10F80000u, 0x30017E00u, 0x20000000u, ump(0x90, 65, 1)}}});
  got = take(in, 2);
  CHECK(got.size() == 2 && got[0].note() == 64 && got[1].note() == 65);
  midilib::Message extra;
  CHECK(!in.poll(extra));

  // Times: a packet's host time, in ns on the clock now_ns() reads; a time
  // of 0 is when it came.
  auto t0 = mach_absolute_time();
  auto before = midilib::now_ns();
  send({{t0 - 3000, {ump(0x90, 66, 1)}}, {0, {ump(0x90, 67, 1)}}});
  got = take(in, 2);
  auto after = midilib::now_ns();
  CHECK(got.size() == 2 && got[0].time_ns == midilib::detail::host_to_ns(t0 - 3000));
  CHECK(got.size() == 2 && got[1].time_ns >= before && got[1].time_ns <= after);

  // drain() takes what came by a time, in order, and leaves the rest.
  auto t1 = mach_absolute_time() - 30000;
  send({{t1, {ump(0x90, 70, 1)}}, {t1 + 10000, {ump(0x90, 71, 1)}}, {t1 + 20000, {ump(0x90, 72, 1)}}});
  std::vector<midilib::Message> block;
  wait_for([&] {
    in.drain(block, midilib::detail::host_to_ns(t1 + 10000));
    return block.size() >= 2;
  });
  CHECK(block.size() == 2 && block[0].note() == 70 && block[1].note() == 71);
  got = take(in, 1);
  CHECK(got.size() == 1 && got[0].note() == 72);

  // The controls: breath at 14 bits (CC 2, then CC 34), a new upper half
  // clearing the lower; a 7-bit controller; a note on's velocity with its
  // CC 88 prefix, and the next without one.
  midilib::Controls controls;
  send({{0, {ump(0xB0, 2, 0x40), ump(0xB0, 34, 0x10), ump(0xB0, 7, 100)}}});
  for (auto &m : take(in, 3)) { controls.apply(m); }
  CHECK(controls.value14(0, 2) == ((0x40 << 7) | 0x10));
  CHECK(controls.value14(0, 7) == (100 << 7));
  send({{0, {ump(0xB0, 2, 0x41)}}});
  for (auto &m : take(in, 1)) { controls.apply(m); }
  CHECK(controls.value14(0, 2) == (0x41 << 7));
  CHECK(controls.value14(1, 2) == 0);
  send({{0, {ump(0xB0, 88, 0x22), ump(0x90, 60, 80)}}});
  for (auto &m : take(in, 2)) { controls.apply(m); }
  CHECK(controls.velocity14(0) == ((80 << 7) | 0x22));
  send({{0, {ump(0x90, 61, 80)}}});
  for (auto &m : take(in, 1)) { controls.apply(m); }
  CHECK(controls.velocity14(0) == (80 << 7));

  // A full queue keeps what it has, drops what comes, and counts it.
  midilib::Input small(id, 8);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  std::vector<uint32_t> twenty;
  for (int i = 0; i < 20; i++) { twenty.push_back(ump(0x90, uint8_t(i), 1)); }
  send({{0, twenty}});
  CHECK(wait_for([&] { return small.dropped() == 12; }));
  got = take(small, 8);
  CHECK(got.size() == 8 && got[0].note() == 0 && got[7].note() == 7);

  // Moved, an input still reads, after the one it was moved from is gone.
  midilib::Input moved(std::move(in));
  box.reset();
  midilib::Message m;
  while (moved.poll(m)) {}
  send({{0, {ump(0x90, 99, 1)}}});
  got = take(moved, 1);
  CHECK(got.size() == 1 && got[0].note() == 99);

  MIDIEndpointDispose(source);
  if (failures) { return 1; }
  std::printf("test OK\n");
  return 0;
}
