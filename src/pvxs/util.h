/**
 * Copyright - See the COPYRIGHT that is included with this distribution.
 * pvxs is distributed subject to a Software License Agreement found
 * in file LICENSE that is included with this distribution.
 */

#ifndef PVXS_UTIL_H
#define PVXS_UTIL_H

#include <map>
#include <array>
#include <deque>
#include <functional>
#include <ostream>
#include <type_traits>
#include <stdexcept>

#include <osiSock.h>
#include <epicsEvent.h>
#include <epicsMutex.h>
#include <epicsGuard.h>

#include <pvxs/version.h>

namespace pvxs {

namespace detail {
// ref. wrapper to mark string for escaping
class Escaper
{
    const char* val;
    size_t count;
    friend
    PVXS_API
    std::ostream& operator<<(std::ostream& strm, const Escaper& esc);
public:
    PVXS_API explicit Escaper(const char* v);
    constexpr explicit Escaper(const char* v, size_t l) :val(v),count(l) {}
};

PVXS_API
std::ostream& operator<<(std::ostream& strm, const Escaper& esc);

} // namespace detail

//! Print string to output stream with non-printable characters escaped.
//!
//! Outputs (almost) C-style escapes.
//! Prefers short escapes for newline, tab, quote, etc ("\\n").
//! Falls back to hex escape (eg. "\xab").
//!
//! Unlike C, hex escapes are always 2 chars.  eg. the output "\xabcase"
//! would need to be manually changed to "\xab""case" to be used as C source.
//!
//! @code
//!   std::string blah("this \"is a test\"");
//!   std::cout<<pvxs::escape(blah);
//! @endcode
inline detail::Escaper escape(const std::string& s) {
    return detail::Escaper(s.c_str(), s.size());
}
//! Print nil terminated char array to output stream with non-printable characters escaped.
//! @code
//!   std::cout<<pvxs::escape("this \"is a test\"");
//! @endcode
inline detail::Escaper escape(const char* s) {
    return detail::Escaper(s);
}
//! Print fixed length char array to output stream with non-printable characters escaped.
//! @code
//!   std::cout<<pvxs::escape("this \"is a test\"", 6);
//!   // prints 'this \"'
//! @endcode
inline detail::Escaper escape(const char* s,size_t n) {
    return detail::Escaper(s,n);
}

struct GUID : public std::array<uint8_t, 12> {};

PVXS_API
std::ostream& operator<<(std::ostream&, const GUID&);

#if !defined(__rtems__) && !defined(vxWorks)

/** Minimal portable process signal handling in CLI tools.
 *
 * @code
 *     epicsEvent evt;
 *     SigInt handle([&evt]() {
 *          evt.trigger();
 *     });
 *     ... setup network operations
 *     evt.wait();
 *     // completion, or SIGINT
 * @endcode
 *
 * Saves existing handler, which are restored by dtor.
 */
class PVXS_API SigInt {
    void (*prevINT)(int);
    void (*prevTERM)(int);
    const std::function<void()> handler;
    static void _handle(int);
public:
    //! Install signal handler.
    SigInt(decltype (handler)&& handler);
    ~SigInt();
};

#else // !defined(__rtems__) && !defined(vxWorks)

class SigInt {
    const std::function<void()> handler;
public:
    SigInt(std::function<void()>&& handler) :handler(std::move(handler)) {}
}

#endif // !defined(__rtems__) && !defined(vxWorks)

//! return a snapshot of internal instance counters
PVXS_API
std::map<std::string, size_t> instanceSnapshot();

//! See Indented
struct indent {};

PVXS_API
std::ostream& operator<<(std::ostream& strm, const indent&);

//! Scoped indentation for std::ostream
struct PVXS_API Indented {
    explicit Indented(std::ostream& strm, int depth=1);
    Indented(const Indented&) = delete;
    Indented(Indented&& o) noexcept
        :strm(o.strm)
        ,depth(o.depth)
    {
        o.strm = nullptr;
        o.depth = 0;
    }
    ~Indented();
private:
    std::ostream *strm;
    int depth;
};

struct PVXS_API Detailed {
    explicit Detailed(std::ostream& strm, int lvl=1);
    Detailed(const Detailed&) = delete;
    Detailed(Detailed&& o) noexcept
        :strm(o.strm)
        ,lvl(o.lvl)
    {
        o.strm = nullptr;
        o.lvl = 0;
    }
    ~Detailed();
    static
    int level(std::ostream& strm);
private:
    std::ostream *strm;
    int lvl;
};

/** Describe build and runtime configuration of current system.
 *
 * Print information which may be using for when troubleshooting,
 * or creating a bug report.
 *
 * Printed by CLI "pvxinfo -D" and iocsh "pvxs_target_information".
 *
 * @returns The same ostream passed as argument.
 */
PVXS_API
std::ostream& target_information(std::ostream&);

/** Thread-safe, bounded, multi-producer, single-consumer queue
 *
 * @code
 * MPSCFIFO<std::function<void()>> Q;
 * ...
 * while(auto work = Q.pop()) { // Q.push(nullptr) to break loop
 *     work();
 * }
 * @endcode
 */
template<typename T>
class MPSCFIFO {
    epicsMutex lock;
    epicsEvent notifyW, notifyR;
    std::deque<T> Q;
    const size_t nlimit;
    unsigned nwriters=0u;

    typedef epicsGuard<epicsMutex> Guard;
    typedef epicsGuardRelease<epicsMutex> UnGuard;
public:
    typedef T value_type;

    //! Construct a new queue
    explicit MPSCFIFO(size_t limit)
        :nlimit(limit)
    {
        if(!nlimit)
            throw std::invalid_argument("MPSCFIFO limit must be >0");
    }

    /** Construct a new element into the queue.
     *
     * A bounded queue will block while full.
     */
    template<typename ...Args>
    void emplace(Args&&... args) {
        bool wakeup;
        {
            Guard G(lock);
            // while full, wait for reader to consume an entry
            while(Q.size()>=nlimit) {
                nwriters++;
                {
                    UnGuard U(G);
                    notifyW.wait();
                }
                nwriters--;
            }
            // notify reader when queue becomes not empty
            wakeup = Q.empty();
            Q.emplace_back(std::forward<Args>(args)...);
        }
        if(wakeup)
            notifyR.signal();
    }

    //! Move a new element to the queue
    void push(T&& ent) {
        // delegate to T::T(T&&)
        emplace(std::move(ent));
    }

    //! Copy a new element to the queue
    void push(const T& ent) {
        // delegate to T::T(const T&)
        emplace(ent);
    }

    /** Remove an element from the queue.
     *
     * Blocks while queue is empty.
     */
    T pop() {
        bool wakeup;
        T ret;
        {
            Guard G(lock);
            // wait for queue to become not empty
            while(Q.empty()) {
                UnGuard U(G);
                notifyR.wait();
            }
            // wakeup a writer since the queue will have an empty entry
            wakeup = nwriters;
            ret = std::move(Q.front());
            Q.pop_front();
        }
        if(wakeup)
            notifyW.signal();
        return ret;
    }
};

} // namespace pvxs

#endif // PVXS_UTIL_H
