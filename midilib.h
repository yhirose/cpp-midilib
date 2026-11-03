//
//  midilib.h
//
//  MIDI for a program that plays sound as it is played: the input ports, one
//  opened by its id, and its messages in the order they came, each with the
//  time it came on a clock that only goes forward; and the output ports, to
//  send channel messages and sysexes to.
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
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// The MIDI API alone: <mmsystem.h>, which a full <windows.h> takes too, would
// also bring PlaySound and the rest of winmm as macros, which rename functions
// of the same names elsewhere (raylib's).
#include <mmsyscom.h>
#include <mmeapi.h>
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
#include <tuple>
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
  sysex,
};

// One channel message as it came, or a sysex for an input that asked for
// them. A note on with velocity 0 is a note off (MIDI 1.0), so kind() says
// note_off; its bytes stay as they came.
struct Message {
  int64_t time_ns = 0;
  uint8_t bytes[3] = {0, 0, 0};
  uint8_t size = 0;
  // A sysex's bytes, F0 to F7 (then bytes[0] is F0 and size 0); else empty.
  std::vector<uint8_t> sysex;

  Kind kind() const {
    if (bytes[0] == 0xF0) { return Kind::sysex; }
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

// The bytes a channel message with this status takes, or 0 for any other
// byte.
inline size_t channel_size(uint8_t status) {
  if (status < 0x80 || status >= 0xF0) { return 0; }
  return (status & 0xF0) == 0xC0 || (status & 0xF0) == 0xD0 ? 2 : 3;
}

// A channel message from its status and data bytes, or false for anything
// else (system messages are left out).
inline bool channel_message(uint8_t status, uint8_t d1, uint8_t d2, int64_t time_ns, Message &m) {
  auto size = channel_size(status);
  if (!size) { return false; }
  m.time_ns = time_ns;
  m.bytes[0] = status;
  m.bytes[1] = d1 & 0x7F;
  m.bytes[2] = size == 2 ? 0 : (d2 & 0x7F);
  m.size = static_cast<uint8_t>(size);
  return true;
}

inline std::string hex(uint8_t b) {
  const char *digits = "0123456789ABCDEF";
  return {digits[b >> 4], digits[b & 0xF]};
}

// The error for a port that is not there.
inline Error no_port(const std::string &id, bool output) {
  return Error(std::string("midilib: no MIDI ") + (output ? "output " : "input ") + id);
}

// The messages in `bytes`: whole channel messages and sysexes, one after
// another, with no running status. Anything else is an Error, so nothing of
// it is sent.
inline std::vector<Message> messages_in(const uint8_t *bytes, size_t n) {
  std::vector<Message> out;
  auto at = [](size_t i) { return " at byte " + std::to_string(i); };
  for (size_t i = 0; i < n;) {
    Message m;
    if (bytes[i] == 0xF0) {
      size_t j = i + 1;
      while (j < n && bytes[j] < 0x80) { j++; }
      if (j == n || bytes[j] != 0xF7) {
        throw Error("midilib: the sysex" + at(i) + " does not end with F7 after its data bytes");
      }
      m.bytes[0] = 0xF0;
      m.sysex.assign(bytes + i, bytes + j + 1);
      i = j + 1;
    } else {
      auto size = channel_size(bytes[i]);
      if (!size) { throw Error("midilib: " + hex(bytes[i]) + at(i) + " begins no channel message or sysex"); }
      if (i + size > n) { throw Error("midilib: the message" + at(i) + " is cut short"); }
      for (size_t k = 1; k < size; k++) {
        if (bytes[i + k] >= 0x80) { throw Error("midilib: the message" + at(i) + " has a status byte for data"); }
      }
      channel_message(bytes[i], bytes[i + 1], size == 3 ? bytes[i + 2] : 0, 0, m);
      i += size;
    }
    out.push_back(std::move(m));
  }
  return out;
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

  void push(Message m) {
    auto head = head_.load(std::memory_order_relaxed);
    if (head - tail_.load(std::memory_order_acquire) == slots_.size()) {
      drop();
      return;
    }
    slots_[head & mask_] = std::move(m);
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

  // A message that came and was not kept, counted.
  void drop() { dropped_.fetch_add(1, std::memory_order_relaxed); }

  // The oldest message, left in place for the reader to take; nullptr when
  // there is none.
  Message *front() {
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

// Where an input's messages go: its queue, and for an input that asked for
// sysexes, the one being put together from the pieces it comes in
// (CoreMIDI's packets of six bytes, WinMM's buffers, ALSA's events). A
// sysex is handed on whole, with the time its first piece came; one cut
// short by a status byte, or longer than MAX_SYSEX, is dropped and counted.
// Real-time bytes may come between its bytes and are passed over. Fed on one
// thread.
class Inbox {
public:
  static constexpr size_t MAX_SYSEX = 65536;

  Inbox(size_t capacity, bool sysex) : queue(capacity), wants_sysex(sysex) {}

  void channel(uint8_t status, uint8_t d1, uint8_t d2, int64_t time_ns) {
    Message m;
    if (channel_message(status, d1, d2, time_ns, m)) { queue.push(std::move(m)); }
  }

  void sysex(const uint8_t *bytes, size_t n, int64_t time_ns) {
    if (!wants_sysex) { return; }
    for (size_t i = 0; i < n; i++) {
      auto b = bytes[i];
      if (b >= 0xF8) { continue; }
      if (b == 0xF0) {
        if (open_) { queue.drop(); }
        open_ = true;
        start_ns_ = time_ns;
        pending_.assign(1, b);
      } else if (!open_) {
        continue;
      } else if (pending_.size() == MAX_SYSEX) {
        open_ = false;
        queue.drop();
      } else if (b < 0x80) {
        pending_.push_back(b);
      } else if (b != 0xF7) {
        open_ = false;
        queue.drop();
      } else {
        open_ = false;
        pending_.push_back(b);
        Message m;
        m.time_ns = start_ns_;
        m.bytes[0] = 0xF0;
        m.sysex = std::move(pending_);
        pending_ = {};
        queue.push(std::move(m));
      }
    }
  }

  Queue queue;
  const bool wants_sysex;

private:
  std::vector<uint8_t> pending_;
  bool open_ = false;
  int64_t start_ns_ = 0;
};

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

// The messages of an event list, in order, each with its packet's time.
// CoreMIDI gives MIDI 1.0 as UMP type 2, and a sysex as type 3 packets of up
// to six data bytes (whole, or its start, middle or end); anything else
// (system messages) is left out.
inline void receive(const MIDIEventList *list, Inbox &in) {
  const MIDIEventPacket *p = &list->packet[0];
  for (UInt32 i = 0; i < list->numPackets; i++) {
    auto time = host_to_ns(p->timeStamp ? p->timeStamp : mach_absolute_time());
    for (UInt32 w = 0; w < p->wordCount; w += ump_words(p->words[w] >> 28)) {
      auto word = p->words[w];
      if ((word >> 28) == kMIDIMessageTypeChannelVoice1) {
        in.channel(static_cast<uint8_t>(word >> 16), static_cast<uint8_t>(word >> 8), static_cast<uint8_t>(word),
                   time);
      } else if ((word >> 28) == kMIDIMessageTypeSysEx && w + 1 < p->wordCount) {
        auto status = (word >> 20) & 0xF;
        auto n = std::min<uint32_t>((word >> 16) & 0xF, 6);
        auto next = p->words[w + 1];
        uint8_t data[6] = {uint8_t(word >> 8), uint8_t(word),         uint8_t(next >> 24),
                           uint8_t(next >> 16), uint8_t(next >> 8), uint8_t(next)};
        uint8_t bytes[8];
        size_t k = 0;
        if (status == kMIDISysExStatusComplete || status == kMIDISysExStatusStart) { bytes[k++] = 0xF0; }
        for (uint32_t j = 0; j < n; j++) { bytes[k++] = data[j]; }
        if (status == kMIDISysExStatusComplete || status == kMIDISysExStatusEnd) { bytes[k++] = 0xF7; }
        in.sysex(bytes, k, time);
      }
    }
    p = MIDIEventPacketNext(p);
  }
}

// The sources (`output` false) or destinations there are. A process sees
// them come and go only once it has a client, so the client comes first: a
// device plugged in after the program started is listed too.
inline std::vector<Port> ports(bool output) {
  client();
  std::vector<Port> out;
  auto n = output ? MIDIGetNumberOfDestinations() : MIDIGetNumberOfSources();
  for (ItemCount i = 0; i < n; i++) {
    auto end = output ? MIDIGetDestination(i) : MIDIGetSource(i);
    SInt32 uid = 0;
    if (!end || MIDIObjectGetIntegerProperty(end, kMIDIPropertyUniqueID, &uid) != noErr) { continue; }
    out.push_back({std::to_string(uid), name_of(end)});
  }
  return out;
}

// The source (or destination) whose unique id is `id`, or 0.
inline MIDIEndpointRef endpoint(const std::string &id, MIDIObjectType want) {
  MIDIObjectRef obj = 0;
  MIDIObjectType type;
  char *end = nullptr;
  auto uid = std::strtol(id.c_str(), &end, 10);
  if (id.empty() || *end != '\0' || MIDIObjectFindByUniqueID(static_cast<MIDIUniqueID>(uid), &obj, &type) != noErr ||
      type != want) {
    return 0;
  }
  return static_cast<MIDIEndpointRef>(obj);
}

// A source of CoreMIDI's, connected to a port of its own.
class Connection {
public:
  Connection(const std::string &id, std::shared_ptr<Inbox> in) {
    auto src = endpoint(id, kMIDIObjectType_Source);
    if (!src) { throw no_port(id, false); }
    check(MIDIInputPortCreateWithProtocol(client(), CFSTR("midilib input"), kMIDIProtocol_1_0, &port_,
                                          ^(const MIDIEventList *list, void *) { receive(list, *in); }),
          "MIDIInputPortCreateWithProtocol");
    auto err = MIDIPortConnectSource(port_, src, nullptr);
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

// A destination of CoreMIDI's, sent to from a port of its own.
class Sender {
public:
  explicit Sender(const std::string &id) : dest_(endpoint(id, kMIDIObjectType_Destination)) {
    if (!dest_) { throw no_port(id, true); }
    check(MIDIOutputPortCreate(client(), CFSTR("midilib output"), &port_), "MIDIOutputPortCreate");
  }
  ~Sender() { MIDIPortDispose(port_); }
  Sender(const Sender &) = delete;
  Sender &operator=(const Sender &) = delete;

  // Each message as a UMP of MIDI 1.0: a channel message in one word, a
  // sysex in packets of two words holding six data bytes at most.
  void send(const std::vector<Message> &messages) {
    std::vector<uint32_t> words;
    for (auto &m : messages) {
      if (m.kind() != Kind::sysex) {
        words.push_back(MIDI1UPChannelVoiceMessage(0, m.bytes[0] >> 4, m.bytes[0] & 0xF, m.bytes[1], m.bytes[2]));
        continue;
      }
      auto first = m.sysex.data() + 1, last = m.sysex.data() + m.sysex.size() - 1;  // the data bytes
      for (auto at = first; at == first || at < last; at += kMIDI1UPMaxSysexSize) {
        auto end = last - at > kMIDI1UPMaxSysexSize ? at + kMIDI1UPMaxSysexSize : last;
        bool starts = at == first, ends = end == last;
        auto status = starts && ends ? kMIDISysExStatusComplete
                      : starts       ? kMIDISysExStatusStart
                      : ends         ? kMIDISysExStatusEnd
                                     : kMIDISysExStatusContinue;
        auto ump = MIDI1UPSysExArray(0, status, at, end);
        words.insert(words.end(), {ump.word0, ump.word1});
      }
    }
    // A packet holds 64 words at most, and each has a header of its own.
    std::vector<uint64_t> buf((sizeof(MIDIEventList) + words.size() * 4 + (words.size() / 32 + 2) * sizeof(MIDIEventPacket)) / 8 + 1);
    auto list = reinterpret_cast<MIDIEventList *>(buf.data());
    auto p = MIDIEventListInit(list, kMIDIProtocol_1_0);
    for (size_t w = 0; w < words.size(); w += ump_words(words[w] >> 28)) {
      p = MIDIEventListAdd(list, buf.size() * 8, p, 0, ump_words(words[w] >> 28), &words[w]);
      if (!p) { throw Error("midilib: MIDIEventListAdd ran out of room"); }
    }
    check(MIDISendEventList(port_, dest_, list), "MIDISendEventList");
  }

private:
  MIDIEndpointRef dest_ = 0;
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

inline std::string utf8(const WCHAR *w) {
  char buf[MAXPNAMELEN * 4];
  int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof buf, nullptr, nullptr);
  return len > 0 ? std::string(buf) : std::string();
}

// WinMM numbers its inputs (and its outputs), and a number moves when a
// device comes or goes; a name stays. The id is the name, with " (2)" and on
// for a second port of the same name.
inline std::vector<std::pair<std::string, UINT>> named_ports(bool output) {
  std::vector<std::pair<std::string, UINT>> out;
  std::vector<std::string> names;  // as the devices give them
  UINT n = output ? midiOutGetNumDevs() : midiInGetNumDevs();
  for (UINT i = 0; i < n; i++) {
    std::string name;
    if (output) {
      MIDIOUTCAPSW caps;
      if (midiOutGetDevCapsW(i, &caps, sizeof caps) != MMSYSERR_NOERROR) { continue; }
      name = utf8(caps.szPname);
    } else {
      MIDIINCAPSW caps;
      if (midiInGetDevCapsW(i, &caps, sizeof caps) != MMSYSERR_NOERROR) { continue; }
      name = utf8(caps.szPname);
    }
    auto same = std::count(names.begin(), names.end(), name);
    names.push_back(name);
    out.push_back({same ? name + " (" + std::to_string(same + 1) + ")" : name, i});
  }
  return out;
}

inline std::vector<Port> ports(bool output) {
  std::vector<Port> out;
  for (auto &[id, i] : named_ports(output)) { out.push_back({id, id}); }
  return out;
}

inline UINT port_index(const std::string &id, bool output) {
  for (auto &[name, i] : named_ports(output)) {
    if (name == id) { return i; }
  }
  throw no_port(id, output);
}

// An input of WinMM's, open and started. A sysex comes in buffers given to
// it beforehand, each given back once it is read (or found broken); there
// are none for an input that did not ask for sysexes, so WinMM drops theirs.
class Connection {
public:
  static constexpr size_t BUFFERS = 4, BUFFER_SIZE = 1024;

  Connection(const std::string &id, std::shared_ptr<Inbox> in) : inbox_(std::move(in)) {
    auto index = port_index(id, false);
    if (midiInOpen(&in_, index, reinterpret_cast<DWORD_PTR>(&on_input), reinterpret_cast<DWORD_PTR>(this),
                   CALLBACK_FUNCTION) != MMSYSERR_NOERROR) {
      throw Error("midilib: midiInOpen failed for " + id);
    }
    if (inbox_->wants_sysex) {
      buffers_.assign(BUFFERS, std::vector<char>(BUFFER_SIZE));
      headers_.resize(BUFFERS);
      for (size_t i = 0; i < BUFFERS; i++) {
        headers_[i] = MIDIHDR{};
        headers_[i].lpData = buffers_[i].data();
        headers_[i].dwBufferLength = static_cast<DWORD>(BUFFER_SIZE);
        midiInPrepareHeader(in_, &headers_[i], sizeof(MIDIHDR));
        midiInAddBuffer(in_, &headers_[i], sizeof(MIDIHDR));
      }
    }
    midiInStart(in_);
  }
  ~Connection() {
    closing_ = true;
    midiInStop(in_);
    midiInReset(in_);  // gives back the buffers, unread
    for (auto &h : headers_) { midiInUnprepareHeader(in_, &h, sizeof(MIDIHDR)); }
    midiInClose(in_);
  }
  Connection(const Connection &) = delete;
  Connection &operator=(const Connection &) = delete;

private:
  static void CALLBACK on_input(HMIDIIN h, UINT msg, DWORD_PTR instance, DWORD_PTR p1, DWORD_PTR) {
    auto self = reinterpret_cast<Connection *>(instance);
    if (msg == MIM_DATA) {
      self->inbox_->channel(static_cast<uint8_t>(p1), static_cast<uint8_t>(p1 >> 8), static_cast<uint8_t>(p1 >> 16),
                            now_ns());
    } else if (msg == MIM_LONGDATA || msg == MIM_LONGERROR) {
      auto hdr = reinterpret_cast<MIDIHDR *>(p1);
      if (self->closing_) { return; }
      if (msg == MIM_LONGDATA) {
        self->inbox_->sysex(reinterpret_cast<const uint8_t *>(hdr->lpData), hdr->dwBytesRecorded, now_ns());
      }
      midiInAddBuffer(h, hdr, sizeof(MIDIHDR));
    }
  }

  HMIDIIN in_ = nullptr;
  std::shared_ptr<Inbox> inbox_;
  std::vector<std::vector<char>> buffers_;
  std::vector<MIDIHDR> headers_;
  std::atomic<bool> closing_{false};
};

// An output of WinMM's, open. A sysex is sent whole before send() returns.
class Sender {
public:
  explicit Sender(const std::string &id) {
    if (midiOutOpen(&out_, port_index(id, true), 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
      throw Error("midilib: midiOutOpen failed for " + id);
    }
  }
  ~Sender() { midiOutClose(out_); }
  Sender(const Sender &) = delete;
  Sender &operator=(const Sender &) = delete;

  void send(const std::vector<Message> &messages) {
    for (auto &m : messages) {
      if (m.kind() != Kind::sysex) {
        DWORD packed = m.bytes[0] | (DWORD(m.bytes[1]) << 8) | (DWORD(m.bytes[2]) << 16);
        if (midiOutShortMsg(out_, packed) != MMSYSERR_NOERROR) { throw Error("midilib: midiOutShortMsg failed"); }
        continue;
      }
      MIDIHDR h{};
      h.lpData = reinterpret_cast<LPSTR>(const_cast<uint8_t *>(m.sysex.data()));  // only read
      h.dwBufferLength = static_cast<DWORD>(m.sysex.size());
      if (midiOutPrepareHeader(out_, &h, sizeof h) != MMSYSERR_NOERROR) {
        throw Error("midilib: midiOutPrepareHeader failed");
      }
      auto err = midiOutLongMsg(out_, &h, sizeof h);
      while (err == MMSYSERR_NOERROR && !(h.dwFlags & MHDR_DONE)) { Sleep(1); }
      midiOutUnprepareHeader(out_, &h, sizeof h);
      if (err != MMSYSERR_NOERROR) { throw Error("midilib: midiOutLongMsg failed"); }
    }
  }

private:
  HMIDIOUT out_ = nullptr;
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
  X(snd_seq_client_id) X(snd_seq_connect_to) X(snd_seq_event_output_direct)
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

// A sequencer handle of its own, for one listing, one input or one output
// (`mode` SND_SEQ_OPEN_INPUT or _OUTPUT). A machine with no sequencer device
// (a server, a container) has none, and ALSA is not asked, since it would
// say so on stderr each time.
inline snd_seq_t *open_sequencer(int mode = SND_SEQ_OPEN_INPUT) {
  auto &a = alsa();
  snd_seq_t *seq = nullptr;
  if (!a.ok || access("/dev/snd/seq", R_OK) != 0 || a.snd_seq_open(&seq, "default", mode, 0) < 0) {
    return nullptr;
  }
  a.snd_seq_set_client_name(seq, "midilib");
  return seq;
}

// Every port another client lets be read from (or, `output`, written to),
// as "client:port".
// The capabilities a port that is read from (or written to) has.
inline unsigned port_caps(bool write) {
  return write ? SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE : SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ;
}

inline std::vector<Port> ports(bool output) {
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
      unsigned want = port_caps(output);
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

// A sequencer event handed on as the message it stands for: a channel
// message, or a piece of a sysex (ALSA may split a long one).
inline void receive(const snd_seq_event_t *e, Inbox &in) {
  auto t = now_ns();
  switch (e->type) {
  case SND_SEQ_EVENT_NOTEON:
    return in.channel(0x90 | e->data.note.channel, e->data.note.note, e->data.note.velocity, t);
  case SND_SEQ_EVENT_NOTEOFF:
    return in.channel(0x80 | e->data.note.channel, e->data.note.note, e->data.note.velocity, t);
  case SND_SEQ_EVENT_KEYPRESS:
    return in.channel(0xA0 | e->data.note.channel, e->data.note.note, e->data.note.velocity, t);
  case SND_SEQ_EVENT_CONTROLLER:
    return in.channel(0xB0 | e->data.control.channel, e->data.control.param, e->data.control.value, t);
  case SND_SEQ_EVENT_PGMCHANGE:
    return in.channel(0xC0 | e->data.control.channel, e->data.control.value, 0, t);
  case SND_SEQ_EVENT_CHANPRESS:
    return in.channel(0xD0 | e->data.control.channel, e->data.control.value, 0, t);
  case SND_SEQ_EVENT_PITCHBEND: {
    int v = e->data.control.value + 8192;
    return in.channel(0xE0 | e->data.control.channel, v & 0x7F, (v >> 7) & 0x7F, t);
  }
  case SND_SEQ_EVENT_SYSEX:
    return in.sysex(static_cast<const uint8_t *>(e->data.ext.ptr), e->data.ext.len, t);
  default: return;
  }
}

// The client and port of id "client:port" when it is listed, or an Error
// naming it an input or an output.
inline std::pair<int, int> address(const std::string &id, bool output) {
  auto colon = id.find(':');
  char *end = nullptr;
  long client = colon == std::string::npos ? -1 : std::strtol(id.substr(0, colon).c_str(), &end, 10);
  long port = colon == std::string::npos ? -1 : std::strtol(id.c_str() + colon + 1, &end, 10);
  bool listed = false;
  for (auto &p : ports(output)) { listed = listed || p.id == id; }
  if (!listed || client < 0 || port < 0) { throw no_port(id, output); }
  return {static_cast<int>(client), static_cast<int>(port)};
}

// A sequencer of its own with a port of this client's, connected to `id`:
// subscribed to it, or (`output`) writing to it.
inline std::pair<snd_seq_t *, int> open_port(const std::string &id, bool output) {
  auto &a = alsa();
  auto [client, port] = address(id, output);
  auto seq = open_sequencer(output ? SND_SEQ_OPEN_OUTPUT : SND_SEQ_OPEN_INPUT);
  if (!seq) { throw Error("midilib: no ALSA sequencer"); }
  int mine = a.snd_seq_create_simple_port(seq, output ? "midilib output" : "midilib input", port_caps(!output),
                                          SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
  auto connect = output ? a.snd_seq_connect_to : a.snd_seq_connect_from;
  if (mine < 0 || connect(seq, mine, client, port) < 0) {
    a.snd_seq_close(seq);
    throw Error(std::string("midilib: cannot ") + (output ? "connect to " : "subscribe to ") + id);
  }
  return {seq, mine};
}

// A port of another client's, subscribed to from a port of this one's, read
// on a thread of its own.
class Connection {
public:
  Connection(const std::string &id, std::shared_ptr<Inbox> in) {
    seq_ = open_port(id, false).first;
    alsa().snd_seq_nonblock(seq_, 1);
    reader_ = std::thread([this, in] { read(*in); });
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
  void read(Inbox &in) {
    auto &a = alsa();
    std::vector<pollfd> fds(static_cast<size_t>(a.snd_seq_poll_descriptors_count(seq_, POLLIN)));
    a.snd_seq_poll_descriptors(seq_, fds.data(), static_cast<unsigned>(fds.size()), POLLIN);
    while (!stop_) {
      if (poll(fds.data(), fds.size(), 50) <= 0) { continue; }
      snd_seq_event_t *e = nullptr;
      while (a.snd_seq_event_input(seq_, &e) >= 0 && e) { receive(e, in); }
    }
  }

  snd_seq_t *seq_ = nullptr;
  std::atomic<bool> stop_{false};
  std::thread reader_;
};

// A port of another client's, written to through a port of this one's.
class Sender {
public:
  explicit Sender(const std::string &id) { std::tie(seq_, port_) = open_port(id, true); }
  ~Sender() { alsa().snd_seq_close(seq_); }
  Sender(const Sender &) = delete;
  Sender &operator=(const Sender &) = delete;

  void send(const std::vector<Message> &messages) {
    for (auto &m : messages) {
      snd_seq_event_t ev{};
      snd_seq_ev_set_source(&ev, port_);
      snd_seq_ev_set_subs(&ev);
      snd_seq_ev_set_direct(&ev);
      uint8_t ch = m.bytes[0] & 0x0F, d1 = m.bytes[1], d2 = m.bytes[2];
      switch (m.kind()) {
      case Kind::note_off: snd_seq_ev_set_noteoff(&ev, ch, d1, d2); break;
      case Kind::note_on: snd_seq_ev_set_noteon(&ev, ch, d1, d2); break;
      case Kind::poly_pressure: snd_seq_ev_set_keypress(&ev, ch, d1, d2); break;
      case Kind::cc: snd_seq_ev_set_controller(&ev, ch, d1, d2); break;
      case Kind::program: snd_seq_ev_set_pgmchange(&ev, ch, d1); break;
      case Kind::channel_pressure: snd_seq_ev_set_chanpress(&ev, ch, d1); break;
      case Kind::bend: snd_seq_ev_set_pitchbend(&ev, ch, m.bend()); break;
      case Kind::sysex: snd_seq_ev_set_sysex(&ev, m.sysex.size(), const_cast<uint8_t *>(m.sysex.data())); break;
      }
      if (alsa().snd_seq_event_output_direct(seq_, &ev) < 0) {
        throw Error("midilib: snd_seq_event_output_direct failed");
      }
    }
  }

private:
  snd_seq_t *seq_ = nullptr;
  int port_ = -1;
};

#else

inline std::vector<Port> ports(bool) { return {}; }

class Connection {
public:
  Connection(const std::string &id, std::shared_ptr<Inbox>) { throw no_port(id, false); }
};

class Sender {
public:
  explicit Sender(const std::string &id) { throw no_port(id, true); }
  void send(const std::vector<Message> &) {}
};

#endif
#endif

} // namespace detail

// Now, on the clock the messages' times are on: on macOS the host clock,
// which CoreAudio stamps its buffers with; QueryPerformanceCounter on
// Windows; CLOCK_MONOTONIC on Linux.
inline int64_t now_ns() { return detail::now_ns(); }

inline std::vector<Port> input_ports() { return detail::ports(false); }

inline std::vector<Port> output_ports() { return detail::ports(true); }

// A port, open: its messages wait in a queue until poll() or drain() takes
// them. Read them from one thread. An input that asks for sysexes (`sysex`)
// gets each whole as one message; one that does not never sees them. A
// sysex's bytes are freed where its message is, so read such an input off
// the audio thread.
class Input {
public:
  explicit Input(const std::string &id, size_t capacity = 4096, bool sysex = false)
      : inbox_(std::make_shared<detail::Inbox>(capacity, sysex)),
        connection_(std::make_unique<detail::Connection>(id, inbox_)) {}

  // The next message; false when none is waiting.
  bool poll(Message &m) {
    auto f = inbox_->queue.front();
    if (!f) { return false; }
    m = std::move(*f);
    inbox_->queue.pop();
    return true;
  }

  // Every message that came at or before `until_ns`, in order, added to
  // `out`; a later one stays for the next call. Answers how many were added.
  size_t drain(std::vector<Message> &out, int64_t until_ns) {
    size_t n = 0;
    while (auto f = inbox_->queue.front()) {
      if (f->time_ns > until_ns) { break; }
      out.push_back(std::move(*f));
      inbox_->queue.pop();
      n++;
    }
    return n;
  }

  // Waits until a message is waiting or `timeout` has passed; whether one
  // is.
  bool wait(std::chrono::nanoseconds timeout) { return inbox_->queue.wait(timeout); }

  // Messages dropped because the queue was full, and sysexes cut short or
  // too long.
  uint64_t dropped() const { return inbox_->queue.dropped(); }

private:
  std::shared_ptr<detail::Inbox> inbox_;
  std::unique_ptr<detail::Connection> connection_;
};

// An output port, open. send() takes whole messages, one after another:
// channel messages and sysexes (F0, data bytes, F7), with no running status.
// Bytes that are anything else are refused with an Error, and none of them
// is sent. Send from one thread.
class Output {
public:
  explicit Output(const std::string &id) : sender_(std::make_unique<detail::Sender>(id)) {}

  void send(const uint8_t *bytes, size_t n) { sender_->send(detail::messages_in(bytes, n)); }
  void send(const std::vector<uint8_t> &bytes) { send(bytes.data(), bytes.size()); }

private:
  std::unique_ptr<detail::Sender> sender_;
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
