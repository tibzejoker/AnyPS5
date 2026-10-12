#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libkernel/Pthread/Posix/Common.hpp"
#include "prx/libc/include/general/VabiMacros.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

extern "C" int* APS5_VABI __error_nid_postfix();
extern "C" int APS5_VABI getpid_nid_postfix(void);
extern "C" Pthread APS5_VABI scePthreadSelf(void);

struct GuestSignalSet {
    std::uint32_t bits[4];
};

struct GuestSigaction {
    std::uintptr_t handler;
    int flags;
    GuestSignalSet mask;
};
static_assert(sizeof(GuestSigaction) == 32 && offsetof(GuestSigaction, flags) == 8 && offsetof(GuestSigaction, mask) == 12);

namespace {
using GuestHandler = void (APS5_VABI *)(int);
constexpr int MaxSignal = 128;
constexpr int GuestSigkill = 9;
constexpr int GuestSigstop = 17;
constexpr int SaRestart = 0x2;
constexpr int SaSiginfo = 0x40;
std::atomic<GuestHandler> handlers[MaxSignal + 1]{};
static_assert(std::atomic<GuestHandler>::is_always_lock_free);
std::atomic<std::uint32_t> blockedMask{0};
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
std::mutex registration;
GuestSigaction dispositions[MaxSignal + 1]{};
int NativeSignal(int guest) {
    switch (guest) {
        case 2: return SIGINT;
        case 4: return SIGILL;
        case 6: return SIGABRT;
        case 8: return SIGFPE;
        case 11: return SIGSEGV;
        case 15: return SIGTERM;
        default: return 0;
    }
}
void Dispatch(int native) {
    int guest = 0;
    for (int candidate : {2, 4, 6, 8, 11, 15})
        if (NativeSignal(candidate) == native) { guest = candidate; break; }
    if (!guest) return;
#ifdef _WIN32
    // Preserve the guest's persistent registration across CRT delivery.
    std::signal(native, Dispatch);
#endif
    if ((blockedMask.load() & (1u << (guest - 1))) != 0) return;
    const auto callback = handlers[guest].load();
    if (reinterpret_cast<std::uintptr_t>(callback) > 1) callback(guest);
}
bool Delivered(const GuestSigaction& action) {
    return action.handler > 1 && (action.flags & SaSiginfo) == 0;
}
bool Install(int guest, const GuestSigaction& action) {
    const int native = NativeSignal(guest);
    if (!native) return true;
    const auto previous = handlers[guest].exchange(Delivered(action) ? reinterpret_cast<GuestHandler>(action.handler) : nullptr);
    auto hostHandler = action.handler == 1 ? SIG_IGN : Delivered(action) ? Dispatch : SIG_DFL;
    if (std::signal(native, hostHandler) == SIG_ERR) {
        handlers[guest].store(previous);
        return false;
    }
    return true;
}
}

namespace {
constexpr int GuestSigalrm = 14;
struct AlarmState {
    std::mutex lock;
    std::condition_variable changed;
    std::optional<std::chrono::steady_clock::time_point> deadline;
    std::chrono::microseconds interval{0};
    bool threadStarted = false;
};

AlarmState& Alarms() {
    static auto* state = new AlarmState();
    return *state;
}

std::string DescribeInterval(std::chrono::microseconds interval) {
    char text[64];
    std::snprintf(text, sizeof(text), "%lld.%06lld s", static_cast<long long>(interval.count() / 1000000),
        static_cast<long long>(interval.count() % 1000000));
    return text;
}

void ExpireAlarm(std::chrono::microseconds interval) {
    std::uintptr_t handler = 0;
    {
        std::lock_guard lock(registration);
        handler = dispositions[GuestSigalrm].handler;
    }
    if (handler == 1) return;
    if ((blockedMask.load() & (1u << (GuestSigalrm - 1))) != 0)
        throw std::runtime_error("ITIMER_REAL (it_interval " + DescribeInterval(interval) + ") expired while SIGALRM is blocked: pending signals are not implemented");
    if (handler == 0) {
        std::fprintf(stderr, "SIGALRM: the alarm expired with the default disposition; the process terminates\n");
        std::fflush(stderr);
        std::_Exit(128 + GuestSigalrm);
    }
    throw std::runtime_error("ITIMER_REAL (it_interval " + DescribeInterval(interval) + ") expired: delivering SIGALRM to a guest handler is not implemented");
}

void RunAlarms() {
    auto& alarms = Alarms();
    std::unique_lock lock(alarms.lock);
    for (;;) {
        if (!alarms.deadline) {
            alarms.changed.wait(lock);
            continue;
        }
        const auto deadline = *alarms.deadline;
        if (alarms.changed.wait_until(lock, deadline) != std::cv_status::timeout || alarms.deadline != deadline) continue;
        const auto interval = alarms.interval;
        if (interval.count() == 0) alarms.deadline.reset();
        else {
            const auto now = std::chrono::steady_clock::now();
            const auto periods = now < deadline ? 1 : (now - deadline) / interval + 1;
            alarms.deadline = deadline + periods * interval;
        }
        lock.unlock();
        ExpireAlarm(interval);
        lock.lock();
    }
}

std::chrono::microseconds RemainingAlarm(const AlarmState& alarms, std::chrono::steady_clock::time_point now) {
    if (!alarms.deadline || *alarms.deadline <= now) return std::chrono::microseconds(0);
    return std::chrono::duration_cast<std::chrono::microseconds>(*alarms.deadline - now);
}

void NotifyAlarms(AlarmState& alarms) {
    if (!alarms.threadStarted) {
        std::thread(RunAlarms).detach();
        alarms.threadStarted = true;
    }
    alarms.changed.notify_all();
}
}

extern "C" unsigned int APS5_VABI GuestAlarm_nid_no_patch(unsigned int seconds) {
    auto& alarms = Alarms();
    std::lock_guard lock(alarms.lock);
    const auto now = std::chrono::steady_clock::now();
    const auto micros = RemainingAlarm(alarms, now).count();
    const auto remaining = static_cast<unsigned int>((micros + 999999) / 1000000);
    alarms.interval = std::chrono::microseconds(0);
    if (seconds == 0) alarms.deadline.reset();
    else alarms.deadline = now + std::chrono::seconds(seconds);
    NotifyAlarms(alarms);
    return remaining;
}

struct GuestItimerval {
    KernelTimeval interval;
    KernelTimeval value;
};
static_assert(sizeof(GuestItimerval) == 32 && offsetof(GuestItimerval, value) == 16);

extern "C" int APS5_VABI getitimer_nid_postfix(int which, GuestItimerval* output) {
    if (which < 0 || which > 2) {
        *__error_nid_postfix() = 22;
        return -1;
    }
    if (which != 0) throw std::runtime_error("getitimer: CPU interval timers are not implemented");
    if (output == nullptr) {
        *__error_nid_postfix() = 14;
        return -1;
    }
    GuestItimerval result{};
    auto& alarms = Alarms();
    {
        std::lock_guard lock(alarms.lock);
        const auto now = std::chrono::steady_clock::now();
        if (alarms.deadline && *alarms.deadline > now) {
            const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(*alarms.deadline - now).count();
            result.value.tv_sec = micros / 1000000;
            result.value.tv_usec = micros % 1000000;
        }
        const auto interval = alarms.interval.count();
        result.interval.tv_sec = interval / 1000000;
        result.interval.tv_usec = interval % 1000000;
    }
    *output = result;
    return 0;
}

namespace {
constexpr int ItimerReal = 0;
constexpr int ItimerProf = 2;
constexpr std::int64_t MaxItimerSeconds = 0x7fffffff / 2;
constexpr std::int64_t MinExactIntervalMicros = 1000;

bool ValidItimer(const KernelTimeval& time) {
    return time.tv_sec >= 0 && time.tv_usec >= 0 && time.tv_usec < 1000000 && time.tv_sec <= MaxItimerSeconds;
}

bool ItimerSet(const KernelTimeval& time) {
    return time.tv_sec != 0 || time.tv_usec != 0;
}

std::chrono::microseconds ItimerDuration(const KernelTimeval& time) {
    return std::chrono::seconds(time.tv_sec) + std::chrono::microseconds(time.tv_usec);
}

KernelTimeval ItimerTimeval(std::chrono::microseconds duration) {
    return {duration.count() / 1000000, duration.count() % 1000000};
}
}

extern "C" int APS5_VABI setitimer_nid_postfix(int which, const GuestItimerval* value, GuestItimerval* previous) {
    if (which < ItimerReal || which > ItimerProf) { *__error_nid_postfix() = 22; return -1; }
    if (value == nullptr && previous == nullptr) { *__error_nid_postfix() = 14; return -1; }
    GuestItimerval requested{};
    if (value != nullptr) {
        requested = *value;
        if (!ValidItimer(requested.value)) { *__error_nid_postfix() = 22; return -1; }
        if (!ItimerSet(requested.value)) requested.interval = {};
        else if (!ValidItimer(requested.interval)) { *__error_nid_postfix() = 22; return -1; }
    }
    if (which != ItimerReal) {
        if (ItimerSet(requested.value))
            throw std::runtime_error(std::string("setitimer: ") + (which == ItimerProf ? "ITIMER_PROF" : "ITIMER_VIRTUAL") + " needs process CPU time accounting, which is not implemented");
        if (previous != nullptr) *previous = {};
        return 0;
    }
    if (requested.interval.tv_sec == 0 && requested.interval.tv_usec != 0 && requested.interval.tv_usec < MinExactIntervalMicros)
        throw std::runtime_error("setitimer: it_interval " + DescribeInterval(ItimerDuration(requested.interval)) + " is below 1 ms, where the kernel rounds it up to a clock tick that is not known");
    GuestItimerval old{};
    {
        auto& alarms = Alarms();
        std::lock_guard lock(alarms.lock);
        const auto now = std::chrono::steady_clock::now();
        old.interval = ItimerTimeval(alarms.interval);
        old.value = ItimerTimeval(RemainingAlarm(alarms, now));
        if (value != nullptr) {
            alarms.interval = ItimerDuration(requested.interval);
            if (ItimerSet(requested.value)) alarms.deadline = now + ItimerDuration(requested.value);
            else alarms.deadline.reset();
            NotifyAlarms(alarms);
        }
    }
    if (previous != nullptr) *previous = old;
    return 0;
}

struct GuestStack {
    void* sp;
    std::size_t size;
    int flags;
};
static_assert(sizeof(GuestStack) == 24 && offsetof(GuestStack, flags) == 16);
namespace {
constexpr int SsDisable = 4;
constexpr std::size_t MinSignalStackSize = 2048;
thread_local GuestStack alternateStack{nullptr, 0, SsDisable};
}

extern "C" {
GuestHandler APS5_VABI signal_nid_postfix(int guest, GuestHandler handler) {
    const auto invalid = reinterpret_cast<GuestHandler>(static_cast<std::uintptr_t>(-1));
    if (guest < 1 || guest > MaxSignal || ((guest == GuestSigkill || guest == GuestSigstop) && handler != nullptr)) { *__error_nid_postfix() = 22; return invalid; }
    std::lock_guard lock(registration);
    const GuestSigaction action{reinterpret_cast<std::uintptr_t>(handler), SaRestart, {}};
    if (!Install(guest, action)) { *__error_nid_postfix() = 22; return invalid; }
    const auto previous = reinterpret_cast<GuestHandler>(dispositions[guest].handler);
    dispositions[guest] = action;
    return previous;
}
int APS5_VABI sigaction_nid_postfix(int guest, const GuestSigaction* action, GuestSigaction* previous) {
    if (guest < 1 || guest > MaxSignal) { *__error_nid_postfix() = 22; return -1; }
    if (action && (guest == GuestSigkill || guest == GuestSigstop) && action->handler != 0) { *__error_nid_postfix() = 22; return -1; }
    std::lock_guard lock(registration);
    const GuestSigaction old = dispositions[guest];
    if (action) {
        if (!Install(guest, *action)) { *__error_nid_postfix() = 22; return -1; }
        dispositions[guest] = *action;
    }
    if (previous) *previous = old;
    return 0;
}
int APS5_VABI raise_nid_postfix(int guest) {
    const int native = NativeSignal(guest);
    if (!native) { *__error_nid_postfix() = 22; return -1; }
    {
        std::lock_guard lock(registration);
        if (dispositions[guest].handler > 1 && !Delivered(dispositions[guest])) throw std::runtime_error("raise: SA_SIGINFO handlers are not delivered");
    }
    const int result = std::raise(native);
    if (result) *__error_nid_postfix() = 22;
    return result ? -1 : 0;
}
int APS5_VABI kill_nid_postfix(int pid, int guest) {
    if (guest < 0 || guest > MaxSignal) { *__error_nid_postfix() = 22; return -1; }
    const int self = getpid_nid_postfix();
    if (pid != self && pid != 0 && pid != -self) { *__error_nid_postfix() = 3; return -1; }
    return guest == 0 ? 0 : raise_nid_postfix(guest);
}
int APS5_VABI pthread_kill_nid_postfix(Pthread thread, int guest) {
    if (guest < 0 || guest > MaxSignal) return PosixThread::GUEST_EINVAL;
    if (thread == nullptr) return PosixThread::GUEST_ESRCH;
    if (guest == 0) return 0;
    if (thread != scePthreadSelf()) NotImplemented_nid_no_patch(("pthread_kill signal " + std::to_string(guest) + " to another thread").c_str());
    return raise_nid_postfix(guest) == 0 ? 0 : *__error_nid_postfix();
}
int APS5_VABI sigaltstack_nid_postfix(const GuestStack* stack, GuestStack* previous) {
    GuestStack replacement = alternateStack;
    if (stack != nullptr) {
        if ((stack->flags & ~SsDisable) != 0) { *__error_nid_postfix() = 22; return -1; }
        if (stack->flags & SsDisable) replacement.flags = SsDisable;
        else if (stack->size < MinSignalStackSize) { *__error_nid_postfix() = 12; return -1; }
        else replacement = {stack->sp, stack->size, 0};
    }
    if (previous != nullptr) *previous = alternateStack;
    alternateStack = replacement;
    return 0;
}
int APS5_VABI _sigprocmask_nid_postfix(int how, const GuestSignalSet* set, GuestSignalSet* previousSet) {
    std::lock_guard lock(registration);
    if (previousSet != nullptr) {
        previousSet->bits[0] = blockedMask.load();
        previousSet->bits[1] = 0;
        previousSet->bits[2] = 0;
        previousSet->bits[3] = 0;
    }
    if (set != nullptr) {
        switch (how) {
            case 1: blockedMask.fetch_or(set->bits[0]); break;
            case 2: blockedMask.fetch_and(~set->bits[0]); break;
            case 3: blockedMask.store(set->bits[0]); break;
            default: throw std::invalid_argument("_sigprocmask: invalid how");
        }
    }
    return 0;
}

int APS5_VABI sigprocmask_nid_postfix(int how, const void* set, void* previousSet) {
    return _sigprocmask_nid_postfix(how, static_cast<const GuestSignalSet*>(set),
                                    static_cast<GuestSignalSet*>(previousSet));
}

int APS5_VABI pthread_sigmask_nid_postfix(int how, const GuestSignalSet* set, GuestSignalSet* previousSet) {
    if (set != nullptr && (how < 1 || how > 3)) return 22;
    return _sigprocmask_nid_postfix(how, set, previousSet);
}

int APS5_VABI sigtimedwait_nid_postfix(const GuestSignalSet* set, void* info, const void* timeout) {
    (void)set;
    (void)info;
    (void)timeout;
    NotImplemented_nid_no_patch("sigtimedwait: waiting for a pending signal");
    return -1;
}

int APS5_VABI sigwaitinfo_nid_postfix(const GuestSignalSet* set, void* info) {
    (void)set;
    (void)info;
    NotImplemented_nid_no_patch("sigwaitinfo: waiting for a pending signal");
    return -1;
}

int APS5_VABI sigwait_nid_postfix(const GuestSignalSet* set, int* received) {
    (void)set;
    (void)received;
    NotImplemented_nid_no_patch("sigwait: waiting for a pending signal");
    return -1;
}

int APS5_VABI pause_nid_postfix(void) {
    NotImplemented_nid_no_patch("pause: waiting for a delivered signal");
    return -1;
}
}

extern "C" {

int APS5_VABI _is_signal_return_nid_postfix(std::uint64_t programCounter) {
    (void)programCounter;
    return 0;
}

}
