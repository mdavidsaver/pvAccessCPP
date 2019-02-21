#ifndef DISCOVERINTERFACES_H
#define DISCOVERINTERFACES_H

#include <vector>

#include <osiSock.h>
#include <shareLib.h>

namespace epics {
namespace pvAccess {

typedef std::vector<osiSockAddr> InetAddrVector;

//! Information about a single address of a single network interface
struct epicsShareClass ifaceNode {
    osiSockAddr addr, //!< Our address
                peer, //!< point to point peer
                bcast,//!< sub-net broadcast address
                mask; //!< Net mask
    bool loopback, //!< true if this is a loopback interface
         validP2P, //!< true if peer has been set.
         validBcast; //!< true if bcast and mask have been set
    ifaceNode();
    ~ifaceNode();
};
typedef std::vector<ifaceNode> IfaceNodeVector;

/** Inspect the host network configuration.
 *
 * @param list Any network interfaces found are appended to this vector (which is never cleared).
 * @param pMatchAddr If !NULL, only matching interfaces (if any) is appended.
 *        NULL is shorthand for a wildcard match on 0.0.0.0 (aka. INADDR_ANY).
 * @param matchLoopback Whether the loopback interface should be appended.
 * @returns 0 on success, even if no entries are found or appended.
 *          Returns a negative number if inspection was not possible.
 *
 * Pseudo-code for the matching process is as follows:
 *
 @code
 *   IfaceNodeVector& list;
 *   for(const auto& iface : all_ifaces) {
 *     if(!matchLoopback && iface.loopback)
 *       continue; //skip
 *     if(matchAddr!=INADDR_ANY && matchAddr!=iface.addr)
 *       continue; //skip
 *     list.push_back(iface);
 *   }
 @endcode
 */
epicsShareFunc int discoverInterfaces(IfaceNodeVector &list,
                                      const osiSockAddr *pMatchAddr = 0,
                                      bool matchLoopback = false);


}} // namespace epics::pvAccess

#endif // DISCOVERINTERFACES_H
