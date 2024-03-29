/**
 * threadpool -- A thread pool with support for multiple queues and ultra fast timers.
 *
 * @file
 * @brief Thread pool implementation.
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
 *
 * CHANGELOG
 *   and additional copyright holders.
 *
 *   29/04/2017
 *   - Initial version, written by Carlo Wood.
 */

#pragma once

#include "AIObjectQueue.h"
#include "AIQueueHandle.h"
#include "Timer.h"
#include "signal_safe_printf.h"
#include "threadsafe/AIReadWriteMutex.h"
#include "threadsafe/AIReadWriteSpinLock.h"
#include "threadsafe/threadsafe.h"
#include "utils/threading/SpinSemaphore.h"
#include "utils/threading/aithreadid.h"
#include "debug.h"
#include <thread>
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <deque>
#ifdef SPINSEMAPHORE_STATS
#include <iomanip>
#endif
#ifdef CWDEBUG
#include "utils/ColorPool.h"
#endif

#if defined(CWDEBUG) && !defined(DOXYGEN)
NAMESPACE_DEBUG_CHANNELS_START
extern channel_ct action;
extern channel_ct threadpool;
extern channel_ct threadpoolloop;
NAMESPACE_DEBUG_CHANNELS_END
#endif

/**
 * @brief The thread pool class.
 *
 * Only one AIThreadPool may exist at a time; and can subsequently be
 * accessed by a call to the static function AIThreadPool::instance().
 *
 * However, an AIThreadPool is not a singleton: it doesn't have
 * private constructors and it may not be constructed before main().
 * Also, it has a public move constructor (although only the thread
 * that created it may move it).
 * It is allowed to create an AIThreadPool and after some usage
 * destruct it; and then create a new one (this is not recommended).
 *
 * The <em>recommended</em> usage is:
 *
 * @code
 * int main()
 * {
 *   Debug(NAMESPACE_DEBUG::init());
 *
 *   AIThreadPool thread_pool;
 *   AIQueueHandle handler = thread_pool.new_queue(capacity);
 * ...
 *   // Use thread_pool or AIThreadPool::instance()
 * }
 * @endcode
 *
 * In order to <em>use</em> the thread pool one has to create one
 * or more task queues by calling AIThreadPool::new_queue. The
 * handle returned by that function can subsequently be used
 * to access the underlaying queue and move a <code>std::function<bool()></code>
 * object into it.
 *
 * For example,
 *
 * @code
 * ... (see above)
 *   // Create a new queue with a capacity of 32 and default priority.
 *   AIQueueHandle queue_handle = thread_pool.new_queue(32);
 *
 *   {
 *     // Get read access to AIThreadPool::m_queues.
 *     auto queues_access = thread_pool.queues_read_access();
 *     // Get a reference to one of the queues in m_queues.
 *     auto& queue = thread_pool.get_queue(queues_access, queue_handle);
 *     bool queue_full;
 *     {
 *       // Get producer accesses to this queue.
 *       auto queue_access = queue.producer_access();
 *       int length = queue_access.length();
 *       queue_full = length == queue.capacity();
 *       if (!queue_full)
 *       {
 *         // Place a lambda in the queue.
 *         queue_access.move_in([](){ std::cout << "Hello pool!\n"; return false; });
 *       }
 *     } // Release producer accesses, so another thread can write to this queue again.
 *     // This function must be called every time move_in was called
 *     // on a queue that was returned by thread_pool.get_queue.
 *     if (!queue_full) // Was move_in called?
 *       queue.notify_one();
 *   } // Release read access to AIThreadPool::m_queues so another thread can use AIThreadPool::new_queue again.
 * @endcode
 *
 * It is necessary to keep <code>AIThreadPool::m_queues</code> read locked @htmlonly&mdash;\endhtmlonly
 * by not destroying the object returned by @ref queues_read_access @htmlonly&mdash;\endhtmlonly
 * for as long as the write lock on the queue exists.
 * I.e., in the above code, the lifetime of <code>queues_access</code>
 * must exceed the lifetime of <code>queue_access</code>.
 *
 * This is necessary because as soon as that read lock is released
 * some other thread can call AIThreadPool::new_queue causing a
 * resize of <code>AIThreadPool::m_queues</code>, the underlaying vector of
 * <code>AIObjectQueue<std::function<bool()>></code> objects,
 * possibly moving the AIObjectQueue objects in memory, invalidating the
 * returned reference to the queue.
 *
 * @sa AIObjectQueue
 * @sa @link AIPackagedTask< R(Args...)> AIPackagedTask@endlink
 */
class AIThreadPool
{
 public:
  struct Worker;

 private:
  using worker_function_t = void (*)(int const);
  using worker_container_t = std::vector<Worker>;
  using workers_t = threadsafe::Unlocked<worker_container_t, threadsafe::policy::ReadWrite<AIReadWriteMutex>>;

  class Action
  {
    static utils::threading::SpinSemaphore s_semaphore;                 // Global semaphore to wake up all threads of the thread pool.
    std::atomic<uint64_t> m_skipped_required;                           // See constexpr below.
    static constexpr uint64_t required_mask = 0xffffffff;               // Mask for the bits that count the number of actions required for this specific Action.
    static constexpr uint64_t skipped_mask = 0xffffffff00000000UL;      // Mask for the bits that count the number of times wakeup_n(1) was skipped in required().
    static constexpr int skipped_shift = 32;
#ifdef CWDEBUG
    std::string m_quoted_name;
#endif

   public:
    void wakeup_n(uint32_t n)
    {
      s_semaphore.post(n);
    }

    static void wait()
    {
      // If this is the last thread that goes to sleep, it might be the last debug output for a while.
      // Therefore flush all debug output here.
      DoutEntering(dc::action, "Action::wait()");
      Debug(libcw_do.get_ostream()->flush());
      s_semaphore.wait();
      Dout(dc::action, "After calling sem_wait, " << s_semaphore);
    }

   public:
    Action(CWDEBUG_ONLY(std::string name)) : m_skipped_required(0) COMMA_CWDEBUG_ONLY(m_quoted_name(name)) { }
    Action(Action&& DEBUG_ONLY(rvalue)) : m_skipped_required(0) COMMA_CWDEBUG_ONLY(m_quoted_name(rvalue.m_quoted_name)) { ASSERT(rvalue.m_skipped_required == 0); }

    void still_required()
    {
      CWDEBUG_ONLY(uint64_t skipped_required =) m_skipped_required.fetch_add(1);
#ifdef CWDEBUG
      uint32_t prev_required = skipped_required & required_mask;
      Dout(dc::action, m_quoted_name << " Action::still_required(): required count " << prev_required << " --> " << prev_required + 1);
      //signal_safe_printf("\n>>>%s Action::required(): m_required %d --> %d<<<\n", m_quoted_name.c_str(), prev_required, prev_required + 1);
#endif
    }

    void required(uint32_t n)
    {
      uint64_t skipped_required = m_skipped_required.load(std::memory_order_relaxed);
      uint64_t new_skipped_required;
      do
      {
        // The number of times that wakeup_n(1) was skipped is less than or equal the number of times this action was required.
        ASSERT((skipped_required >> skipped_shift) <= (skipped_required & required_mask));
        // If m_skipped_required is zero then we will call wakeup_n(n) and increment required (the lower 32 bits) with n,
        // otherwise wakeup_n() will not be called and both, required and skipped (upper and lower 32 bits) are incremented with n.
        new_skipped_required = skipped_required == 0 ? skipped_required + n : skipped_required + (static_cast<uint64_t>(n) << skipped_shift) + n;
      }
      while (!m_skipped_required.compare_exchange_weak(skipped_required, new_skipped_required, std::memory_order_release));
      if (skipped_required == 0)
        wakeup_n(n);

#ifdef CWDEBUG
      uint32_t prev_required = skipped_required & required_mask;
      Dout(dc::action, m_quoted_name << " Action::required(" << n << "): required count " << prev_required << " --> " << prev_required + n);
      //signal_safe_printf("\n%s Action::required(): m_required %d --> %d\n", m_quoted_name.c_str(), prev_required, prev_required + n);
#endif
    }

    bool try_obtain(int& duty)
    {
      DoutEntering(dc::action|continued_cf, m_quoted_name << " Action::try_obtain(" << duty << ") : ");
      uint64_t skipped_required;
      while ((skipped_required = m_skipped_required.load(std::memory_order_relaxed)) > 0)       // If >0 then the required count must be larger than zero.
      {
        uint64_t new_skipped_required = (skipped_required & required_mask) - 1;                 // Decrement required count and reset skipped count.
        if (!m_skipped_required.compare_exchange_weak(skipped_required, new_skipped_required, std::memory_order_acquire))
          continue;
        uint32_t skipped = skipped_required >> skipped_shift;
        if (skipped > 0)
          wakeup_n(skipped);
        if (duty++)             // Is this our second or higher task?
        {
          // Try to avoid waking up a thread that subsequently will have nothing to do.
          CWDEBUG_ONLY(bool token_grab =) s_semaphore.try_wait();
#ifdef CWDEBUG
          if (!token_grab)
          {
            // It is possible that the semaphore count was zero if the task
            // that we just claimed also woke up a non-idle thread. That
            // thread now will have to go around the loop doing nothing.
            Dout(dc::action, "sem_trywait: count was already 0");
          }
          else
          {
            Dout(dc::action, "After calling sem_trywait, " << s_semaphore);
          }
#endif
        }
#ifdef CWDEBUG
        int prev_required = skipped_required & required_mask;
        ASSERT(prev_required > 0);
        Dout(dc::finish, "Required count " << prev_required << " --> " << prev_required - 1);
#endif
        return true;
      }
      Dout(dc::finish, "no");
      return false;
    }

#ifdef SPINSEMAPHORE_STATS
    static std::atomic_int s_woken_up;
    static std::atomic_int s_woken_but_nothing_to_do;
    static std::atomic_int s_new_thread_duty;
    static std::atomic_int s_woken_duty;
    static void print_semaphore_stats_on(std::ostream& os)
    {
      os << "Times that a thread woke up: " << s_woken_up << '\n';
      os << "Times that a woken thread had nothing to do: " << s_woken_but_nothing_to_do << std::fixed << std::setprecision(2) <<
        " (" << (100.0 * s_woken_but_nothing_to_do / s_woken_up) << "%).\n";
      os << "Total duty: " << (s_new_thread_duty + s_woken_duty) << " (average " << std::setprecision(2) << (1.0 * s_woken_duty / s_woken_up) << " per woken thread + " << s_new_thread_duty << " for new threads).\n";
      s_semaphore.print_stats_on(os);
    }
#endif
  };

  struct PriorityQueue : public AIObjectQueue<std::function<bool()>>
  {
    int const m_previous_total_reserved_threads;// The number of threads that are reserved for all higher priority queues together.
    int const m_total_reserved_threads;         // The number of threads that are reserved for this queue, plus all higher priority queues, together.
    std::atomic_int m_available_workers;        // The number of workers that may be added to work on queues of lower priority
                                                // (number of worker threads minus m_total_reserved_threads minus the number of
                                                //  worker threads that already work on lower priority queues).
                                                // The lowest priority queue must have a value of 0.
    // m_execute_task is mutable because it must be changed in const member functions:
    // The 'const' there means 'by multiple threads at the same time' (aka "read access").
    // The non-const member functions of Action are thread-safe.
    mutable Action m_execute_task;              // Keep track of number of actions required (number of tasks in the queue).

    PriorityQueue(int capacity, int previous_total_reserved_threads, int reserved_threads) :
        AIObjectQueue<std::function<bool()>>(capacity),
        m_previous_total_reserved_threads(previous_total_reserved_threads),
        m_total_reserved_threads(previous_total_reserved_threads + reserved_threads),
        m_available_workers(AIThreadPool::instance().number_of_workers() - m_total_reserved_threads)
        COMMA_CWDEBUG_ONLY(m_execute_task("\"Task queue #" + std::to_string(m_previous_total_reserved_threads) + "\""))
            // m_previous_total_reserved_threads happens to be equal to the queue number, provided each queue on reserves one thread :/.
      { }

    PriorityQueue(PriorityQueue&& rvalue) :
        AIObjectQueue<std::function<bool()>>(std::move(rvalue)),
        m_previous_total_reserved_threads(rvalue.m_previous_total_reserved_threads),
        m_total_reserved_threads(rvalue.m_total_reserved_threads),
        m_available_workers(rvalue.m_available_workers.load())
        COMMA_CWDEBUG_ONLY(m_execute_task(std::move(rvalue.m_execute_task)))
      { }

    void available_workers_add(int n) { m_available_workers.fetch_add(n, std::memory_order_relaxed); }
    // As with AIObjectQueue, the 'const' here means "safe under concurrent access".
    // Therefore the const cast is ok, because the atomic m_available_workers is thread-safe.
    // Return true when the number of workers active on this queue may no longer be reduced.
    bool decrement_active_workers() const { return const_cast<std::atomic_int&>(m_available_workers).fetch_sub(1, std::memory_order_relaxed) <= 0; }
    void increment_active_workers() const { const_cast<std::atomic_int&>(m_available_workers).fetch_add(1, std::memory_order_relaxed); }
    int get_total_reserved_threads() const { return m_total_reserved_threads; }

    /**
     * Wake up one thread to process the just added function, if needed.
     *
     * When the threads of the thread pool have nothing to do, they go to
     * sleep by waiting on a semaphore. Call this function every time a new
     * message was added to a queue in order to make sure that there is a
     * thread that will handle it.
     *
     * This function is const because it is OK if multiple threads call it at
     * the same time and therefore accessed as part of getting a "read" lock,
     * which only gives const access.
     */
    void notify_one() const
    {
      m_execute_task.required(1);
    }

    void still_required() const
    {
      m_execute_task.still_required();
    }

    /**
     * If a task is available then take ownership.
     *
     * If this function returns true then the current thread is responsible
     * for executing one task from the queue.
     *
     * This function is const because it is OK if multiple threads call it at
     * the same time and therefore accessed as part of getting a "read" lock,
     * which only gives const access.
     */
    bool task_available(int& duty) const
    {
      return m_execute_task.try_obtain(duty);
    }
  };

  // The life cycle of a Quit object:
  // - There is one Quit object per Worker and thus per (thread pool) thread.
  // - The Quit object is created with quit_ptr == nullptr before the thread is created.
  //   From this moment on forward it is possible that quit() is called.
  // - Once the thread is initialized and the std::atomic_bool exists, Quit::running is called.
  // - The thread never exits (and thus quit_ptr stays valid) until Quit::quit() was called.
  // Hence,
  // state 1: quit_ptr == nullptr and quit_called == false.
  // state 2: quit_ptr == adress of valid std::atomic_bool which is false and quit_called == false.
  // state 3: quit_ptr != nullptr (nl. address of valid std::atomic_bool which is true) and quit_called == true.
  // state 4: quit_ptr != nullptr but invalid and quit_called == true.
  // state 5: quit_ptr == nullptr and quit_called == true.
  //
  // 1 -(running called)-> 2 -(quit called)-> 3 -(thread exited)-> 4.
  // or
  // 1 -(quit called)-> 5 -(running called)-> 3 -(thread exited)-> 4.
  //
  // cleanly_terminated() is for debugging purposes and tests that both running() and quit() were called.
  struct Quit
  {
    bool m_quit_called;                 // Set after calling quit().
    std::atomic_bool* m_quit_ptr;       // Only valid while m_quit_called is false (or while we're still inside Worker::tmain()).

    Quit() : m_quit_called(false), m_quit_ptr(nullptr) { }

    bool cleanly_terminated() const { return m_quit_ptr && m_quit_called; }
    bool quit_called() const { return m_quit_called; }

    void running(std::atomic_bool* quit)
    {
      m_quit_ptr = quit;
      *m_quit_ptr = m_quit_called;
    }

    void quit()
    {
      if (m_quit_ptr)
        m_quit_ptr->store(true, std::memory_order_relaxed);
      // From this point on we might leave Worker::tmain() which makes m_quit invalid.
      m_quit_called = true;
    }
  };

 public:        // Because Timer needs to add Worker::tmain as a friend.
  struct Worker
  {
    using quit_t = threadsafe::Unlocked<Quit, threadsafe::policy::Primitive<std::mutex>>;
    // A Worker is only const when we access it from a const worker_container_t.
    // However, the (read) lock on the worker_container_t only protects the internals
    // of the container, not its elements. So, all elements are considered mutable.
    mutable quit_t m_quit;              // Create m_quit before m_thread.
    mutable std::thread m_thread;
#ifdef CWDEBUG
    std::thread::native_handle_type m_thread_id;

    void set_color(int color) const
    {
      // Only set the color when the color functions have already been initialized...
      if (AI_LIKELY(AIThreadPool::instance().m_color2code_on))
      {
        std::string color_on_str = AIThreadPool::instance().m_color2code_on(color);
        std::string color_off_str = AIThreadPool::instance().m_color2code_off(color);
        libcwd::libcw_do.color_on().assign(color_on_str.c_str(), color_on_str.size());
        libcwd::libcw_do.color_off().assign(color_off_str.c_str(), color_off_str.size());
      }
    }
#endif

    // Construct a new Worker; do not associate it with a running thread yet.
    // A write lock on m_workers is required before calling this constructor;
    // that then blocks the thread from accessing m_quit until that lock is released
    // so that we have time to move the Worker in place (using emplace_back()).
    Worker(worker_function_t worker_function, int self) :
        m_thread(std::bind(worker_function, self)) COMMA_CWDEBUG_ONLY(m_thread_id(m_thread.native_handle())) { }

    // The move constructor can only be called as a result of a reallocation, as a result
    // of a size increase of the std::vector<Worker> (because Worker`s are put into it with
    // emplace_back(), Worker is not copyable and we never move a Worker out of the vector).
    // That means that at the moment the move constuctor is called we have the exclusive
    // write lock on the vector and therefore no other thread can access this Worker.
    // It is therefore safe to simply copy m_quit.
    Worker(Worker&& rvalue) : m_thread(std::move(rvalue.m_thread)) COMMA_CWDEBUG_ONLY(m_thread_id(m_thread.native_handle()))
    {
      quit_t::wat quit_w(m_quit);
      quit_t::wat rvalue_quit_w(rvalue.m_quit);
      quit_w->m_quit_called = rvalue_quit_w->m_quit_called;
      quit_w->m_quit_ptr = rvalue_quit_w->m_quit_ptr;
      rvalue_quit_w->m_quit_called = false;
      rvalue_quit_w->m_quit_ptr = nullptr;
    }

    // Destructor.
    ~Worker()
    {
      DoutEntering(dc::threadpool, "~Worker() [" << (void*)this << "][" << std::hex << m_thread_id << "]");
      // Call quit() before destructing a Worker.
      ASSERT(quit_t::rat(m_quit)->cleanly_terminated());
      // Call join() before destructing a Worker.
      ASSERT(!m_thread.joinable());
    }

   public:
    // Set thread to running.
    void running(std::atomic_bool* quit) const { quit_t::wat(m_quit)->running(quit); }

    // Inform the thread that we want it to stop running.
    void quit() const
    {
      Dout(dc::threadpool, "Calling Worker::quit() [" << (void*)this << "]");
      quit_t::wat(m_quit)->quit();
    }

    // Wait for the thread to have exited.
    void join() const
    {
      // Call quit() before calling join().
      ASSERT(quit_t::rat(m_quit)->quit_called());
      // Only call join() once (this should be true for all Worker`s that were created and not moved).
      ASSERT(m_thread.joinable());
      m_thread.join();
    }

    // The main function for each of the worker threads.
    static void tmain(int const self);

    // Called from worker thread.
    static int get_handle();
  };

 private:
  // Number of idle workers.
  static std::atomic_int s_idle_threads;

  // Number of times that update_RunningTimers::update_current_timer() needs to be called.
  static Action s_call_update_current_timer;

  // Define a read/write lock protected container with all Worker`s.
  //
  // Obtaining and releasing a read lock by constructing and destructing a workers_t::rat object,
  // takes 178 ns (without optimization) / 117 ns (with optimization) [measured with microbench
  // on a 3.6GHz AMD FX(tm)-8150 core].
  //
  // [ Note that construcing and destructing a workers_t::wat object, for write access, takes 174 ns
  //   respectively 121 ns; although speed is not relevant in that case. ]
  //
  workers_t m_workers;

  // Mutex to protect critical areas in which conversion from read to write lock is necessary
  // (allowing concurrent conversion attempts can cause an exception to be thrown that we
  // can't recover from in our case).
  std::mutex m_workers_r_to_w_mutex;

  // Add new threads to the already write locked m_workers container.
  void add_threads(workers_t::wat& workers_w, int n);

  // Remove threads from the already read locked m_workers container.
  void remove_threads(workers_t::rat& workers_r, int n);

 public:
  /// The container type in which the queues are stored.
  using queues_container_t = utils::Vector<PriorityQueue, AIQueueHandle>;

 private:
  static std::atomic<AIThreadPool*> s_instance;         // The only instance of AIThreadPool that should exist at a time.
  // m_queues is seldom write locked and very often read locked, so use AIReadWriteSpinLock.
  using queues_t = threadsafe::Unlocked<queues_container_t, threadsafe::policy::ReadWrite<AIReadWriteSpinLock>>;
  queues_t m_queues;                                    // Vector of PriorityQueue`s.
  std::thread::id m_constructor_id;                     // Thread id of the thread that created and/or moved AIThreadPool.
  int m_max_number_of_threads;                          // Current capacity of m_workers.
  bool m_pillaged;                                      // If true, this object was moved and the destructor should do nothing.
  using defered_tasks_queue_t = threadsafe::Unlocked<std::deque<threadpool::Timer>, threadsafe::policy::Primitive<std::mutex>>;
  defered_tasks_queue_t m_defered_tasks_queue;
#ifdef CWDEBUG
  static constexpr int g_number_of_colors = 30;         // We have 32 colors, but have to reserve two colors for the main thread and EventLoopThread.
  using color_pool_type = threadsafe::Unlocked<utils::ColorPool<g_number_of_colors>, threadsafe::policy::Primitive<std::mutex>>;
  color_pool_type m_color_pool;
  std::function<std::string(int)> m_color2code_on;      // Function that converts a color in the range [0, number_of_colors> to a terminal escape string to turn that color on.
  std::function<std::string(int)> m_color2code_off;     // Function that converts a color in the range [0, number_of_colors> to a terminal escape string to turn that color off.
#endif

 public:
  /**
   * Construct an AIThreadPool with @a number_of_threads number of threads.
   *
   * @param number_of_threads The initial number of worker threads in this pool.
   * @param max_number_of_threads The largest value that you expect to pass to @ref change_number_of_threads_to during the execution of the program.
   */
  AIThreadPool(int number_of_threads = std::thread::hardware_concurrency() - 2, int max_number_of_threads = std::thread::hardware_concurrency());

  /// Copying is not possible.
  AIThreadPool(AIThreadPool const&) = delete;

  /**
   * Move constructor.
   *
   * The move constructor is not thread-safe. Usage is only intended to be used
   * directly after creation of the AIThreadPool, by the thread that created it,
   * to move it into place, if needed.
   */
  AIThreadPool(AIThreadPool&& rvalue) :
      m_constructor_id(rvalue.m_constructor_id),
      m_max_number_of_threads(rvalue.m_max_number_of_threads),
      m_pillaged(false)
  {
    // The move constructor is not thread-safe. Only the thread that constructed us may move us.
    assert(aithreadid::is_single_threaded(m_constructor_id));
    // Cannot move ThreadPool while there are workers.
    assert(number_of_workers() == 0);
    rvalue.m_pillaged = true;
    // Move the queues_container_t.
    *queues_t::wat(m_queues) = std::move(*queues_t::wat(rvalue.m_queues));
    // Once we're done with constructing this object, other threads (that likely still have to be started,
    // but that is not enforced) should be allowed to call AIThreadPool::instance(). In order to enforce
    // that all initialization of this object will be visible to those other threads, we need to prohibit
    // that stores done before this point arrive in such threads reordered to after this store.
    s_instance.store(this, std::memory_order_release);
  }

  /// Destructor terminates all threads and joins them.
  ~AIThreadPool();

  //------------------------------------------------------------------------
  // Threads management.

  /**
   * Change the number of threads.
   *
   * You bought more cores and updated it while running your program.
   *
   * @param number_of_threads The new number of threads.
   */
  void change_number_of_threads_to(int number_of_threads);

  /// Return the number of worker threads.
  int number_of_workers() const { return workers_t::crat(m_workers)->size(); }

  //------------------------------------------------------------------------
  // Queue management.

  /// Lock m_queues and get access (return value is to be passed to @ref get_queue).
  AIThreadPool::queues_t::rat queues_read_access() { return static_cast<AIThreadPool::queues_t::rat>(m_queues); }

  /**
   * Create a new queue.
   *
   * @param capacity The capacity of the new queue.
   * @param reserved_threads The number of threads that are rather idle than work on lower priority queues.
   *
   * The new queue is of a lower priority than all previously created queues.
   * The priority is determined by two things: the order in which queues are
   * searched for new tasks and the fact that @a reserved_threads threads
   * won't work on tasks of a lower priority (if any). Hence, passing a value
   * of zero to @a reserved_threads only has influence on the order in which
   * the tasks are processed, while using a larger value reduces the number
   * of threads that will work on lower priority tasks.
   *
   * @returns A handle for the new queue.
   */
  AIQueueHandle new_queue(int capacity, int reserved_threads = 1);

  /**
   * Return a reference to the queue that belongs to @a queue_handle.
   *
   * The returned pointer is only valid until a new queue is requested, which
   * is blocked only as long as @a queues_r isn't destructed: keep the read-access
   * object around until the returned reference is no longer used.
   *
   * Note that despite using a Read Access Type (rat) this function returns
   * a non-const reference! The reasoning is that "read access" here should be
   * interpreted as "may be accessed by an arbitrary number of threads at the
   * same time".
   *
   * @param queues_r The read-lock object as returned by @ref queues_read_access.
   * @param queue_handle An AIQueueHandle as returned by @ref new_queue.
   *
   * @returns A reference to AIThreadPool::PriorityQueue.
   */
  queues_container_t::value_type const& get_queue(queues_t::rat& queues_r, AIQueueHandle queue_handle) { return queues_r->at(queue_handle); }

  /**
   * Change the maximum number of thread pool task queues.
   *
   * This member function must be called immediately after constructing the AIThreadPool.
   * The default is 8.
   */
  void set_max_number_of_queues(size_t max_number_of_queues) { queues_t::wat(m_queues)->reserve(max_number_of_queues); }

  /**
   * Cause a call to RunningTimers::update_current_timer() (possibly by another thread).
   *
   * Called from the timer signal handler.
   */
  static void call_update_current_timer()
  {
    //write(1, "\nCalling s_call_update_current_timer.required()\n", 48);
    s_call_update_current_timer.required(1);
  }

  // Called when handler was full. Executing lambda should recover the delay and continue possibly halted tasks.
  void defer(AIQueueHandle queue_handle, uint8_t failure_count, std::function<void()> const& lambda);

#ifdef CWDEBUG
  // Color management.
  void set_color_functions(std::function<std::string(int)> color2code_on, std::function<std::string(int)> color2code_off = [](int) -> std::string { return "\e[0m"; })
  {
    m_color2code_on = color2code_on;
    m_color2code_off = color2code_off;
  }

  // Called by threadpool code.
  void use_color(int color)
  {
    color_pool_type::wat color_pool_w(m_color_pool);
    color_pool_w->use_color(color);
  }

  // Called by threadpool code.
  int get_and_use_color()
  {
    color_pool_type::wat color_pool_w(m_color_pool);
    return color_pool_w->get_and_use_color();
  }
#endif

  //------------------------------------------------------------------------

  /**
   * Obtain a reference to the thread pool.
   *
   * Use this in threads that did not create the thread pool.
   */
  static AIThreadPool& instance()
  {
    // Construct an AIThreadPool somewhere, preferably at the beginning of main().
    ASSERT(s_instance.load(std::memory_order_relaxed) != nullptr);
    // In order to see the full construction of the AIThreadPool instance, we need to prohibit
    // reads done after this point from being reordered before this load, because that could
    // potentially still read memory locations in the object from before when it was constructed.
    return *s_instance.load(std::memory_order_acquire);
  }

#ifdef SPINSEMAPHORE_STATS
  static void print_semaphore_stats_on(std::ostream& os)
  {
    Action::print_semaphore_stats_on(os);
  }
#endif
};
