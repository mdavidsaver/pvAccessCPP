/**
 * Copyright - See the COPYRIGHT that is included with this distribution.
 * pvAccessCPP is distributed subject to a Software License Agreement found
 * in file LICENSE that is included with this distribution.
 */

#if __cplusplus>=201103L

#include <set>
#include <queue>
#include <vector>
#include <string>
#include <exception>
#include <iostream>

#if !defined(_WIN32)
#include <signal.h>
#define USE_SIGNAL
#endif

#include <epicsEvent.h>
#include <epicsMutex.h>
#include <epicsGuard.h>
#include <epicsGetopt.h>
#include <epicsAtomic.h>

#include <pv/configuration.h>
#include <pv/caProvider.h>
#include <pv/reftrack.h>
#include <pv/thread.h>
#include <pva/client.h>

namespace pvd = epics::pvData;
namespace pva = epics::pvAccess;

namespace {

typedef epicsGuard<epicsMutex> Guard;
typedef epicsGuardRelease<epicsMutex> UnGuard;

// We want to gracefully cleanup on SIGINT
epicsEvent done;
int ret = 0;

#ifdef USE_SIGNAL
void sigdone(int num)
{
    (void)num;
    done.signal();
}
#endif

} // namespace

int main(int argc, char *argv[]) {
    try {
        epics::RefMonitor refmon;
        double waitTime = -1.0;
        // default to native PVA client
        std::string providerName("pva");
        // default to "pvRequest" which asks for all fields
        std::string requestStr("field()");
        typedef std::vector<std::string> pvs_t;
        pvs_t pvs;

        int opt;
        while((opt = getopt(argc, argv, "hRp:w:r:")) != -1) {
            switch(opt) {
            case 'R':
                refmon.start(5.0);
                break;
            case 'p':
                providerName = optarg;
                break;
            case 'w':
                waitTime = pvd::castUnsafe<double, std::string>(optarg);
                break;
            case 'r':
                requestStr = optarg;
                break;
            case 'h':
                std::cout<<"Usage: "<<argv[0]<<" [-p <provider>] [-w <timeout>] [-r <request>] [-R] <pvname> ...\n";
                return 0;
            default:
                std::cerr<<"Unknown argument: "<<(char)opt<<"\n";
                return -1;
            }
        }

        for(int i=optind; i<argc; i++)
            pvs.push_back(argv[i]);

#ifdef USE_SIGNAL
        signal(SIGINT, sigdone);
        signal(SIGTERM, sigdone);
        signal(SIGQUIT, sigdone);
#endif

        pvd::PVStructure::shared_pointer pvReq(pvd::createRequest(requestStr));

        // "pva" provider automatically in registry
        // add "ca" provider to registry even when we won't use it.
        pva::ca::CAClientFactory::start();

        // Note: With "pva" provider, could provide a Configuration as the second argument
        //       to override default (EPICS_PVA_* environment variables).
        //       Doesn't apply to "ca".
        pvac::ClientProvider provider(providerName);

        size_t remaining = pvs.size();

        // the ordering of the following is critical for a safe shutdown.
        // first created is last destroyed

        bool clockoff = false;
        epicsEvent ready;
        epicsMutex mutex;

        // work queue
        std::deque<pvac::MonitorEvent> work;

        // holder for subscriptions.
        // subscriptions are implicitly closed when Monitor's are destroyed (when 'subs' goes out of scope)
        std::vector<pvac::Monitor> subs;
        subs.reserve(pvs.size());

        // Subscribe to all requested PVs, and feed updates into our work queue.
        // Will do I/O on our own worker thread instead of the PVA worker threads.
        for(const auto& pv : pvs) {
            subs.push_back(provider
                           .connect(pv)
                           .monitor([&work, &ready, &mutex](const pvac::MonitorEvent& evt)
            {
                // Callback on a shared PVA worker thread.
                bool poke;
                {
                    Guard G(mutex);
                    poke = work.empty();
                    work.push_back(evt);
                }
                // signal when the work queue is no longer empty
                if(poke)
                    ready.signal();
            }));
        }

        // Start up our worker thread
        pvd::Thread worker(pvd::Thread::Config()
                           .name("worker")
                           .autostart(true)
                           .run([&work, &ready, &mutex, &remaining, &clockoff]()
        {
            // our worker thread run()
            Guard G(mutex);
            while(!clockoff) {
                // wait for the queue to become not-empty
                {
                    UnGuard U(G);
                    ready.wait();
                }
                // loop until the queue is drained
                while(!work.empty()) {
                    const pvac::MonitorEvent evt = work.front();
                    work.pop_front();
                    bool requeue = false;

                    pvac::Monitor mon(evt.monitor());

                    {
                        // MonitorEvent is a copy, and may be handled w/o lockingn
                        UnGuard U(G);

                        auto name = mon.name();

                        switch(evt.event) {
                        case pvac::MonitorEvent::Fail:
                            std::cout<<name<<" Error : "<<evt.message<<"\n";
                            epics::atomic::set(ret, 1);
                            break;
                        case pvac::MonitorEvent::Cancel:
                            std::cout<<name<<" Cancel\n";
                            break;
                        case pvac::MonitorEvent::Disconnect:
                            std::cout<<name<<" Disconnect\n";
                            break;
                        case pvac::MonitorEvent::Data:
                        {
                            unsigned n;
                            for(n=0; n<2 && mon.poll(); n++) {
                                pvd::PVField::const_shared_pointer fld(mon.root->getSubField("value"));
                                if(!fld)
                                    fld = mon.root;

                                std::cout<<"Event "<<name<<" "<<fld
                                         <<" Changed:"<<mon.changed
                                         <<" overrun:"<<mon.overrun<<"\n";
                            }
                            if(n==2) {
                                // too many updates, re-queue to balance with others
                                requeue = true;
                                break;
                            } else if(n==0) {
                                std::cerr<<"Spurious Data event "<<name<<"\n";
                            }
                        }
                            break;
                        }
                    }

                    if(requeue)
                        work.push_back(evt);

                    if(mon.complete() && epics::atomic::decrement(remaining)==0)
                        done.signal();
                }
            }

        }));
        // now need to make sure the worker is interruptedon success or error

        try {
            if(waitTime<0.0) {
                done.wait();

            } else if(!done.wait(waitTime)) {
                std::cerr<<"Timeout\n";
                epics::atomic::set(ret, 1);
            }

        }catch(...){
            clockoff = true;
            ready.signal();
            throw;
        }
        clockoff = true;
        ready.signal();

        return ret;

    } catch(std::exception& e){
        std::cout<<"Error: "<<e.what()<<"\n";
        return 2;
    }
}

#else // ! __cplusplus>=201103L

#include <iostream>

int main(int argc, char *argv[]) {
    std::cerr<<"This Example must be built with c++11 enabled\n";
    return 1;
}
#endif // __cplusplus>=201103L
