/**
 * Copyright - See the COPYRIGHT that is included with this distribution.
 * pvAccessCPP is distributed subject to a Software License Agreement found
 * in file LICENSE that is included with this distribution.
 */

#ifndef BLOCKINGTCP_H_
#define BLOCKINGTCP_H_

#include <set>
#include <map>
#include <deque>

#ifdef epicsExportSharedSymbols
#   define blockingTCPEpicsExportSharedSymbols
#   undef epicsExportSharedSymbols
#endif

#include <shareLib.h>
#include <osdSock.h>
#include <osiSock.h>
#include <epicsTime.h>
#include <epicsThread.h>

#include <pv/byteBuffer.h>
#include <pv/pvType.h>
#include <pv/lock.h>
#include <pv/timer.h>
#include <pv/event.h>

#ifdef blockingTCPEpicsExportSharedSymbols
#   define epicsExportSharedSymbols
#	undef blockingTCPEpicsExportSharedSymbols
#endif

#include <pv/pvaConstants.h>
#include <pv/remote.h>
#include <pv/transportRegistry.h>
#include <pv/introspectionRegistry.h>
#include <pv/namedLockPattern.h>
#include <pv/inetAddressUtil.h>

namespace epics {
    namespace pvAccess {

        /**
         * Channel Access TCP connector.
         * @author <a href="mailto:matej.sekoranjaATcosylab.com">Matej Sekoranja</a>
         * @version $Id: BlockingTCPConnector.java,v 1.1 2010/05/03 14:45:47 mrkraimer Exp $
         */
        class BlockingTCPConnector : public Connector {
        public:
        	POINTER_DEFINITIONS(BlockingTCPConnector);

            BlockingTCPConnector(Context::shared_pointer const & context, int receiveBufferSize,
                    float beaconInterval);

            virtual ~BlockingTCPConnector();

            virtual Transport::shared_pointer connect(TransportClient::shared_pointer const & client,
                    std::auto_ptr<ResponseHandler>& responseHandler, osiSockAddr& address,
                    epics::pvData::int8 transportRevision, epics::pvData::int16 priority);
        private:
            /**
             * Lock timeout
             */
            static const int LOCK_TIMEOUT = 20*1000; // 20s

            /**
             * Context instance.
             */
            Context::weak_pointer _context;

            /**
             * named lock
             */
            NamedLockPattern<const osiSockAddr*, comp_osiSockAddrPtr> _namedLocker;

            /**
             * Receive buffer size.
             */
            int _receiveBufferSize;

            /**
             * Heartbeat interval.
             */
            float _heartbeatInterval;

            /**
             * Tries to connect to the given address.
             * @param[in] address
             * @param[in] tries
             * @return the SOCKET
             * @throws IOException
             */
            SOCKET tryConnect(osiSockAddr& address, int tries);

        };

        class ResponseHandlerFactory
        {
            public:
        	POINTER_DEFINITIONS(ResponseHandlerFactory);
            
            virtual ~ResponseHandlerFactory() {};

            virtual std::auto_ptr<ResponseHandler> createResponseHandler() = 0;
        };
        
        /**
         * Channel Access Server TCP acceptor.
         * @author <a href="mailto:matej.sekoranjaATcosylab.com">Matej Sekoranja</a>
         * @version $Id: BlockingTCPAcceptor.java,v 1.1 2010/05/03 14:45:42 mrkraimer Exp $
         */
        class BlockingTCPAcceptor {
        public:
        	POINTER_DEFINITIONS(BlockingTCPAcceptor);

            /**
             * @param context
             * @param port
             * @param receiveBufferSize
             * @throws PVAException
             */
            BlockingTCPAcceptor(Context::shared_pointer const & context,
                                ResponseHandlerFactory::shared_pointer const & responseHandlerFactory,
                                int port, int receiveBufferSize);

            virtual ~BlockingTCPAcceptor();

            void handleEvents();

            /**
             * Bind socket address.
             * @return bind socket address, <code>null</code> if not binded.
             */
            const osiSockAddr* getBindAddress() {
                return &_bindAddress;
            }

            /**
             * Destroy acceptor (stop listening).
             */
            void destroy();

        private:
            /**
             * Context instance.
             */
            Context::shared_pointer _context;
            
            /**
             * ResponseHandler factory.
             */
            ResponseHandlerFactory::shared_pointer _responseHandlerFactory;

            /**
             * Bind server socket address.
             */
            osiSockAddr _bindAddress;

            /**
             * Server socket channel.
             */
            SOCKET _serverSocketChannel;

            /**
             * Receive buffer size.
             */
            int _receiveBufferSize;

            /**
             * Destroyed flag.
             */
            bool _destroyed;
            
            epics::pvData::Mutex _mutex;

            epicsThreadId _threadId;

            /**
             * Initialize connection acception.
             * @return port where server is listening
             */
            int initialize(unsigned short port);

            /**
             * Validate connection by sending a validation message request.
             * @return <code>true</code> on success.
             */
            bool validateConnection(Transport::shared_pointer const & transport, const char* address);

            static void handleEventsRunner(void* param);
        };

    }
}

#endif /* BLOCKINGTCP_H_ */