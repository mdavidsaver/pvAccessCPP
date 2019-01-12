/**
 * Copyright - See the COPYRIGHT that is included with this distribution.
 * pvAccessCPP is distributed subject to a Software License Agreement found
 * in file LICENSE that is included with this distribution.
 */

#include <sstream>
#include <stdexcept>

#include <stdarg.h>

#include <gssapi.h>
#include <gssapi/gssapi_krb5.h>

#include <epicsGuard.h>
#include <errlog.h>

#define epicsExportSharedSymbols
#include <pv/securityImpl.h>

#include <epicsExport.h>

#ifdef major
#  undef major
#endif
#ifdef minor
#  undef minor
#endif

namespace pvd = epics::pvData;
namespace pva = epics::pvAccess;

typedef epicsGuard<epicsMutex> Guard;

namespace {

struct GSSError : public std::runtime_error
{
    OM_uint32 major, minor;
    gss_OID mech;
    GSSError(OM_uint32 major, OM_uint32 minor, const gss_OID mech, const std::string& msg)
        :std::runtime_error(msg)
        ,major(major)
        ,minor(minor)
        ,mech(mech)
    {}
    virtual ~GSSError() {}
};

struct GSSBuffer {
    gss_buffer_desc handle;
    GSSBuffer() :handle(GSS_C_EMPTY_BUFFER) {}
    ~GSSBuffer() {
        clear();
    }
    void clear() {
        OM_uint32 ignore;
        (void)gss_release_buffer(&ignore, &handle);
    }
    gss_buffer_t ref() {
        return &handle;
    }
    gss_buffer_t operator->() {
        return &handle;
    }

    EPICS_NOT_COPYABLE(GSSBuffer)
};

struct GSSName {

    gss_name_t handle;
    GSSName()
        :handle(GSS_C_NO_NAME)
    {}
    ~GSSName() {
        clear();
    }
    void clear() {
        OM_uint32 ignore;
        (void)gss_release_name(&ignore, &handle);
    }
    gss_name_t* ref() {
        return &handle;
    }

    void import(const std::string& name, const gss_OID type) {
        OM_uint32 major, minor;
        gss_buffer_desc buf;
        buf.length = name.size();
        buf.value = const_cast<char*>(name.c_str()); // not actually modified
        major = gss_import_name(&minor, &buf, type, &handle);
        if(GSS_ERROR(major))
            throw GSSError(major, minor, type, name+" unable to import");
    }

    void cannonicalize(const gss_OID mech) {
        OM_uint32 major, minor;
        GSSName cannon;

        major = gss_canonicalize_name(&minor, handle, mech, cannon.ref());
        if(GSS_ERROR(major))
            throw GSSError(major, minor, mech, "unable to cannonicalize");
        std::swap(handle, cannon.handle);
    }

    std::string display() {
        OM_uint32 major, minor;
        gss_OID mech = 0;
        GSSBuffer buf;
        major = gss_display_name(&minor, handle, buf.ref(), &mech);
        if(GSS_ERROR(major))
            throw GSSError(major, minor, mech, "unable to display");
        return std::string((char*)buf->value, buf->length);
    }

    EPICS_NOT_COPYABLE(GSSName)
};

struct GSSCred {
    gss_cred_id_t handle;
    GSSCred() :handle(GSS_C_NO_CREDENTIAL) {}
    ~GSSCred() {
        clear();
    }
    void clear() {
        OM_uint32 ignore;
        (void)gss_release_cred(&ignore, &handle);
    }
    gss_cred_id_t* ref() {
        return &handle;
    }

    void acquire(const GSSName& name, gss_cred_usage_t usage) {
        OM_uint32 major, minor;
        major = gss_acquire_cred(&minor, name.handle,
                                 GSS_C_INDEFINITE,
                                 GSS_C_NO_OID_SET,
                                 usage,
                                 &handle,
                                 0, 0);
        if(GSS_ERROR(major))
            throw GSSError(major, minor, GSS_C_NO_OID, "unable to import name");

    }

    void name(GSSName& name) {

        OM_uint32 major, minor;
        major = gss_inquire_cred(&minor, handle, name.ref(), NULL, NULL, NULL);
        if(GSS_ERROR(major))
            throw GSSError(major, minor, 0, "unable to inquire");
    }

    EPICS_NOT_COPYABLE(GSSCred)
};

struct GSSContext {
    gss_ctx_id_t handle;

    // Error from previous iteration.
    // Thrown on the next iteration (after previous error was sent to peer).
    // Idea seen in python-gssapi
    OM_uint32 last_major, last_minor;
    gss_OID last_mech;

    bool done;
    const bool server;

    explicit GSSContext(bool server)
        :handle(GSS_C_NO_CONTEXT)
        ,last_major(0)
        ,last_minor(0)
        ,last_mech(0)
        ,done(false)
        ,server(server)
    {}
    ~GSSContext() {
        clear();
    }
    void clear() {
        OM_uint32 ignore;
        (void)gss_delete_sec_context(&ignore, &handle, NULL);
    }
    gss_ctx_id_t* ref() {
        return &handle;
    }

    bool step(GSSCred& cred, GSSName& target,
              const pvd::shared_vector<const pvd::uint8>& input,
              pvd::shared_vector<const pvd::uint8>& output)
    {
        OM_uint32 major, minor;
        gss_OID mech = 0;

        if(last_major) {
            major = last_major; last_major = 0;
            minor = last_minor; last_minor = 0;
            mech = last_mech; last_mech = 0;
            throw GSSError(major, minor, mech, "client iteration");
        }

        gss_buffer_desc input_token = GSS_C_EMPTY_BUFFER;
        GSSBuffer output_token;

        if(!input.empty()) {
            input_token.length = input.size();
            input_token.value = const_cast<pvd::uint8*>(input.data());
        }

        if(!server) {
            //client

            major = gss_init_sec_context(&minor,
                                         cred.handle,
                                         &handle,
                                         target.handle,
                                         GSS_C_NO_OID, // allow default mech
                                         GSS_C_MUTUAL_FLAG | GSS_C_CONF_FLAG | GSS_C_INTEG_FLAG,
                                         0, // req time
                                         NULL,
                                         &input_token,
                                         &mech,
                                         output_token.ref(),
                                         NULL,
                                         NULL);

        } else {
            //server
            major = gss_accept_sec_context(&minor,
                                           &handle,
                                           cred.handle,
                                           &input_token,
                                           GSS_C_NO_CHANNEL_BINDINGS,
                                           target.ref(),
                                           NULL,
                                           output_token.ref(),
                                           NULL, NULL, NULL);
        }
        if(output_token->length) {
            pvd::shared_vector<pvd::uint8> temp(output_token->length);
            memcpy(temp.data(), output_token->value, output_token->length);
            output = pvd::freeze(temp);
        }
        bool send = !output.empty() || (major & GSS_S_CONTINUE_NEEDED);
        LOG(pva::logLevelDebug, "KRB5 %s step %x:%x len %u%s%s\n",
            server ? "server" : "client", major, minor, output_token->length,
            send?" send":"", GSS_ERROR(major)?" error":"");
        if(GSS_ERROR(major)) {
            if(send) {
                last_major = major;
                last_minor = minor;
                last_mech = mech;
            } else {
                throw GSSError(major, minor, mech, "step()");
            }
        } else if(major==GSS_S_COMPLETE) {
            done = true;
        }
        return send;
    }

    EPICS_NOT_COPYABLE(GSSContext)
};

void errlogGSS(const GSSError& err, const char *fmt, ...) EPICS_PRINTF_STYLE(2,3);

void errlogGSS(const GSSError& err, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    errlogPrintf("GSS Error: ");
    errlogVprintf(fmt, args);
    va_end(args);

    OM_uint32 junk, state;

    GSSBuffer msg;

    state=0;
    do {
        if(gss_display_status(&junk, err.major, GSS_C_GSS_CODE, err.mech, &state, msg.ref()) != GSS_S_COMPLETE) break;
        errlogPrintf("GSS Major: %.*s\n", (int)msg->length, (char*)msg->value);
    } while(state!=0);

    if(err.minor==GSS_S_COMPLETE) return;

    state=0;
    do {
        if(gss_display_status(&junk, err.minor, GSS_C_MECH_CODE, err.mech, &state, msg.ref()) != GSS_S_COMPLETE) break;
        errlogPrintf("GSS Minor: %.*s\n", (int)msg->length, (char*)msg->value);
    } while(state!=0);
}

pvd::StructureConstPtr krbMsg(pvd::FieldBuilder::begin()
                              ->addArray("payload", pvd::pvUByte) // context message
                              ->add("hostname", pvd::pvString)    // canonical host name (according to peer)
                              ->add("domainname", pvd::pvString)
                              ->createStructure());

struct KerbSession : public pva::AuthenticationSession
{
    mutable epicsMutex mutex;

    GSSContext context;
    GSSCred cred;
    GSSName peername;

    pva::PeerInfo::shared_pointer peer;
    const pva::AuthenticationPluginControl::weak_pointer control;

    const bool server;

    KerbSession(bool server,
                const std::tr1::shared_ptr<pva::PeerInfo>& peer,
                const pva::AuthenticationPluginControl::shared_pointer& control)
        :context(server)
        ,peer(peer)
        ,control(control)
        ,server(server)
    {
        GSSName myname; // use whatever is default
        cred.acquire(myname, server ? GSS_C_ACCEPT : GSS_C_INITIATE);
        cred.name(myname); // find actual name
        LOG(pva::logLevelDebug, "KRB5 cred %s", myname.display().c_str());
    }
    virtual ~KerbSession() {}

    virtual void messageReceived(pvd::PVStructure::const_shared_pointer const & data) OVERRIDE FINAL
    {
        pvd::shared_vector<const pvd::uint8> input, output;
        LOG(pva::logLevelDebug, "KRB5 messageReceived()");

        if(data) {
            pvd::PVUByteArray::const_shared_pointer payload(data->getSubField<pvd::PVUByteArray>("payload"));
            if(payload)
                input = payload->view();
        }

        bool send;
        pva::PeerInfo::shared_pointer done;
        try {
            Guard G(mutex);

            if(!server && data && peername.handle==GSS_C_NO_NAME) {
                /*TODO: Using the server provided names is questionable,
                 *      though seemingly not so bad.  A dishonest server
                 *      would still need a valid service ticket in this domain.
                 *      One compromised server could impersonate any other.
                 *      Though why this would be necesary given the nature of
                 *      PVA searches isn't clear.
                 *      Maybe Better to reverse DNS lookup?
                 */
                pvd::PVString::const_shared_pointer hostname(data->getSubField<pvd::PVString>("hostname"));
                pvd::PVString::const_shared_pointer domainname(data->getSubField<pvd::PVString>("domainname"));
                if(hostname && domainname) {
                    std::ostringstream strm;
                    // TODO: user "pva/" service name.
                    //       configuration of non-default realm.
                    //       override domain name?
                    strm<<"host/"<<hostname->get()<<'.'<<domainname->get()<<"@";
                    peername.import(strm.str(), GSS_KRB5_NT_PRINCIPAL_NAME);
                    //peername.cannonicalize(gss_mech_krb5);
                    LOG(pva::logLevelDebug, "KRB5 target %s", peername.display().c_str());
                }

            }

            send = context.step(cred, peername, input, output);
            if(context.done) {
                done.swap(peer);
                done->account = peername.display();
                // TODO: split out realm, and verify that this is actually kerberos (or change plugin name to gss)
                // done->realm = ;
            }

        }catch(GSSError& err){
            errlogGSS(err, "messageReceived()");
            return;
        }

        if(!send && !done)
            return; // nothing to do

        pva::AuthenticationPluginControl::shared_pointer ctrl(control);
        if(!ctrl) return; // dead connection?

        if(done) {
            LOG(pva::logLevelDebug, "KRB5 messageReceived() done\n");
            ctrl->authenticationCompleted(pvd::Status(), done);

        }
        if(send) {
            pvd::PVStructure::shared_pointer tosend(krbMsg->build());
            tosend->getSubFieldT<pvd::PVUByteArray>("payload")->replace(output);

            LOG(pva::logLevelDebug, "KRB5 messageReceived() sending\n");
            ctrl->sendSecurityPluginMessage(tosend);
        }
    }

    /** For client plugins only.  Notification that server has declared the exchange complete.
     * @param status Check Status::isSuccess()
     * @param peer Final information about pe
     */
    virtual void authenticationComplete(const pvd::Status& status) OVERRIDE FINAL
    {
        LOG(pva::logLevelDebug, "KRB5 authenticationComplete()");
    }
};

struct KerbPlugin : public pva::AuthenticationPlugin
{
    const bool server;

    explicit KerbPlugin(bool server) :server(server) {}
    virtual ~KerbPlugin() {}

    virtual std::tr1::shared_ptr<pva::AuthenticationSession> createSession(
        const std::tr1::shared_ptr<pva::PeerInfo>& peer,
        std::tr1::shared_ptr<pva::AuthenticationPluginControl> const & control,
        pvd::PVStructure::shared_pointer const & data) OVERRIDE FINAL
    {
        pva::AuthenticationSession::shared_pointer sess;
        try {
            sess.reset(new KerbSession(server, peer, control));

            if(server) {
                LOG(pva::logLevelDebug, "KRB5 createSession() proc %p", data.get());
                pvd::PVStructurePtr msg(krbMsg->build());

                std::vector<char> buffer(256u);

                gethostname(&buffer[0], buffer.size());
                buffer[buffer.size()-1] = '\0';
                msg->getSubFieldT<pvd::PVString>("hostname")->put(std::string(&buffer[0]));

                //(void)getdomainname(&buffer[0], buffer.size());
                strcpy(&buffer[0], "example");
                buffer[buffer.size()-1] = '\0';
                msg->getSubFieldT<pvd::PVString>("domainname")->put(std::string(&buffer[0]));

                control->sendSecurityPluginMessage(msg);
            }
        }catch(GSSError& err){
            errlogGSS(err, "createSession()");
            control->authenticationCompleted(pvd::Status::error("GSS Error"), pva::PeerInfo::shared_pointer());
        }
        return sess;
    }
};

} // namespace

namespace epics {
namespace pvAccess {

void krb5authRegistrar(AuthenticationRegistry& servers, AuthenticationRegistry& clients)
{
    {
        KerbPlugin::shared_pointer plugin(new KerbPlugin(false));
        clients.add(1, "krb5", plugin);
    }
    {
        KerbPlugin::shared_pointer plugin(new KerbPlugin(true));
        servers.add(1, "krb5", plugin);
    }
}

}
}
