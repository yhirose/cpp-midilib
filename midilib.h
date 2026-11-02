//
//  midilib.h
//
//  MIDI input for a program that plays sound as it is played: the ports, one
//  opened by its id, and its messages in the order they came, each with the
//  time it came on the clock the audio device keeps.
//
//  Copyright (c) 2026 Yuji Hirose. All rights reserved.
//  MIT License
//

#pragma once

#if !defined(__APPLE__)
#error "midilib: only macOS (CoreMIDI) for now"
#endif

#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>
#include <mach/mach_time.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace midilib {

struct Error : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// A port to open: `id` stays the same while the device is plugged in, and
// across runs where the system keeps it; `name` is for people.
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

// The one CoreMIDI client of the process, made when it is first needed. It
// is never disposed: the system ends it with the process.
inline MIDIClientRef client() {
  static const MIDIClientRef c = [] {
    MIDIClientRef r = 0;
    check(MIDIClientCreateWithBlock(CFSTR("midilib"), &r, ^(const MIDINotification *) {}),
          "MIDIClientCreateWithBlock");
    return r;
  }();
  return c;
}

// Messages from the MIDI thread (the one producer) to the thread that reads
// them (the one consumer), without a lock. When it is full, what comes is
// counted and dropped, and what is in it is kept.
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
  }

  // The oldest message, left in place; nullptr when there is none.
  const Message *front() const {
    auto tail = tail_.load(std::memory_order_relaxed);
    if (tail == head_.load(std::memory_order_acquire)) { return nullptr; }
    return &slots_[tail & mask_];
  }

  void pop() { tail_.store(tail_.load(std::memory_order_relaxed) + 1, std::memory_order_release); }

  uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }

private:
  std::vector<Message> slots_;
  size_t mask_ = 0;
  alignas(64) std::atomic<size_t> head_{0};
  alignas(64) std::atomic<size_t> tail_{0};
  std::atomic<uint64_t> dropped_{0};
};

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
      if ((word >> 28) != 0x2) { continue; }
      Message m;
      m.time_ns = time;
      m.bytes[0] = static_cast<uint8_t>(word >> 16);
      m.bytes[1] = static_cast<uint8_t>((word >> 8) & 0x7F);
      m.bytes[2] = static_cast<uint8_t>(word & 0x7F);
      auto status = m.bytes[0] & 0xF0;
      m.size = status == 0xC0 || status == 0xD0 ? 2 : 3;
      if (m.size == 2) { m.bytes[2] = 0; }
      q.push(m);
    }
    p = MIDIEventPacketNext(p);
  }
}

} // namespace detail

// Now, on the clock the messages' times are on: the host clock, which is
// also the one CoreAudio stamps its buffers with (AudioTimeStamp.mHostTime).
inline int64_t now_ns() { return detail::host_to_ns(mach_absolute_time()); }

inline std::vector<Port> input_ports() {
  std::vector<Port> ports;
  auto n = MIDIGetNumberOfSources();
  for (ItemCount i = 0; i < n; i++) {
    auto src = MIDIGetSource(i);
    SInt32 uid = 0;
    if (!src || MIDIObjectGetIntegerProperty(src, kMIDIPropertyUniqueID, &uid) != noErr) { continue; }
    ports.push_back({std::to_string(uid), detail::name_of(src)});
  }
  return ports;
}

// A port, open: its messages wait in a queue until poll() or drain() takes
// them. Read them from one thread.
class Input {
public:
  explicit Input(const std::string &id, size_t capacity = 4096)
      : queue_(std::make_shared<detail::Queue>(capacity)) {
    MIDIObjectRef obj = 0;
    MIDIObjectType type;
    char *end = nullptr;
    auto uid = std::strtol(id.c_str(), &end, 10);
    if (id.empty() || *end != '\0' ||
        MIDIObjectFindByUniqueID(static_cast<MIDIUniqueID>(uid), &obj, &type) != noErr ||
        type != kMIDIObjectType_Source) {
      throw Error("midilib: no MIDI input " + id);
    }
    auto q = queue_;
    detail::check(MIDIInputPortCreateWithProtocol(
                      detail::client(), CFSTR("midilib input"), kMIDIProtocol_1_0, &port_,
                      ^(const MIDIEventList *list, void *) { detail::receive(list, *q); }),
                  "MIDIInputPortCreateWithProtocol");
    auto err = MIDIPortConnectSource(port_, static_cast<MIDIEndpointRef>(obj), nullptr);
    if (err != noErr) {
      MIDIPortDispose(port_);
      detail::check(err, "MIDIPortConnectSource");
    }
  }

  ~Input() {
    if (port_) { MIDIPortDispose(port_); }
  }

  Input(const Input &) = delete;
  Input &operator=(const Input &) = delete;

  Input(Input &&other) noexcept : port_(other.port_), queue_(std::move(other.queue_)) { other.port_ = 0; }

  Input &operator=(Input &&other) noexcept {
    if (this != &other) {
      if (port_) { MIDIPortDispose(port_); }
      port_ = other.port_;
      queue_ = std::move(other.queue_);
      other.port_ = 0;
    }
    return *this;
  }

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

  // Messages dropped because the queue was full.
  uint64_t dropped() const { return queue_->dropped(); }

private:
  MIDIPortRef port_ = 0;
  std::shared_ptr<detail::Queue> queue_;
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
