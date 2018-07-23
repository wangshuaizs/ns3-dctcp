/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Copyright (c) 2007 Georgia Tech Research Corporation
 * Copyright (c) 2010 Adrian Sai-wah Tam
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation;
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Author: Adrian Sai-wah Tam <adrian.sw.tam@gmail.com>
 */
#ifndef TCP_SOCKET_BASE_H
#define TCP_SOCKET_BASE_H

#include <stdint.h>
#include <queue>
#include "ns3/callback.h"
#include "ns3/traced-value.h"
#include "ns3/tcp-socket.h"
#include "ns3/ptr.h"
#include "ns3/ipv4-address.h"
#include "ns3/ipv4-header.h"
#include "ns3/ipv4-interface.h"
#include "ns3/ipv6-header.h"
#include "ns3/ipv6-interface.h"
#include "ns3/event-id.h"
#include "tcp-tx-buffer.h"
#include "tcp-rx-buffer.h"
#include "rtt-estimator.h"

namespace ns3 {

class Ipv4EndPoint;
class Ipv6EndPoint;
class Node;
class Packet;
class TcpL4Protocol;
class TcpHeader;
class TcpCongestionOps;

/**
 * \ingroup tcp
 *
 * \brief Helper class to store RTT measurements
 */
class RttHistory
{
public:
  /**
   * \brief Constructor - builds an RttHistory with the given parameters
   * \param s First sequence number in packet sent
   * \param c Number of bytes sent
   * \param t Time this one was sent
   */
  RttHistory (SequenceNumber32 s, uint32_t c, Time t);
  /**
   * \brief Copy constructor
   * \param h the object to copy
   */
  RttHistory (const RttHistory& h); // Copy constructor
public:
  SequenceNumber32  seq;  //!< First sequence number in packet sent
  uint32_t        count;  //!< Number of bytes sent
  Time            time;   //!< Time this one was sent
  bool            retx;   //!< True if this has been retransmitted
};

/// Container for RttHistory objects
typedef std::deque<RttHistory> RttHistory_t;

/**
 * \brief Data structure that records the congestion state of a connection
 *
 * In this data structure, basic informations that should be passed between
 * socket and the congestion control algorithm are saved. Through the code,
 * it will be referred as Transmission Control Block (TCB), but there are some
 * differencies. In the RFCs, the TCB contains all the variables that defines
 * a connection, while we preferred to maintain in this class only the values
 * that should be exchanged between socket and other parts, like congestion
 * control algorithms.
 *
 */
class TcpSocketState : public Object
{
public:
  /**
   * Get the type ID.
   * \brief Get the type ID.
   * \return the object TypeId
   */
  static TypeId GetTypeId (void);

  TcpSocketState ();

  /**
   * \brief Copy constructor.
   * \param other object to copy.
   */
  TcpSocketState (const TcpSocketState &other);

  /**
   * \brief Definition of the Congestion state machine
   *
   * The design of this state machine is taken from Linux v4.0, but it has been
   * maintained in the Linux mainline from ages. It basically avoids to maintain
   * a lot of boolean variables, and it allows to check the transitions from
   * different algorithm in a cleaner way.
   *
   * These states represent the situation from a congestion control point of view:
   * in fact, apart the CA_OPEN state, the other states represent a situation in
   * which there is a congestion, and different actions should be taken,
   * depending on the case.
   *
   */
  typedef enum
  {
    CA_OPEN,      /**< Normal state, no dubious events */
    CA_DISORDER,  /**< In all the respects it is "Open",
                    *  but requires a bit more attention. It is entered when
                    *  we see some SACKs or dupacks. It is split of "Open" */
    CA_CWR,       /**< cWnd was reduced due to some Congestion Notification event.
                    *  It can be ECN, ICMP source quench, local device congestion.
                    *  Not used in NS-3 right now. */
    CA_RECOVERY,  /**< CWND was reduced, we are fast-retransmitting. */
    CA_LOSS,      /**< CWND was reduced due to RTO timeout or SACK reneging. */
    CA_LAST_STATE /**< Used only in debug messages */
  } TcpCongState_t;

  /**
   * \ingroup tcp
   * TracedValue Callback signature for TcpCongState_t
   *
   * \param [in] oldValue original value of the traced variable
   * \param [in] newValue new value of the traced variable
   */
  typedef void (* TcpCongStatesTracedValueCallback)(const TcpCongState_t oldValue,
                                                    const TcpCongState_t newValue);

  /**
   * \brief Literal names of TCP states for use in log messages
   */
  static const char* const TcpCongStateName[TcpSocketState::CA_LAST_STATE];

  // Congestion control
  TracedValue<uint32_t>  m_cWnd;            //!< Congestion window
  TracedValue<uint32_t>  m_ssThresh;        //!< Slow start threshold
  uint32_t               m_initialCWnd;     //!< Initial cWnd value
  uint32_t               m_initialSsThresh; //!< Initial Slow Start Threshold value

  // Segment
  uint32_t               m_segmentSize;     //!< Segment size
  SequenceNumber32       m_lastAckedSeq;    //!< Last sequence ACKed

  TracedValue<TcpCongState_t> m_congState;    //!< State in the Congestion state machine
  TracedValue<SequenceNumber32> m_highTxMark; //!< Highest seqno ever sent, regardless of ReTx
  TracedValue<SequenceNumber32> m_nextTxSequence; //!< Next seqnum to be sent (SND.NXT), ReTx pushes it back

  /**
   * \brief Get cwnd in segments rather than bytes
   *
   * \return Congestion window in segments
   */
  uint32_t GetCwndInSegments () const
  {
    return m_cWnd / m_segmentSize;
  }

  /**
   * \brief Get slow start thresh in segments rather than bytes
   *
   * \return Slow start threshold in segments
   */
  uint32_t GetSsThreshInSegments () const
  {
    return m_ssThresh / m_segmentSize;
  }
};

/**
 * \ingroup socket
 * \ingroup tcp
 *
 * \brief A base class for implementation of a stream socket using TCP.
 *
 * This class contains the essential components of TCP, as well as a sockets
 * interface for upper layers to call. This serves as a base for other TCP
 * functions where the sliding window mechanism is handled here. This class
 * provides connection orientation and sliding window flow control. Part of
 * this class is modified from the original NS-3 TCP socket implementation
 * (TcpSocketImpl) by Raj Bhattacharjea <raj.b@gatech.edu> of Georgia Tech.
 *
 * For IPv4 packets, the TOS set for the socket is used. The Bind and Connect
 * operations set the TOS for the socket to the value specified in the provided
 * address. A SocketIpTos tag is only added to the packet if the resulting
 * TOS is non-null.
 * Each packet is assigned the priority set for the socket. Setting a TOS
 * for a socket also sets a priority for the socket (according to the
 * Socket::IpTos2Priority function). A SocketPriority tag is only added to the
 * packet if the priority is non-null.
 *
 * Congestion state machine
 * ---------------------------
 *
 * The socket maintains two state machines; the TCP one, and another called
 * "Congestion state machine", which keeps track of the phase we are in. Currently,
 * ns-3 manages the states:
 *
 * - CA_OPEN
 * - CA_DISORDER
 * - CA_RECOVERY
 * - CA_LOSS
 *
 * Another one (CA_CWR) is present but not used. For more information, see
 * the TcpCongState_t documentation.
 *
 * Congestion control interface
 * ---------------------------
 *
 * Congestion control, unlike older releases of ns-3, has been splitted from
 * TcpSocketBase. In particular, each congestion control is now a subclass of
 * the main TcpCongestionOps class. Switching between congestion algorithm is
 * now a matter of setting a pointer into the TcpSocketBase class.
 *
 * The variables needed to congestion control classes to operate correctly have
 * been moved inside the TcpSocketState class. It contains information on the
 * congestion window, slow start threshold, segment size and the state of the
 * Congestion state machine.
 *
 * To track the trace inside the TcpSocketState class, a "forward" technique is
 * used, which consists in chaining callbacks from TcpSocketState to TcpSocketBase
 * (see for example cWnd trace source).
 *
 * Fast retransmit
 * ---------------------------
 *
 * The fast retransmit enhancement is introduced in RFC 2581 and updated in
 * RFC 5681. It basically reduces the time a sender waits before retransmitting
 * a lost segment, through the assumption that if it receives a certain number
 * of duplicate ACKs, a segment has been lost and it can be retransmitted.
 * Usually it is coupled with the Limited Transmit algorithm, defined in
 * RFC 3042.
 *
 * In ns-3, these algorithms are included in this class, and it is implemented inside
 * the ReceivedAck method. The attribute which manages the number of dup ACKs
 * necessary to start the fast retransmit algorithm is named "ReTxThreshold",
 * and its default value is 3, while the Limited Transmit one can be enabled
 * by setting the attribute "LimitedTransmit" to true.
 *
 * Fast recovery
 * --------------------------
 *
 * The fast recovery algorithm is introduced RFC 2001, and it basically
 * avoids to reset cWnd to 1 segment after sensing a loss on the channel. Instead,
 * the slow start threshold is halved, and the cWnd is set equal to such value,
 * plus segments for the cWnd inflation.
 *
 * The algorithm is implemented in the ReceivedAck method.
 *
 */
class TcpSocketBase : public TcpSocket
{
public:
  /**
   * Get the type ID.
   * \brief Get the type ID.
   * \return the object TypeId
   */
  static TypeId GetTypeId (void);

  /**
   * \brief Get the instance TypeId
   * \return the instance TypeId
   */
  virtual TypeId GetInstanceTypeId () const;

  friend class TcpGeneralTest;

  /**
   * Create an unbound TCP socket
   */
  TcpSocketBase (void);

  /**
   * Clone a TCP socket, for use upon receiving a connection request in LISTEN state
   *
   * \param sock the original Tcp Socket
   */
  TcpSocketBase (const TcpSocketBase& sock);
  virtual ~TcpSocketBase (void);

  // Set associated Node, TcpL4Protocol, RttEstimator to this socket

  /**
   * \brief Set the associated node.
   * \param node the node
   */
  virtual void SetNode (Ptr<Node> node);

  /**
   * \brief Set the associated TCP L4 protocol.
   * \param tcp the TCP L4 protocol
   */
  virtual void SetTcp (Ptr<TcpL4Protocol> tcp);

  /**
   * \brief Set the associated RTT estimator.
   * \param rtt the RTT estimator
   */
  virtual void SetRtt (Ptr<RttEstimator> rtt);

  /**
   * \brief Sets the Minimum RTO.
   * \param minRto The minimum RTO.
   */
  void SetMinRto (Time minRto);

  /**
   * \brief Get the Minimum RTO.
   * \return The minimum RTO.
   */
  Time GetMinRto (void) const;

  /**
   * \brief Sets the Clock Granularity (used in RTO calcs).
   * \param clockGranularity The Clock Granularity
   */
  void SetClockGranularity (Time clockGranularity);

  /**
   * \brief Get the Clock Granularity (used in RTO calcs).
   * \return The Clock Granularity.
   */
  Time GetClockGranularity (void) const;

  /**
   * \brief Get a pointer to the Tx buffer
   * \return a pointer to the tx buffer
   */
  Ptr<TcpTxBuffer> GetTxBuffer (void) const;

  /**
   * \brief Get a pointer to the Rx buffer
   * \return a pointer to the rx buffer
   */
  Ptr<TcpRxBuffer> GetRxBuffer (void) const;

  /**
   * \brief Callback pointer for cWnd trace chaining
   */
  TracedCallback<uint32_t, uint32_t> m_cWndTrace;

  /**
   * \brief Callback pointer for ssTh trace chaining
   */
  TracedCallback<uint32_t, uint32_t> m_ssThTrace;

  /**
   * \brief Callback pointer for congestion state trace chaining
   */
  TracedCallback<TcpSocketState::TcpCongState_t, TcpSocketState::TcpCongState_t> m_congStateTrace;

  /**
   * \brief Callback pointer for high tx mark chaining
   */
  TracedCallback <SequenceNumber32, SequenceNumber32> m_highTxMarkTrace;

  /**
   * \brief Callback pointer for next tx sequence chaining
   */
  TracedCallback<SequenceNumber32, SequenceNumber32> m_nextTxSequenceTrace;

  /**
   * \brief Callback function to hook to TcpSocketState congestion window
   * \param oldValue old cWnd value
   * \param newValue new cWnd value
   */
  void UpdateCwnd (uint32_t oldValue, uint32_t newValue);

  /**
   * \brief Callback function to hook to TcpSocketState slow start threshold
   * \param oldValue old ssTh value
   * \param newValue new ssTh value
   */
  void UpdateSsThresh (uint32_t oldValue, uint32_t newValue);

  /**
   * \brief Callback function to hook to TcpSocketState congestion state
   * \param oldValue old congestion state value
   * \param newValue new congestion state value
   */
  void UpdateCongState (TcpSocketState::TcpCongState_t oldValue,
                        TcpSocketState::TcpCongState_t newValue);

  /**
   * \brief Callback function to hook to TcpSocketState high tx mark
   * \param oldValue old high tx mark
   * \param newValue new high tx mark
   */
  void UpdateHighTxMark (SequenceNumber32 oldValue, SequenceNumber32 newValue);

  /**
   * \brief Callback function to hook to TcpSocketState next tx sequence
   * \param oldValue old nextTxSeq value
   * \param newValue new nextTxSeq value
   */
  void UpdateNextTxSequence (SequenceNumber32 oldValue, SequenceNumber32 newValue);

  /**
   * \brief Install a congestion control algorithm on this socket
   *
   * \param algo Algorithm to be installed
   */
  void SetCongestionControlAlgorithm (Ptr<TcpCongestionOps> algo);

  // Necessary implementations of null functions from ns3::Socket
  virtual enum SocketErrno GetErrno (void) const;    // returns m_errno
  virtual enum SocketType GetSocketType (void) const; // returns socket type
  virtual Ptr<Node> GetNode (void) const;            // returns m_node
  virtual int Bind (void);    // Bind a socket by setting up endpoint in TcpL4Protocol
  virtual int Bind6 (void);    // Bind a socket by setting up endpoint in TcpL4Protocol
  virtual int Bind (const Address &address);         // ... endpoint of specific addr or port
  virtual int Connect (const Address &address);      // Setup endpoint and call ProcessAction() to connect
  virtual int Listen (void);  // Verify the socket is in a correct state and call ProcessAction() to listen
  virtual int Close (void);   // Close by app: Kill socket upon tx buffer emptied
  virtual int ShutdownSend (void);    // Assert the m_shutdownSend flag to prevent send to network
  virtual int ShutdownRecv (void);    // Assert the m_shutdownRecv flag to prevent forward to app
  virtual int Send (Ptr<Packet> p, uint32_t flags);  // Call by app to send data to network
  virtual int SendTo (Ptr<Packet> p, uint32_t flags, const Address &toAddress); // Same as Send(), toAddress is insignificant
  virtual Ptr<Packet> Recv (uint32_t maxSize, uint32_t flags); // Return a packet to be forwarded to app
  virtual Ptr<Packet> RecvFrom (uint32_t maxSize, uint32_t flags, Address &fromAddress); // ... and write the remote address at fromAddress
  virtual uint32_t GetTxAvailable (void) const; // Available Tx buffer size
  virtual uint32_t GetRxAvailable (void) const; // Available-to-read data size, i.e. value of m_rxAvailable
  virtual int GetSockName (Address &address) const; // Return local addr:port in address
  virtual int GetPeerName (Address &address) const;
  virtual void BindToNetDevice (Ptr<NetDevice> netdevice); // NetDevice with my m_endPoint

  /**
   * TracedCallback signature for tcp packet transmission or reception events.
   *
   * \param [in] packet The packet.
   * \param [in] ipv4
   * \param [in] interface
   */
  typedef void (* TcpTxRxTracedCallback)(const Ptr<const Packet> packet, const TcpHeader& header,
                                         const Ptr<const TcpSocketBase> socket);

protected:
  // Implementing ns3::TcpSocket -- Attribute get/set
  // inherited, no need to doc

  virtual void     SetSndBufSize (uint32_t size);
  virtual uint32_t GetSndBufSize (void) const;
  virtual void     SetRcvBufSize (uint32_t size);
  virtual uint32_t GetRcvBufSize (void) const;
  virtual void     SetSegSize (uint32_t size);
  virtual uint32_t GetSegSize (void) const;
  virtual void     SetInitialSSThresh (uint32_t threshold);
  virtual uint32_t GetInitialSSThresh (void) const;
  virtual void     SetInitialCwnd (uint32_t cwnd);
  virtual uint32_t GetInitialCwnd (void) const;
  virtual void     SetConnTimeout (Time timeout);
  virtual Time     GetConnTimeout (void) const;
  virtual void     SetSynRetries (uint32_t count);
  virtual uint32_t GetSynRetries (void) const;
  virtual void     SetDataRetries (uint32_t retries);
  virtual uint32_t GetDataRetries (void) const;
  virtual void     SetDelAckTimeout (Time timeout);
  virtual Time     GetDelAckTimeout (void) const;
  virtual void     SetDelAckMaxCount (uint32_t count);
  virtual uint32_t GetDelAckMaxCount (void) const;
  virtual void     SetTcpNoDelay (bool noDelay);
  virtual bool     GetTcpNoDelay (void) const;
  virtual void     SetPersistTimeout (Time timeout);
  virtual Time     GetPersistTimeout (void) const;
  virtual bool     SetAllowBroadcast (bool allowBroadcast);
  virtual bool     GetAllowBroadcast (void) const;



  // Helper functions: Connection set up

  /**
   * \brief Common part of the two Bind(), i.e. set callback and remembering local addr:port
   *
   * \returns 0 on success, -1 on failure
   */
  int SetupCallback (void);

  /**
   * \brief Perform the real connection tasks: Send SYN if allowed, RST if invalid
   *
   * \returns 0 on success
   */
  int DoConnect (void);

  /**
   * \brief Schedule-friendly wrapper for Socket::NotifyConnectionSucceeded()
   */
  void ConnectionSucceeded (void);

  /**
   * \brief Configure the endpoint to a local address. Called by Connect() if Bind() didn't specify one.
   *
   * \returns 0 on success
   */
  int SetupEndpoint (void);

  /**
   * \brief Configure the endpoint v6 to a local address. Called by Connect() if Bind() didn't specify one.
   *
   * \returns 0 on success
   */
  int SetupEndpoint6 (void);

  /**
   * \brief Complete a connection by forking the socket
   *
   * This function is called only if a SYN received in LISTEN state. After
   * TcpSocketBase cloned, allocate a new end point to handle the incoming
   * connection and send a SYN+ACK to complete the handshake.
   *
   * \param p the packet triggering the fork
   * \param tcpHeader the TCP header of the triggering packet
   * \param fromAddress the address of the remote host
   * \param toAddress the address the connection is directed to
   */
  virtual void CompleteFork (Ptr<Packet> p, const TcpHeader& tcpHeader,
                             const Address& fromAddress, const Address& toAddress);



  // Helper functions: Transfer operation

  /**
   * \brief Called by the L3 protocol when it received a packet to pass on to TCP.
   *
   * \param packet the incoming packet
   * \param header the packet's IPv4 header
   * \param port the remote port
   * \param incomingInterface the incoming interface
   */
  void ForwardUp (Ptr<Packet> packet, Ipv4Header header, uint16_t port, Ptr<Ipv4Interface> incomingInterface);

  /**
   * \brief Called by the L3 protocol when it received a packet to pass on to TCP.
   *
   * \param packet the incoming packet
   * \param header the packet's IPv6 header
   * \param port the remote port
   * \param incomingInterface the incoming interface
   */
  void ForwardUp6 (Ptr<Packet> packet, Ipv6Header header, uint16_t port, Ptr<Ipv6Interface> incomingInterface);

  /**
   * \brief Called by TcpSocketBase::ForwardUp{,6}().
   *
   * Get a packet from L3. This is the real function to handle the
   * incoming packet from lower layers. This is
   * wrapped by ForwardUp() so that this function can be overloaded by daughter
   * classes.
   *
   * \param packet the incoming packet
   * \param fromAddress the address of the sender of packet
   * \param toAddress the address of the receiver of packet (hopefully, us)
   */
  virtual void DoForwardUp (Ptr<Packet> packet, const Address &fromAddress,
                            const Address &toAddress);

  /**
   * \brief Called by the L3 protocol when it received an ICMP packet to pass on to TCP.
   *
   * \param icmpSource the ICMP source address
   * \param icmpTtl the ICMP Time to Live
   * \param icmpType the ICMP Type
   * \param icmpCode the ICMP Code
   * \param icmpInfo the ICMP Info
   */
  void ForwardIcmp (Ipv4Address icmpSource, uint8_t icmpTtl, uint8_t icmpType, uint8_t icmpCode, uint32_t icmpInfo);

  /**
   * \brief Called by the L3 protocol when it received an ICMPv6 packet to pass on to TCP.
   *
   * \param icmpSource the ICMP source address
   * \param icmpTtl the ICMP Time to Live
   * \param icmpType the ICMP Type
   * \param icmpCode the ICMP Code
   * \param icmpInfo the ICMP Info
   */
  void ForwardIcmp6 (Ipv6Address icmpSource, uint8_t icmpTtl, uint8_t icmpType, uint8_t icmpCode, uint32_t icmpInfo);

  /**
   * \brief Send as much pending data as possible according to the Tx window.
   *
   * Note that this function did not implement the PSH flag.
   *
   * \param withAck forces an ACK to be sent
   * \returns true if some data have been sent
   */
  bool SendPendingData (bool withAck = false);

  /**
   * \brief Extract at most maxSize bytes from the TxBuffer at sequence seq, add the
   *        TCP header, and send to TcpL4Protocol
   *
   * \param seq the sequence number
   * \param maxSize the maximum data block to be transmitted (in bytes)
   * \param withAck forces an ACK to be sent
   * \returns the number of bytes sent
   */
  virtual uint32_t SendDataPacket (SequenceNumber32 seq, uint32_t maxSize, bool withAck);

  /**
   * \brief Send a empty packet that carries a flag, e.g. ACK
   *
   * \param flags the packet's flags
   */
  virtual void SendEmptyPacket (uint8_t flags);

  /**
   * \brief Send reset and tear down this socket
   */
  void SendRST (void);

  /**
   * \brief Check if a sequence number range is within the rx window
   *
   * \param head start of the Sequence window
   * \param tail end of the Sequence window
   * \returns true if it is in range
   */
  bool OutOfRange (SequenceNumber32 head, SequenceNumber32 tail) const;


  // Helper functions: Connection close

  /**
   * \brief Close a socket by sending RST, FIN, or FIN+ACK, depend on the current state
   *
   * \returns 0 on success
   */
  int DoClose (void);

  /**
   * \brief Peacefully close the socket by notifying the upper layer and deallocate end point
   */
  void CloseAndNotify (void);

  /**
   * \brief Kill this socket by zeroing its attributes (IPv4)
   *
   * This is a callback function configured to m_endpoint in
   * SetupCallback(), invoked when the endpoint is destroyed.
   */
  void Destroy (void);

  /**
   * \brief Kill this socket by zeroing its attributes (IPv6)
   *
   * This is a callback function configured to m_endpoint in
   * SetupCallback(), invoked when the endpoint is destroyed.
   */
  void Destroy6 (void);

  /**
   * \brief Deallocate m_endPoint and m_endPoint6
   */
  void DeallocateEndPoint (void);

  /**
   * \brief Received a FIN from peer, notify rx buffer
   *
   * \param p the packet
   * \param tcpHeader the packet's TCP header
   */
  void PeerClose (Ptr<Packet> p, const TcpHeader& tcpHeader);

  /**
   * \brief FIN is in sequence, notify app and respond with a FIN
   */
  void DoPeerClose (void);

  /**
   * \brief Cancel all timer when endpoint is deleted
   */
  void CancelAllTimers (void);

  /**
   * \brief Move from CLOSING or FIN_WAIT_2 to TIME_WAIT state
   */
  void TimeWait (void);

  // State transition functions

  /**
   * \brief Received a packet upon ESTABLISHED state.
   *
   * This function is mimicking the role of tcp_rcv_established() in tcp_input.c in Linux kernel.
   *
   * \param packet the packet
   * \param tcpHeader the packet's TCP header
   */
  void ProcessEstablished (Ptr<Packet> packet, const TcpHeader& tcpHeader); // Received a packet upon ESTABLISHED state

  /**
   * \brief Received a packet upon LISTEN state.
   *
   * \param packet the packet
   * \param tcpHeader the packet's TCP header
   * \param fromAddress the source address
   * \param toAddress the destination address
   */
  void ProcessListen (Ptr<Packet> packet, const TcpHeader& tcpHeader,
                      const Address& fromAddress, const Address& toAddress);

  /**
   * \brief Received a packet upon SYN_SENT
   *
   * \param packet the packet
   * \param tcpHeader the packet's TCP header
   */
  void ProcessSynSent (Ptr<Packet> packet, const TcpHeader& tcpHeader);

  /**
   * \brief Received a packet upon SYN_RCVD.
   *
   * \param packet the packet
   * \param tcpHeader the packet's TCP header
   * \param fromAddress the source address
   * \param toAddress the destination address
   */
  void ProcessSynRcvd (Ptr<Packet> packet, const TcpHeader& tcpHeader,
                       const Address& fromAddress, const Address& toAddress);

  /**
   * \brief Received a packet upon CLOSE_WAIT, FIN_WAIT_1, FIN_WAIT_2
   *
   * \param packet the packet
   * \param tcpHeader the packet's TCP header
   */
  void ProcessWait (Ptr<Packet> packet, const TcpHeader& tcpHeader);

  /**
   * \brief Received a packet upon CLOSING
   *
   * \param packet the packet
   * \param tcpHeader the packet's TCP header
   */
  void ProcessClosing (Ptr<Packet> packet, const TcpHeader& tcpHeader);

  /**
   * \brief Received a packet upon LAST_ACK
   *
   * \param packet the packet
   * \param tcpHeader the packet's TCP header
   */
  void ProcessLastAck (Ptr<Packet> packet, const TcpHeader& tcpHeader);

  // Window management

  /**
   * \brief Return count of number of unacked bytes
   * \returns count of number of unacked bytes
   */
  virtual uint32_t UnAckDataCount (void) const;

  /**
   * \brief Return total bytes in flight
   * \returns total bytes in flight
   */
  virtual uint32_t BytesInFlight (void);

  /**
   * \brief Return the max possible number of unacked bytes
   * \returns the max possible number of unacked bytes
   */
  virtual uint32_t Window (void) const;

  /**
   * \brief Return unfilled portion of window
   * \return unfilled portion of window
   */
  virtual uint32_t AvailableWindow (void) const;

  /**
   * \brief The amount of Rx window announced to the peer
   * \param scale indicate if the window should be scaled. True for
   * almost all cases, except when we are sending a SYN
   * \returns size of Rx window announced to the peer
   */
  virtual uint16_t AdvertisedWindowSize (bool scale = true) const;

  /**
   * \brief Update the receiver window (RWND) based on the value of the
   * window field in the header.
   *
   * This method suppresses updates unless one of the following three
   * conditions holds:  1) segment contains new data (advancing the right
   * edge of the receive buffer), 2) segment does not contain new data
   * but the segment acks new data (highest sequence number acked advances),
   * or 3) the advertised window is larger than the current send window
   *
   * \param header TcpHeader from which to extract the new window value
   */
  void UpdateWindowSize (const TcpHeader& header);


  // Manage data tx/rx

  /**
   * \brief Call CopyObject<> to clone me
   * \returns a copy of the socket
   */
  virtual Ptr<TcpSocketBase> Fork (void);

  /**
   * \brief Received an ACK packet
   * \param packet the packet
   * \param tcpHeader the packet's TCP header
   */
  virtual void ReceivedAck (Ptr<Packet> packet, const TcpHeader& tcpHeader);

  /**
   * \brief Recv of a data, put into buffer, call L7 to get it if necessary
   * \param packet the packet
   * \param tcpHeader the packet's TCP header
   */
  virtual void ReceivedData (Ptr<Packet> packet, const TcpHeader& tcpHeader);

  /**
   * \brief Take into account the packet for RTT estimation
   * \param tcpHeader the packet's TCP header
   */
  virtual void EstimateRtt (const TcpHeader& tcpHeader);

  /**
   * \brief Update the RTT history, when we send TCP segments
   *
   * \param seq The sequence number of the TCP segment
   * \param sz The segment's size
   * \param isRetransmission Whether or not the segment is a retransmission
   */

  virtual void UpdateRttHistory (const SequenceNumber32 &seq, uint32_t sz,
                                 bool isRetransmission);

  /**
   * \brief Update buffers w.r.t. ACK
   * \param seq the sequence number
   * \param resetRTO indicates if RTO should be reset
   */
  virtual void NewAck (SequenceNumber32 const& seq, bool resetRTO);

  /**
   * \brief Dupack management
   */
  void DupAck ();

  /**
   * \brief Limited transmit algorithm
   */
  void LimitedTransmit ();

  /**
   * \brief Enter the FastRetransmit, and retransmit the head
   */
  void FastRetransmit ();

  /**
   * \brief Call Retransmit() upon RTO event
   */
  virtual void ReTxTimeout (void);

  /**
   * \brief Halving cwnd and call DoRetransmit()
   */
  virtual void Retransmit (void);

  /**
   * \brief Action upon delay ACK timeout, i.e. send an ACK
   */
  virtual void DelAckTimeout (void);

  /**
   * \brief Timeout at LAST_ACK, close the connection
   */
  virtual void LastAckTimeout (void);

  /**
   * \brief Send 1 byte probe to get an updated window size
   */
  virtual void PersistTimeout (void);

  /**
   * \brief Retransmit the oldest packet
   */
  virtual void DoRetransmit (void);

  /** \brief Add options to TcpHeader
   *
   * Test each option, and if it is enabled on our side, add it
   * to the header
   *
   * \param tcpHeader TcpHeader to add options to
   */
  virtual void AddOptions (TcpHeader& tcpHeader);

  /**
   * \brief Read and parse the Window scale option
   *
   * Read the window scale option (encoded logarithmically) and save it.
   * Per RFC 1323, the value can't exceed 14.
   *
   * \param option Window scale option read from the header
   */
  void ProcessOptionWScale (const Ptr<const TcpOption> option);
  /**
   * \brief Add the window scale option to the header
   *
   * Calculate our factor from the rxBuffer max size, and add it
   * to the header.
   *
   * \param header TcpHeader where the method should add the window scale option
   */
  void AddOptionWScale (TcpHeader& header);

  /**
   * \brief Calculate window scale value based on receive buffer space
   *
   * Calculate our factor from the rxBuffer max size
   *
   * \returns the Window Scale factor
   */
  uint8_t CalculateWScale () const;

  /** \brief Process the timestamp option from other side
   *
   * Get the timestamp and the echo, then save timestamp (which will
   * be the echo value in our out-packets) and save the echoed timestamp,
   * to utilize later to calculate RTT.
   *
   * \see EstimateRtt
   * \param option Option from the segment
   * \param seq Sequence number of the segment
   */
  void ProcessOptionTimestamp (const Ptr<const TcpOption> option,
                               const SequenceNumber32 &seq);
  /**
   * \brief Add the timestamp option to the header
   *
   * Set the timestamp as the lower bits of the Simulator::Now time,
   * and the echo value as the last seen timestamp from the other part.
   *
   * \param header TcpHeader to which add the option to
   */
  void AddOptionTimestamp (TcpHeader& header);

  /**
   * @brief Send Ack packet; add ecn mark if needed
   */
  virtual void SendACK (void);

  /**
   * @brief UpdateEcnState
   * @param tcpHeader the tcp header of current received packet.
   * called in DoForwardUp before other actions.
   * update ECN states according to current received packet.
   */
  virtual void UpdateEcnState (const TcpHeader &tcpHeader);

  /**
   * @brief Get ssthresh after a loss event
   */
  virtual uint32_t GetSsThresh (void);

  /**
   * @brief Congestion avoidance algorithm implementation
   * @param segmentsAcked count of segments acked
   */
  virtual void IncreaseWindow (uint32_t segmentAcked);

  /**
   * @brief determining whether empty packet like pure ACK or control packet should be marked ECT
   * @return true if mark is needed
   */
  virtual bool MarkEmptyPacket (void) const;

  /**
   * \brief Performs a safe subtraction between a and b (a-b)
   *
   * Safe is used to indicate that, if b>a, the results returned is 0.
   *
   * \param a first number
   * \param b second number
   * \return 0 if b>0, (a-b) otherwise
   */
  static uint32_t SafeSubtraction (uint32_t a, uint32_t b);

protected:
  // Counters and events
  EventId           m_retxEvent;       //!< Retransmission event
  EventId           m_lastAckEvent;    //!< Last ACK timeout event
  EventId           m_delAckEvent;     //!< Delayed ACK timeout event
  EventId           m_persistEvent;    //!< Persist event: Send 1 byte to probe for a non-zero Rx window
  EventId           m_timewaitEvent;   //!< TIME_WAIT expiration event: Move this socket to CLOSED state
  uint32_t          m_dupAckCount;     //!< Dupack counter
  uint32_t          m_delAckCount;     //!< Delayed ACK counter
  uint32_t          m_delAckMaxCount;  //!< Number of packet to fire an ACK before delay timeout
  bool              m_noDelay;         //!< Set to true to disable Nagle's algorithm
  uint32_t          m_synCount;        //!< Count of remaining connection retries
  uint32_t          m_synRetries;      //!< Number of connection attempts
  uint32_t          m_dataRetrCount;   //!< Count of remaining data retransmission attempts
  uint32_t          m_dataRetries;     //!< Number of data retransmission attempts
  TracedValue<Time> m_rto;             //!< Retransmit timeout
  Time              m_minRto;          //!< minimum value of the Retransmit timeout
  Time              m_clockGranularity; //!< Clock Granularity used in RTO calcs
  TracedValue<Time> m_lastRtt;         //!< Last RTT sample collected
  Time              m_delAckTimeout;   //!< Time to delay an ACK
  Time              m_persistTimeout;  //!< Time between sending 1-byte probes
  Time              m_cnTimeout;       //!< Timeout for connection retry
  RttHistory_t      m_history;         //!< List of sent packet

  // Connections to other layers of TCP/IP
  Ipv4EndPoint*       m_endPoint;   //!< the IPv4 endpoint
  Ipv6EndPoint*       m_endPoint6;  //!< the IPv6 endpoint
  Ptr<Node>           m_node;       //!< the associated node
  Ptr<TcpL4Protocol>  m_tcp;        //!< the associated TCP L4 protocol
  Callback<void, Ipv4Address,uint8_t,uint8_t,uint8_t,uint32_t> m_icmpCallback;  //!< ICMP callback
  Callback<void, Ipv6Address,uint8_t,uint8_t,uint8_t,uint32_t> m_icmpCallback6; //!< ICMPv6 callback

  Ptr<RttEstimator> m_rtt; //!< Round trip time estimator

  // Rx and Tx buffer management
  Ptr<TcpRxBuffer>              m_rxBuffer;       //!< Rx buffer (reordering buffer)
  Ptr<TcpTxBuffer>              m_txBuffer;       //!< Tx buffer

  // State-related attributes
  TracedValue<TcpStates_t> m_state;         //!< TCP state
  mutable enum SocketErrno m_errno;         //!< Socket error code
  bool                     m_closeNotified; //!< Told app to close socket
  bool                     m_closeOnEmpty;  //!< Close socket upon tx buffer emptied
  bool                     m_shutdownSend;  //!< Send no longer allowed
  bool                     m_shutdownRecv;  //!< Receive no longer allowed
  bool                     m_connected;     //!< Connection established
  double                   m_msl;           //!< Max segment lifetime

  // Window management
  uint16_t              m_maxWinSize;  //!< Maximum window size to advertise
  TracedValue<uint32_t> m_rWnd;        //!< Receiver window (RCV.WND in RFC793)
  TracedValue<SequenceNumber32> m_highRxMark;     //!< Highest seqno received
  SequenceNumber32 m_highTxAck;                   //!< Highest ack sent
  TracedValue<SequenceNumber32> m_highRxAckMark;  //!< Highest ack received
  uint32_t                      m_bytesAckedNotProcessed;  //!< Bytes acked, but not processed
  TracedValue<uint32_t>         m_bytesInFlight; //!< Bytes in flight

  // Options
  bool    m_winScalingEnabled; //!< Window Scale option enabled (RFC 7323)
  uint8_t m_rcvWindShift;      //!< Window shift to apply to outgoing segments
  uint8_t m_sndWindShift;      //!< Window shift to apply to incoming segments

  bool     m_timestampEnabled;    //!< Timestamp option enabled
  uint32_t m_timestampToEcho;     //!< Timestamp to echo

  EventId m_sendPendingDataEvent; //!< micro-delay event to send pending data

  // Fast Retransmit and Recovery
  SequenceNumber32       m_recover;      //!< Previous highest Tx seqnum for fast recovery
  uint32_t               m_retxThresh;   //!< Fast Retransmit threshold
  bool                   m_limitedTx;    //!< perform limited transmit
  uint32_t               m_retransOut;   //!< Number of retransmission in this window

  // Transmission Control Block
  Ptr<TcpSocketState>    m_tcb;               //!< Congestion control informations
  Ptr<TcpCongestionOps>  m_congestionControl; //!< Congestion control

  // Guesses over the other connection end
  bool m_isFirstPartialAck; //!< First partial ACK during RECOVERY

  // The following two traces pass a packet with a TCP header
  TracedCallback<Ptr<const Packet>, const TcpHeader&,
                 Ptr<const TcpSocketBase> > m_txTrace; //!< Trace of transmitted packets

  TracedCallback<Ptr<const Packet>, const TcpHeader&,
                 Ptr<const TcpSocketBase> > m_rxTrace; //!< Trace of received packets
  
  // Parameters related to Explicit Congestion Notification
  bool                          m_ecn;             //!< Socket ECN capability
  TracedValue<uint8_t>          m_ecnState;        //!< Current ECN State, represented as combination of EcnState values
  TracedValue<SequenceNumber32> m_ecnEchoSeq;      //!< Sequence number of the last received ECN Echo
  bool                          m_ceReceived;      //!< Flag indicating a received CE packet

};

/**
 * \ingroup tcp
 * TracedValue Callback signature for TcpCongState_t
 *
 * \param [in] oldValue original value of the traced variable
 * \param [in] newValue new value of the traced variable
 */
typedef void (* TcpCongStatesTracedValueCallback)(const TcpSocketState::TcpCongState_t oldValue,
                                                  const TcpSocketState::TcpCongState_t newValue);

} // namespace ns3

#endif /* TCP_SOCKET_BASE_H */
