/**
 * threadpool -- A thread pool with support for multiple queues and ultra fast timers.
 *
 * @file
 * @brief Implementation of RunningTimers.
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

#include "sys.h"
#include "RunningTimers.h"
#include "AIThreadPool.h"
#include "utils/macros.h"
#include "utils/Signals.h"
#include <new>
#include <cstring>

extern "C" void timer_signal_handler(int)
{
  //write(1, "\nEntering timer_signal_handler()\n", 33);
  threadpool::RunningTimers::instance().set_POSIX_timer_expired();
  AIThreadPool::call_update_current_timer();
  //write(1, "\nLeaving timer_signal_handler()\n", 32);
}

namespace threadpool {

RunningTimers::RunningTimers() : m_timer_signum(utils::Signal::reserve_and_next_rt_signum()), m_POSIX_timer_expired(false),
  m_timer_sigset{[&]{ sigset_t tmp; sigemptyset(&tmp); sigaddset(&tmp, m_timer_signum); return tmp; }()}
{
  // Initialize m_cache and m_tree.
  for (int interval = 0; interval < tree_size; ++interval)
  {
    m_cache[interval] = Timer::s_none;
    int parent_ti = interval_to_parent_index(interval);
    m_tree[parent_ti] = interval & ~1;
  }
  // Initialize the rest of m_tree.
  for (int index = tree_size / 2 - 1; index > 0; --index)
    m_tree[index] = m_tree[left_child_of(index)];

  // Call timer_signal_handler when the m_timer_signum signal is caught by a thread.
  //std::cerr << "m_timer_signum = " << m_timer_signum << std::endl;
  utils::Signal::instance().register_callback(m_timer_signum, timer_signal_handler);
}

RunningTimers::Current::Current() : expired_timer(nullptr), expire_point(Timer::time_point::max())
{
  // Create a monotonic timer.
  struct sigevent sigevent;
  std::memset(&sigevent, 0, sizeof(struct sigevent));
  sigevent.sigev_notify = SIGEV_SIGNAL;
  // Even though this is the constructor of a global object, m_timer_signum is
  // already initialized at this point, even though RunningTimers hasn't fully
  // been constructed yet.
  sigevent.sigev_signo = RunningTimers::instance().m_timer_signum;
  //std::cerr << "sigevent.sigev_signo = " << sigevent.sigev_signo << std::endl;
  if (timer_create(CLOCK_MONOTONIC, &sigevent, &posix_timer) == -1)
  {
    DoutFatal(dc::fatal|error_cf, "timer_create (with m_timer_signum = " << sigevent.sigev_signo << ")");
  }
}

Timer::Handle RunningTimers::push(Timer::Interval interval, Timer* timer, Timer::time_point& expiration_point)
{
  DoutEntering(dc::notice, "RunningTimers::push(" << interval.duration() << ", " << (void*)timer << ")");
  TimerQueueIndex const interval_index = interval.index();
  assert(interval_index.get_value() < m_queues.size());
  uint64_t sequence;
  bool is_front;
  {
    timer_queue_t::wat queue_w(m_queues[interval_index]);
    expiration_point = interval.duration() + Timer::clock_type::now();
    // expiration_point must be a reference to timer->m_expiration_point.
    ASSERT(expiration_point == timer->get_expiration_point());

    Dout(dc::timer, "Inserting " << expiration_point << " into the queue.");
    sequence = queue_w->push(timer);
    is_front = queue_w->is_front(sequence);
    // Being 'front' means that it is the next timer to expire with this interval.
    // Since the Handle for this timer isn't returned yet, it can't be canceled
    // by another thread, so it will remain the front timer until we leave this
    // function. It is not necessary to keep the queue locked.
  }
  if (is_front)
  {
    int cache_index = to_cache_index(interval_index);
    m_mutex.lock();                                                                                     // ^
    decrease_cache(cache_index, expiration_point);                                                      // | m_mutex locked
    bool is_next = m_tree[1] == cache_index;                                                            // |
    m_mutex.unlock();                                                                                   // v
    if (is_next)
      AIThreadPool::call_update_current_timer();
  }
  return {interval_index, sequence};
}

#ifdef DEBUG_SPECIFY_NOW
// Deprecated function left in for testsuite.
Timer::Handle RunningTimers::push(TimerQueueIndex interval_index, Timer* timer)
{
  DoutEntering(dc::notice, "RunningTimers::push(" << interval_index << ", " << (void*)timer << ")");
  assert(interval_index.get_value() < m_queues.size());
  uint64_t sequence;
  bool is_front;
  {
    timer_queue_t::wat queue_w(m_queues[interval_index]);
    sequence = queue_w->push(timer);
    is_front = queue_w->is_front(sequence);
  }
  if (is_front)
  {
    int cache_index = to_cache_index(interval_index);
    m_mutex.lock();                                                                                     // ^
    decrease_cache(cache_index, timer->get_expiration_point());                                         // | m_mutex locked
    bool is_next = m_tree[1] == cache_index;                                                            // |
    m_mutex.unlock();                                                                                   // v
    if (is_next)
      AIThreadPool::call_update_current_timer();
  }
  return {interval_index, sequence};
}
#endif

// If there is a timer that has already expired, return it.
// Otherwise return nullptr and, if there is a timer that didn't expire yet, call timer_settime(2).
bool RunningTimers::update_current_timer(current_t::wat& current_w, Timer::time_point now)
{
  DoutEntering(dc::notice, "RunningTimers::update_current_timer(current_w, " << now.time_since_epoch() << " s)");

  // Initialize interval, next and timer to correspond to the Timer in RunningTimers
  // that is the first to expire next, if any (if not then return nullptr).
  int interval;
  Timer::time_point next;
  Timer* timer;
  Timer::time_point::duration duration;
  bool current_unlocked = false;
  m_mutex.lock();                                                                                       // ^
  while (true)  // So we can use continue.                                                              // | m_mutex locked
  {                                                                                                     // |
    interval = m_tree[1];                       // The interval of the timer that will expire next.     // |
    if (AI_LIKELY(!current_unlocked))                                                                   // |
    {                                                                                                   // |
      // current_w must be unlocked before we lock m_queues to avoid a dead lock.                       // |
      current_w.unlock();                       // Unlock m_current.                                    // |
      current_unlocked = true;                                                                          // |
    }                                                                                                   // v
    m_mutex.unlock();                           // The queue must be locked first in order to avoid a deadlock.
    timer_queue_t::wat queue_w(m_queues[to_queues_index(interval)]);
    // *** THIS IS THE CANONICAL POINT AT WHICH MOMENT THE TIMER EXPIRES *).
    // Where 'expires' means that timer->expire() will be called.
    // *) Provided that all boolean expressions have the right value.
    //    It might still not happen if,
    //    1) Right here m_tree[1] changes to a different interval because
    //       a new timer with a different interval is added that expires sooner.
    //    2) Before m_mutex was locked, or right here while it is not locked,
    //       all timers were canceled, causing m_tree[1] to point to an
    //       interval for which m_cache[interval] == Timer::s_none and this
    //       remains the case until we locked m_mutex in the next line.
    //    3) Before m_mutex was locked, or right here while it is not locked,
    //       all expired timers canceled and/or never existed (we got here
    //       because a new timer was added that expires sooner than the
    //       current timer). In that case there are no expired timers,
    //       so no timer->expire() will be called.

    m_mutex.lock();                            // Lock m_mutex again.                                   // ^
    // Because of the short unlock of m_mutex, m_tree[1] might have changed.                            // | m_mutex locked
    if (AI_UNLIKELY(m_tree[1] != interval))     // Was there a race? See 1) above.                      // |
      continue;                                 // Then try again.                                      // |
    next = m_cache[interval];                                                                           // |
    m_mutex.unlock();                                                                                   // v

    // update_current_timer should only be called when there is an expired timer,
    // or a new timer was just added that became the next timer to expire.
    // However, that timer could have been canceled in the meantime,
    // so it is possible that there is no timer left at all.
    if (AI_UNLIKELY(next == Timer::s_none))     // Are all timer canceled? See 2) above.
    {
      current_w.relock(m_current);              // Lock m_current again.
      current_w->expired_timer = nullptr;
      return false;
    }
    duration = next - now;
    if (duration.count() <= 0)                  // Did this timer already expire? See 3) above.
    {
      // *** THIS IS THE POINT AT WHICH IT IS CERTAIN THAT timer->expire() WILL BE CALLED.
      timer = queue_w->pop(m_mutex);            // This act of removing it from the queue makes it      // ^
                                                // impossible that it is canceled once the queue is     // |
                                                // unlocked again.                                      // |
      Dout(dc::notice|flush_cf, "Timer " << (void*)timer << " expired " <<                              // | m_mutex locked
          -std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count() << " ns ago.");       // | (by pop(m_mutex))
      // Update m_tree and m_cache.                                                                     // |
      increase_cache(interval, queue_w->next_expiration_point());                                       // |
      duration = m_cache[m_tree[1]] - now;      // Time till the next timer expires (if any;            // |
                                                // otherwise this is just very large).                  // |
      m_mutex.unlock();                                                                                 // v
      timer->expired_start();                   // *** STOP DESTRUCTION OF THE TIMER. This has to be
                                                // done before we unlock queue_w because once that is
                                                // unlocked a call to timer->stop() can return, and that
                                                // could be called from ~Timer().
      Dout(dc::notice, "Expired timer " << timer);
      current_w.relock(m_current);              // Lock m_current again.
      current_w->expired_timer = timer;         // Do the call back.
      return duration.count() <= 0;             // Return true if the next timer also has already expired.
    }
#ifdef CWDEBUG
    timer = queue_w->peek();
#endif
    break;
  }

  // Calculate the timespec at which the current timer will expire.
  struct itimerspec new_value;
  memset(&new_value.it_interval, 0, sizeof(struct timespec));
  // This rounds down since duration is positive.
  auto s = std::chrono::duration_cast<std::chrono::seconds>(duration);
  new_value.it_value.tv_sec = s.count();
  auto ns  = std::chrono::duration_cast<std::chrono::nanoseconds>(duration - s);
  new_value.it_value.tv_nsec = ns.count();

  // Update the POSIX timer.
  Dout(dc::notice|flush_cf, "Calling timer_settime() for " << new_value.it_value.tv_sec << " seconds and " << new_value.it_value.tv_nsec << " nanoseconds.");
  current_w.relock(m_current);                  // Lock m_current again.
  // Only set the POSIX timer again when it has to expire sooner, or has already expired.
  if (next < current_w->expire_point || m_POSIX_timer_expired.load(std::memory_order_relaxed))
  {
    sigprocmask(SIG_BLOCK, &m_timer_sigset, nullptr);
    m_POSIX_timer_expired.store(false, std::memory_order_relaxed);
    [[maybe_unused]] bool pending = timer_settime(current_w->posix_timer, 0, &new_value, nullptr) == 0;
    ASSERT(pending);
    current_w->expire_point = next;
    sigprocmask(SIG_UNBLOCK, &m_timer_sigset, nullptr);
    Dout(dc::notice|flush_cf, "Timer " << (void*)timer << " started.");
  }

  // We just set the POSIX timer, so obviously there is no expired timer.
  current_w->expired_timer = nullptr;
  return false;
}

RunningTimers::~RunningTimers()
{
  DoutEntering(dc::notice(m_queues.size() > 0), "RunningTimers::~RunningTimers() with m_queues.size() == " << m_queues.size());
  // Set all timers to 'not running', otherwise they call cancel() on us when they're being destructed.
  for (TimerQueueIndex interval = m_queues.ibegin(); interval != m_queues.iend(); ++interval)
    timer_queue_t::wat(m_queues[interval])->set_not_running();
}

} // namespace threadpool

namespace {
  SingletonInstance<threadpool::RunningTimers> dummy __attribute__ ((__unused__));
} // namespace
