#include "ten/logging.hh"
#include "../src/context.hh"
#include "ten/descriptors.hh"
#include "ten/buffer.hh"
#include "ten/http/http_message.hh"

#include <memory>
#include <thread>
#include <atomic>

#include <boost/algorithm/string/predicate.hpp>

#include "t2/channel.hh"

#include <sys/syscall.h> // gettid
#include <sys/stat.h> // umask
#include <signal.h> // sigaction

using namespace ten;
using namespace t2;

static void runtime_init() {
    // allow log files and message queues to be created group writable
    umask(0);
    InitGoogleLogging(program_invocation_short_name);
    InstallFailureSignalHandler();
    FLAGS_logtostderr = true;


    CHECK(getpid() == syscall(SYS_gettid)) << "must call in main thread before anything else";
    //ncpu_ = sysconf(_SC_NPROCESSORS_ONLN);
    stack_t ss;
    ss.ss_sp = calloc(1, SIGSTKSZ);
    ss.ss_size = SIGSTKSZ;
    ss.ss_flags = 0;
    PCHECK(sigaltstack(&ss, NULL) == 0) << "setting signal stack failed";

    struct sigaction act;

    // ignore SIGPIPE
    memset(&act, 0, sizeof(act));
    PCHECK(sigaction(SIGPIPE, NULL, &act) == 0) << "getting sigpipe handler failed";
    if (act.sa_handler == SIG_DFL) {
        act.sa_handler = SIG_IGN;
        PCHECK(sigaction(SIGPIPE, &act, NULL) == 0) << "setting sigpipe handler failed";
    }

    //netinit();
}

static std::atomic<uint64_t> taskcount{0};

struct tasklet;

struct thread_data {
    context ctx;
    std::unique_ptr<tasklet> self;
};

__thread thread_data *tld = nullptr;

struct tasklet {

    context _ctx;
    std::function<void ()> _f;
    std::function<void ()> _post;

    tasklet(std::function<void ()> &&f)
        : _ctx(tasklet::trampoline),
        _f(f)
    {
        ++taskcount;
    }

    ~tasklet() {
        DVLOG(5) << "freeing task: " << this;
        --taskcount;
    }

    static void yield() {
        tld->self->_yield();
    }

    void _yield() {
        _ctx.swap(tld->ctx, 0);
    }

    bool swap() {
        if (_f) {
            tld->ctx.swap(_ctx, reinterpret_cast<intptr_t>(this));
        }
        return static_cast<bool>(_f);
    }

    static void trampoline(intptr_t arg) noexcept;
};

void tasklet::trampoline(intptr_t arg) noexcept {
    tasklet *self = reinterpret_cast<tasklet *>(arg);
    self->_f();
    self->_f = nullptr;
    self->_ctx.swap(tld->ctx, 0);
    LOG(FATAL) << "Oh no! You fell through the trampoline " << self;
}

struct Foo {
    int value;

    Foo(const Foo &) = delete;

    Foo(Foo &&other) : value(0) {
        std::swap(value, other.value);
    }

    Foo &operator= (Foo &&other) {
        std::swap(value, other.value);
        return *this;
    }

    //Foo(const Foo &other) {
    //    LOG(INFO) << "copy ctor";
    //    value = other.value;
    //}

    Foo(int v) : value(v) {}
};

static void recver(channel<Foo> chan) {
    optional<Foo> v;
    for (;;) {
        optional<Foo> tmp = chan.recv();
        if (!tmp) break;
        v = std::move(tmp);
    }
    if (v) {
        VLOG(5) << "last value: " << v->value;
    }
}

static void counter(int n) {
    for (int i=0; i<100; ++i) {
        VLOG(5) << n << " i: " << i;
        tasklet::yield();
        if (i == 5) {
            VLOG(5) << "sleepy time";
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
    VLOG(5) << "end counter " << n;
}

static channel<std::unique_ptr<tasklet>> sched_chan;
static channel<std::tuple<int, std::unique_ptr<tasklet>>> io_chan;
static epoll_fd efd;
static event_fd evfd;
static std::thread io_thread;

static void net_init() {
    epoll_event ev{EPOLLIN | EPOLLONESHOT};
    ev.data.fd = evfd.fd;
    efd.add(evfd.fd, ev);
}

static void wait_for_event(int wait_fd, uint32_t event) {
    tld->self->_post = [=] {
        epoll_event ev{};
        ev.events = event | EPOLLONESHOT;
        ev.data.fd = wait_fd;
        VLOG(5) << "sending self to io chan";
        io_chan.send(std::make_pair(wait_fd, std::move(tld->self)));
        if (efd.add(wait_fd, ev) < 0) {
            efd.modify(wait_fd, ev);
        }
    };
    tasklet::yield();
}

static void connection(socket_fd fd) {
    VLOG(5) << "in connection handler " << fd.fd;

    buffer buf{4096};
    http_request req;
    http_parser parser;
    req.parser_init(&parser);
    for (;;) {
        buf.reserve(4096);
        ssize_t nr;
        while ((nr = fd.recv(buf.back(), buf.available())) > 0) {
            VLOG(5) << fd.fd << " got " << nr << " bytes\n";
            buf.commit(nr);
            size_t nparse = buf.size();
            req.parse(&parser, buf.front(), nparse);
            buf.remove(nparse);
            if (req.complete) {
                VLOG(5) << " got request " << req.data();
                http_response resp{200, http_headers{"Content-Length", "0"}};
#if 0
                char buf[128];
                struct tm tm;
                time_t now = std::chrono::system_clock::to_time_t(
                        std::chrono::system_clock::now()
                        );
                strftime(buf, sizeof(buf)-1, "%a, %d %b %Y %H:%M:%S GMT", gmtime_r(&now, &tm));
                resp.set("Date", buf);
#endif

                if (resp.get("Connection").empty()) {
                    // obey clients wishes if we have none of our own
                    std::string conn = req.get("Connection");
                    if (!conn.empty())
                        resp.set("Connection", conn);
                    else if (req.version == http_1_0)
                        resp.set("Connection", "close");
                }

                std::string data = resp.data();
                ssize_t nw = fd.send(data.data(), data.size());
                (void)nw;

                if (boost::iequals(resp.get("Connection"), "close")) {
                    return;
                }
            }
        }
        if (nr == 0) return;
        wait_for_event(fd.fd, EPOLLIN);
    }
}

static void accepter(socket_fd fd) {
    DVLOG(5) << "in accepter";
    fd.setnonblock(); // needed because of dup()
    for (;;) {
        wait_for_event(fd.fd, EPOLLIN);
        address addr;
        int nfd = fd.accept(addr, SOCK_NONBLOCK);
        if (nfd != -1) {
            DVLOG(5) << "accepted: " << nfd;
            std::unique_ptr<tasklet> t(new tasklet(std::bind(connection, nfd)));
            sched_chan.send(std::move(t));
        } else {
            LOG(ERROR) << "ready on " << fd.fd << " but accept returned error";
        }
    }
}

static void scheduler() {
    thread_data td;
    tld = &td;
    while (taskcount > 0) {
        DVLOG(5) << "waiting for task to schedule";
        auto t = sched_chan.recv();
        DVLOG(5) << "got task to schedule: " << (*t).get();
        if (!t) break;
        tld->self = std::move(*t);
        if (tld->self->swap()) {
            CHECK(tld->self);
            if (tld->self->_post) {
                std::function<void ()> post = std::move(tld->self->_post);
                post();
            }
            if (tld->self) {
                DVLOG(5) << "scheduling " << tld->self.get();
                sched_chan.send(std::move(tld->self));
            }
        }
    }
    DVLOG(5) << "exiting scheduler";
    sched_chan.close();
}

static void io_scheduler() {
    std::vector<std::unique_ptr<tasklet>> io_tasks;
    std::vector<epoll_event> events;
    events.reserve(1000);
    for (;;) {
        events.resize(1000);
        efd.wait(events, -1);
        auto new_tasks = io_chan.recv_all();
        for (auto &tup : new_tasks) {
            int fd = std::get<0>(tup);
            std::unique_ptr<tasklet> t = std::move(std::get<1>(tup));
            if ((size_t)fd >= io_tasks.size()) {
                io_tasks.resize(fd+1);
            }
            DVLOG(5) << "inserting task for io on fd " << fd;
            io_tasks[fd] = std::move(t);
        }

        for (epoll_event &ev : events) {
            int fd = ev.data.fd;
            DVLOG(5) << "events " << ev.events << " on fd " << fd;
            if (fd == evfd.fd) {
                uint64_t e = evfd.read();
                DVLOG(5) << "event " << e << " on eventfd";
                if (e % 7 == 0) return;
                continue;
            }
            // TODO: send them all in one go
            if ((size_t)fd >= io_tasks.size()) {
                LOG(ERROR) << "BUG: " << fd << " larger than " << io_tasks.size();
                continue;
            }
            if (io_tasks[fd]) {
                sched_chan.send(std::move(io_tasks[fd]));
            }
        }
    }
}

void install_sigint_handler() {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGQUIT);
    // This is lame because i can't move signal_fd into the lambda
    auto sigfd = std::make_shared<signal_fd>(mask);
    std::unique_ptr<tasklet> t(new tasklet([sigfd] {
        LOG(INFO) << "waiting for signal";
        wait_for_event(sigfd->fd, EPOLLIN);
        LOG(INFO) << "signal handler";
        // TODO: lame hack
        evfd.write(7);
        io_thread.join();
        sched_chan.recv_all();
        sched_chan.close();
    }));
    sched_chan.send(std::move(t));
}

int main() {
    runtime_init();
    net_init();
    install_sigint_handler();
    DVLOG(5) << "take2";

    io_thread = std::thread{io_scheduler};

#if 0
    for (int i=0; i<2; ++i) {
        std::unique_ptr<tasklet> t(new tasklet(std::bind(counter, i)));
        sched_chan.send(std::move(t));
    }
#endif
    socket_fd listen_fd{AF_INET, SOCK_STREAM};
    address addr{"0.0.0.0", 7700};
    listen_fd.setsockopt(SOL_SOCKET, SO_REUSEADDR, 1);
    listen_fd.bind(addr);
    LOG(INFO) << "bound to " << addr;
    listen_fd.listen();

    for (int i=0; i<4; ++i) {
        std::unique_ptr<tasklet> t(new tasklet(std::bind(accepter, dup(listen_fd.fd))));
        sched_chan.send(std::move(t));
    }

    std::vector<std::thread> schedulers;
    for (int i=0; i<4; ++i) {
        schedulers.emplace_back(scheduler);
    }

    LOG(INFO) << "joining";

    for (auto &sched : schedulers) {
        sched.join();
    }
}

