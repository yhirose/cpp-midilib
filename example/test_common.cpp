// What midilib does the same on every platform, with no device: messages as
// they read, the controls' 14-bit values, the queue and waiting on it, and
// the ports (whatever there are) and a port that is not there.
#include <midilib.h>

#include <cstdio>
#include <functional>

static int failures = 0;

#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
      failures++;                                                         \
    }                                                                     \
  } while (0)

// What `f` throws, or "" when it throws nothing.
static std::string error_of(const std::function<void()> &f) {
  try {
    f();
    return "";
  } catch (const midilib::Error &e) { return e.what(); }
}

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

  // What send() takes: whole channel messages and sysexes, as messages;
  // anything else is refused, naming where it went wrong.
  auto parse = [](std::vector<uint8_t> b) { return midilib::detail::messages_in(b.data(), b.size()); };
  auto why = [&](std::vector<uint8_t> b) { return error_of([&] { parse(b); }); };
  auto parsed = parse({0x90, 60, 100, 0xC0, 5, 0xF0, 0x47, 0x7F, 0xF7, 0xF0, 0xF7, 0xE0, 0, 0x40});
  CHECK(parsed.size() == 5);
  CHECK(parsed.size() == 5 && parsed[0].kind() == Kind::note_on && parsed[0].note() == 60 && parsed[0].velocity() == 100);
  CHECK(parsed.size() == 5 && parsed[1].kind() == Kind::program && parsed[1].size == 2 && parsed[1].value() == 5 &&
        parsed[1].bytes[2] == 0);
  CHECK(parsed.size() == 5 && parsed[2].kind() == Kind::sysex &&
        parsed[2].sysex == (std::vector<uint8_t>{0xF0, 0x47, 0x7F, 0xF7}));
  CHECK(parsed.size() == 5 && parsed[3].sysex == (std::vector<uint8_t>{0xF0, 0xF7}));
  CHECK(parsed.size() == 5 && parsed[4].kind() == Kind::bend && parsed[4].bend() == 0);
  CHECK(parse({}).empty());
  CHECK(why({0x90, 60, 100, 61, 100}) == "midilib: 3D at byte 3 begins no channel message or sysex");
  CHECK(why({0xF8}) == "midilib: F8 at byte 0 begins no channel message or sysex");
  CHECK(why({0xB0, 7}) == "midilib: the message at byte 0 is cut short");
  CHECK(why({0xB0, 0x90, 1}) == "midilib: the message at byte 0 has a status byte for data");
  CHECK(why({0xF0, 1, 2}) == "midilib: the sysex at byte 0 does not end with F7 after its data bytes");
  CHECK(why({0xF0, 1, 0x90, 0xF7}) == "midilib: the sysex at byte 0 does not end with F7 after its data bytes");

  // Sysexes put together from their pieces: whole in one, in three pieces
  // with a real-time byte between two of them, and between channel
  // messages; one cut short by a status byte, or by another F0, or too
  // long, is dropped and counted; an inbox that did not ask has none.
  auto sysexes = [](midilib::detail::Inbox &in) {
    std::vector<std::vector<uint8_t>> out;
    while (auto f = in.queue.front()) {
      if (f->kind() == Kind::sysex) { out.push_back(f->sysex); }
      in.queue.pop();
    }
    return out;
  };
  midilib::detail::Inbox inbox(64, true);
  std::vector<uint8_t> whole = {0xF0, 0x47, 0x7F, 0x6D, 0x40, 0x00, 0x00, 0xF7};
  inbox.sysex(whole.data(), whole.size(), 5);
  CHECK(inbox.queue.front() && inbox.queue.front()->kind() == Kind::sysex && inbox.queue.front()->time_ns == 5 &&
        inbox.queue.front()->size == 0);
  CHECK(sysexes(inbox) == std::vector<std::vector<uint8_t>>{whole});
  inbox.sysex(whole.data(), 3, 7);
  uint8_t clock = 0xF8;
  inbox.sysex(&clock, 1, 8);
  inbox.sysex(whole.data() + 3, 2, 8);
  inbox.channel(0x90, 60, 1, 8);
  inbox.sysex(whole.data() + 5, 3, 9);
  CHECK(inbox.queue.front() && inbox.queue.front()->kind() == Kind::note_on);
  inbox.queue.pop();
  CHECK(inbox.queue.front() && inbox.queue.front()->time_ns == 7);
  CHECK(sysexes(inbox) == std::vector<std::vector<uint8_t>>{whole});
  std::vector<uint8_t> cut = {0xF0, 1, 2, 0x90, 3, 0xF7, 0xF0, 4, 0xF0, 5, 0xF7};
  inbox.sysex(cut.data(), cut.size(), 0);
  CHECK(inbox.queue.dropped() == 2 && sysexes(inbox) == (std::vector<std::vector<uint8_t>>{{0xF0, 5, 0xF7}}));
  std::vector<uint8_t> big(midilib::detail::Inbox::MAX_SYSEX + 1, 1);
  big.front() = 0xF0;
  big.back() = 0xF7;
  inbox.sysex(big.data(), big.size(), 0);
  CHECK(inbox.queue.dropped() == 3 && sysexes(inbox).empty());
  big.pop_back();
  big.back() = 0xF7;
  inbox.sysex(big.data(), big.size(), 0);
  CHECK(inbox.queue.dropped() == 3 && sysexes(inbox).size() == 1);
  midilib::detail::Inbox deaf(8, false);
  deaf.sysex(whole.data(), whole.size(), 0);
  CHECK(!deaf.queue.front() && deaf.queue.dropped() == 0);

  // The ports there are, each with an id and a name; a port that is not
  // there does not open.
  auto ports = midilib::input_ports();
  for (auto &p : ports) { CHECK(!p.id.empty()); }
  for (auto &p : midilib::output_ports()) { CHECK(!p.id.empty()); }
  CHECK(error_of([] { midilib::Input in("no such input"); }) == "midilib: no MIDI input no such input");
  CHECK(error_of([] { midilib::Output out("no such output"); }) == "midilib: no MIDI output no such output");

  if (failures) { return 1; }
  std::printf("test_common OK (%zu ports)\n", ports.size());
  return 0;
}
