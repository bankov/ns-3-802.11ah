/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Copyright (c) 2006, 2009 INRIA
 * Copyright (c) 2009 MIRKO BANCHI
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
 * Authors: Mathieu Lacage <mathieu.lacage@sophia.inria.fr>
 *          Mirko Banchi <mk.banchi@gmail.com>
 */

#include "sta-wifi-mac.h"
#include "ns3/log.h"
#include "ns3/simulator.h"
#include "ns3/string.h"
#include "ns3/pointer.h"
#include "ns3/boolean.h"
#include "ns3/trace-source-accessor.h"
#include "qos-tag.h"
#include "mac-low.h"
#include "dcf-manager.h"
#include "mac-rx-middle.h"
#include "mac-tx-middle.h"
#include "wifi-mac-header.h"
#include "extension-headers.h"
#include "msdu-aggregator.h"
#include "amsdu-subframe-header.h"
#include "mgt-headers.h"
#include "ht-capabilities.h"
#include "random-stream.h"

/*
 * The state machine for this STA is:
 --------------                                          -----------
 | Associated |   <--------------------      ------->    | Refused |
 --------------                        \    /            -----------
    \                                   \  /
     \    -----------------     -----------------------------
      \-> | Beacon Missed | --> | Wait Association Response |
          -----------------     -----------------------------
                \                       ^
                 \                      |
                  \    -----------------------
                   \-> | Wait Probe Response |
                       -----------------------
 */

namespace ns3 {

NS_LOG_COMPONENT_DEFINE ("StaWifiMac");

NS_OBJECT_ENSURE_REGISTERED (StaWifiMac);

TypeId
StaWifiMac::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::StaWifiMac")
    .SetParent<RegularWifiMac> ()
    .SetGroupName ("Wifi")
    .AddConstructor<StaWifiMac> ()
    .AddAttribute ("ProbeRequestTimeout", "The interval between two consecutive probe request attempts.",
                   TimeValue (Seconds (0.05)),
                   MakeTimeAccessor (&StaWifiMac::m_probeRequestTimeout),
                   MakeTimeChecker ())
    .AddAttribute ("AssocRequestTimeout", "The interval between two consecutive assoc request attempts.",
                   TimeValue (Seconds (0.5)),
                   MakeTimeAccessor (&StaWifiMac::m_assocRequestTimeout),
                   MakeTimeChecker ())
    .AddAttribute ("AuthRequestTimeout", "The interval between two consecutive auth request attempts.",
                   TimeValue (Seconds (0.5)),
                   MakeTimeAccessor (&StaWifiMac::m_authRequestTimeout),
                   MakeTimeChecker ())
    .AddAttribute ("RawDuration", "The duration of one RAW group.",
                   TimeValue (MicroSeconds (102400)),
                   MakeTimeAccessor (&StaWifiMac::GetRawDuration,
                                     &StaWifiMac::SetRawDuration),
                   MakeTimeChecker ())
    .AddAttribute ("MaxMissedBeacons",
                   "Number of beacons which much be consecutively missed before "
                   "we attempt to restart association.",
                   UintegerValue (10),
                   MakeUintegerAccessor (&StaWifiMac::m_maxMissedBeacons),
                   MakeUintegerChecker<uint32_t> ())
    .AddAttribute ("ActiveProbing",
                   "If true, we send probe requests. If false, we don't."
                   "NOTE: if more than one STA in your simulation is using active probing, "
                   "you should enable it at a different simulation time for each STA, "
                   "otherwise all the STAs will start sending probes at the same time resulting in collisions. "
                   "See bug 1060 for more info.",
                   BooleanValue (false),
                   MakeBooleanAccessor (&StaWifiMac::SetActiveProbing, &StaWifiMac::GetActiveProbing),
                   MakeBooleanChecker ())
    .AddTraceSource ("Assoc", "Associated with an access point.",
                     MakeTraceSourceAccessor (&StaWifiMac::m_assocLogger),
                     "ns3::Mac48Address::TracedCallback")
    .AddTraceSource ("DeAssoc", "Association with an access point lost.",
                     MakeTraceSourceAccessor (&StaWifiMac::m_deAssocLogger),
                     "ns3::Mac48Address::TracedCallback")
  ;
  return tid;
}

StaWifiMac::StaWifiMac ()
  : m_state (BEACON_MISSED),
    m_probeRequestEvent (),
    m_assocRequestEvent (),
    m_authRequestEvent (),
    m_countBeaconEvent (),
    m_beaconWatchdogEnd (Seconds (0.0))
{
  NS_LOG_FUNCTION (this);
  m_rawStart = false;
  m_dataBuffered = false;
  m_aid = 8192;
  l = 0;
  uint32_t cwmin = 15;
  uint32_t cwmax = 1023;
  m_pspollDca = CreateObject<DcaTxop> ();
  m_pspollDca->SetAifsn (2);
  m_pspollDca->SetMinCw ((cwmin + 1) / 4 - 1);
  m_pspollDca->SetMaxCw ((cwmin + 1) / 2 - 1);  //same as AC_VO
  m_pspollDca->SetLow (m_low);
  m_pspollDca->SetManager (m_dcfManager);
  m_pspollDca->SetTxMiddle (m_txMiddle);
  fasTAssocType = false; //centraied control
  fastAssocThreshold = 0; // allow some station to associate at the begining
    Ptr<UniformRandomVariable> m_rv = CreateObject<UniformRandomVariable> ();
    assocVaule = m_rv->GetValue (0, 1022);
  m_minTI = 3;
  m_maxTI = 10;
  m_Tac = 4;
  m_localTI = 0;
  m_countingBeacons = 0;
  //Let the lower layers know that we are acting as a non-AP STA in
  //an infrastructure BSS.
  SetTypeOfStation (STA);
}

StaWifiMac::~StaWifiMac ()
{
  NS_LOG_FUNCTION (this);
}

void
StaWifiMac::DoDispose ()
{
  NS_LOG_FUNCTION (this);
  m_pspollDca = 0;
  RegularWifiMac::DoDispose ();
}

uint32_t
StaWifiMac::GetAID (void) const
{
  NS_ASSERT ((1 <= m_aid) && (m_aid<= 8191) || (m_aid == 8192));
  return m_aid;
}
    
Time
StaWifiMac::GetRawDuration (void) const
{
  NS_LOG_FUNCTION (this);
  return m_rawDuration;
}
 
bool
StaWifiMac::Is(uint8_t blockbitmap, uint8_t j)
{
  return (((blockbitmap >> j) & 0x01) == 0x01);
}
    
void
StaWifiMac::SetAID (uint32_t aid)
{
  NS_ASSERT ((1 <= aid) && (aid <= 8191));
  m_aid = aid;
}
    
void
StaWifiMac::SetRawDuration (Time interval)
{
  NS_LOG_FUNCTION (this << interval);
  m_rawDuration = interval;
}
    
void
StaWifiMac::SetDataBuffered()
{
  m_dataBuffered = true;
}
    
void
StaWifiMac::ClearDataBuffered()
{
  m_dataBuffered = false;
}
    
void
StaWifiMac::SetInRAWgroup()
{
  m_inRawGroup = true;
}
    
void
StaWifiMac::UnsetInRAWgroup()
{
  m_inRawGroup = false;
}
        
void
StaWifiMac::SetMaxMissedBeacons (uint32_t missed)
{
  NS_LOG_FUNCTION (this << missed);
  m_maxMissedBeacons = missed;
}

void
StaWifiMac::SetProbeRequestTimeout (Time timeout)
{
  NS_LOG_FUNCTION (this << timeout);
  m_probeRequestTimeout = timeout;
}

void
StaWifiMac::SetAuthRequestTimeout (Time timeout)
{
  NS_LOG_FUNCTION (this << timeout);
  m_authRequestTimeout = timeout;
}

void
StaWifiMac::SetAssocRequestTimeout (Time timeout)
{
  NS_LOG_FUNCTION (this << timeout);
  m_assocRequestTimeout = timeout;
}

void
StaWifiMac::StartActiveAssociation (void)
{
  NS_LOG_FUNCTION (this);
  TryToEnsureAssociated ();
}

void
StaWifiMac::SetActiveProbing (bool enable)
{
  NS_LOG_FUNCTION (this << enable);
  if (enable)
    {
      Simulator::ScheduleNow (&StaWifiMac::TryToEnsureAssociated, this);
    }
  else
    {
      m_probeRequestEvent.Cancel ();
    }
  m_activeProbing = enable;
}

bool StaWifiMac::GetActiveProbing (void) const
{
  return m_activeProbing;
}

void
StaWifiMac::SendPspoll (void)
{
  NS_LOG_FUNCTION (this);
  WifiMacHeader hdr;
  hdr.SetType (WIFI_MAC_CTL_PSPOLL);
  hdr.SetId (GetAID());
  hdr.SetAddr1 (GetBssid());
  hdr.SetAddr2 (GetAddress ());
    
  Ptr<Packet> packet = Create<Packet> ();
  packet->AddHeader (hdr);

  //The standard is not clear on the correct queue for management
  //frames if we are a QoS AP. The approach taken here is to always
  //use the DCF for these regardless of whether we have a QoS
  //association or not.
  m_pspollDca->Queue (packet, hdr);
}
    
void
StaWifiMac::SendPspollIfnecessary (void)
{
  //assume only send one beacon during RAW
  if ( m_rawStart & m_inRawGroup && m_pagedStaRaw && m_dataBuffered )
    {
     // SendPspoll ();  //pspoll not really send, just put ps-poll frame in m_pspollDca queue
    }
  else if (!m_rawStart && m_dataBuffered && !m_outsideRawEvent.IsRunning ()) //in case the next beacon coming during RAW, could it happen?
   {
     // SendPspoll ();
   }
}

void
StaWifiMac::S1gBeaconReceived (void)
{
    if (m_outsideRawEvent.IsRunning ())
     {
        m_outsideRawEvent.Cancel ();          //avoid error when actual beacon interval become shorter, otherwise, AccessAllowedIfRaw will set again after raw starting
        //Simulator::ScheduleNow(&StaWifiMac::OutsideRawStartBackoff, this);
     }
    
  if (m_aid == 8192) // send assoication request when Staion is not assoicated
    {
      m_dca->AccessAllowedIfRaw (true);
    }
  else if (m_rawStart & m_inRawGroup && m_pagedStaRaw && m_dataBuffered ) // if m_pagedStaRaw is true, only m_dataBuffered can access channel
    {
      m_outsideRawEvent = Simulator::Schedule(m_lastRawDurationus, &StaWifiMac::OutsideRawStartBackoff, this);
        
      m_pspollDca->AccessAllowedIfRaw (true);
      m_dca->AccessAllowedIfRaw (false);
      m_edca.find (AC_VO)->second->AccessAllowedIfRaw (false);
      m_edca.find (AC_VI)->second->AccessAllowedIfRaw (false);
      m_edca.find (AC_BE)->second->AccessAllowedIfRaw (false);
      m_edca.find (AC_BK)->second->AccessAllowedIfRaw (false);
      StartRawbackoff();
    }
  else if (m_rawStart && m_inRawGroup && !m_pagedStaRaw  )
    {
      m_outsideRawEvent = Simulator::Schedule(m_lastRawDurationus, &StaWifiMac::OutsideRawStartBackoff, this);
        
      m_pspollDca->AccessAllowedIfRaw (false);
      m_dca->AccessAllowedIfRaw (false);
      m_edca.find (AC_VO)->second->AccessAllowedIfRaw (false);
      m_edca.find (AC_VI)->second->AccessAllowedIfRaw (false);
      m_edca.find (AC_BE)->second->AccessAllowedIfRaw (false);
      m_edca.find (AC_BK)->second->AccessAllowedIfRaw (false);
      Simulator::Schedule(m_statSlotStart, &StaWifiMac::RawSlotStartBackoff, this);
    }
  else if (m_rawStart && !m_inRawGroup) //|| (m_rawStart && m_inRawGroup && m_pagedStaRaw && !m_dataBuffered)
    {
      m_outsideRawEvent = Simulator::Schedule(m_lastRawDurationus, &StaWifiMac::OutsideRawStartBackoff, this);
        
      m_pspollDca->AccessAllowedIfRaw (false);
      m_dca->AccessAllowedIfRaw (false);
      m_edca.find (AC_VO)->second->AccessAllowedIfRaw (false);
      m_edca.find (AC_VI)->second->AccessAllowedIfRaw (false);
      m_edca.find (AC_BE)->second->AccessAllowedIfRaw (false);
      m_edca.find (AC_BK)->second->AccessAllowedIfRaw (false);
      StartRawbackoff();
    }
    // else (!m_rawStart),  this case cannot happen, since we assume s1g beacon always indicating one raw
    m_rawStart = false;
}

void
StaWifiMac::RawSlotStartBackoff (void)
{
    if (m_insideBackoffEvent.IsRunning ())
     {
        m_insideBackoffEvent.Cancel ();
     } //a bug is fixed, prevent previous RAW from disabling current RAW.
    m_insideBackoffEvent = Simulator::Schedule(m_slotDuration, &StaWifiMac::InsideBackoff, this);
    m_pspollDca->AccessAllowedIfRaw (true);
    m_dca->AccessAllowedIfRaw (true);
    m_edca.find (AC_VO)->second->AccessAllowedIfRaw (true);
    m_edca.find (AC_VI)->second->AccessAllowedIfRaw (true);
    m_edca.find (AC_BE)->second->AccessAllowedIfRaw (true);
    m_edca.find (AC_BK)->second->AccessAllowedIfRaw (true);
    StartRawbackoff();
}
    
void
StaWifiMac::InsideBackoff (void)
{
   m_pspollDca->AccessAllowedIfRaw (false);
   m_dca->AccessAllowedIfRaw (false);
   m_edca.find (AC_VO)->second->AccessAllowedIfRaw (false);
   m_edca.find (AC_VI)->second->AccessAllowedIfRaw (false);
   m_edca.find (AC_BE)->second->AccessAllowedIfRaw (false);
   m_edca.find (AC_BK)->second->AccessAllowedIfRaw (false);
}
    
    
void
StaWifiMac::StartRawbackoff (void)
{
  m_pspollDca->RawStart (); //not really start raw useless allowedAccessRaw is true;
  m_dca->RawStart ();
  m_edca.find (AC_VO)->second->RawStart ();
  m_edca.find (AC_VI)->second->RawStart ();
  m_edca.find (AC_BE)->second->RawStart ();
  m_edca.find (AC_BK)->second->RawStart ();

}


void
StaWifiMac::OutsideRawStartBackoff (void)
{
  Simulator::ScheduleNow(&DcaTxop::OutsideRawStart, StaWifiMac::m_pspollDca);
  Simulator::ScheduleNow(&DcaTxop::OutsideRawStart, StaWifiMac::m_dca);
  Simulator::ScheduleNow(&EdcaTxopN::OutsideRawStart, StaWifiMac::m_edca.find (AC_VO)->second);
  Simulator::ScheduleNow(&EdcaTxopN::OutsideRawStart, StaWifiMac::m_edca.find (AC_VI)->second);
  Simulator::ScheduleNow(&EdcaTxopN::OutsideRawStart, StaWifiMac::m_edca.find (AC_BE)->second);
  Simulator::ScheduleNow(&EdcaTxopN::OutsideRawStart, StaWifiMac::m_edca.find (AC_BK)->second);
}
    
void
StaWifiMac::SetWifiRemoteStationManager (Ptr<WifiRemoteStationManager> stationManager)
{
  NS_LOG_FUNCTION (this << stationManager);
  m_pspollDca->SetWifiRemoteStationManager (stationManager);
  RegularWifiMac::SetWifiRemoteStationManager (stationManager);
}
    
void
StaWifiMac::SendProbeRequest (void)
{
  NS_LOG_FUNCTION (this);
  WifiMacHeader hdr;
  hdr.SetProbeReq ();
  hdr.SetAddr1 (Mac48Address::GetBroadcast ());
  hdr.SetAddr2 (GetAddress ());
  hdr.SetAddr3 (Mac48Address::GetBroadcast ());
  hdr.SetDsNotFrom ();
  hdr.SetDsNotTo ();
  Ptr<Packet> packet = Create<Packet> ();
  MgtProbeRequestHeader probe;
  probe.SetSsid (GetSsid ());
  probe.SetSupportedRates (GetSupportedRates ());
  if (m_htSupported)
    {
      probe.SetHtCapabilities (GetHtCapabilities ());
      hdr.SetNoOrder ();
    }

  packet->AddHeader (probe);

  //The standard is not clear on the correct queue for management
  //frames if we are a QoS AP. The approach taken here is to always
  //use the DCF for these regardless of whether we have a QoS
  //association or not.
  m_dca->Queue (packet, hdr);

  if (m_probeRequestEvent.IsRunning ())
    {
      m_probeRequestEvent.Cancel ();
    }
  m_probeRequestEvent = Simulator::Schedule (m_probeRequestTimeout,
                                             &StaWifiMac::ProbeRequestTimeout, this);
}

void
StaWifiMac::SendAuthenticationRequest (void)
{
  NS_LOG_FUNCTION (this << GetBssid ());
  if (!m_s1gSupported)
    {
      fastAssocThreshold = 1023;
    }
  if (fasTAssocType == 1)
    {
      m_countingBeacons = 0;
    }
  if (assocVaule < fastAssocThreshold || fasTAssocType == 1)
    {
      SetState (WAIT_AUTH_RESP);
      WifiMacHeader hdr;
      hdr.SetAuthFrame ();
      hdr.SetAddr1 (GetBssid ());
      hdr.SetAddr2 (GetAddress ());
      hdr.SetAddr3 (GetBssid ());
      hdr.SetDsNotFrom ();
      hdr.SetDsNotTo ();
      Ptr<Packet> packet = Create<Packet> ();
      MgtAuthFrameHeader auth;
      auth.SetAuthAlgorithmNumber (0);
      auth.SetAuthTransactionSeqNumber (1);

      packet->AddHeader (auth);

      m_dca->Queue (packet, hdr);

      if (m_authRequestEvent.IsRunning ())
        {
          m_authRequestEvent.Cancel ();
        }
      m_authRequestEvent = Simulator::Schedule (m_authRequestTimeout,
                                       &StaWifiMac::AuthRequestTimeout, this);
    }
  else
    {
      SetState (BEACON_MISSED);
    }
}

void
StaWifiMac::SendAssociationRequest (void)
{
  NS_LOG_FUNCTION (this << GetBssid ());
      SetState (WAIT_ASSOC_RESP);
      WifiMacHeader hdr;
      hdr.SetAssocReq ();
      hdr.SetAddr1 (GetBssid ());
      hdr.SetAddr2 (GetAddress ());
      hdr.SetAddr3 (GetBssid ());
      hdr.SetDsNotFrom ();
      hdr.SetDsNotTo ();
      Ptr<Packet> packet = Create<Packet> ();
      MgtAssocRequestHeader assoc;
      assoc.SetSsid (GetSsid ());
      assoc.SetSupportedRates (GetSupportedRates ());
      if (m_htSupported)
         {
    	    assoc.SetHtCapabilities (GetHtCapabilities ());
    	    hdr.SetNoOrder ();
         }

      packet->AddHeader (assoc);

      //The standard is not clear on the correct queue for management
      //frames if we are a QoS AP. The approach taken here is to always
      //use the DCF for these regardless of whether we have a QoS
      //association or not.
      m_dca->Queue (packet, hdr);

      if (m_assocRequestEvent.IsRunning ())
         {
    	    m_assocRequestEvent.Cancel ();
         }
      m_assocRequestEvent = Simulator::Schedule (m_assocRequestTimeout,
                                       &StaWifiMac::AssocRequestTimeout, this);
}

void
StaWifiMac::TryToEnsureAssociated (void)
{
  NS_LOG_FUNCTION (this);
  switch (m_state)
    {
    case ASSOCIATED:
      return;
      break;
    case WAIT_PROBE_RESP:
      /* we have sent a probe request earlier so we
         do not need to re-send a probe request immediately.
         We just need to wait until probe-request-timeout
         or until we get a probe response
       */
      break;
    case BEACON_MISSED:
      /* we were associated but we missed a bunch of beacons
       * so we should assume we are not associated anymore.
       * We try to initiate a probe request now.
       */
      m_linkDown ();
      if (m_activeProbing)
        {
          SetState (WAIT_PROBE_RESP);
          SendProbeRequest ();
        }
      break;
    case WAIT_ASSOC_RESP:
      /* we have sent an assoc request so we do not need to
         re-send an assoc request right now. We just need to
         wait until either assoc-request-timeout or until
         we get an assoc response.
       */
      break;
    case REFUSED:
      /* we have sent an assoc request and received a negative
         assoc resp. We wait until someone restarts an
         association with a given ssid.
       */
      break;
    case WAIT_AUTH_RESP:
      break;
    }
}

void
StaWifiMac::AuthRequestTimeout (void)
{
  NS_LOG_FUNCTION (this);
  if (fasTAssocType == 0)
    {
      SetState (WAIT_AUTH_RESP);
      SendAuthenticationRequest ();
    }
  else
    {
      SetState (BEACON_MISSED);
      if (2 * m_localTI < m_maxTI)
        {
          m_localTI *= 2;
        }
      else
        {
          m_localTI = m_maxTI;
        }
    }
}

void
StaWifiMac::AssocRequestTimeout (void)
{
  NS_LOG_FUNCTION (this);
  SetState (WAIT_ASSOC_RESP);
  SendAssociationRequest ();
}

void
StaWifiMac::ProbeRequestTimeout (void)
{
  NS_LOG_FUNCTION (this);
  SetState (WAIT_PROBE_RESP);
  SendProbeRequest ();
}

void
StaWifiMac::MissedBeacons (void)
{
  NS_LOG_FUNCTION (this);
  if (m_beaconWatchdogEnd > Simulator::Now ())
    {
      if (m_beaconWatchdog.IsRunning ())
        {
          m_beaconWatchdog.Cancel ();
        }
      m_beaconWatchdog = Simulator::Schedule (m_beaconWatchdogEnd - Simulator::Now (),
                                              &StaWifiMac::MissedBeacons, this);
      return;
    }
  NS_LOG_DEBUG ("beacon missed");
  SetState (BEACON_MISSED);
  TryToEnsureAssociated ();
}

void
StaWifiMac::RestartBeaconWatchdog (Time delay)
{
  NS_LOG_FUNCTION (this << delay);
  m_beaconWatchdogEnd = std::max (Simulator::Now () + delay, m_beaconWatchdogEnd);
  if (Simulator::GetDelayLeft (m_beaconWatchdog) < delay
      && m_beaconWatchdog.IsExpired ())
    {
      NS_LOG_DEBUG ("really restart watchdog.");
      m_beaconWatchdog = Simulator::Schedule (delay, &StaWifiMac::MissedBeacons, this);
    }
}

bool
StaWifiMac::IsAssociated (void) const
{
  return m_state == ASSOCIATED;
}

bool
StaWifiMac::IsWaitAssocResp (void) const
{
  return m_state == WAIT_ASSOC_RESP;
}

void
StaWifiMac::Enqueue (Ptr<const Packet> packet, Mac48Address to)
{
  NS_LOG_FUNCTION (this << packet << to);
  if (!IsAssociated ())
    {
      NotifyTxDrop (packet);
      TryToEnsureAssociated ();
      return;
    }
  WifiMacHeader hdr;

  //If we are not a QoS AP then we definitely want to use AC_BE to
  //transmit the packet. A TID of zero will map to AC_BE (through \c
  //QosUtilsMapTidToAc()), so we use that as our default here.
  uint8_t tid = 0;

  //For now, an AP that supports QoS does not support non-QoS
  //associations, and vice versa. In future the AP model should
  //support simultaneously associated QoS and non-QoS STAs, at which
  //point there will need to be per-association QoS state maintained
  //by the association state machine, and consulted here.
  if (m_qosSupported)
    {
      hdr.SetType (WIFI_MAC_QOSDATA);
      hdr.SetQosAckPolicy (WifiMacHeader::NORMAL_ACK);
      hdr.SetQosNoEosp ();
      hdr.SetQosNoAmsdu ();
      //Transmission of multiple frames in the same TXOP is not
      //supported for now
      hdr.SetQosTxopLimit (0);

      //Fill in the QoS control field in the MAC header
      tid = QosUtilsGetTidForPacket (packet);
      //Any value greater than 7 is invalid and likely indicates that
      //the packet had no QoS tag, so we revert to zero, which'll
      //mean that AC_BE is used.
      if (tid > 7)
        {
          tid = 0;
        }
      hdr.SetQosTid (tid);
    }
  else
    {
      hdr.SetTypeData ();
    }
  if (m_htSupported)
    {
      hdr.SetNoOrder ();
    }

  hdr.SetAddr1 (GetBssid ());
  hdr.SetAddr2 (m_low->GetAddress ());
  hdr.SetAddr3 (to);
  hdr.SetDsNotFrom ();
  hdr.SetDsTo ();

  if (m_qosSupported)
    {
      //Sanity check that the TID is valid
      NS_ASSERT (tid < 8);
      m_edca[QosUtilsMapTidToAc (tid)]->Queue (packet, hdr);
    }
  else
    {
      m_dca->Queue (packet, hdr);
    }
}

void
StaWifiMac::Receive (Ptr<Packet> packet, const WifiMacHeader *hdr)
{
  NS_LOG_FUNCTION (this << packet << hdr);
  NS_ASSERT (!hdr->IsCtl ());
  if (hdr->GetAddr3 () == GetAddress ())
    {
      NS_LOG_LOGIC ("packet sent by us.");
      return;
    }
  else if (hdr->GetAddr1 () != GetAddress ()
           && !hdr->GetAddr1 ().IsGroup ())
    {
      NS_LOG_LOGIC ("packet is not for us");
      NotifyRxDrop (packet);
      return;
    }
  else if (hdr->IsData ())
    {
      if (!IsAssociated ())
        {
          NS_LOG_LOGIC ("Received data frame while not associated: ignore");
          NotifyRxDrop (packet);
          return;
        }
      if (!(hdr->IsFromDs () && !hdr->IsToDs ()))
        {
          NS_LOG_LOGIC ("Received data frame not from the DS: ignore");
          NotifyRxDrop (packet);
          return;
        }
      if (hdr->GetAddr2 () != GetBssid ())
        {
          NS_LOG_LOGIC ("Received data frame not from the BSS we are associated with: ignore");
          NotifyRxDrop (packet);
          return;
        }
      if (hdr->IsQosData ())
        {
          if (hdr->IsQosAmsdu ())
            {
              NS_ASSERT (hdr->GetAddr3 () == GetBssid ());
              DeaggregateAmsduAndForward (packet, hdr);
              packet = 0;
            }
          else
            {
              ForwardUp (packet, hdr->GetAddr3 (), hdr->GetAddr1 ());
            }
        }
      else
        {
          ForwardUp (packet, hdr->GetAddr3 (), hdr->GetAddr1 ());
        }
      return;
    }
  else if (hdr->IsProbeReq ()
           || hdr->IsAssocReq ())
    {
      //This is a frame aimed at an AP, so we can safely ignore it.
      NotifyRxDrop (packet);
      return;
    }
  else if (hdr->IsBeacon ())
    {
      MgtBeaconHeader beacon;
      packet->RemoveHeader (beacon);
      bool goodBeacon = false;
      if (GetSsid ().IsBroadcast ()
          || beacon.GetSsid ().IsEqual (GetSsid ()))
        {
          goodBeacon = true;
        }
      SupportedRates rates = beacon.GetSupportedRates ();
      for (uint32_t i = 0; i < m_phy->GetNBssMembershipSelectors (); i++)
        {
          uint32_t selector = m_phy->GetBssMembershipSelector (i);
          if (!rates.IsSupportedRate (selector))
            {
              goodBeacon = false;
            }
        }
      if ((IsWaitAssocResp () || IsAssociated ()) && hdr->GetAddr3 () != GetBssid ())
        {
          goodBeacon = false;
        }
      if (goodBeacon)
        {
          Time delay = MicroSeconds (beacon.GetBeaconIntervalUs () * m_maxMissedBeacons);
          RestartBeaconWatchdog (delay);
          SetBssid (hdr->GetAddr3 ());
        }
      if (goodBeacon && m_state == BEACON_MISSED)
        {
          SetState (WAIT_AUTH_RESP);
          SendAuthenticationRequest ();
        }
      return;
    }
  else if (hdr->IsS1gBeacon ())
    {
      S1gBeaconHeader beacon;
      packet->RemoveHeader (beacon);
      bool goodBeacon = false;
    if ((IsWaitAssocResp () || IsAssociated ()) && hdr->GetAddr3 () != GetBssid ()) // for debug
     {
       goodBeacon = false;
     }
    else
     {
      goodBeacon = true;
     }
    if (goodBeacon)
     {
       Time delay = MicroSeconds (beacon.GetBeaconCompatibility().GetBeaconInterval () * m_maxMissedBeacons);
       RestartBeaconWatchdog (delay);
       //SetBssid (beacon.GetSA ());
       SetBssid (hdr->GetAddr3 ()); //for debug
     }
    if (goodBeacon)
      {
        if (beacon.GetRPS ().GetInformationFieldSize ())
          {
            UnsetInRAWgroup ();
            uint8_t * rawassign;
            rawassign = beacon.GetRPS().GetRawAssignment();
            uint8_t raw_len = beacon.GetRPS().GetInformationFieldSize();
            uint8_t rawtypeindex = rawassign[0] & 0x07;
            uint8_t pageindex = rawassign[4] & 0x03;

            uint16_t m_rawslot;
            m_rawslot = (uint16_t(rawassign[2]) << 8) | (uint16_t(rawassign[1]));
            uint8_t m_SlotFormat = uint8_t (m_rawslot >> 15) & 0x0001;
            uint8_t m_slotCrossBoundary = uint8_t (m_rawslot >> 14) & 0x0002;
            uint16_t m_slotDurationCount;
            uint16_t m_slotNum;

            NS_ASSERT (m_SlotFormat <= 1);

            if (m_SlotFormat == 0)
              {
                m_slotDurationCount = (m_rawslot >> 6) & 0x00ff;
                m_slotNum = m_rawslot & 0x003f;
              }
            else if (m_SlotFormat == 1)
              {
                m_slotDurationCount = (m_rawslot >> 3) & 0x07ff;
                m_slotNum = m_rawslot & 0x0007;
              }

            m_slotDuration = MicroSeconds(500 + m_slotDurationCount * 120);
            m_lastRawDurationus = m_slotDuration * m_slotNum;

            if (pageindex == ((GetAID() >> 11 ) & 0x0003)) //in the page indexed
              {
                uint8_t rawgroup_l = rawassign[4];
                uint8_t rawgroup_m = rawassign[5];
                uint8_t rawgroup_h = rawassign[6];
                uint32_t rawgroup = (uint32_t(rawassign[6]) << 16) | (uint32_t(rawassign[5]) << 8) | uint32_t(rawassign[4]);
                uint16_t raw_start = (rawgroup >> 2) & 0x000003ff;
                uint16_t raw_end = (rawgroup >> 13) & 0x000003ff;
                if ((raw_start <= (GetAID() & 0x03ff)) && ((GetAID() & 0x03ff) <= raw_end))
                  {
                    SetInRAWgroup ();

                    uint16_t statsPerSlot = 0;
                    uint16_t statRawSlot = 0;

                    Ptr<UniformRandomVariable> m_rv = CreateObject<UniformRandomVariable> ();
                    uint16_t offset = m_rv->GetValue (0, 1023);
                    offset =0; // for test
                    statsPerSlot = (raw_end - raw_start + 1)/m_slotNum;
                    //statRawSlot = ((GetAID() & 0x03ff)-raw_start)/statsPerSlot;
                    statRawSlot = ((GetAID() & 0x03ff)+offset)%m_slotNum;
                    m_statSlotStart = MicroSeconds((500 + m_slotDurationCount * 120)*statRawSlot);
                  }
               }

             m_rawStart = true;
             if (rawtypeindex == 4) // only support Generic Raw (paged STA RAW or not)
               {
                 m_pagedStaRaw = true;
               }
             else
               {
                 m_pagedStaRaw = false;
               }
            }

            AuthenticationCtrl AuthenCtrl;
            AuthenCtrl = beacon.GetAuthCtrl ();
            fasTAssocType = AuthenCtrl.GetControlType ();
            if (!fasTAssocType)  //only support centralized control
              {
                fastAssocThreshold = AuthenCtrl.GetThreshold();
              }
            else
              {
                m_minTI = AuthenCtrl.GetMinInterval();
                m_maxTI = AuthenCtrl.GetMaxInterval();
                m_Tac = AuthenCtrl.GetSlotDuration();
                m_beaconInterval = beacon.GetBeaconCompatibility().GetBeaconInterval ();
                if (m_localTI == 0)
                  {
                    m_localTI = m_minTI;
                  }
              }
     }
    if (goodBeacon && m_state == BEACON_MISSED)
      {
        if (fasTAssocType == 0)
          {
            SendAuthenticationRequest ();
          }
        else
          {
            if (m_countingBeacons == 0)
              {
                uint64_t L = 1;
                uint64_t tac = 1024 * m_Tac;
                L = m_beaconInterval / tac;
                Ptr<UniformRandomVariable> m_rv = CreateObject<UniformRandomVariable> ();
                m_beaconsLeft = m_rv->GetValue (0, m_localTI);
                Ptr<UniformRandomVariable> m_r = CreateObject<UniformRandomVariable> ();
                l = m_r->GetValue (0, L);
                m_countingBeacons = 1;
              }
            else
              {
                if (m_beaconsLeft > 0)
                  {
                    m_beaconsLeft--;
                  }
                if (m_beaconsLeft == 0)
                  {
                    m_countSlotEvent = Simulator::Schedule (MicroSeconds(l * 1024 * m_Tac), &StaWifiMac::SendAuthenticationRequest, this);
                  }
              }
          }
      }
    S1gBeaconReceived ();
    return;
   }
  else if (hdr->IsProbeResp ())
    {
      if (m_state == WAIT_PROBE_RESP)
        {
          MgtProbeResponseHeader probeResp;
          packet->RemoveHeader (probeResp);
          if (!probeResp.GetSsid ().IsEqual (GetSsid ()))
            {
              //not a probe resp for our ssid.
              return;
            }
          SupportedRates rates = probeResp.GetSupportedRates ();
          for (uint32_t i = 0; i < m_phy->GetNBssMembershipSelectors (); i++)
            {
              uint32_t selector = m_phy->GetBssMembershipSelector (i);
              if (!rates.IsSupportedRate (selector))
                {
                  return;
                }
            }
          SetBssid (hdr->GetAddr3 ());
          Time delay = MicroSeconds (probeResp.GetBeaconIntervalUs () * m_maxMissedBeacons);
          RestartBeaconWatchdog (delay);
          if (m_probeRequestEvent.IsRunning ())
            {
              m_probeRequestEvent.Cancel ();
            }
          SetState (WAIT_AUTH_RESP);
          SendAuthenticationRequest ();
        }
      return;
    }
  else if (hdr->IsAssocResp ())
    {
      if (m_state == WAIT_ASSOC_RESP)
        {
          MgtAssocResponseHeader assocResp;
          packet->RemoveHeader (assocResp);
          if (m_assocRequestEvent.IsRunning ())
            {
              m_assocRequestEvent.Cancel ();
            }
          if (assocResp.GetStatusCode ().IsSuccess ())
            {
              SetState (ASSOCIATED);
              NS_LOG_DEBUG ("assoc completed");
              SetAID (assocResp.GetAID ());
              SupportedRates rates = assocResp.GetSupportedRates ();
              if (m_htSupported)
                {
                  HtCapabilities htcapabilities = assocResp.GetHtCapabilities ();
                  m_stationManager->AddStationHtCapabilities (hdr->GetAddr2 (),htcapabilities);
                }

              for (uint32_t i = 0; i < m_phy->GetNModes (); i++)
                {
                  WifiMode mode = m_phy->GetMode (i);
                  if (rates.IsSupportedRate (mode.GetDataRate ()))
                    {
                      m_stationManager->AddSupportedMode (hdr->GetAddr2 (), mode);
                      if (rates.IsBasicRate (mode.GetDataRate ()))
                        {
                          m_stationManager->AddBasicMode (mode);
                        }
                    }
                }
              if (m_htSupported)
                {
                  HtCapabilities htcapabilities = assocResp.GetHtCapabilities ();
                  for (uint32_t i = 0; i < m_phy->GetNMcs (); i++)
                    {
                      uint8_t mcs = m_phy->GetMcs (i);
                      if (htcapabilities.IsSupportedMcs (mcs))
                        {
                          m_stationManager->AddSupportedMcs (hdr->GetAddr2 (), mcs);
                          //here should add a control to add basic MCS when it is implemented
                        }
                    }
                }
              if (!m_linkUp.IsNull ())
                {
                  m_linkUp ();
                }
            }
          else if ((assocResp.GetStatusCode ().IsFailurewith17())||(assocResp.GetStatusCode ().IsFailurewith30())||(assocResp.GetStatusCode ().IsFailurewith34()))
		    {
              if (m_assocRequestEvent.IsRunning ())
                 {
            	    m_assocRequestEvent.Cancel ();
                 }
        	  m_assocRequestEvent = Simulator::Schedule (Seconds (2),
        	                                         &StaWifiMac::AssocRequestTimeout, this);
		    }
          else
            {
              NS_LOG_DEBUG ("assoc refused");
              SetState (REFUSED);
            }
        }
      return;
    }
  else if (hdr->IsAuthentication ())
    {
      if (m_state == WAIT_AUTH_RESP)
        {
          MgtAuthFrameHeader authResp;
          packet->RemoveHeader (authResp);
          if (m_authRequestEvent.IsRunning ())
            {
              m_authRequestEvent.Cancel ();
            }
          if (authResp.GetStatusCode ().IsSuccess ())
            {
              m_localTI = 0;
              SetState (WAIT_ASSOC_RESP);
              NS_LOG_DEBUG ("auth completed");
              SendAssociationRequest ();
            }
          else
            {
              NS_LOG_DEBUG ("auth refused");
              SetState (REFUSED);
            }
        }
      return;
    }

  //Invoke the receive handler of our parent class to deal with any
  //other frames. Specifically, this will handle Block Ack-related
  //Management Action frames.
  RegularWifiMac::Receive (packet, hdr);
}

SupportedRates
StaWifiMac::GetSupportedRates (void) const
{
  SupportedRates rates;
  if (m_htSupported)
    {
      for (uint32_t i = 0; i < m_phy->GetNBssMembershipSelectors (); i++)
        {
          rates.SetBasicRate (m_phy->GetBssMembershipSelector (i));
        }
    }
  for (uint32_t i = 0; i < m_phy->GetNModes (); i++)
    {
      WifiMode mode = m_phy->GetMode (i);
      rates.AddSupportedRate (mode.GetDataRate ());
    }
  return rates;
}

HtCapabilities
StaWifiMac::GetHtCapabilities (void) const
{
  HtCapabilities capabilities;
  capabilities.SetHtSupported (1);
  capabilities.SetLdpc (m_phy->GetLdpc ());
  capabilities.SetShortGuardInterval20 (m_phy->GetGuardInterval ());
  capabilities.SetGreenfield (m_phy->GetGreenfield ());
  for (uint8_t i = 0; i < m_phy->GetNMcs (); i++)
    {
      capabilities.SetRxMcsBitmask (m_phy->GetMcs (i));
    }
  return capabilities;
}

void
StaWifiMac::SetState (MacState value)
{
  if (value == ASSOCIATED
      && m_state != ASSOCIATED)
    {
      m_assocLogger (GetAddress ());
    }
  else if (value != ASSOCIATED
           && m_state == ASSOCIATED)
    {
      m_deAssocLogger (GetAddress ());
    }
  m_state = value;
}

} //namespace ns3

