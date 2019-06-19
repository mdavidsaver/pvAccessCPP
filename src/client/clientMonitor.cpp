/*
 * Copyright information and license terms for this software can be
 * found in the file LICENSE that is included with the distribution
 */

#include <epicsMutex.h>
#include <epicsGuard.h>
#include <epicsEvent.h>
#include <epicsThread.h>

#include <pv/current_function.h>
#include <pv/pvData.h>
#include <pv/bitSet.h>
#include <pv/reftrack.h>

#define epicsExportSharedSymbols
#include "pv/logger.h"
#include "clientpvt.h"
#include "pv/pvAccess.h"

namespace pvac {
using pvac::detail::CallbackGuard;
using pvac::detail::CallbackUse;

struct Monitor::Impl : public pvac::detail::CallbackStorage,
                       public pva::MonitorRequester,
                       public pvac::detail::wrapped_shared_from_this<Monitor::Impl>
{
    pva::Channel::shared_pointer chan;
    operation_type::shared_pointer op;
    bool started, done, seenEmpty;

    ClientChannel::MonitorCallback *cb;
#if __cplusplus>=201103L
    std::function<void(const MonitorEvent&)> cb11;
#endif
    MonitorEvent event;

    pva::MonitorElement::Ref last;

    static size_t num_instances;

    Impl(ClientChannel::MonitorCallback* cb)
        :started(false)
        ,done(false)
        ,seenEmpty(false)
        ,cb(cb)
    {REFTRACE_INCREMENT(num_instances);}
#if __cplusplus>=201103L
    Impl(std::function<void(const MonitorEvent&)>&& cb)
        :started(false)
        ,done(false)
        ,seenEmpty(false)
        ,cb(0)
        ,cb11(std::move(cb))
    {REFTRACE_INCREMENT(num_instances);}
#endif
    virtual ~Impl() {
        CallbackGuard G(*this);
        cb = 0;
#if __cplusplus>=201103L
        // for want of std::function::clear()
        // swap with empty
        decltype(cb11)().swap(cb11);
#endif
        G.wait(); // paranoia
        REFTRACE_DECREMENT(num_instances);
    }

    bool valid() const {
        return cb
#if __cplusplus>=201103L
                || cb11
#endif
                ;
    }

    void callEvent(CallbackGuard& G, MonitorEvent::event_t evt = MonitorEvent::Fail)
    {
        ClientChannel::MonitorCallback *cb=this->cb;
#if __cplusplus>=201103L
        auto cb11 = this->cb11;
#endif
        if(!cb
#if __cplusplus>=201103L
                && !cb11
#endif
                ) return;

        event.event = evt;

        if(evt==MonitorEvent::Fail || evt==MonitorEvent::Cancel) {
            this->done = true;
            this->cb = 0; // last event
#if __cplusplus>=201103L
            decltype(cb11)().swap(this->cb11);
#endif
        }

        try {
            CallbackUse U(G);
            if(cb)
                cb->monitorEvent(event);
#if __cplusplus>=201103L
            else
                cb11(event);
#endif
            return;
        }catch(std::exception& e){
            if(!this->cb || evt==MonitorEvent::Fail) {
                LOG(pva::logLevelError, "Unhandled exception in ClientChannel::MonitorCallback::monitorEvent(): %s", e.what());
                return;
            } else {
               event.event = MonitorEvent::Fail;
               event.message = e.what();
            }
        }
        // continues error handling
        try {
            CallbackUse U(G);
            if(cb)
                cb->monitorEvent(event);
#if __cplusplus>=201103L
            else
                cb11(event);
#endif
            return;
        }catch(std::exception& e){
            LOG(pva::logLevelError, "Unhandled exception following exception in ClientChannel::MonitorCallback::monitorEvent(): %s", e.what());
        }
    }

    // called automatically via wrapped_shared_from_this
    void cancel()
    {
        operation_type::shared_pointer temp;
        {
            // keepalive for safety in case callback wants to destroy us
            std::tr1::shared_ptr<Monitor::Impl> keepalive(internal_shared_from_this());

            CallbackGuard G(*this);

            last.reset();

            if(started && op) {
                op->stop();
                started = false;
            }
            temp.swap(op);

            callEvent(G, MonitorEvent::Cancel);
            G.wait();
        }
        if(temp)
            temp->destroy();
    }

    virtual std::string getRequesterName() OVERRIDE FINAL
    {
        Guard G(mutex);
        return chan ? chan->getRequesterName() : "<dead>";
    }


    virtual void monitorConnect(pvd::Status const & status,
                                pva::MonitorPtr const & operation,
                                pvd::StructureConstPtr const & structure) OVERRIDE FINAL
    {
        std::tr1::shared_ptr<Monitor::Impl> keepalive(internal_shared_from_this());
        CallbackGuard G(*this);
        if(!valid() || started || done) return;

        if(!status.isOK()) {
            event.message = status.getMessage();
        } else {
            event.message.clear();
        }
        if(!status.isSuccess()) {
            callEvent(G);

        } else {
            pvd::Status sts(operation->start());
            if(sts.isSuccess()) {
                started = true;
                /* storing raw pointer to operation, which is expected
                 * to outlive our 'op'.
                 */
                last.attach(operation);
            } else {
                event.message = sts.getMessage();
                callEvent(G);
            }
        }
    }


    virtual void channelDisconnect(bool destroy) OVERRIDE FINAL
    {
        std::tr1::shared_ptr<Monitor::Impl> keepalive(internal_shared_from_this());
        CallbackGuard G(*this);
        if(!valid() || done) return;
        event.message = "Disconnect";
        started = false;
        callEvent(G, MonitorEvent::Disconnect);
    }

    virtual void monitorEvent(pva::MonitorPtr const & monitor) OVERRIDE FINAL
    {
        std::tr1::shared_ptr<Monitor::Impl> keepalive(internal_shared_from_this());
        CallbackGuard G(*this);
        if(!valid() || done) return;
        event.message.clear();

        callEvent(G, MonitorEvent::Data);
    }

    virtual void unlisten(pva::MonitorPtr const & monitor) OVERRIDE FINAL
    {
        std::tr1::shared_ptr<Monitor::Impl> keepalive(internal_shared_from_this());
        CallbackGuard G(*this);
        if(!valid() || done) return;
        done = true;

        if(seenEmpty)
            callEvent(G, MonitorEvent::Data);
        // else // wait until final poll()
    }
};

size_t Monitor::Impl::num_instances;

Monitor::Monitor(const std::tr1::shared_ptr<Impl>& impl)
    :impl(impl)
{}

Monitor::~Monitor() {}


std::string Monitor::name() const
{
    return impl ? impl->chan->getChannelName() : "<NULL>";
}

void Monitor::cancel()
{
    changed.clear();
    overrun.clear();
    root.reset();
    if(impl) impl->cancel();
}

bool Monitor::poll()
{
    if(!impl) return false;
    Guard G(impl->mutex);

    if(!impl->done && impl->op && impl->started && impl->last.next()) {
        const epics::pvData::PVStructurePtr& ptr = impl->last->pvStructurePtr;
        changed = *impl->last->changedBitSet;
        overrun = *impl->last->overrunBitSet;

        /* copy the exposed PVStructure for two reasons.
         * 1. Prevent accidental use of shared container after release()
         * 2. Allows caller to cache results of getSubField() until root.get() changes.
         */
        if(!root || (void*)root->getField().get()!=(void*)ptr->getField().get()) {
            // initial connection, or new type
            root = pvd::getPVDataCreate()->createPVStructure(ptr); // also calls copyUnchecked()
        } else {
            // same type
            const_cast<pvd::PVStructure&>(*root).copyUnchecked(*ptr, changed);
        }

        impl->seenEmpty = false;
    } else {
        changed.clear();
        overrun.clear();
        impl->seenEmpty = true;
    }
    return !impl->seenEmpty;
}

bool Monitor::complete() const
{
    if(!impl) return true;
    Guard G(impl->mutex);
    return impl->done && impl->seenEmpty;
}

Monitor
ClientChannel::monitor(MonitorCallback *cb,
                           epics::pvData::PVStructure::const_shared_pointer pvRequest)
{
    if(!impl) throw std::logic_error("Dead Channel");
    if(!pvRequest)
        pvRequest = pvd::createRequest("field()");

    std::tr1::shared_ptr<Monitor::Impl> ret(Monitor::Impl::build(cb));
    ret->event.monimpl = ret;
    ret->chan = getChannel();

    {
        Guard G(ret->mutex);
        ret->op = ret->chan->createMonitor(ret->internal_shared_from_this(),
                                           std::tr1::const_pointer_cast<pvd::PVStructure>(pvRequest));
    }

    return Monitor(ret);
}

#if __cplusplus>=201103L
Monitor
ClientChannel::monitor(std::function<void(const MonitorEvent&)>&& cb,
                       epics::pvData::PVStructure::const_shared_pointer pvRequest)
{
    if(!impl) throw std::logic_error("Dead Channel");
    if(!pvRequest)
        pvRequest = pvd::createRequest("field()");

    std::tr1::shared_ptr<Monitor::Impl> ret(Monitor::Impl::build(std::move(cb)));
    ret->event.monimpl = ret;
    ret->chan = getChannel();

    {
        Guard G(ret->mutex);
        ret->op = ret->chan->createMonitor(ret->internal_shared_from_this(),
                                           std::tr1::const_pointer_cast<pvd::PVStructure>(pvRequest));
    }

    return Monitor(ret);
}
#endif

::std::ostream& operator<<(::std::ostream& strm, const Monitor& op)
{
    if(op.impl) {
        strm << "Monitor("
                "\"" << op.impl->chan->getChannelName() <<"\", "
                "\"" << op.impl->chan->getProvider()->getProviderName() <<"\", "
                "connected="<<(op.impl->chan->isConnected()?"true":"false")
             <<"\")";
    } else {
        strm << "Monitor()";
    }
    return strm;
}

namespace detail {

void registerRefTrackMonitor()
{
    epics::registerRefCounter("pvac::Monitor::Impl", &Monitor::Impl::num_instances);
}

}

}//namespace pvac
