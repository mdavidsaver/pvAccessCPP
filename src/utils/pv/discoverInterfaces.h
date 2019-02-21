#ifndef DISCOVERINTERFACES_H
#define DISCOVERINTERFACES_H

#include <vector>

#include <osiSock.h>
#include <shareLib.h>

namespace epics {
namespace pvAccess {

typedef std::vector<osiSockAddr> InetAddrVector;

struct epicsShareClass ifaceNode {
    osiSockAddr addr, //!< Our address
                peer, //!< point to point peer
                bcast,//!< sub-net broadcast address
                mask; //!< Net mask
    bool loopback,
         validP2P, //!< true if peer has been set.
         validBcast; //!< true if bcast and mask have been set
    ifaceNode();
    ~ifaceNode();
};
typedef std::vector<ifaceNode> IfaceNodeVector;
epicsShareFunc int discoverInterfaces(IfaceNodeVector &list, SOCKET socket, const osiSockAddr *pMatchAddr = 0);


}} // namespace epics::pvAccess

#endif // DISCOVERINTERFACES_H
