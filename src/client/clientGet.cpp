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

namespace {
using pvac::detail::CallbackGuard;
using pvac::detail::CallbackUse;

static size_t num_instances;

template<class Leaf>
struct GetterCommon : public pvac::detail::CallbackStorage,
                public pva::ChannelGetRequester,
                public pvac::Operation::Impl,
                public pvac::detail::wrapped_shared_from_this<Leaf>
{
    operation_type::shared_pointer op;

    pvac::ClientChannel::GetCallback *cb;
    pvac::GetEvent event;

    explicit GetterCommon(pvac::ClientChannel::GetCallback* cb) :cb(cb)
    {REFTRACE_INCREMENT(num_instances);}
    virtual ~GetterCommon() {
        CallbackGuard G(*this);
        cb = 0;
        G.wait(); // paranoia
        REFTRACE_DECREMENT(num_instances);
    }

    void callEvent(CallbackGuard& G, pvac::GetEvent::event_t evt = pvac::GetEvent::Fail)
    {
        if(!cb) return;

        event.event = evt;
        pvac::ClientChannel::GetCallback *C=cb;
        cb = 0;
        CallbackUse U(G);
        try {
            C->getDone(event);
        } catch(std::exception& e) {
            LOG(pva::logLevelInfo, "Lost exception during getDone(): %s", e.what());
        }
    }

    virtual std::string name() const OVERRIDE FINAL
    {
        Guard G(mutex);
        return op ? op->getChannel()->getChannelName() : "<dead>";
    }

    // called automatically via wrapped_shared_from_this
    virtual void cancel() OVERRIDE FINAL
    {
        // keepalive for safety in case callback wants to destroy us
        std::tr1::shared_ptr<Leaf> keepalive(this->internal_shared_from_this());
        CallbackGuard G(*this);
        if(op) op->cancel();
        callEvent(G, pvac::GetEvent::Cancel);
        G.wait();
    }

    virtual std::string getRequesterName() OVERRIDE FINAL
    {
        Guard G(mutex);
        return op ? op->getChannel()->getRequesterName() : "<dead>";
    }

    virtual void channelGetConnect(
        const epics::pvData::Status& status,
        pva::ChannelGet::shared_pointer const & channelGet,
        epics::pvData::Structure::const_shared_pointer const & structure) OVERRIDE FINAL
    {
        std::tr1::shared_ptr<Leaf> keepalive(this->internal_shared_from_this());
        CallbackGuard G(*this);
        if(!cb) return;

        if(!status.isOK()) {
            event.message = status.getMessage();
        } else {
            event.message.clear();
        }
        if(!status.isSuccess()) {
            callEvent(G);

        } else {
            channelGet->get();

        }
    }

    virtual void channelDisconnect(bool destroy) OVERRIDE FINAL
    {
        CallbackGuard G(*this);
        event.message = "Disconnect";

        callEvent(G);
    }

    virtual void getDone(
        const epics::pvData::Status& status,
        pva::ChannelGet::shared_pointer const & channelGet,
        epics::pvData::PVStructure::shared_pointer const & pvStructure,
        epics::pvData::BitSet::shared_pointer const & bitSet) OVERRIDE FINAL
    {
        std::tr1::shared_ptr<Leaf> keepalive(this->internal_shared_from_this());
        CallbackGuard G(*this);
        if(!cb) return;

        if(!status.isOK()) {
            event.message = status.getMessage();
        } else {
            event.message.clear();
        }
        event.value = pvStructure;
        event.valid = bitSet;

        callEvent(G, status.isSuccess()? pvac::GetEvent::Success : pvac::GetEvent::Fail);
    }

    virtual void show(std::ostream &strm) const
    {
        strm << "Operation(Get"
                "\"" << name() <<"\""
             ")";
    }
};

struct Getter : public GetterCommon<Getter> {
    explicit Getter(pvac::ClientChannel::GetCallback* cb) :GetterCommon<Getter>(cb) {}
    virtual ~Getter() {}
};

} //namespace

namespace pvac {

Operation
ClientChannel::get(ClientChannel::GetCallback* cb,
                  epics::pvData::PVStructure::const_shared_pointer pvRequest)
{
    if(!impl) throw std::logic_error("Dead Channel");
    if(!pvRequest)
        pvRequest = pvd::createRequest("field()");

    std::tr1::shared_ptr<Getter> ret(Getter::build(cb));

    {
        Guard G(ret->mutex);
        ret->op = getChannel()->createChannelGet(ret->internal_shared_from_this(),
                                                 std::tr1::const_pointer_cast<pvd::PVStructure>(pvRequest));
    }

    return Operation(ret);
}

#if __cplusplus>=201103L

namespace {
struct Getter11 : public GetterCommon<Getter11>,
                  public ClientChannel::GetCallback
{
    const std::function<void(const GetEvent& evt)> cb;

    Getter11(std::function<void(const GetEvent& evt)>&& cb)
        :GetterCommon<Getter11>(this)
        ,cb(cb)
    {}

    virtual void getDone(const GetEvent& evt) override final
    {
        cb(evt);
    }
};
}// namespace

Operation
ClientChannel::get(std::function<void(const GetEvent& evt)>&& cb,
                   epics::pvData::PVStructure::const_shared_pointer pvRequest)
{
    if(!impl) throw std::logic_error("Dead Channel");
    if(!pvRequest)
        pvRequest = pvd::createRequest("field()");

    std::tr1::shared_ptr<Getter11> ret(Getter11::build(std::move(cb)));

    {
        Guard G(ret->mutex);
        ret->op = getChannel()->createChannelGet(ret->internal_shared_from_this(),
                                                 std::tr1::const_pointer_cast<pvd::PVStructure>(pvRequest));
    }

    return Operation(ret);
}
#endif // __cplusplus>=201103L

namespace detail {

void registerRefTrackGet()
{
    epics::registerRefCounter("pvac::Getter", &num_instances);
}

}

}//namespace pvac
