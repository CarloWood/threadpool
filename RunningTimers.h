/**
 * threadpool -- A thread pool with support for multiple queues and ultra fast timers.
 *
 * @file
 * @brief Declaration of class RunningTimers.
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

#include "utils/nearest_power_of_two.h"
#include "utils/Singleton.h"
#include "threadsafe/aithreadsafe.h"
#include "TimerQueue.h"
#include "debug.h"
#include <array>
#include <csignal>
#include <mutex>

namespace threadpool {

// This is a tournament tree of queues of timers with the same interval.
//
// The advantage of using queues of timers with the same interval
// is that it is extremely cheap to insert a new timer with a
// given interval when there is already another timer with the
// same interval running. On top of that, it is likely that even
// a program that has a million timers running simultaneously
// only uses a handful of distinct intervals, so the size of
// the tree/heap shrinks dramatically.
//
// Assume the number of different intervals is 6, and thus tree_size == 8,
// then the structure of the data in RunningTimers could look like:
//
// Tournament tree (tree index:tree index)
// m_tree:                                              1:4
//                                             /                  \                             .
//                                  2:0                                     3:4
//                              /         \                             /         \             .
//                        4:0                 5:3                 6:4                 7:6
//                      /     \             /     \             /     \             /     \     .
// m_cache:           18  no_timer       102        55        10        60  no_timer  no_timer
// index of m_cache:   0         1         2         3         4         5         6         7
//
// Thus, m_tree[1] contains 4, m_tree[2] contains 0, m_tree[3] contains 4 and so on.
// Each time a parent contains the same interval (in) as one of its children and well
// such that m_cache[in] contains the smallest time_point value of the two.
// m_cache[in] contains a copy of the top of m_queues[in].
//
class RunningTimers : public Singleton<RunningTimers>
{
  friend_Instance;
 private:
  RunningTimers();
  ~RunningTimers();
  RunningTimers(RunningTimers const&) = delete;

 protected:
  static constexpr int tree_size = 64;                                  // Allow at most 64 different time intervals (so that m_tree fits in a single cache line).

  std::mutex m_mutex;                                                   // Protects (the consistency of) m_tree and m_cache and the first element of each TimerQueue.
  std::array<uint8_t, tree_size> m_tree;
  std::array<Timer::time_point, tree_size> m_cache;

  // m_queues doesn't need a mutex because it is initialized before main() and then never changed anymore.
  using timer_queue_t = aithreadsafe::Wrapper<TimerQueue, aithreadsafe::policy::Primitive<std::mutex>>;
  utils::Vector<timer_queue_t, TimerQueueIndex> m_queues;

  struct Current {
    timer_t posix_timer;                                                // The POSIX per-process timer.
    Timer* expired_timer;                                               // Set by update_current_timer when this timer has expired.
    Timer::time_point expire_point;

    Current();
  };
  using current_t = aithreadsafe::Wrapper<Current, aithreadsafe::policy::Primitive<std::mutex>>;
  int const m_timer_signum;                                             // Signal number used for the m_current::posix_timer.
  sigset_t const m_timer_sigset;                                        // Same, but as a mask.
  std::atomic_bool m_POSIX_timer_expired;                               // Set by the m_timer_signum signal handler.
  // Must be constructed AFTER m_timer_signum because its constructor uses m_timer_signum.
  current_t m_current;

  static int constexpr parent_of(int index)                             // Used in increase_cache and decrease_cache.
  {
    return index >> 1;
  }

  static int constexpr interval_to_parent_index(int in)                 // Used in increase_cache and decrease_cache.
  {
    return (in + tree_size) >> 1;
  }

  static int constexpr sibling_of(int index)                            // Used in increase_cache.
  {
    return index ^ 1;
  }

  static int constexpr left_child_of(int index)                         // Only used in constructor.
  {
    return index << 1;
  }

  //=====================================================================================
  // DANGER: do not change the two functions below! They are extremely sensitive to bugs!
  // They were created by trial and error in a testsuite that brute force tested all 1500
  // possibilities. If you make any change you WILL break it.
  //=====================================================================================
  //
  void increase_cache(int interval, Timer::time_point tp)       // m_mutex must be locked.
  {
    ASSERT(tp >= m_cache[interval]);
    m_cache[interval] = tp;

    int parent_ti = interval_to_parent_index(interval); // Let 'parent_ti' be the index of the parent node in the tree above 'interval'.

    int in = interval;                                  // Let 'in' be the interval whose value is changed with respect to m_tree[parent_ti].
    int si = in ^ 1;                                    // Let 'si' be the interval of the current sibling of in.
    for(;;)
    {
      Timer::time_point sv = m_cache[si];
      if (tp > sv)
      {
        if (m_tree[parent_ti] == si)
          break;
        tp = sv;
        in = si;
      }
      m_tree[parent_ti] = in;                           // Update the tree.
      if (parent_ti == 1)                               // If this was the top-most node in the tree then we're done.
        break;
      si = m_tree[sibling_of(parent_ti)];               // Update the sibling interval.
      parent_ti = parent_of(parent_ti);                 // Set 'parent_ti' to be the index of the parent node in the tree above 'parent_ti'.
    }
  }

  void decrease_cache(int interval, Timer::time_point tp)       // m_mutex must be locked.
  {
    ASSERT(tp <= m_cache[interval]);
    m_cache[interval] = tp;                             // Replace no_timer with tp.
    // We just put a SMALLER value in the cache at position interval than what there was before.
    // Therefore all we have to do is overwrite parents with our interval until the time_point
    // value of the parent is less than tp.
    int parent_ti = interval_to_parent_index(interval); // Let 'parent_ti' be the index of the parent node in the tree above 'interval'.
    while (tp <= m_cache[m_tree[parent_ti]])            // m_tree[parent_ti] is the content of that node. m_cache[m_tree[parent_ti]] is the value.
    {
      m_tree[parent_ti] = interval;                     // Update that tree node.
      if (parent_ti == 1)                               // If this was the top-most node in the tree then we're done.
        break;
      parent_ti = parent_of(parent_ti);                 // Set 'i' to be the index of the parent node in the tree above 'i'.
    }
  }
  //=====================================================================================

 public:
  // Return access type for m_current.
  current_t::wat access_current() { return static_cast<current_t::wat>(m_current); }

  /**
   * current_w->expired_timer is set to the timer that has expired, if any.
   * Timer::expire must be called on that Timer.
   *
   * The hardware timer is set to raise the s_timer_signum signal
   * when the next Timer expires, if any.
   *
   * Returns true when the next timer also already expired.
   */
  bool update_current_timer(current_t::wat& current_w, Timer::time_point now);

  sigset_t const* get_timer_sigset() const { return &m_timer_sigset; }
  // This may only be called from timer_signal_handler().
  void set_POSIX_timer_expired() { m_POSIX_timer_expired.store(true, std::memory_order_relaxed); }

  int to_cache_index(TimerQueueIndex index) const { return index.get_value(); }
  TimerQueueIndex to_queues_index(int index) const { return TimerQueueIndex(index); }

  /// Cancel the timer associated with handle.
  void cancel(Timer::Handle const& handle)
  {
    DoutEntering(dc::notice, "RunningTimers::cancel(" << &handle << ")");

    Timer::time_point expiration_point;
    int cache_index = to_cache_index(handle.interval_index());
    {
      timer_queue_t::wat queue_w(m_queues[handle.interval_index()]);
      // If cancel() returns true then it locked m_mutex.
      if (!queue_w->cancel(handle.sequence(), m_mutex))         // Not the current timer for this interval?
        return;                                                 // Then not the current timer.

      // m_mutex is now locked.

      // At this point the canceled timer is at the front of the queue;
      // because of that we need to update (increase) the corresponding cache value.
      // The queue is kept locked during this process to assure that 'expiration_point'
      // for this cache_index isn't changed by having that THAT timer be canceled
      // as well.

      expiration_point = queue_w->next_expiration_point();
    } // Unlock the queue so new timers can be added. There is no danger that the front timer
      // will be canceled because it is required to own m_mutex for that.

    bool is_current = m_tree[1] == cache_index;
    increase_cache(cache_index, expiration_point);
    m_mutex.unlock();

    return;
  }

  // Add @a timer to the list of running timers, using @a interval as timeout.
  // @a expiration_point must be a reference to timer->m_expiration_point and will
  // be filled with Timer::clock_type::now() + interval.duration().
  Timer::Handle push(Timer::Interval interval, Timer* timer, Timer::time_point& expiration_point);

#ifdef DEBUG_SPECIFY_NOW
  // The testsuite uses this. timer->m_expiration_point must already be set.
  // It is deprecated and will be removed in the future because it results in race conditions.
  Timer::Handle push(TimerQueueIndex interval_index, Timer* timer);
#endif

  void initialize(size_t number_of_intervals)
  {
    ASSERT(number_of_intervals <= (size_t)tree_size);
    // No need to lock m_mutex here because initialize is only called before we even reached main().
    // Just construct a completely new vector, because we can't resize a vector with mutexes.
    utils::Vector<timer_queue_t, TimerQueueIndex> new_queues(number_of_intervals);
    m_queues.swap(new_queues);
  }

 private:
#ifdef CWDEBUG
  //--------------------------------------------------------------------------
  // Everything below is just for debugging.

  size_t debug_size() const
  {
    size_t sz = 0;
    for (auto&& queue : m_queues)
      sz += timer_queue_t::crat(queue)->debug_size();
    return sz;
  }

  int debug_canceled_in_queue() const
  {
    int sz = 0;
    for (auto&& queue : m_queues)
      sz += timer_queue_t::crat(queue)->debug_canceled_in_queue();
    return sz;
  }
#endif
};

} // namespace threadpool
