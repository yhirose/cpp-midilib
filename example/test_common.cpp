// What midilib does the same on every platform, with no device: messages as
// they read, the controls' 14-bit values, the queue and waiting on it, and
// the ports (whatever there are) and a port that is not there.
#include <midilib.h>

#include <cstdio>

static int failures = 0;

#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
      failures++;                                                         \
    }                                                                     \
  } while (0)

static midilib::Message msg(uint8_t status, uint8_t d1, uint8_t d2 = 0) {
  midilib::Message m;
  midilib::detail::channel_message(status, d1, d2, 0, m);
  return m;
}

int main() {
  using midilib::Kind;

  // Messages: what each is, a note on at velocity 0 is a note off with its
  // bytes kept, two-byte messages, the bend's range, and system messages
  // left out.
  CHECK(msg(0x93, 60, 100).kind() == Kind::note_on && msg(0x93, 60, 100).channel() == 3);
  CHECK(msg(0x90, 60, 0).kind() == Kind::note_off && msg(0x90, 60, 0).bytes[0] == 0x90);
  CHECK(msg(0xB0, 2, 99).kind() == Kind::cc && msg(0xB0, 2, 99).value() == 99);
  CHECK(msg(0xD0, 77).size == 2 && msg(0xD0, 77).value() == 77 && msg(0xC0, 5).size == 2);
  CHECK(msg(0xE0, 0, 0x40).bend() == 0 && msg(0xE0, 0x7F, 0x7F).bend() == 8191 && msg(0xE0, 0, 0).bend() == -8192);
  midilib::Message none;
  CHECK(!midilib::detail::channel_message(0xF8, 0, 0, 0, none) && !midilib::detail::channel_message(0x40, 0, 0, 0, none));

  // Controls: 14 bits from CC 2 and CC 34, a new upper half clearing the
  // lower; a 7-bit controller; a velocity with its CC 88 prefix, and the
  // next without one.
  midilib::Controls c;
  for (auto m : {msg(0xB0, 2, 0x40), msg(0xB0, 34, 0x10), msg(0xB0, 7, 100)}) { c.apply(m); }
  CHECK(c.value14(0, 2) == ((0x40 << 7) | 0x10) && c.value14(0, 7) == (100 << 7));
  c.apply(msg(0xB0, 2, 0x41));
  CHECK(c.value14(0, 2) == (0x41 << 7) && c.value14(1, 2) == 0);
  c.apply(msg(0xB0, 88, 0x22));
  c.apply(msg(0x90, 60, 80));
  CHECK(c.velocity14(0) == ((80 << 7) | 0x22));
  c.apply(msg(0x90, 61, 80));
  CHECK(c.velocity14(0) == (80 << 7));

  // The queue: a full one keeps what it has and counts what it drops; a wait
  // ends when a message comes from another thread, and times out when none
  // does.
  midilib::detail::Queue q(8);
  for (int i = 0; i < 20; i++) { q.push(msg(0x90, static_cast<uint8_t>(i), 1)); }
  CHECK(q.dropped() == 12 && q.front() && q.front()->note() == 0);
  while (q.front()) { q.pop(); }
  auto t0 = midilib::now_ns();
  CHECK(!q.wait(std::chrono::milliseconds(30)));
  CHECK(midilib::now_ns() - t0 >= 25000000);
  std::thread later([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    q.push(msg(0x90, 99, 1));
  });
  t0 = midilib::now_ns();
  CHECK(q.wait(std::chrono::seconds(5)) && q.front() && q.front()->note() == 99);
  CHECK(midilib::now_ns() - t0 < 2000000000);
  later.join();

  // The ports there are, each with an id and a name; a port that is not
  // there does not open.
  auto ports = midilib::input_ports();
  for (auto &p : ports) { CHECK(!p.id.empty()); }
  bool refused = false;
  try {
    midilib::Input in("no such input");
  } catch (const midilib::Error &) { refused = true; }
  CHECK(refused);

  if (failures) { return 1; }
  std::printf("test_common OK (%zu ports)\n", ports.size());
  return 0;
}
