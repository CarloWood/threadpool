/**
 * threadpool -- A thread pool with support for multiple queues and ultra fast timers.
 *
 * @file
 * @brief Declaration of class TimerQueue.
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

#include <deque>
#include <cstdint>
#include "Timer.h"
#include "debug.h"

namespace threadpool {

class RunningTimers;

/**
 * A queue of running (possibly canceled) timers, all of the same interval.
 *
 * This queue stores Timer*'s. Each Timer will have the same interval (which interval
 * that is depends on the context in which the TimerQueue was found). If a pointer
 * is nullptr then it represents a canceled timer; such timers are not removed
 * from the queue because that would cost too much CPU.
 *
 * In the description of the member functions, 'front' means the next timer
 * that will be returned by pop(), also if that timer was already canceled!
 */
class TimerQueue
{
  using running_timers_type = std::deque<Timer*>;

 private:
  uint64_t m_sequence_offset;                   // The number of timers that were popped from m_running_timers.
  running_timers_type m_running_timers;         // All running timers for the related interval.

 public:
  /// Construct an empty queue.
  TimerQueue() : m_sequence_offset(0) { }

  /**
   * Add a new timer to the end of the queue.
   *
   * The TimerQueue must be locked before calling push(), and
   * without releasing that lock the result should be passed
   * to is_front() to check if m_running_timers was empty.
   * If that is the case then RunningTimers::m_mutex must
   * be locked before releasing the lock on this queue.
   * timer->get_expiration_point() can subsequently be
   * used to update RunningTimers::m_cache.
   *
   * @returns An ever increasing sequence number starting with 0.
   */
  uint64_t push(Timer* timer)
  {
    uint64_t size = m_running_timers.size();
    m_running_timers.emplace_back(timer);
    return size + m_sequence_offset;
  }

  /**
   * Check if a timer is at the front of the queue.
   *
   * This function might need to be called after calling push, in order
   * to check if a newly added timer expires sooner than what we're
   * currently waiting for.
   *
   * @returns True if @a sequence is the value returned by a call to push() for a timer that is now at the front (will be returned by pop() next).
   */
  bool is_front(uint64_t sequence) const
  {
    return sequence == m_sequence_offset;
  }

  /**
   * Cancel a running timer.
   *
   * The @a sequence passed must be returned by a previous call to push() and may not have been canceled before.
   *
   * If this function returns true then the mutex @a m has been locked
   * and RunningTimers needs updating.
   *
   * @param sequence : The sequence number of a previously pushed timer.
   * @param m : A reference to RunningTimers::m_mutex.
   *
   * @returns True if the canceled Timer was the front timer.
   */
  bool cancel(uint64_t sequence, std::mutex& m)
  {
    // A negative value can happen when this timer has expired and was popped in the meantime.
    int64_t index = sequence - m_sequence_offset;
    // Sequence must be returned by a previous call to push() and the Timer may not already have expired.
    ASSERT(index < static_cast<int64_t>(m_running_timers.size()));
    // Do not cancel a timer twice.
    ASSERT(index < 0 || m_running_timers[index]);
    bool is_front = index == 0;
    if (is_front)
      pop(m);
    else if (index > 0)
      m_running_timers[index] = nullptr;
    return is_front;
  }

  /**
   * Return the timer at the front of the queue.
   *
   * This function may only be called when the queue is not empty.
   * RunningTimers::m_mutex must be locked before calling this function.
   * The returned pointer will never be null.
   *
   * @returns The front timer.
   */
  Timer* peek()
  {
    // Do not call peek() when the queue is empty.
    ASSERT(!m_running_timers.empty());
    return m_running_timers.front();
  }

  /**
   * Remove one timer from the front of the queue and return it.
   *
   * This function may only be called when the queue is not empty.
   * The returned pointer will never be null.
   *
   * Afterwards, the mutex @a m is locked.
   *
   * @param m : A reference to RunningTimers::m_mutex.
   *
   * @returns The popped front timer.
   */
  Timer* pop(std::mutex& m)
  {
    running_timers_type::iterator b = m_running_timers.begin();
    running_timers_type::iterator const e = m_running_timers.end();

    // Do not call pop() when the queue is empty.
    ASSERT(b != e);

    Timer* timer = *b;

    // The front timer may never be null?!
    ASSERT(timer);

    do
    {
      ++m_sequence_offset;              // This effectively marks removal when m_sequence_offset > Timer::m_sequence (by causing index in cancel() to become negative).
      ++b;
    }
    while (b != e && *b == nullptr);    // Is the next timer canceled?

    // Erase the range [begin, b).
    m.lock();
    m_running_timers.erase(m_running_timers.begin(), b);

    return timer;
  }

  /**
   * Return the next time point at which a timer of this interval will expire.
   *
   * This function returns Timer::none if the queue is empty.
   * This makes it suitable to be passed to increase_cache.
   *
   * RunningTimers::m_mutex must be locked before calling this function.
   * The returned expiration point can be used to update RunningTimers::m_cache
   * while keeping m_mutex locked.
   */
  Timer::time_point next_expiration_point() const
  {
    if (m_running_timers.empty())
      return Timer::s_none;
    // Note that front() is never a canceled timer.
    return m_running_timers.front()->get_expiration_point();
  }

 private:
  friend class RunningTimers;
  void set_not_running()
  {
    for (Timer* timer : m_running_timers)
      if (timer)
        timer->set_not_running();
  }

 public:
  //--------------------------------------------------------------------------
  // Everything below is just for debugging.

  // Return true if there are no running timers for the related interval.
  bool debug_empty() const { return m_running_timers.empty(); }

  // Return the number of elements in m_running_timers. This includes canceled timers.
  running_timers_type::size_type debug_size() const { return m_running_timers.size(); }

  // Return the number of element in m_running_timers that are canceled.
  int debug_canceled_in_queue() const
  {
    int sz = 0;
    for (auto timer : m_running_timers)
      sz += timer ? 0 : 1;
    return sz;
  }

  // Accessor for m_sequence_offset.
  uint64_t debug_get_sequence_offset() const { return m_sequence_offset; }

  // Allow iterating directly over all elements of m_running_timers.
  auto debug_begin() const { return m_running_timers.begin(); }
  auto debug_end() const { return m_running_timers.end(); }
};

} // namespace threadpool
