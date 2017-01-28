/*
    csound_threaded.hpp:

    Copyright (C) 2017 Michael Gogins

    This file is part of Csound.

    The Csound Library is free software; you can redistribute it
    and/or modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    Csound is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with Csound; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
    02111-1307 USA

    As a special exception, if other files instantiate templates or
    use macros or inline functions from this file, this file does not
    by itself cause the resulting executable or library to be covered
    by the GNU Lesser General Public License. This exception does not
    however invalidate any other reasons why the library or executable
    file might be covered by the GNU Lesser General Public License.
*/

#ifndef __CSOUND_THREADED_HPP__
#define __CSOUND_THREADED_HPP__

#if __cplusplus <= 199711L
  #error To use csound_threaded.hpp you need at least a C++11 compliant compiler.
#endif

#ifdef SWIG
%module csnd6
%{
#include "csound.hpp"
%}
#else
#include "csound.hpp"
#ifdef __BUILDING_CSOUND_INTERFACES
#endif

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>

/**
 * A thread-safe queue, or first-in first-out (FIFO) queue, implemented using 
 * only the standard C++11 library. The Data should be a simple type, such as 
 * a pointer.
 */
template<typename Data>
class concurrent_queue
{
private:
    std::queue<Data> queue_;
    std::mutex mutex_;
    std::condition_variable condition_variable_;
public:
    void push(Data const& data)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        queue_.push(data);
        lock.unlock();
        condition_variable_.notify_one();
    }
    bool empty() const
    {
        std::unique_lock<std::mutex> lock(mutex_);
        return queue_.empty();
    }
    bool try_pop(Data& popped_value)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if(queue_.empty()) {
            return false;
        }
        popped_value = queue_.front();
        queue_.pop();
        return true;
    }
    void wait_and_pop(Data& popped_value)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        while(queue_.empty()) {
            condition_variable_.wait(lock);
        }
        popped_value = queue_.front();
        queue_.pop();
    }
};

/**
 * Abstract base class for Csound events to be enqueued
 * for performance.
 */
struct CsoundEvent
{
    virtual ~CsoundEvent() {};
    /**
     * Dispatches the event to Csound during performance.
     */
    virtual int operator ()(CSOUND *csound) = 0;
};

/**
 * Specialization of CsoundEvent for low-level 
 * score events with raw pfields.
 */
struct CsoundScoreEvent : public CsoundEvent
{
    char opcode;
    std::vector<MYFLT> pfields;
    CsoundScoreEvent(char opcode_, const MYFLT *pfields_, long pfield_count) 
    {
        opcode = opcode_;
        for (long i = 0; i < pfield_count; i++) {
            pfields.push_back(pfields_[i]);
        }
    }
    virtual int operator ()(CSOUND *csound) {
        return csoundScoreEvent(csound, opcode, pfields.data(), pfields.size());
    }
};

/**
 * Specialization of CsoundEvent for high-level textual score 
 * events, fragments of scores, or entire scores.
 */
struct CsoundTextEvent : public CsoundEvent
{
    std::string events;
    CsoundTextEvent(const char *text) 
    {
        events = text;
    }
    virtual int operator ()(CSOUND *csound) {
        return csoundReadScore(csound, events.data());
    }
};

/**
 * This class provides a multi-threaded C++ interface to the "C" Csound API. 
 * The interface is identical to the C++ interface of the Csound class in 
 * csound.hpp; however, the ::Perform() function runs in a separate thread of 
 * execution that is fed by a thread-safe FIFO of messages from ::ScoreEvent, 
 * ::InputMessage, and ::ReadScore. 
 *
 * This is a header-file-only facility. The multi-threaded features of this 
 * class are minimalistic, but seem sufficient for most purposes. There are 
 * no external dependences apart from Csound and the standard C++ library.
 */
class PUBLIC CsoundThreaded : public Csound
{
protected:
    std::thread *performance_thread;
    std::atomic<bool> keep_running;
    void (*kperiod_callback)(CSOUND *, void *);
    void *kperiod_callback_user_data;
    concurrent_queue<CsoundEvent *> input_queue;
    void ClearQueue()
    {
        CsoundEvent *event = 0;
        while (input_queue.try_pop(event)) {
            delete event;
        }
    }
public:
    CsoundThreaded() : Csound(), performance_thread(0), keep_running(false), kperiod_callback(nullptr), kperiod_callback_user_data(nullptr) {};
    CsoundThreaded(CSOUND *csound_) : Csound(csound_), performance_thread(0), keep_running(false), kperiod_callback(nullptr), kperiod_callback_user_data(nullptr) {};
    CsoundThreaded(void *host_data) : Csound(host_data), performance_thread(0), keep_running(false), kperiod_callback(nullptr), kperiod_callback_user_data(nullptr) {}; 
    virtual ~CsoundThreaded() 
    {
        Stop();
        Join();
        ClearQueue();
    }
    virtual void SetKperiodCallback(void (*kperiod_callback_)(CSOUND *, void *), void *kperiod_callback_user_data_)
    {
        kperiod_callback = kperiod_callback_;
        kperiod_callback_user_data = kperiod_callback_user_data_;
    }
    virtual int PerformRoutine(bool reset)
    {
        Message("Began threaded Perform()...\n");
        keep_running = true;
        int result = 0;
        while (true) {
            if (keep_running == false) {
                break;
            }
            CsoundEvent *event = 0;
            while (input_queue.try_pop(event)) {
                (*event)(csound);
            }                
            if (kperiod_callback != nullptr) {
                kperiod_callback(csound, kperiod_callback_user_data);
            }
            result = Csound::PerformKsmps();
            if (result != 0) {
                break;
            }
        }
        keep_running = false;
        ClearQueue();
        if (reset) {
            Message("Ended Perform() thread, cleaning up now.\n");
            result = Cleanup();
            Reset();
        } else {
            Message("Ended Perform() thread.\n");
        }
        return result;
    }
    /**
     * Overrides Csound::Perform to run in a separate thread of execution. 
     * The granularity of time is one kperiod. Returns the native handle 
     * of the performance thread. If a kperiod callback has been set, 
     * it is called with the CSOUND object and any user data on every
     * kperiod. If the reset parameter is false, then csoundCleanup and 
     * csoundReset are not called at the end of the performance.
     */
    virtual int Perform(bool reset = true) 
    {
        Stop();
        Join();
        ClearQueue();
        performance_thread = new std::thread(&CsoundThreaded::PerformRoutine, this, reset);
        return performance_thread->native_handle();
    }
    /** 
     * Enqueues a low-level score event with raw pfields for dispatch from 
     * the performance thread routine.
     */
    virtual int ScoreEvent(char opcode, const MYFLT *pfields, long pfield_count) 
    {
        int result = 0;
        CsoundScoreEvent *event = new CsoundScoreEvent(opcode, pfields, pfield_count);
        input_queue.push(event);
        return result;
    }
    /** 
     * Enqueues a textual score event or events for dispatch from the 
     * performance thread routine.
     */
    virtual void InputMessage(const char *message)
    {
        CsoundTextEvent *event = new CsoundTextEvent(message);
        input_queue.push(event);
    }
    /** 
     * Enqueues a textual score event, score fragment, or entire score for 
     * dispatch from the performance thread routine.
     */
    virtual int ReadScore(const char *score)
    {
        int result = 0;
        CsoundTextEvent *event = new CsoundTextEvent(score);
        input_queue.push(event);
        return result;
    }    
    /** 
     * Signals the performance thread routine to stop and return.
     */
    virtual void Stop() 
    {
        keep_running = false;
        Csound::Stop();
    }
    /**
     * Causes the calling thread to wait for the end of the performance
     * thread routine.
     */
    virtual void Join() 
    {
        if (performance_thread != nullptr) {
            performance_thread->join();
            performance_thread = nullptr;
        }
    }
    /**
     * Returns whether or not the performance thread routine is running.
     */
    virtual bool IsPlaying() const 
    {
       return keep_running;
    }
};

#endif  // __cplusplus

#endif  // __CSOUND_HPP__