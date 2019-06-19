/**
 * Copyright - See the COPYRIGHT that is included with this distribution.
 * pvAccessCPP is distributed subject to a Software License Agreement found
 * in file LICENSE that is included with this distribution.
 */

/** @file getme.cpp
 *
 * This example issues multiple gets concurrently,
 * then waits for for all to complete, or a global
 * timeout to expire.
 */

#if __cplusplus>=201103L

#include <set>
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
#include <pva/client.h>

namespace pvd = epics::pvData;
namespace pva = epics::pvAccess;

namespace {

// We want to gracefully cleanup on SIGINT
epicsEvent done;
int ret = 0;

#ifdef USE_SIGNAL
void alldone(int num)
{
    (void)num;
    epics::atomic::set(ret, 1);
    done.signal();
}
#endif

} // namespace

int main(int argc, char *argv[]) {
    try {
        double waitTime = -1.0;
        std::string providerName("pva");
        std::vector<std::string> pvs;

        int opt;
        while((opt = getopt(argc, argv, "hp:w:")) != -1) {
            switch(opt) {
            case 'p':
                providerName = optarg;
                break;
            case 'w':
                waitTime = pvd::castUnsafe<double, std::string>(optarg);
                break;
            default:
                std::cerr<<"Unknown argument: "<<(char)opt<<"\n\n";
                ret = 2;
                // fall through
            case 'h':
                std::cout<<"Usage: "<<argv[0]<<" [-p <provider>] [-w <timeout>] [-R] <pvname> ...\n";
                return ret;
            }
        }

        for(int i=optind; i<argc; i++)
            pvs.push_back(argv[i]);

#ifdef USE_SIGNAL
        signal(SIGINT, alldone);
        signal(SIGTERM, alldone);
        signal(SIGQUIT, alldone);
#endif

        // "pva" provider automatically in registry
        // add "ca" provider to registry even when we won't use it.
        pva::ca::CAClientFactory::start();

        // Note: With "pva" provider, could provide a Configuration as the second argument
        //       to override default (EPICS_PVA_* environment variables).
        //       Doesn't apply to "ca".
        pvac::ClientProvider provider(providerName);

        size_t remaining = pvs.size();

        // Store references to Operations to prevent them from being implicitly cancelled.
        std::vector<pvac::Operation> ops;
        ops.reserve(pvs.size());

        for(const auto& pv : pvs) {

            ops.push_back(provider
                          // Internal connection cache avoids creation of duplicate Channels through this ClientProvider
                          .connect(pv)
                          // Safe to capture references to local variables declared __before__ ops
                          // Note: implement pvac::ClientChannel::GetCallback when c++11 lambdas not availble.
                          .get([&pv, &remaining](const pvac::GetEvent& event)
            {
                // Get now completed
                switch(event.event) {
                case pvac::GetEvent::Fail:
                    std::cout<<pv<<" Error : "<<event.message<<"\n";
                    epics::atomic::set(ret, 1);
                    break;

                case pvac::GetEvent::Cancel:
                    std::cout<<pv<<" Cancel\n";
                    break;

                case pvac::GetEvent::Success:
                    auto valfld(event.value->getSubField("value"));
                    if(!valfld)
                        valfld = event.value;
                    std::cout<<pv<<" : "<<*valfld<<"\n";
                    break;
                }

                if(epics::atomic::decrement(remaining)==0)
                    done.signal();
            }));
        }

        if(waitTime<0.0) {
            done.wait();

        } else if(!done.wait(waitTime)) {
            std::cerr<<"Timeout\n";
            epics::atomic::set(ret, 1);
        }

        return ret;
    } catch(std::exception& e){
        std::cerr<<"Error: "<<e.what()<<"\n";
        return 1;
    }
}


#else // ! __cplusplus>=201103L

#include <iostream>

int main(int argc, char *argv[]) {
    std::cerr<<"This Example must be built with c++11 enabled\n";
    return 1;
}
#endif // __cplusplus>=201103L
