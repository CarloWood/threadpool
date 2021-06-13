/**
 * threadpool -- A thread pool with support for multiple queues and ultra fast timers.
 *
 * @file
 * @brief Declaration of class Timer.
 *
 * @Copyright (C) 2018  Carlo Wood.
 *
 * RSA-1024 0x624ACAD5 1997-01-26                    Sign & Encrypt
 * Fingerprint16 = 32 EC A7 B6 AC DB 65 A6  F6 F6 55 DD 1C DC FF 61
 *
 * This file is part of threadpool.
 *
 * Threadpool is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Threadpool is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with threadpool.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "AIThreadPool.h"
#include "utils/Vector.h"
#include "utils/Singleton.h"
#include "utils/FuzzyBool.h"
#include "debug.h"
#ifdef CWDEBUG
#include <libcwd/type_info.h>
#endif
#include <limits>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>

#if defined(CWDEBUG) && !defined(DOXYGEN)
NAMESPACE_DEBUG_CHANNELS_START
extern channel_ct timer;
NAMESPACE_DEBUG_CHANNELS_END
#endif

namespace threadpool {

#ifndef DOXYGEN
namespace ordering_category {
struct TimerQueue;	// Ordering category of TimerQueue;
} // namespace ordering_category
#endif

/// The type of an index into RunningTimers::m_queues.
using TimerQueueIndex = utils::VectorIndex<ordering_category::TimerQueue>;

class TimerQueue;
class RunningTimers;

#ifndef DOXYGEN
struct TimerTypes
{
  using clock_type = std::chrono::steady_clock;                 // It being monotonic is a requirement.
  using time_point = std::chrono::time_point<clock_type>;

  // Require at least 100 microseconds resolution (which is 1/10000th of a second).
  // This is more like a warning; if this asserts you have to sit down and think about what to do.
  static_assert(clock_type::period::den >= 10000 * clock_type::period::num, "Your clock doesn't have enough resolution.");
};
#endif

template<TimerTypes::time_point::rep count, typename Unit>
struct Interval;

class TimerStart
{
 public:
  using clock_type = TimerTypes::clock_type;    ///< The underlaying clock type.
  using time_point = TimerTypes::time_point;    ///< The underlaying time point.

#if CW_DEBUG && !defined(DOXYGEN)
  static bool s_interval_constructed;
#endif

#ifndef DOXYGEN
  // Use a value far in the future to represent 'no timer' (aka, a "timer" that will never expire).
  static constexpr time_point s_none{time_point::max()};

  /**
   * A timer interval.
   *
   * A TimerStart::Interval can only be instantiated from a <code>threadpool::Interval<count, Unit></code>
   * and only after main() is already reached. Normally you just want to pass a
   * <code>threadpool::Interval<count, Unit></code> directly to @ref start.
   */
  struct Interval
  {
   private:
    TimerQueueIndex m_index;
    time_point::duration m_duration;

   private:
    // Only threadpool::Interval maybe construct these objects (which happens before main()).
    template<TimerTypes::time_point::rep count, typename Unit>
    friend struct threadpool::Interval;
    Interval(TimerQueueIndex index_, time_point::duration duration_) : m_index(index_), m_duration(duration_) { DEBUG_ONLY(TimerStart::s_interval_constructed = true); }

   public:
    Interval() : m_index{}, m_duration{} { }

    // Should only be used from AITimer::set_interval for its member variable AITimer::mInterval directly after construction.
    Interval const& operator=(Interval const& rhs)
    {
      // We're trying to keep Interval objects immutable: do not assign to an Interval that wasn't just default constructed.
      ASSERT(m_index.undefined());
      m_index = rhs.m_index;
      m_duration = rhs.m_duration;
      return *this;
    }

   public:
    /// A copy constructor is provided, but doesn't seem needed.
    Interval(Interval const& interval) : m_index(interval.m_index), m_duration(interval.m_duration) { DEBUG_ONLY(TimerStart::s_interval_constructed |= !m_index.undefined()); }

    /// Return the duration of this interval.
    time_point::duration duration() const { return m_duration; }

    /// Return the index of this interval.
    TimerQueueIndex index() const { return m_index; }

#ifdef CWDEBUG
    friend std::ostream& operator<<(std::ostream& os, Interval const& interval)
    {
      os << '{' << interval.m_index << ", " << interval.m_duration << '}';
      return os;
    }
#endif
  };

  /// Start the timer providing a (new) call back function, to expire after time period `interval`.
  void start(Interval interval, std::function<void()> call_back);

  /// Start the timer, using a previously assigned call back function, to expire after time period `interval`.
  void start(Interval interval);

#ifdef DEBUG_SPECIFY_NOW
  /// Start this timer providing a (new) call back function, to expire after time
  /// period `interval` relative to `now`. This causes a race that will assert(!)
  /// if another timer with the same interval is started shortly after: each timer
  /// is added to queue (one per interval) and new expiration points of added timers
  /// must monotonically increase. Concurrent calls to start() do not ensure
  /// they are added to this queue in the order that start() is called.
  /// Therefore only use this function when you know what you are doing and feel
  /// confident that at all times the timer will be added to the queue before another
  /// timer with the same interval is started.
  void start(Interval interval, std::function<void()> call_back, time_point now);

  /// Start this timer using a previously assigned call back function. See comment
  /// above with regard to race condition.
  void start(Interval interval, time_point now);
#endif
};

/**
 * A timer.
 *
 * Allows a callback to some <code>std::function<void()></code> that can
 * be specified during construction or while calling the @ref start member function.
 *
 * The @ref start member function must be passed an Interval object.
 */
class Timer : public TimerStart
{
 public:
  struct Handle
  {
   private:
    static constexpr int s_not_running = -1;
    static constexpr int s_expire_called_first = -2;
    static constexpr int s_stop_called_first = -3;
    // A Handle is read-only for the duration that the corresponding Timer is running.
    // *ALL* methods must be either const or private. The only function that may
    // change a Handle is Timer::start.
    uint64_t m_sequence;                ///< A sequence number that is unique within the set of Timer-s with the same interval. Only valid when running.
    TimerQueueIndex m_interval_index;   ///< A TimerQueueIndex.
    mutable std::atomic_int m_flags;

   public:
    /// Default constructor: construct a handle for a "not running timer".
    // The value of m_sequence is irrelevant since the default constructor of m_interval_index will turn this into a 'not running' timer,
    // the assigned magic number however will cause operator== to function.
    Handle() : m_sequence(std::numeric_limits<uint64_t>::max() - 1), m_flags(s_not_running) { }

    /// Construct a Handle for a running timer with interval @a interval and number sequence @a sequence.
    constexpr Handle(TimerQueueIndex interval_index, uint64_t sequence) : m_sequence(sequence), m_interval_index(interval_index), m_flags(0) { }

    uint64_t sequence() const { return m_sequence; }
    TimerQueueIndex interval_index() const { return m_interval_index; }
    bool operator==(Handle const& rhs) const { return m_sequence == rhs.m_sequence; }

    utils::FuzzyBool can_expire() const
    {
      return (!m_interval_index.undefined() && m_flags.load(std::memory_order_relaxed) == 0) ? fuzzy::WasTrue : fuzzy::False;
    }
    bool do_call_back() const
    {
      int flags = m_flags.load(std::memory_order_relaxed);
      return flags == 0 && m_flags.compare_exchange_strong(flags, s_expire_called_first, std::memory_order_relaxed);
    }
    bool do_call_cancel() const
    {
      int flags = m_flags.load(std::memory_order_relaxed);
      return flags == 0 && m_flags.compare_exchange_strong(flags, s_stop_called_first, std::memory_order_relaxed);
    }
    bool stop_called_first() const
    {
      return m_flags.load(std::memory_order_relaxed) == s_stop_called_first;
    }
    void set_not_running()
    {
      m_interval_index.set_to_undefined();
    }

   private:
    // start() is the only function that may assign to Handle.
    friend void TimerStart::start(Interval interval, std::function<void()> call_back);
    friend void TimerStart::start(Interval interval);
#ifdef DEBUG_SPECIFY_NOW
    friend void TimerStart::start(Interval interval, std::function<void()> call_back, time_point now);
    friend void TimerStart::start(Interval interval, time_point now);
#endif
    Handle& operator=(Handle const& rhs)
    {
      m_sequence = rhs.m_sequence;
      m_interval_index = rhs.m_interval_index;
      m_flags.store(0);
      return *this;
    }
  };
#endif

 private:
  friend class TimerStart;
  Handle m_handle;                      ///< If m_handle.is_running() returns true then this timer is running
                                        ///  and m_handle can be used to find the corresponding Timer object.
  time_point m_expiration_point;        ///< The time at which we should expire (only valid when this is a running timer).
  std::function<void()> m_call_back;    ///< The callback function (only valid when this is a running timer).
  std::mutex m_calling_expire;          ///< Locked while calling expire() to prevent destructing or reusing the Timer before that finished.

 public:
  Timer() = default;
  Timer(std::function<void()> call_back) : m_call_back(call_back) { }

  void wait_for_possible_expire_to_finish()
  {
    std::lock_guard<std::mutex> lk(m_calling_expire);
  }

  /// Destruct the timer. If it is (still) running, stop it.
  // If stop() fails then the timer expired and might be still in the process of calling Timer::expire.
  // Therefore wait until m_calling_expire can be locked again before destructing this object.
  // If stop() doesn't fail, then it is still possible that Timer::expire will be called, although
  // the actual call to m_call_back() was prohibited. Therefore we unconditionally must attempt to lock
  // m_calling_expire.
  ~Timer()
  {
    stop();
    wait_for_possible_expire_to_finish();
  }

  /// Stop this timer if it is (still) running.
  /// Returns true if the call to the call_back was successfully prohibited.
  bool stop();

  /// Call this to reset the call back function, destructing any possible objects that it might contain.
  void release_callback()
  {
    // Don't call release_callback while the timer is running.
    // You can call it from the call back itself however.
    ASSERT(m_handle.can_expire().is_false());
    m_call_back = std::function<void()>();
  }

  // Return the current time in the appropriate type.
  static time_point now() { return clock_type::now(); }

 private:
  friend class TimerQueue;
  // Called by RunningTimers upon destruction. Causes a later call to stop() not to access RunningTimers anymore.
  void set_not_running();

 private:
  // Called from RunningTimers::update_current_timer when this timer is found to have expired
  // and is thus returned as current_w->expired_timer.
  friend class RunningTimers;
  void expired_start()
  {
    m_calling_expire.lock();       // Unlocked upon leaving expire().
  }

  // Called when this timer expires.
  // Called from AIThreadPool::Worker::tmain for timers that expired (were just returned
  // by RunningTimers::update_current_timer). Hence m_calling_expire is locked (see expired_start).
  friend class AIThreadPool::Worker;
  void expire()
  {
    DoutEntering(dc::timer, "Timer::expire() [" << this << "]");
    std::lock_guard<std::mutex> lk(m_calling_expire, std::adopt_lock);
    if (m_handle.do_call_back())
      m_call_back();
  }

 public:
  // Accessors.

  /// Return the handle of this timer.
//  Handle handle() const { return m_handle; }

  /// Return the point at which this timer will expire. Only valid when is_running.
  time_point get_expiration_point() const { return m_expiration_point; }

#ifdef CWDEBUG
  // Called from testsuite.
  void debug_expire()
  {
    expire();
  }

  bool debug_has_handle(Handle const& handle) const
  {
    return m_handle == handle;
  }

  void print_on(std::ostream& os) const { os << "Timer:" << get_expiration_point(); }
  friend std::ostream& operator<<(std::ostream& os, Timer const& timer)
  {
    timer.print_on(os);
    return os;
  }
#endif
};

namespace detail {

class Index;

class Indexes : public Singleton<Indexes>
{
  friend_Instance;

 private:
  Indexes() = default;
  ~Indexes() = default;
  Indexes(Indexes const&) = delete;

  std::multimap<Timer::time_point::rep, Index*> m_map;
  std::vector<Timer::time_point::rep> m_intervals;

 public:
  void add(Timer::time_point::rep period, Index* index);
  size_t number() const { return m_intervals.size(); }
  Timer::time_point::duration duration(int index) const { return Timer::time_point::duration{m_intervals[index]}; }
};

class Index
{
 private:
  friend class Indexes;
  std::size_t m_index;

 public:
  operator std::size_t() const { return m_index; }
};

template<Timer::time_point::rep period>
struct Register : public Index
{
  Register() { Indexes::instantiate().add(period, this); }
};

} // namespace detail

template<Timer::time_point::rep count, typename Unit>
struct Interval
{
  static constexpr Timer::time_point::rep period = std::chrono::duration_cast<Timer::time_point::duration>(Unit{count}).count();
  static detail::Register<period> index;
  Interval() { }
  operator Timer::Interval() const { return {TimerQueueIndex(index), Timer::time_point::duration{period}}; }
};

//static
template<Timer::time_point::rep count, typename Unit>
constexpr Timer::time_point::rep Interval<count, Unit>::period;

//static
template<Timer::time_point::rep count, typename Unit>
detail::Register<Interval<count, Unit>::period> Interval<count, Unit>::index;

static constexpr int number_of_slow_down_intervals = 12;
extern std::array<Timer::Interval, number_of_slow_down_intervals> slow_down_intervals;

} // namespace threadpool
