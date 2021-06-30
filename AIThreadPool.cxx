/**
 * threadpool -- A thread pool with support for multiple queues and ultra fast timers.
 *
 * @file
 * @brief Implementation of AIThreadPool.
 *
 * @Copyright (C) 2017  Carlo Wood.
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
#include "AIThreadPool.h"
#include "RunningTimers.h"
#include "Timer.h"
#include "utils/Signals.h"
#include "utils/AIAlert.h"
#include "utils/macros.h"
#include "utils/debug_ostream_operators.h"
#ifdef CWDEBUG
#include <libcwd/type_info.h>
#include <iomanip>
#endif

using Timer = threadpool::Timer;
using RunningTimers = threadpool::RunningTimers;

//static
std::atomic<AIThreadPool*> AIThreadPool::s_instance = ATOMIC_VAR_INIT(nullptr);

//static
std::atomic_int AIThreadPool::s_idle_threads = ATOMIC_VAR_INIT(0);

//static
AIThreadPool::Action AIThreadPool::s_call_update_current_timer CWDEBUG_ONLY(("\"Timer\""));

struct TimerCopy
{
  std::function<void()> m_callback;
  TimerCopy(Timer& timer) : m_callback(std::move(timer.release_callback())) { }
  ~TimerCopy() { m_callback(); }
};

//static
void AIThreadPool::Worker::tmain(int const self)
{
#ifdef CWDEBUG
  {
    std::ostringstream thread_name;
    thread_name << "ThreadPool" << std::dec << std::setfill('0') << std::setw(2) << self;
    Debug(NAMESPACE_DEBUG::init_thread(thread_name.str()));
  }
#endif
  AIThreadPool& thread_pool{AIThreadPool::instance()};

  // Unblock the POSIX signals that are used by the timer.
  utils::Signal::unblock(RunningTimers::instance().get_timer_sigset());

  // Wait until we have at least one queue.
  int count = 0;
  while (thread_pool.queues_read_access()->size() == 0)
  {
    std::this_thread::sleep_for(std::chrono::microseconds(10));
    if (++count > 10000)
      DoutFatal(dc::fatal, "No thread pool queue found after 100 ms. Call thread_pool.new_queue() at least once immediately after creating the ThreadPool object!");
  }

  AIQueueHandle q;              // The current / next queue that this thread is processing.
  q.set_to_zero();              // Start with the highest priority queue (zero).
  std::function<bool()> task;   // The current / last task (moved out of the queue) that this thread is executing / executed.
  int duty = 0;                 // The number of required actions that this thread performed since it woke up from sem_wait().
#ifdef SPINSEMAPHORE_STATS
  bool new_thread = true;
#endif

  std::atomic_bool quit(false); // Keep this boolean outside of the Worker so it isn't necessary to lock m_workers every loop.

  // This must be a read lock. We are not allowed to write-lock m_workers because
  // at termination m_workers is read locked while joining threads and if that
  // happens before a thread reaches this point, it would deadlock.
  workers_t::rat(thread_pool.m_workers)->at(self).running(&quit);

  // The thread will keep running until Worker::quit() is called.
  while (!quit.load(std::memory_order_relaxed))
  {
    Dout(dc::threadpool, "Beginning of thread pool main loop (q = " << q << ')');

    // Handle Timers.
    while (s_call_update_current_timer.try_obtain(duty))
    {
      Dout(dc::action, "Took ownership of timer action.");
      Timer::time_point now = Timer::clock_type::now();

      // First check if there have any timers expired.
      Timer* expired_timer;
      bool another_timer_expired;
      {
        auto current_w{RunningTimers::instance().access_current()};
        another_timer_expired = RunningTimers::instance().update_current_timer(current_w, now);
        if (!(expired_timer = current_w->expired_timer))
        {
          // If no timer just expired then there can't be another already expired timer.
          ASSERT(!another_timer_expired);
          // There are no more expired timers left.
          // This thread just called timer_settime (if there is any timer left at all) and set current_w->debug_timer.
          // Other threads won't call update_current_timer anymore until that timer expired and new timer was added that expires sooner.
          break;
        }
      } // Do the callback with RunningTimers::m_current unlocked.
      if (another_timer_expired)
        call_update_current_timer();            // Keep calling update_current_timer until it returns nullptr.
      try
      {
        expired_timer->expire();
      }
      catch (AIAlert::Error const& error)
      {
        Dout(dc::warning, error);
      }
    }

    bool go_idle = false;
    { // Lock the queue for other consumer threads.
      auto queues_r = thread_pool.queues_read_access();
      // Obtain a reference to queue `q`.
      queues_container_t::value_type const& queue = (*queues_r)[q];
      bool empty = !queue.task_available(duty);
      if (!empty)
      {
        Dout(dc::action, "Took ownership of queue " << q << " action.");

        // Obtain and lock consumer access for this queue.
        auto access = queue.consumer_access();
#ifdef CWDEBUG
        // The number of messages in the queue.
        int length = access.length();
        // Only call queue.notify_one() when you just called queue_access.move_in(...).
        // For example, if the queue is full so you don't call move_in then ALSO don't call notify_one!
        ASSERT(length > 0);
#endif
        // Move one object from the queue to `task`.
        task = access.move_out();
      }
      else
      {
        // Note that at this point it is possible that queue.consumer_access().length() > 0 (aka, the queue is NOT empty);
        // if that is the case than another thread is between the lines 'bool empty = !queue.task_available(duty)'
        // and 'auto access = queue.consumer_access()'. I.e. it took ownership of the task but didn't extract it from
        // the queue yet.

        // Process lower priority queues if any.
        // We are not available anymore to work on lower priority queues: we're already going to work on them.
        // However, go idle if we're not allowed to process queues with a lower priority.
        if ((go_idle = queue.decrement_active_workers()))
          queue.increment_active_workers();             // Undo the above decrement.
        else if (!(go_idle = ++q == queues_r->iend()))  // If there is no lower priority queue left, then just go idle.
        {
          Dout(dc::threadpool, "Continuing with next queue.");
          continue;                                     // Otherwise, handle the lower priority queue.
        }
        if (go_idle)
        {
          // We're going idle and are available again for queues of any priority.
          // Increment the available workers on all higher priority queues again before going idle.
          while (q != queues_r->ibegin())
            (*queues_r)[--q].increment_active_workers();
        }
      } // Not empty - not idle - need to invoke the functor task().
    } // Unlock the queue.

    if (!go_idle)
    {
      Dout(dc::threadpool, "Not going idle.");

      bool active = true;
      AIQueueHandle next_q;

      while (active)
      {
        try
        {
          // ***************************************************
          active = task();   // Invoke the functor.            *
          // ***************************************************
          Dout(dc::threadpool, "task() returned " << active);
        }
        catch (AIAlert::Error const& error)
        {
          // We should never get here. This exception must be catch-ed sooner and be handled.
          DoutFatal(dc::core, error);
          active = false;
        }

        // Determine the next queue to handle: the highest priority queue that doesn't have all reserved threads idle.
        next_q = q;
        { // Get read access to AIThreadPool::m_queues.
          auto queues_r = thread_pool.queues_read_access();
          // See if any higher priority queues need our help.
          while (next_q != queues_r->ibegin())
          {
            // Obtain a reference to the queue with the next higher priority.
            queues_container_t::value_type const& queue = (*queues_r)[--next_q];

            // If the number of idle threads is greater or equal than the total
            // number of threads reserved for higher priority queues, then that
            // must mean that the higher priority queues are empty so that we
            // can stay on the current queue.
            //
            // Note that we're not locking s_idle_mutex because it doesn't matter
            // how precise the read value of s_idle_threads is here:
            // if the result of the comparison is false while it should have been
            // true then this thread simply will move to high priority queues,
            // find out that they are empty and quickly return to the lower
            // priority queue that it is on now. While, if the result of the
            // comparison is true while it should have been false, then this
            // thread will run a task of lower priority queue before returning
            // to the higher priority queue the next time.
            //
            // Nevertheless, this defines the canonical meaning of s_idle_threads:
            // It must be the number of threads that are readily available to start
            // working on higher priority queues; which in itself means "can be
            // woken up instantly by a call to AIThreadPool::notify_one()".
            if (s_idle_threads.load(std::memory_order_relaxed) >= queue.get_total_reserved_threads())
            {
              ++next_q;   // Stay on the last queue.
              break;
            }
            // Increment the available workers on this higher priority queue before checking it for new tasks.
            queue.increment_active_workers();
          }
          if (active)
          {
            queues_container_t::value_type const& queue = (*queues_r)[q];
            bool put_back;
            {
              auto pa = queue.producer_access();
              int length = pa.length();
              put_back = length < queue.capacity() && (next_q != q || length > 0);
              if (put_back)
              {
                // Put task() back into q.
                pa.move_in(std::move(task));
              }
            } // Unlock queue.
            if (put_back)
            {
              // Don't call required() because that also wakes up a thread
              // while this thread already goes to the top of the loop again.
              queue.still_required();
              break;
            }
            // Otherwise, call task() again.
          }
        }
      }
      q = next_q;
    }
    else // go_idle
    {
      Dout(dc::action(duty == 0), "Thread had nothing to do.");

      // Destruct the current task if any, to decrement the RefCount.
      task = nullptr;

      {
        std::vector<TimerCopy> timer_copies;
        {
          AIThreadPool::defered_tasks_queue_t::wat defered_tasks_queue_w(AIThreadPool::instance().m_defered_tasks_queue);
          if (AI_UNLIKELY(!defered_tasks_queue_w->empty()))
          {
            int count = std::min(defered_tasks_queue_w->size(), workers_t::rat(thread_pool.m_workers)->size());
            timer_copies.reserve(count);
            do
            {
              // Move tasks from m_defered_tasks_queue to the thread pool queue.
              Timer& timer = defered_tasks_queue_w->front();
              if (timer.stop())
              {
                timer_copies.emplace_back(timer);
                defered_tasks_queue_w->pop_front();
              }
              else if (timer.has_expired().is_true())
                defered_tasks_queue_w->pop_front();
            }
            while (--count > 0);
          }
        }
        // Now destruct timer_copies, which will 'expire' the copied Timer's.
      }

#ifdef SPINSEMAPHORE_STATS
      if (AI_UNLIKELY(new_thread))
      {
        new_thread = false;
        Action::s_new_thread_duty.fetch_add(duty, std::memory_order_relaxed);
      }
      else if (duty == 0)
        Action::s_woken_but_nothing_to_do.fetch_add(1, std::memory_order_relaxed);
      else
        Action::s_woken_duty.fetch_add(duty, std::memory_order_relaxed);
#endif
      // A thread that enters this block has nothing to do.
      s_idle_threads.fetch_add(1);
      Action::wait();
      s_idle_threads.fetch_sub(1);
#ifdef SPINSEMAPHORE_STATS
      Action::s_woken_up.fetch_add(1, std::memory_order_relaxed);
#endif
#ifdef CWDEBUG
      // Thread woke up, give its debug output a new color, if any.
      if (thread_pool.m_color2code_on)
      {
        int color = thread_pool.get_color();
        workers_t::rat(thread_pool.m_workers)->at(self).set_color(color);
        thread_pool.use_color(color);
      }
#endif
      // We just left sem_wait(); reset 'duty' to count how many tasks we perform
      // for this single wake-up. Calling try_obtain() with a duty larger than
      // zero will call sem_trywait in an attempt to decrease the semaphore
      // count alongside the Action::m_required atomic counter.
      duty = 0;
    }
  }

  Dout(dc::threadpool, "Thread terminated.");
}

void AIThreadPool::add_threads(workers_t::wat& workers_w, int n)
{
  DoutEntering(dc::threadpool, "add_threads(" << n << ")");
  {
    queues_t::wat queues_w(m_queues);
    for (auto&& queue : *queues_w)
      queue.available_workers_add(n);
  }
  int const current_number_of_threads = workers_w->size();
  for (int i = 0; i < n; ++i)
    workers_w->emplace_back(&Worker::tmain, current_number_of_threads + i);
}

// This function is called inside a criticial area of m_workers_r_to_w_mutex
// so we may convert the read lock to a write lock.
void AIThreadPool::remove_threads(workers_t::rat& workers_r, int n)
{
  if (n == 0)
    return;

  DoutEntering(dc::threadpool, "remove_threads(" << n << ")");
  {
    queues_t::wat queues_w(m_queues);
    for (auto&& queue : *queues_w)
      queue.available_workers_add(-n);
  }

  // Since we only have a read lock on `m_workers` we can only
  // get access to const Worker's; in order to allow us to manipulate
  // individual Worker objects anywhere all their members are mutable.
  // This means that we need another mechanism to protect them
  // from concurrent access: all functions calling any method of
  // Worker must be inside the critical area of some mutex.
  // Both, quit() and join() are ONLY called in this function;
  // and this function is only called while in the critical area
  // of m_workers_r_to_w_mutex; so we're OK.

  // Call quit() on the n last threads in the container.
  int const number_of_threads = workers_r->size();
  for (int i = 0; i < n; ++i)
    workers_r->at(number_of_threads - 1 - i).quit();
  // Wake up all threads, so the ones that need to quit can quit.
  Action remove_threads{CWDEBUG_ONLY("remove_threads")};
  remove_threads.wakeup_n(number_of_threads);
  // If the relaxed stores to the quit atomic_bool`s is very slow
  // then we might be calling join() on threads before they can see
  // the flag being set. This should not be a problem but theoretically
  // the store could be delayed forever, so to be formerly correct
  // lets flush all stores here- before calling join().
  std::atomic_thread_fence(std::memory_order_release);
  // Join the n last threads in the container.
  for (int i = 0; i < n; ++i)
  {
    // Worker threads need to take a read lock on m_workers, so it
    // would not be smart to have a write lock on that while trying
    // to join them.
    workers_r->back().join();
    // Finally, obtain a write lock and remove/destruct the Worker.
    workers_t::wat(workers_r)->pop_back();
  }
}

AIThreadPool::AIThreadPool(int number_of_threads, int max_number_of_threads) :
    m_constructor_id(aithreadid::none), m_max_number_of_threads(std::max(number_of_threads, max_number_of_threads)), m_pillaged(false)
{
  // Only here to record the id of the thread who constructed us.
  // Do not create a second AIThreadPool from another thread;
  // use AIThreadPool::instance() to access the thread pool created from main.
  assert(aithreadid::is_single_threaded(m_constructor_id));

  // Only construct ONE AIThreadPool, preferably somewhere at the beginning of tmain().
  // If you want more than one thread pool instance, don't. One is enough and much
  // better than having two or more.
  assert(s_instance == nullptr);

  // Allow access to the thread pool from everywhere without having to pass it around.
  s_instance = this;

  workers_t::wat workers_w(m_workers);
  assert(workers_w->empty());                    // Paranoia; even in the case of constructing a second AIThreadPool
                                                 // after destructing the first, this should be the case here.
  workers_w->reserve(m_max_number_of_threads);   // Attempt to avoid reallocating the vector in the future.
  add_threads(workers_w, number_of_threads);     // Create and run number_of_threads threads.

  // Set a default value.
  set_max_number_of_queues(8);
}

AIThreadPool::~AIThreadPool()
{
  DoutEntering(dc::threadpool, "AIThreadPool::~AIThreadPool()");

#ifdef SPINSEMAPHORE_STATS
#ifdef CWDEBUG
  Dout(dc::semaphorestats, "AIThreadPool semaphore stats:\n" << utils::print_using(&print_semaphore_stats_on));
#else
  std::cout << "AIThreadPool semaphore stats:\n" << utils::print_using(&print_semaphore_stats_on) << std::endl;
#endif
#endif

  // Construction and destruction is not thread-safe.
  assert(aithreadid::is_single_threaded(m_constructor_id));
  if (m_pillaged) return;                        // This instance was moved. We don't really exist.

  // Kill all threads.
  {
    std::lock_guard<std::mutex> lock(m_workers_r_to_w_mutex);
    workers_t::rat workers_r(m_workers);
    remove_threads(workers_r, workers_r->size());
  }

  // Allow construction of another AIThreadPool.
  s_instance = nullptr;
}

void AIThreadPool::change_number_of_threads_to(int requested_number_of_threads)
{
  if (requested_number_of_threads > m_max_number_of_threads)
  {
    Dout(dc::warning, "Increasing number of thread beyond the initially set maximum.");
    workers_t::wat workers_w(m_workers);
    // This might reallocate the vector and therefore move existing Workers,
    // but that should be ok while holding the write-lock... I think.
    workers_w->reserve(requested_number_of_threads);
    m_max_number_of_threads = requested_number_of_threads;
  }

  // This must be locked before locking m_workers.
  std::lock_guard<std::mutex> lock(m_workers_r_to_w_mutex);
  // Kill or add threads.
  workers_t::rat workers_r(m_workers);
  int const current_number_of_threads = workers_r->size();
  if (requested_number_of_threads < current_number_of_threads)
    remove_threads(workers_r, current_number_of_threads - requested_number_of_threads);
  else if (requested_number_of_threads > current_number_of_threads)
  {
    workers_t::wat workers_w(workers_r);
    add_threads(workers_w, requested_number_of_threads - current_number_of_threads);
  }
}

AIQueueHandle AIThreadPool::new_queue(int capacity, int reserved_threads)
{
  DoutEntering(dc::threadpool, "AIThreadPool::new_queue(" << capacity << ", " << reserved_threads << ")");
  queues_t::wat queues_w(m_queues);
  AIQueueHandle index(queues_w->size());
  int previous_reserved_threads = index.is_zero() ? 0 : (*queues_w)[index - 1].get_total_reserved_threads();
  queues_w->emplace_back(capacity, previous_reserved_threads, reserved_threads);
  Dout(dc::threadpool, "Returning index " << index << "; size is now " << queues_w->size() <<
      " for " << libcwd::type_info_of(*queues_w).demangled_name() << " at " << (void*)&*queues_w);
  return index;
}

void AIThreadPool::defer(AIQueueHandle queue_handle, uint8_t failure_count, std::function<void()> const& lambda)
{
  DoutEntering(dc::notice, "AIThreadPool::defer(" << queue_handle << ", " << (int)failure_count << ", lambda)");
  if (AI_UNLIKELY(failure_count >= threadpool::slow_down_intervals.size()))
    failure_count = threadpool::slow_down_intervals.size() - 1;

  Timer* new_timer;
  {
    defered_tasks_queue_t::wat defered_tasks_queue_w(m_defered_tasks_queue);
    defered_tasks_queue_w->emplace_back(lambda);
    new_timer = &defered_tasks_queue_w->back();
  }
  new_timer->start(threadpool::slow_down_intervals[failure_count]);
}

//static
utils::threading::SpinSemaphore AIThreadPool::Action::s_semaphore;

#ifdef SPINSEMAPHORE_STATS
//static
std::atomic_int AIThreadPool::Action::s_woken_up = ATOMIC_VAR_INIT(0);
std::atomic_int AIThreadPool::Action::s_woken_but_nothing_to_do = ATOMIC_VAR_INIT(0);
std::atomic_int AIThreadPool::Action::s_new_thread_duty = ATOMIC_VAR_INIT(0);
std::atomic_int AIThreadPool::Action::s_woken_duty = ATOMIC_VAR_INIT(0);
#endif

#if defined(CWDEBUG) && !defined(DOXYGEN)
NAMESPACE_DEBUG_CHANNELS_START
channel_ct threadpool("THREADPOOL");
channel_ct action("ACTION");            // Thread Pool Action's.
NAMESPACE_DEBUG_CHANNELS_END
#endif
