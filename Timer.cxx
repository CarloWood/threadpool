/**
 * threadpool -- A thread pool with support for multiple queues and ultra fast timers.
 *
 * @file
 * @brief Implementation of Timer.
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
#include "debug.h"
#include "Timer.h"
#include "RunningTimers.h"
#include <cassert>

namespace threadpool {

//static
Timer::time_point constexpr threadpool::TimerStart::s_none;

#if CW_DEBUG
//static
bool TimerStart::s_interval_constructed = false;
#endif

void TimerStart::start(Interval interval, std::function<void()> call_back)
{
  DoutEntering(dc::timer(mDebug), "Timer::start(" << interval << ", call_back) [" << this << "]");
  Timer* self = static_cast<Timer*>(this);
  // Call stop() first.
  ASSERT(self->m_handle.can_expire().is_false());
  self->m_call_back = call_back;
  // In case this object is being reused.
  self->m_handle.set_not_running();
  RunningTimers::instance().push(interval, self, self->m_expiration_point);
}

void TimerStart::start(Interval interval)
{
  DoutEntering(dc::timer(mDebug), "Timer::start(" << interval << ") [" << this << "]");
  Timer* self = static_cast<Timer*>(this);
  // Call stop() first.
  ASSERT(self->m_handle.can_expire().is_false());
  // Only use this on Timer objects that were constructed with a call back function.
  ASSERT(self->m_call_back);
  // In case this object is being reused.
  self->m_handle.set_not_running();
  RunningTimers::instance().push(interval, self, self->m_expiration_point);
}

#ifdef DEBUG_SPECIFY_NOW
// Deprecated. Debug only.
void TimerStart::start(Interval interval, std::function<void()> call_back, time_point now)
{
  DoutEntering(dc::timer(mDebug), "Timer::start(" << interval << ", call_back, " << now << ") [" << this << "]");
  Timer* self = static_cast<Timer*>(this);
  // Call stop() first.
  ASSERT(self->m_handle.can_expire().is_false());
  self->m_expiration_point = now + interval.duration();
  self->m_call_back = call_back;
  // In case this object is being reused.
  self->m_handle.set_not_running();
  RunningTimers::instance().push(interval.index(), self);
}

// Deprecated. Debug only.
void TimerStart::start(Interval interval, time_point now)
{
  DoutEntering(dc::timer(mDebug), "Timer::start(" << interval << ", " << now << ") [" << this << "]");
  Timer* self = static_cast<Timer*>(this);
  // Call stop() first.
  ASSERT(self->m_handle.can_expire().is_false());
  // Only use this on Timer objects that were constructed with a call back function.
  ASSERT(self->m_call_back);
  self->m_expiration_point = now + interval.duration();
  // In case this object is being reused.
  self->m_handle.set_not_running();
  RunningTimers::instance().push(interval.index(), self);
}
#endif

bool Timer::stop()
{
  DoutEntering(dc::timer(mDebug), "Timer::stop() [" << this << "]");
  // If do_call_cancel returns true then the timer was still running,
  // which means that m_call_back() wasn't called yet, and now will never be called.
  if (m_handle.do_call_cancel())
  {
    // This call to cancel may fail in that expire() is still called,
    // but that won't call m_call_back() anymore because we called
    // do_call_cancel(), above.
    RunningTimers::instance().cancel(m_handle);
  }
  return m_handle.stop_called_first();
}

void Timer::set_not_running()
{
  m_handle.set_not_running();
}

namespace detail {

void Indexes::add(Timer::time_point::rep period, Index* index)
{
  //DoutEntering(dc::notice, "Indexes::add(" << period << ", ...)");

  // Until all static objects are constructed, the index of an interval
  // can not be determined. Creating a Timer::Interval before reaching
  // main() is therefore doomed to have an incorrect 'index' field.
  //
  // The correct way to use Interval's by either instantiating global
  // variables of type Interval<count, Unit> and use those by name, or
  // simply pass them as a temporary to Timer::start directly.
  //
  // For example:
  //
  // threadpool::Interval<10, milliseconds> interval_10ms;
  //
  // And then use
  //
  //   timer.start(interval_10ms, ...);
  //
  // Or use it directly
  //
  //   timer.start(threadpool::Interval<10, milliseconds>(), ...);
  //
  // Do not contruct a Timer::Interval object before reaching main().
  ASSERT(!Timer::s_interval_constructed);

  m_intervals.resize(0);
  m_map.emplace(period, index);
  int in = -1;
  Timer::time_point::rep last = 0;
  for (auto i = m_map.begin(); i != m_map.end(); ++i)
  {
    if (i->first > last)
    {
      ++in;
      m_intervals.push_back(i->first);
    }
    i->second->m_index = in;
    last = i->first;
  }
  RunningTimers::instantiate().initialize(m_intervals.size());
}

} //namespace detail

namespace {
SingletonInstance<detail::Indexes> dummy __attribute__ ((__unused__));
} // namespace

// When the threadpool queue is full, we slow down the parent task
// by, initially 125 microseconds and defer the task that is being
// added by the same amount. If after that time the queue is still
// full the delay time is doubled every time, till a maximum of 256 ms.
std::array<threadpool::NonTemplateInterval, number_of_slow_down_intervals> slow_down_intervals = {
  Interval<125, std::chrono::microseconds>(),
  Interval<250, std::chrono::microseconds>(),
  Interval<500, std::chrono::microseconds>(),
  Interval<1, std::chrono::milliseconds>(),
  Interval<2, std::chrono::milliseconds>(),
  Interval<4, std::chrono::milliseconds>(),
  Interval<8, std::chrono::milliseconds>(),
  Interval<16, std::chrono::milliseconds>(),
  Interval<32, std::chrono::milliseconds>(),
  Interval<64, std::chrono::milliseconds>(),
  Interval<128, std::chrono::milliseconds>(),
  Interval<256, std::chrono::milliseconds>()
};

} // namespace threadpool

#if defined(CWDEBUG) && !defined(DOXYGEN)
NAMESPACE_DEBUG_CHANNELS_START
channel_ct timer("TIMER");
channel_ct timers("TIMERS");
NAMESPACE_DEBUG_CHANNELS_END
#endif
