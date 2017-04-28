/* -*-  Mode: C++; c-file-style: "gnu"; indent-tabs-mode:nil; -*- */
/*
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
*/
#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/applications-module.h"
#include "ns3/wifi-module.h"
#include "ns3/mobility-module.h"
#include "ns3/ipv4-global-routing-helper.h"
#include "ns3/internet-module.h"
#include <iostream>
#include <fstream>
#include <stdio.h>
#include <stdlib.h>
#include <ctime>
#include <fstream>
#include <sys/stat.h>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE ("s1g-wifi-network-le");

static uint32_t payloadSize = 100;
static uint32_t Nsta = 1;
static uint32_t Nsaturated = 20;
static int64_t AssocTime=0;
static uint32_t AssReqTimeout = 512000;
static uint32_t life = 0;
static std::string TrafficInterval="0.002";
static bool associating_stas_created = false;
static bool hidden = false;
static double sum_time = 0;

static int t;
static int64_t l;
static double simtime, t_b=0, t_e;

static std::map <Mac48Address, double> saturated_times, assoc_times;
static std::map <Mac48Address, int> assoc_indices;

struct Experiment {
  ApplicationContainer serverApp;
  Ipv4InterfaceContainer apNodeInterface;
  NodeContainer wifiStaNode;
  NodeContainer wifiSaturatedStaNode;
  NodeContainer wifiApNode;
  NetDeviceContainer staDevice;
  NetDeviceContainer apDevice;
  YansWifiChannelHelper channel;
  YansWifiPhyHelper phy;
  Ssid ssid = Ssid ("ns380211ah");
  WifiHelper wifi;
  InternetStackHelper stack;
  Ipv4AddressHelper address;
} *experiment;

static void
packet_trace (Ptr<const Packet> packet)
{
std::cout << Simulator::Now().GetSeconds() << '\t' << packet->GetSize() << std::endl;
/*  simtime = Simulator::Now().GetSeconds();
  if ((simtime >= 5)&&(simtime < 55))
    {
      l += packet->GetSize();
      std:: cout << " size " << packet->GetSize() << std::endl;
    }
*/
}

static void
phy_rx_begin (Ptr<const Packet> packet)
{
  double prom;
  prom = t_b;
  t_b = Simulator::Now().GetMicroSeconds();
//  std::cout << Simulator::Now().GetSeconds() << "\t"<< " interval " << t_b - prom << std::endl;
  std::cout << Simulator::Now().GetSeconds() << "\tRX Begin" << std::endl;
}

static void
phy_rx_end (Ptr<const Packet> packet)
{
  t_e = Simulator::Now().GetMicroSeconds();
  std::cout << Simulator::Now().GetSeconds() << "\tRX End" << std::endl;
}

static void
phy_tx_begin (std::string context, Ptr<const Packet> packet)
{
  std::cout << Simulator::Now().GetSeconds() << "\tTX Begin" << std::endl;
}

static void
saturatedDeAssoc (std::string context, Mac48Address address)
{
  std::cout << Simulator::Now().GetSeconds() << '\t' << address << "\tDeAssociated" << std::endl;
  if (saturated_times.find(address) != saturated_times.end())
    {
      saturated_times.erase(address);
    }
}

static void
nonSaturatedDeAssoc (std::string context, Mac48Address address)
{
  std::cout << Simulator::Now().GetSeconds() << '\t' << address << "\tDeAssociated" << std::endl;
  if (assoc_times.find(address) != assoc_times.end())
    {
      assoc_times.erase(address);
    }
}

static void
PopulateArpCache ()
{
    Ptr<ArpCache> arp = CreateObject<ArpCache> ();
    arp->SetAliveTimeout (Seconds(3600 * 24 * 365));
    for (NodeList::Iterator i = NodeList::Begin(); i != NodeList::End(); ++i)
    {
        Ptr<Ipv4L3Protocol> ip = (*i)->GetObject<Ipv4L3Protocol> ();
        NS_ASSERT(ip !=0);
        ObjectVectorValue interfaces;
        ip->GetAttribute("InterfaceList", interfaces);
        for(ObjectVectorValue::Iterator j = interfaces.Begin(); j != interfaces.End (); j++)
        {
            Ptr<Ipv4Interface> ipIface = (j->second)->GetObject<Ipv4Interface> ();
            NS_ASSERT(ipIface != 0);
            Ptr<NetDevice> device = ipIface->GetDevice();
            NS_ASSERT(device != 0);
            Mac48Address addr = Mac48Address::ConvertFrom(device->GetAddress ());
            for(uint32_t k = 0; k < ipIface->GetNAddresses (); k ++)
            {
                Ipv4Address ipAddr = ipIface->GetAddress (k).GetLocal();
                if(ipAddr == Ipv4Address::GetLoopback())
                    continue;
                ArpCache::Entry * entry = arp->Add(ipAddr);
                entry->MarkWaitReply(0);
                entry->MarkAlive(addr);
               // std::cout << "Arp Cache: Adding the pair (" << addr << "," << ipAddr << ")" << std::endl;
            }
        }
    }
    for (NodeList::Iterator i = NodeList::Begin(); i != NodeList::End(); ++i)
    {
        Ptr<Ipv4L3Protocol> ip = (*i)->GetObject<Ipv4L3Protocol> ();
        NS_ASSERT(ip !=0);
        ObjectVectorValue interfaces;
        ip->GetAttribute("InterfaceList", interfaces);
        for(ObjectVectorValue::Iterator j = interfaces.Begin(); j !=interfaces.End (); j ++)
        {
            Ptr<Ipv4Interface> ipIface = (j->second)->GetObject<Ipv4Interface> ();
            ipIface->SetAttribute("ArpCache", PointerValue(arp));
        }
    }
}

static void
nonSaturatedAssoc (Mac48Address address)
{
//  std::cout << assoc_times.size() << '\t' << Simulator::Now().GetSeconds() << '\t' << address << std::endl;
  if (assoc_times.find(address) != assoc_times.end())
    {
      std::cerr << "Double association from " << address << std::endl;
    }
  assoc_times[address] = Simulator::Now().GetSeconds();
  sum_time += Simulator::Now().GetMicroSeconds();
  if (assoc_times.size() == Nsta)
    {
      std::cout << Nsta << "\t" << Simulator::Now().GetMicroSeconds() - t << '\t' << sum_time / Nsta - t << std::endl;
      Simulator::Stop();
    }
}


static void
CreateAssociatingStas(void)
{
  t = Simulator::Now ().GetMicroSeconds ();

  experiment->wifiStaNode.Create (Nsta);

  S1gWifiMacHelper mac = S1gWifiMacHelper::Default ();

  mac.SetType ("ns3::StaWifiMac",
               "Ssid", SsidValue (experiment->ssid),
               "ActiveProbing", BooleanValue (false),
               "AssocRequestTimeout", TimeValue (MicroSeconds(AssReqTimeout)));

//  mac.SetType ("ns3::DcaTxop",
//		 "AddTransmitMSDULifetime", UintegerValue (life));

  experiment->staDevice = experiment->wifi.Install (experiment->phy, mac, experiment->wifiStaNode);

  Config::Set ("/NodeList/*/DeviceList/0/$ns3::WifiNetDevice/Mac/$ns3::RegularWifiMac/BE_EdcaTxopN/Queue/MaxPacketNumber", UintegerValue(60000));
  Config::Set ("/NodeList/*/DeviceList/0/$ns3::WifiNetDevice/Mac/$ns3::RegularWifiMac/BE_EdcaTxopN/Queue/MaxDelay", TimeValue (NanoSeconds (6000000000000)));

  // mobility.
  MobilityHelper mobility;

  if (hidden)
    {
      mobility.SetPositionAllocator ("ns3::RandomBoxPositionAllocator",
                                       "X", StringValue ("ns3::UniformRandomVariable[Min=1180.0|Max=1200.0]"),
                                       "Y", StringValue ("ns3::UniformRandomVariable[Min=990.0|Max=1010.0]"),
                                       "Z", StringValue ("ns3::ConstantRandomVariable[Constant=0]"));
    }
  else
    {
      mobility.SetPositionAllocator ("ns3::RandomBoxPositionAllocator",
                                       "X", StringValue ("ns3::UniformRandomVariable[Min=1010.0|Max=1030.0]"),
                                       "Y", StringValue ("ns3::UniformRandomVariable[Min=990.0|Max=1010.0]"),
                                       "Z", StringValue ("ns3::ConstantRandomVariable[Constant=0]"));
    }
  mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
  mobility.Install(experiment->wifiStaNode);

   /* Internet stack*/
  experiment->stack.Install (experiment->wifiStaNode);
  Ipv4InterfaceContainer staNodeInterface;
  staNodeInterface = experiment->address.Assign (experiment->staDevice);

  //trace association
  for (uint16_t i = 0; i < Nsta; i++)
    {
      Ptr<NetDevice> device = experiment->staDevice.Get(i);
      Ptr<WifiNetDevice> sta_wifi = device->GetObject<WifiNetDevice>();
      Ptr<StaWifiMac> sta_mac = DynamicCast<StaWifiMac>(sta_wifi->GetMac());
      sta_mac->TraceConnectWithoutContext ("Assoc", MakeCallback (&nonSaturatedAssoc));

      Mac48Address addr = Mac48Address::ConvertFrom(device->GetAddress ());
      assoc_indices[addr] = i;
    }

  associating_stas_created = true;
  Ptr<WifiNetDevice> ap = experiment->apDevice.Get(0)->GetObject<WifiNetDevice>();
  Ptr<ApWifiMac> apMac = DynamicCast<ApWifiMac>(ap->GetMac());
  apMac->SetAssociatingStasAppear ();
}

static void
saturatedAssoc (Mac48Address address)
{
//  std::cout << saturated_times.size() << '\t' << Simulator::Now().GetSeconds() << '\t' << address << std::endl;
  if (saturated_times.find(address) != saturated_times.end())
    {
      std::cerr << "Double association from " << address << std::endl;
    }
  saturated_times[address] = Simulator::Now().GetSeconds();
  if (saturated_times.size() == Nsaturated && !associating_stas_created)
    {
      double randomInterval = std::stod (TrafficInterval, nullptr);
      Ptr<UniformRandomVariable> m_rv = CreateObject<UniformRandomVariable> ();
      //UDP flow
      UdpServerHelper myServer (9);
      experiment->serverApp = myServer.Install (experiment->wifiApNode);
      experiment->serverApp.Start (Seconds (0));
      
      UdpClientHelper myClient (experiment->apNodeInterface.GetAddress (0), 9); //address of remote node
      myClient.SetAttribute ("MaxPackets", UintegerValue (4294967295u));
      myClient.SetAttribute ("Interval", TimeValue (Time (TrafficInterval)));
      myClient.SetAttribute ("PacketSize", UintegerValue (payloadSize));
      for (uint16_t i = 0; i < Nsaturated; i++)
        {
          double randomStart = m_rv->GetValue (0, randomInterval);
 
          ApplicationContainer clientApp = myClient.Install (experiment->wifiSaturatedStaNode.Get(i));
          clientApp.Start (Seconds (1 + randomStart));
        }

      m_rv = CreateObject<UniformRandomVariable> ();
      uint16_t offset = m_rv->GetValue (1, 5);
    
      Ptr<WifiNetDevice> ap = experiment->apDevice.Get(0)->GetObject<WifiNetDevice>();
      Ptr<ApWifiMac> apMac = DynamicCast<ApWifiMac>(ap->GetMac());
      apMac->SetSaturatedAssociated ();
      Simulator::Schedule(Seconds(offset), &CreateAssociatingStas);
    }
}

static void
ap_assoc_trace (Mac48Address address)
{
  if (assoc_indices.find (address) != assoc_indices.end())
    {
      Ptr<NetDevice> device = experiment->staDevice.Get(assoc_indices[address]);
      Ptr<WifiNetDevice> wifi_device = device->GetObject<WifiNetDevice>();
      DynamicCast<YansWifiChannel>(wifi_device->GetChannel())->Remove(DynamicCast<YansWifiPhy>(wifi_device->GetPhy()));

      Ptr<WifiNetDevice> ap = experiment->apDevice.Get(0)->GetObject<WifiNetDevice>();
      Ptr<WifiMacQueue> apQueue = DynamicCast<ApWifiMac>(ap->GetMac())->GetDcaTxop()->GetQueue();
      apQueue->DropByAddress (address);
    }
}

int main (int argc, char *argv[])
{
  double simulationTime = 10;
  uint32_t seed = 1;
  uint32_t algorithm = 1;
  uint32_t minvalue = 2;
  uint32_t value = 60;
  uint32_t protocol = 0;
  uint32_t authslot = 8;
  uint32_t SlotFormat=1;
  uint32_t BeaconInterval = 500000;
  uint32_t tiMin = 64;
  uint32_t tiMax = 255;
  double bandWidth = 1;
  bool enableRaw = false;
  std::string DataMode = "OfdmRate600KbpsBW1MHz";
  std::string folder="./scratch/";
  std::string file="./scratch/mac-sta.txt";
  std::string pcapfile="./scratch/mac-s1g-slots";

  experiment = new Experiment;
  CommandLine cmd;
  cmd.AddValue ("seed", "random seed", seed);
  cmd.AddValue ("simulationTime", "Simulation time in seconds", simulationTime); 
  cmd.AddValue ("payloadSize", "Size of payload", payloadSize);
  cmd.AddValue ("Nsaturated", "number of saturated stations", Nsaturated);
  cmd.AddValue ("Nsta", "number of connecting stations", Nsta);
  cmd.AddValue ("BeaconInterval", "Beacon interval time in us", BeaconInterval);
  cmd.AddValue ("DataMode", "Date mode", DataMode);
  cmd.AddValue ("bandWidth", "bandwidth in MHz", bandWidth);
  cmd.AddValue ("UdpInterval", "traffic mode", TrafficInterval);
  cmd.AddValue ("folder", "folder where result files are placed", folder);
  cmd.AddValue ("file", "files containing reslut information", file);
  cmd.AddValue ("pcapfile", "files containing reslut information", pcapfile);
  cmd.AddValue ("algorithm","choice algorithm", algorithm);
  cmd.AddValue ("AssReqTimeout", "Association request timeout", AssReqTimeout);
  cmd.AddValue ("MinValue", "Minimum connected stations", minvalue);
  cmd.AddValue ("Value", "Delta for algorithm", value);
  cmd.AddValue ("addlifetime", "adding timeout MaxMSDULifetime", life);
  cmd.AddValue ("protocol", "adding choice of centralized or distributed protocol", protocol);
  cmd.AddValue ("authslot", "authenticate slot for distributed protocol", authslot);
  cmd.AddValue ("tiMin", "minimal time interval for DAC", tiMin);
  cmd.AddValue ("tiMax", "maximal time interval for DAC", tiMax);
  cmd.AddValue ("hidden", "Saturated STAs should be hidden", hidden);
  cmd.AddValue ("enableRaw", "RAW should be used", enableRaw);
//  cmd.AddValue ("lifetime", "Packet lifetime after start of transmit", lifetime);
  cmd.Parse (argc,argv);
  
  RngSeedManager::SetSeed (seed);

  experiment->wifiSaturatedStaNode.Create (Nsaturated);
  experiment->wifiApNode.Create (1);

  experiment->channel = YansWifiChannelHelper ();
  if (hidden)
    {
      experiment->channel.AddPropagationLoss ("ns3::LogDistancePropagationLossModel","Exponent", DoubleValue(5) ,"ReferenceLoss", DoubleValue(8.0), "ReferenceDistance", DoubleValue(1.0));
    }
  else
    {
      experiment->channel.AddPropagationLoss ("ns3::FriisPropagationLossModel");
    }
  experiment->channel.SetPropagationDelay ("ns3::ConstantSpeedPropagationDelayModel");
        
  experiment->phy = YansWifiPhyHelper::Default ();
  experiment->phy.SetErrorRateModel ("ns3::YansErrorRateModel");
  experiment->phy.SetChannel (experiment->channel.Create ());
  experiment->phy.Set ("ShortGuardEnabled", BooleanValue (false));
  experiment->phy.Set ("ChannelWidth", UintegerValue (bandWidth));
  experiment->phy.Set ("EnergyDetectionThreshold", DoubleValue (-116.0));
  experiment->phy.Set ("CcaMode1Threshold", DoubleValue (-119.0));
  experiment->phy.Set ("RxNoiseFigure", DoubleValue (3.0));
  experiment->phy.Set ("TxPowerEnd", DoubleValue (30.0));
  experiment->phy.Set ("TxPowerStart", DoubleValue (30.0));
  experiment->phy.Set ("TxGain", DoubleValue (3.0));
  experiment->phy.Set ("RxGain", DoubleValue (3.0));
  experiment->phy.Set ("LdpcEnabled", BooleanValue (true));

  experiment->wifi = WifiHelper::Default ();
  experiment->wifi.SetStandard (WIFI_PHY_STANDARD_80211ah);
  S1gWifiMacHelper mac = S1gWifiMacHelper::Default ();

  experiment->ssid = Ssid ("ns380211ah");
  StringValue DataRate;
  DataRate = StringValue (DataMode);
  
  experiment->wifi.SetRemoteStationManager ("ns3::ConstantRateWifiManager","DataMode", DataRate,
                                    "ControlMode", DataRate);
  Config::SetDefault("ns3::DcaTxop::AddTransmitMSDULifetime", UintegerValue (life));

  mac.SetType ("ns3::ApWifiMac",
                 "Ssid", SsidValue (experiment->ssid),
                 "BeaconInterval", TimeValue (MicroSeconds(BeaconInterval)),
		 "AuthenProtocol", UintegerValue (protocol),
		 "Algorithm", UintegerValue (algorithm),
		 "MinValue", UintegerValue (minvalue),
		 "Value", UintegerValue (value),
                 "NAssociating", UintegerValue (Nsta));

  if (enableRaw)
    {
      mac.SetType ("ns3::ApWifiMac",
                   "RawEnabled", BooleanValue (true),
                   "NRawStations", UintegerValue (Nsaturated),
                   "SlotFormat", UintegerValue (1),
                   "SlotCrossBoundary", UintegerValue (1),
                   "SlotDurationCount", UintegerValue (1),
                   "SlotNum", UintegerValue (1));
    }

  mac.SetType ("ns3::ApWifiMac",
		 "AuthenSlot", UintegerValue (authslot),
                 "TIMin", UintegerValue(tiMin),
                 "TIMax", UintegerValue(tiMax));

//  mac.SetType ("ns3::DcaTxop",
//		 "AddTransmitMSDULifetime", UintegerValue (life));

  experiment->apDevice = experiment->wifi.Install (experiment->phy, mac, experiment->wifiApNode);

  experiment->phy.Set ("TxPowerEnd", DoubleValue (16.0206));    // 40 mW
  experiment->phy.Set ("TxPowerStart", DoubleValue (16.0206));  // 40 mW
  experiment->phy.Set ("TxGain", DoubleValue (1.0));
  experiment->phy.Set ("RxGain", DoubleValue (1.0));

  mac.SetType ("ns3::StaWifiMac",
                "Ssid", SsidValue (experiment->ssid),
                "ActiveProbing", BooleanValue (false),
		"AssocRequestTimeout", TimeValue (MicroSeconds(AssReqTimeout)));

  NetDeviceContainer saturatedStaDevice;
  saturatedStaDevice = experiment->wifi.Install (experiment->phy, mac, experiment->wifiSaturatedStaNode);

  Config::Set ("/NodeList/*/DeviceList/0/$ns3::WifiNetDevice/Mac/$ns3::RegularWifiMac/BE_EdcaTxopN/Queue/MaxPacketNumber", UintegerValue(60000));
  Config::Set ("/NodeList/*/DeviceList/0/$ns3::WifiNetDevice/Mac/$ns3::RegularWifiMac/BE_EdcaTxopN/Queue/MaxDelay", TimeValue (NanoSeconds (6000000000000)));

  // mobility.
  MobilityHelper mobility;
  if (hidden)
    {
      mobility.SetPositionAllocator ("ns3::RandomBoxPositionAllocator",
                                         "X", StringValue ("ns3::UniformRandomVariable[Min=800.0|Max=820.0]"),
                                         "Y", StringValue ("ns3::UniformRandomVariable[Min=990.0|Max=1010.0]"),
                                         "Z", StringValue ("ns3::ConstantRandomVariable[Constant=0]"));
    }
  else
    {
      mobility.SetPositionAllocator ("ns3::RandomBoxPositionAllocator",
                                       "X", StringValue ("ns3::UniformRandomVariable[Min=970.0|Max=990.0]"),
                                       "Y", StringValue ("ns3::UniformRandomVariable[Min=990.0|Max=1010.0]"),
                                       "Z", StringValue ("ns3::ConstantRandomVariable[Constant=0]"));
    }
  mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
  mobility.Install(experiment->wifiSaturatedStaNode);

  MobilityHelper mobilityAp;
  Ptr<ListPositionAllocator> positionAlloc = CreateObject<ListPositionAllocator> ();

  positionAlloc->Add (Vector (1000.0, 1000.0, 0.0));

  mobilityAp.SetPositionAllocator (positionAlloc);
  mobilityAp.SetMobilityModel("ns3::ConstantPositionMobilityModel");
  mobilityAp.Install(experiment->wifiApNode);

 
   /* Internet stack*/
  experiment->stack.Install (experiment->wifiApNode);
  experiment->stack.Install (experiment->wifiSaturatedStaNode);

  experiment->address.SetBase ("192.168.0.0", "255.255.0.0");
  Ipv4InterfaceContainer saturatedStaNodeInterface;

  saturatedStaNodeInterface = experiment->address.Assign (saturatedStaDevice);
  experiment->apNodeInterface = experiment->address.Assign (experiment->apDevice);

  //trace association
  for (uint16_t kk = 0; kk < Nsaturated; kk++)
    {
      Ptr<WifiNetDevice> sta_wifi = saturatedStaDevice.Get(kk)->GetObject<WifiNetDevice>();
      Ptr<StaWifiMac> sta_mac = DynamicCast<StaWifiMac>(sta_wifi->GetMac());
      sta_mac->TraceConnectWithoutContext ("Assoc", MakeCallback (&saturatedAssoc));
    }
  
  Ptr<WifiNetDevice> apWifiDevice = experiment->apDevice.Get(0)->GetObject<WifiNetDevice>();
  Ptr<WifiPhy> apphy = apWifiDevice->GetPhy();
  Ptr<RegularWifiMac> apmac = DynamicCast<RegularWifiMac>(apWifiDevice->GetMac());
  Ptr<MacLow> aplow = apmac->GetMacLow();
  aplow->TraceConnectWithoutContext ("ApAssoc", MakeCallback (&ap_assoc_trace));

//  apmac->TraceConnectWithoutContext ("MacRx", MakeCallback (&packet_trace));
//  apphy->TraceConnectWithoutContext ("PhyRxBegin", MakeCallback (&phy_rx_begin));
//  apphy->TraceConnectWithoutContext ("PhyRxEnd", MakeCallback (&phy_rx_end));
//  Config::Connect ("/NodeList/1/DeviceList/0/$ns3::WifiNetDevice/Phy/$ns3::WifiPhy/PhyTxBegin", MakeCallback (&phy_tx_begin));
//    Simulator::Stop(Seconds(55));

    Ipv4GlobalRoutingHelper::PopulateRoutingTables ();
    PopulateArpCache ();
    
 //   Simulator::Stop(Seconds(1));
    Simulator::Run ();
    Simulator::Destroy ();
    delete experiment;

return 0;
}
