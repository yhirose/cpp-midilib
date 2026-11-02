//
//  midilib.h
//
//  MIDI input for a program that plays sound as it is played: the ports, one
//  opened by its id, and its messages in the order they came, each with the
//  time it came on a clock that only goes forward.
//
//  macOS: CoreMIDI (link CoreMIDI and CoreFoundation). Windows: WinMM (link
//  winmm). Linux: the ALSA sequencer, loaded when the first port is asked
//  for, so a program runs without libasound and finds no ports (build with
//  <alsa/asoundlib.h>; link dl and pthread).
//
//  Copyright (c) 2026 Yuji Hirose. All rights reserved.
//  MIT License
//

#pragma once

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>
#include <mach/mach_time.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#elif defined(__linux__)
#include <dlfcn.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>
#if __has_include(<alsa/asoundlib.h>)
#include <alsa/asoundlib.h>
#define MIDILIB_ALSA 1
#endif
#else
#error "midilib: macOS, Windows or Linux"
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace midilib {

struct Error : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// A port to open: `id` stays the same while the device is plugged in;
// `name` is for people.
struct Port {
  std::string id;
  std::string name;
};

enum class Kind {
  note_off,
  note_on,
  poly_pressure,
  cc,
  program,
  channel_pressure,
  bend,
};

// One channel message as it came. A note on with velocity 0 is a note off
// (MIDI 1.0), so kind() says note_off; its bytes stay as they came.
struct Message {
  int64_t time_ns = 0;
  uint8_t bytes[3] = {0, 0, 0};
  uint8_t size = 0;

  Kind kind() const {
    switch (bytes[0] & 0xF0) {
    case 0x80: return Kind::note_off;
    case 0x90: return bytes[2] == 0 ? Kind::note_off : Kind::note_on;
    case 0xA0: return Kind::poly_pressure;
    case 0xB0: return Kind::cc;
    case 0xC0: return Kind::program;
    case 0xD0: return Kind::channel_pressure;
    default: return Kind::bend;
    }
  }
  int channel() const { return bytes[0] & 0x0F; }  // 0 to 15
  int note() const { return bytes[1]; }
  int velocity() const { return bytes[2]; }
  int cc() const { return bytes[1]; }
  // A controller's value, a pressure or a program: 0 to 127.
  int value() const { return size == 2 ? bytes[1] : bytes[2]; }
  // -8192 to 8191, 0 at rest.
  int bend() const { return ((bytes[2] << 7) | bytes[1]) - 8192; }
};

namespace detail {

// A channel message from its status and data bytes, or false for anything
// else (system messages are left out).
inline bool channel_message(uint8_t status, uint8_t d1, uint8_t d2, int64_t time_ns, Message &m) {
  if (status < 0x80 || status >= 0xF0) { return false; }
  m.time_ns = time_ns;
  m.bytes[0] = status;
  m.bytes[1] = d1 & 0x7F;
  bool two = (status & 0xF0) == 0xC0 || (status & 0xF0) == 0xD0;
  m.bytes[2] = two ? 0 : (d2 & 0x7F);
  m.size = two ? 2 : 3;
  return true;
}

// Messages from the MIDI thread (the one producer) to the thread that reads
// them (the one consumer), without a lock. When it is full, what comes is
// counted and dropped, and what is in it is kept. A reader may wait for one.
// The two ends sit on cache lines of their own, which MSVC warns of (C4324).
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
class Queue {
public:
  explicit Queue(size_t capacity) {
    size_t n = 1;
    while (n < capacity) { n <<= 1; }
    slots_.resize(n);
    mask_ = n - 1;
  }

  void push(const Message &m) {
    auto head = head_.load(std::memory_order_relaxed);
    if (head - tail_.load(std::memory_order_acquire) == slots_.size()) {
      dropped_.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    slots_[head & mask_] = m;
    head_.store(head + 1, std::memory_order_release);
    // A store then a load on each side (here head_ then waiting_, in wait()
    // waiting_ then head_): the fences keep both from reading the other's
    // old value, which would leave a reader asleep on a message.
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (waiting_.load(std::memory_order_relaxed)) {
      std::lock_guard<std::mutex> lock(mutex_);
      ready_.notify_one();
    }
  }

  // The oldest message, left in place; nullptr when there is none.
  const Message *front() const {
    auto tail = tail_.load(std::memory_order_relaxed);
    if (tail == head_.load(std::memory_order_acquire)) { return nullptr; }
    return &slots_[tail & mask_];
  }

  void pop() { tail_.store(tail_.load(std::memory_order_relaxed) + 1, std::memory_order_release); }

  // Waits until a message is there or `timeout` has passed; whether one is.
  bool wait(std::chrono::nanoseconds timeout) {
    if (front()) { return true; }
    std::unique_lock<std::mutex> lock(mutex_);
    waiting_.store(true, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    bool any = ready_.wait_for(lock, timeout, [this] { return front() != nullptr; });
    waiting_.store(false, std::memory_order_release);
    return any;
  }

  uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }

private:
  std::vector<Message> slots_;
  size_t mask_ = 0;
  alignas(64) std::atomic<size_t> head_{0};
  alignas(64) std::atomic<size_t> tail_{0};
  std::atomic<uint64_t> dropped_{0};
  std::atomic<bool> waiting_{false};
  std::mutex mutex_;
  std::condition_variable ready_;
};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#if defined(__APPLE__)

inline const mach_timebase_info_data_t &timebase() {
  static const mach_timebase_info_data_t tb = [] {
    mach_timebase_info_data_t t;
    mach_timebase_info(&t);
    return t;
  }();
  return tb;
}

inline int64_t host_to_ns(uint64_t host) {
  auto &tb = timebase();
  return static_cast<int64_t>(static_cast<unsigned __int128>(host) * tb.numer / tb.denom);
}

// The host clock, which CoreAudio stamps its buffers with too.
inline int64_t now_ns() { return host_to_ns(mach_absolute_time()); }

inline std::string to_string(CFStringRef s) {
  if (!s) { return ""; }
  char buf[512];
  if (!CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8)) { return ""; }
  return buf;
}

inline std::string name_of(MIDIObjectRef obj) {
  CFStringRef s = nullptr;
  if (MIDIObjectGetStringProperty(obj, kMIDIPropertyDisplayName, &s) != noErr) { return ""; }
  auto name = to_string(s);
  CFRelease(s);
  return name;
}

inline void check(OSStatus err, const char *what) {
  if (err != noErr) {
    throw Error(std::string("midilib: ") + what + " failed (OSStatus " + std::to_string(err) + ")");
  }
}

// The one CoreMIDI client of the process, made when it is first needed. A
// process learns of sources that come and go on the run loop of the thread
// its client was made on, which a program's own thread may never run; so the
// client is made on a thread of its own that runs its loop for good (a timer
// far off keeps the loop from finding nothing to wait for). It is never
// disposed: the system ends it with the process.
inline MIDIClientRef client() {
  static const MIDIClientRef c = [] {
    std::promise<MIDIClientRef> made;
    auto got = made.get_future();
    std::thread([made = std::move(made)]() mutable {
      MIDIClientRef r = 0;
      if (MIDIClientCreateWithBlock(CFSTR("midilib"), &r, ^(const MIDINotification *) {}) != noErr) {
        made.set_value(0);
        return;
      }
      made.set_value(r);
      auto keep = CFRunLoopTimerCreateWithHandler(nullptr, CFAbsoluteTimeGetCurrent() + 1e9, 1e9, 0, 0,
                                                  ^(CFRunLoopTimerRef) {});
      CFRunLoopAddTimer(CFRunLoopGetCurrent(), keep, kCFRunLoopDefaultMode);
      CFRunLoopRun();
    }).detach();
    auto r = got.get();
    if (!r) { throw Error("midilib: MIDIClientCreateWithBlock failed"); }
    return r;
  }();
  return c;
}

// The words a Universal MIDI Packet of type `mt` takes.
inline uint32_t ump_words(uint32_t mt) {
  static const uint8_t words[16] = {1, 1, 1, 2, 2, 4, 1, 1, 2, 2, 2, 3, 3, 4, 4, 4};
  return words[mt & 0xF];
}

// The channel messages of an event list, in order, each with its packet's
// time. CoreMIDI gives MIDI 1.0 as UMP type 2; everything else (system
// messages, sysex) is left out.
inline void receive(const MIDIEventList *list, Queue &q) {
  const MIDIEventPacket *p = &list->packet[0];
  for (UInt32 i = 0; i < list->numPackets; i++) {
    auto time = host_to_ns(p->timeStamp ? p->timeStamp : mach_absolute_time());
    for (UInt32 w = 0; w < p->wordCount; w += ump_words(p->words[w] >> 28)) {
      auto word = p->words[w];
      Message m;
      if ((word >> 28) == 0x2 &&
          channel_message(static_cast<uint8_t>(word >> 16), static_cast<uint8_t>(word >> 8),
                          static_cast<uint8_t>(word), time, m)) {
        q.push(m);
      }
    }
    p = MIDIEventPacketNext(p);
  }
}

// A process sees the sources come and go only once it has a client, so the
// client comes first: a device plugged in after the program started is
// listed too.
inline std::vector<Port> ports() {
  client();
  std::vector<Port> out;
  auto n = MIDIGetNumberOfSources();
  for (ItemCount i = 0; i < n; i++) {
    auto src = MIDIGetSource(i);
    SInt32 uid = 0;
    if (!src || MIDIObjectGetIntegerProperty(src, kMIDIPropertyUniqueID, &uid) != noErr) { continue; }
    out.push_back({std::to_string(uid), name_of(src)});
  }
  return out;
}

// A source of CoreMIDI's, connected to a port of its own.
class Connection {
public:
  Connection(const std::string &id, std::shared_ptr<Queue> q) {
    MIDIObjectRef obj = 0;
    MIDIObjectType type;
    char *end = nullptr;
    auto uid = std::strtol(id.c_str(), &end, 10);
    if (id.empty() || *end != '\0' ||
        MIDIObjectFindByUniqueID(static_cast<MIDIUniqueID>(uid), &obj, &type) != noErr ||
        type != kMIDIObjectType_Source) {
      throw Error("midilib: no MIDI input " + id);
    }
    check(MIDIInputPortCreateWithProtocol(client(), CFSTR("midilib input"), kMIDIProtocol_1_0, &port_,
                                          ^(const MIDIEventList *list, void *) { receive(list, *q); }),
          "MIDIInputPortCreateWithProtocol");
    auto err = MIDIPortConnectSource(port_, static_cast<MIDIEndpointRef>(obj), nullptr);
    if (err != noErr) {
      MIDIPortDispose(port_);
      check(err, "MIDIPortConnectSource");
    }
  }
  ~Connection() { MIDIPortDispose(port_); }
  Connection(const Connection &) = delete;
  Connection &operator=(const Connection &) = delete;

private:
  MIDIPortRef port_ = 0;
};

#elif defined(_WIN32)

inline int64_t now_ns() {
  static const int64_t freq = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return static_cast<int64_t>(f.QuadPart);
  }();
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return static_cast<int64_t>(static_cast<double>(c.QuadPart) * 1e9 / static_cast<double>(freq));
}

// WinMM numbers its inputs, and a number moves when a device comes or
// goes; a name stays. The id is the name, with " (2)" and on for a second
// input of the same name.
inline std::vector<std::pair<std::string, UINT>> named_inputs() {
  std::vector<std::pair<std::string, UINT>> out;
  std::vector<std::string> names;  // as the devices give them
  UINT n = midiInGetNumDevs();
  for (UINT i = 0; i < n; i++) {
    MIDIINCAPSW caps;
    if (midiInGetDevCapsW(i, &caps, sizeof caps) != MMSYSERR_NOERROR) { continue; }
    char buf[MAXPNAMELEN * 4];
    int len = WideCharToMultiByte(CP_UTF8, 0, caps.szPname, -1, buf, sizeof buf, nullptr, nullptr);
    std::string name = len > 0 ? std::string(buf) : std::string();
    auto same = std::count(names.begin(), names.end(), name);
    names.push_back(name);
    out.push_back({same ? name + " (" + std::to_string(same + 1) + ")" : name, i});
  }
  return out;
}

inline std::vector<Port> ports() {
  std::vector<Port> out;
  for (auto &[id, i] : named_inputs()) { out.push_back({id, id}); }
  return out;
}

inline void CALLBACK on_input(HMIDIIN, UINT msg, DWORD_PTR instance, DWORD_PTR packed, DWORD_PTR) {
  if (msg != MIM_DATA) { return; }
  Message m;
  if (channel_message(static_cast<uint8_t>(packed), static_cast<uint8_t>(packed >> 8),
                      static_cast<uint8_t>(packed >> 16), now_ns(), m)) {
    reinterpret_cast<Queue *>(instance)->push(m);
  }
}

// An input of WinMM's, open and started.
class Connection {
public:
  Connection(const std::string &id, std::shared_ptr<Queue> q) : queue_(std::move(q)) {
    UINT index = UINT_MAX;
    for (auto &[name, i] : named_inputs()) {
      if (name == id) { index = i; }
    }
    if (index == UINT_MAX) { throw Error("midilib: no MIDI input " + id); }
    if (midiInOpen(&in_, index, reinterpret_cast<DWORD_PTR>(&on_input), reinterpret_cast<DWORD_PTR>(queue_.get()),
                   CALLBACK_FUNCTION) != MMSYSERR_NOERROR) {
      throw Error("midilib: midiInOpen failed for " + id);
    }
    midiInStart(in_);
  }
  ~Connection() {
    midiInStop(in_);
    midiInReset(in_);
    midiInClose(in_);
  }
  Connection(const Connection &) = delete;
  Connection &operator=(const Connection &) = delete;

private:
  HMIDIIN in_ = nullptr;
  std::shared_ptr<Queue> queue_;
};

#elif defined(__linux__)

inline int64_t now_ns() {
  timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return static_cast<int64_t>(t.tv_sec) * 1000000000 + t.tv_nsec;
}

#if defined(MIDILIB_ALSA)

// The ALSA sequencer's functions, from libasound when the first port is
// asked for; `ok` false when it is not there.
struct Alsa {
  bool ok = false;
#define MIDILIB_ALSA_FN(name) decltype(&::name) name = nullptr;
#define MIDILIB_ALSA_FNS(X)                                                                                   \
  X(snd_seq_open) X(snd_seq_close) X(snd_seq_set_client_name) X(snd_seq_create_simple_port)                    \
  X(snd_seq_connect_from) X(snd_seq_client_info_malloc) X(snd_seq_client_info_free)                            \
  X(snd_seq_client_info_set_client) X(snd_seq_client_info_get_client) X(snd_seq_client_info_get_name)         \
  X(snd_seq_query_next_client) X(snd_seq_port_info_malloc) X(snd_seq_port_info_free)                           \
  X(snd_seq_port_info_set_client) X(snd_seq_port_info_set_port) X(snd_seq_port_info_get_capability)            \
  X(snd_seq_port_info_get_name) X(snd_seq_port_info_get_port) X(snd_seq_query_next_port)                       \
  X(snd_seq_event_input) X(snd_seq_poll_descriptors_count) X(snd_seq_poll_descriptors) X(snd_seq_nonblock)    \
  X(snd_seq_client_id)
  MIDILIB_ALSA_FNS(MIDILIB_ALSA_FN)
#undef MIDILIB_ALSA_FN
};

inline const Alsa &alsa() {
  static const Alsa a = [] {
    Alsa r;
    void *lib = dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL);
    if (!lib) { return r; }
    bool all = true;
#define MIDILIB_ALSA_LOAD(name)                                                                                \
  r.name = reinterpret_cast<decltype(r.name)>(dlsym(lib, #name));                                              \
  all = all && r.name;
    MIDILIB_ALSA_FNS(MIDILIB_ALSA_LOAD)
#undef MIDILIB_ALSA_LOAD
    r.ok = all;
    return r;
  }();
  return a;
}

// A sequencer handle of its own, for one listing or one input. A machine
// with no sequencer device (a server, a container) has none, and ALSA is not
// asked, since it would say so on stderr each time.
inline snd_seq_t *open_sequencer() {
  auto &a = alsa();
  snd_seq_t *seq = nullptr;
  if (!a.ok || access("/dev/snd/seq", R_OK) != 0 || a.snd_seq_open(&seq, "default", SND_SEQ_OPEN_INPUT, 0) < 0) {
    return nullptr;
  }
  a.snd_seq_set_client_name(seq, "midilib");
  return seq;
}

// Every port another client lets be read from, as "client:port".
inline std::vector<Port> ports() {
  std::vector<Port> out;
  auto &a = alsa();
  auto seq = open_sequencer();
  if (!seq) { return out; }
  snd_seq_client_info_t *c;
  snd_seq_port_info_t *p;
  a.snd_seq_client_info_malloc(&c);
  a.snd_seq_port_info_malloc(&p);
  a.snd_seq_client_info_set_client(c, -1);
  while (a.snd_seq_query_next_client(seq, c) >= 0) {
    int client = a.snd_seq_client_info_get_client(c);
    if (client == SND_SEQ_CLIENT_SYSTEM || client == a.snd_seq_client_id(seq)) { continue; }
    a.snd_seq_port_info_set_client(p, client);
    a.snd_seq_port_info_set_port(p, -1);
    while (a.snd_seq_query_next_port(seq, p) >= 0) {
      unsigned caps = a.snd_seq_port_info_get_capability(p);
      unsigned want = SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ;
      if ((caps & want) != want) { continue; }
      out.push_back({std::to_string(client) + ":" + std::to_string(a.snd_seq_port_info_get_port(p)),
                     a.snd_seq_port_info_get_name(p)});
    }
  }
  a.snd_seq_port_info_free(p);
  a.snd_seq_client_info_free(c);
  a.snd_seq_close(seq);
  return out;
}

// A sequencer event as the channel message it stands for.
inline bool from_event(const snd_seq_event_t *e, Message &m) {
  auto t = now_ns();
  switch (e->type) {
  case SND_SEQ_EVENT_NOTEON:
    return channel_message(0x90 | e->data.note.channel, e->data.note.note, e->data.note.velocity, t, m);
  case SND_SEQ_EVENT_NOTEOFF:
    return channel_message(0x80 | e->data.note.channel, e->data.note.note, e->data.note.velocity, t, m);
  case SND_SEQ_EVENT_KEYPRESS:
    return channel_message(0xA0 | e->data.note.channel, e->data.note.note, e->data.note.velocity, t, m);
  case SND_SEQ_EVENT_CONTROLLER:
    return channel_message(0xB0 | e->data.control.channel, e->data.control.param, e->data.control.value, t, m);
  case SND_SEQ_EVENT_PGMCHANGE:
    return channel_message(0xC0 | e->data.control.channel, e->data.control.value, 0, t, m);
  case SND_SEQ_EVENT_CHANPRESS:
    return channel_message(0xD0 | e->data.control.channel, e->data.control.value, 0, t, m);
  case SND_SEQ_EVENT_PITCHBEND: {
    int v = e->data.control.value + 8192;
    return channel_message(0xE0 | e->data.control.channel, v & 0x7F, (v >> 7) & 0x7F, t, m);
  }
  default: return false;
  }
}

// A port of another client's, subscribed to from a port of this one's, read
// on a thread of its own.
class Connection {
public:
  Connection(const std::string &id, std::shared_ptr<Queue> q) {
    auto &a = alsa();
    auto colon = id.find(':');
    char *end = nullptr;
    long client = colon == std::string::npos ? -1 : std::strtol(id.substr(0, colon).c_str(), &end, 10);
    long port = colon == std::string::npos ? -1 : std::strtol(id.c_str() + colon + 1, &end, 10);
    bool listed = false;
    for (auto &p : ports()) { listed = listed || p.id == id; }
    if (!listed || client < 0 || port < 0) { throw Error("midilib: no MIDI input " + id); }
    seq_ = open_sequencer();
    if (!seq_) { throw Error("midilib: no ALSA sequencer"); }
    int mine = a.snd_seq_create_simple_port(seq_, "midilib input", SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
                                            SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    if (mine < 0 || a.snd_seq_connect_from(seq_, mine, static_cast<int>(client), static_cast<int>(port)) < 0) {
      a.snd_seq_close(seq_);
      throw Error("midilib: cannot subscribe to " + id);
    }
    a.snd_seq_nonblock(seq_, 1);
    reader_ = std::thread([this, q] { read(*q); });
  }
  ~Connection() {
    stop_ = true;
    reader_.join();
    alsa().snd_seq_close(seq_);
  }
  Connection(const Connection &) = delete;
  Connection &operator=(const Connection &) = delete;

private:
  // Wakes at least every 50 ms to see whether to stop.
  void read(Queue &q) {
    auto &a = alsa();
    std::vector<pollfd> fds(static_cast<size_t>(a.snd_seq_poll_descriptors_count(seq_, POLLIN)));
    a.snd_seq_poll_descriptors(seq_, fds.data(), static_cast<unsigned>(fds.size()), POLLIN);
    while (!stop_) {
      if (poll(fds.data(), fds.size(), 50) <= 0) { continue; }
      snd_seq_event_t *e = nullptr;
      while (a.snd_seq_event_input(seq_, &e) >= 0 && e) {
        Message m;
        if (from_event(e, m)) { q.push(m); }
      }
    }
  }

  snd_seq_t *seq_ = nullptr;
  std::atomic<bool> stop_{false};
  std::thread reader_;
};

#else

inline std::vector<Port> ports() { return {}; }

class Connection {
public:
  Connection(const std::string &id, std::shared_ptr<Queue>) { throw Error("midilib: no MIDI input " + id); }
};

#endif
#endif

} // namespace detail

// Now, on the clock the messages' times are on: on macOS the host clock,
// which CoreAudio stamps its buffers with; QueryPerformanceCounter on
// Windows; CLOCK_MONOTONIC on Linux.
inline int64_t now_ns() { return detail::now_ns(); }

inline std::vector<Port> input_ports() { return detail::ports(); }

// A port, open: its messages wait in a queue until poll() or drain() takes
// them. Read them from one thread.
class Input {
public:
  explicit Input(const std::string &id, size_t capacity = 4096)
      : queue_(std::make_shared<detail::Queue>(capacity)),
        connection_(std::make_unique<detail::Connection>(id, queue_)) {}

  // The next message; false when none is waiting.
  bool poll(Message &m) {
    auto f = queue_->front();
    if (!f) { return false; }
    m = *f;
    queue_->pop();
    return true;
  }

  // Every message that came at or before `until_ns`, in order, added to
  // `out`; a later one stays for the next call. Answers how many were added.
  size_t drain(std::vector<Message> &out, int64_t until_ns) {
    size_t n = 0;
    while (auto f = queue_->front()) {
      if (f->time_ns > until_ns) { break; }
      out.push_back(*f);
      queue_->pop();
      n++;
    }
    return n;
  }

  // Waits until a message is waiting or `timeout` has passed; whether one
  // is.
  bool wait(std::chrono::nanoseconds timeout) { return queue_->wait(timeout); }

  // Messages dropped because the queue was full.
  uint64_t dropped() const { return queue_->dropped(); }

private:
  std::shared_ptr<detail::Queue> queue_;
  std::unique_ptr<detail::Connection> connection_;
};

// The controllers' values as the messages given to it set them, at 14 bits
// where a controller sends them so (as an EWI's breath may): controllers 0
// to 31 take the lower 7 bits from 32 to 63, and a new upper half clears the
// lower one, as MIDI 1.0 says. A note on's velocity takes the lower 7 bits
// from the High Resolution Velocity Prefix (CC 88) just before it.
class Controls {
public:
  void apply(const Message &m) {
    auto ch = m.channel();
    switch (m.kind()) {
    case Kind::cc: {
      auto cc = m.cc();
      if (cc == 88) {
        prefix_[ch] = static_cast<int8_t>(m.value());
      } else if (cc < 32) {
        values_[ch][cc] = static_cast<uint16_t>(m.value() << 7);
      } else if (cc < 64) {
        auto &v = values_[ch][cc - 32];
        v = static_cast<uint16_t>((v & ~0x7F) | m.value());
      } else {
        values_[ch][cc] = static_cast<uint16_t>(m.value() << 7);
      }
      break;
    }
    case Kind::note_on:
      velocity_[ch] = static_cast<uint16_t>((m.velocity() << 7) | (prefix_[ch] < 0 ? 0 : prefix_[ch]));
      prefix_[ch] = -1;
      break;
    case Kind::note_off:
      prefix_[ch] = -1;
      break;
    default:
      break;
    }
  }

  // Controller `cc` (0 to 31, or 64 to 127) on `channel`: 0 to 16383. A
  // controller that sends 7 bits has them as its upper 7.
  int value14(int channel, int cc) const { return values_[channel & 0xF][cc & 0x7F]; }

  // The same, 0.0 to 1.0.
  double value(int channel, int cc) const { return value14(channel, cc) / 16383.0; }

  // The velocity of the last note on on `channel`: 0 to 16383.
  int velocity14(int channel) const { return velocity_[channel & 0xF]; }

private:
  uint16_t values_[16][128] = {};
  uint16_t velocity_[16] = {};
  int8_t prefix_[16] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
};

} // namespace midilib
