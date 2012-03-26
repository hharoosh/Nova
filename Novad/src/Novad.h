//============================================================================
// Name        : Novad.h
// Copyright   : DataSoft Corporation 2011-2012
//	Nova is free software: you can redistribute it and/or modify
//   it under the terms of the GNU General Public License as published by
//   the Free Software Foundation, either version 3 of the License, or
//   (at your option) any later version.
//
//   Nova is distributed in the hope that it will be useful,
//   but WITHOUT ANY WARRANTY; without even the implied warranty of
//   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//   GNU General Public License for more details.
//
//   You should have received a copy of the GNU General Public License
//   along with Nova.  If not, see <http://www.gnu.org/licenses/>.
// Description : Nova Daemon to perform network anti-reconnaissance
//============================================================================

#ifndef NOVAD_H_
#define NOVAD_H_

#include "HashMapStructs.h"
#include "Defines.h"
#include "Suspect.h"
#include <arpa/inet.h>

#include <vector>
#include <string>

//Mode to knock on the silent alarm port
#define OPEN true
#define CLOSE false


//Hash table for current list of suspects
typedef google::dense_hash_map<in_addr_t, ANNpoint, std::tr1::hash<in_addr_t>, eqaddr > lastPointHash;


namespace Nova
{

int RunNovaD();

// Send a silent alarm
//		suspect - Suspect to send alarm about
void SilentAlarm(Suspect *suspect, int oldClassification);

// Knocks on the port of the neighboring nova instance to open or close it
//		mode - true for OPEN, false for CLOSE
bool KnockPort(bool mode);

// Receive featureData from another local component.
// This is a blocking function. If nothing is received, then wait on this thread for an answer
// Returns: false if any sort of error
bool Start_Packet_Handler();

// Loads configuration variables
//		configFilePath - Location of configuration file
void LoadConfiguration();

// Append to state file
void AppendToStateFile();

// Cleans up the state file
//	- Removes old entries
//	- Removes cleared suspects
void RefreshStateFile();

// Appends all entries in the state file to the suspect table
void LoadStateFile();

// Force a reload of NOVAConfig/Data.txt while running.
// This will reclassify all the suspects based on the new data.
void Reload();

// Parse through the honeyd config file and get the list of IP addresses used
//		honeyDConfigPath - path to honeyd configuration file
// Returns: vector containing IP addresses of all honeypots
std::vector <std::string> GetHaystackAddresses(std::string honeyDConfigPath);
std::vector <std::string> GetHaystackDhcpAddresses(std::string honeyDConfigPath);

std::string ConstructFilterString();

// Callback function that is passed to pcap_loop(..) and called each time a packet is received
//		useless - Unused
//		pkthdr - pcap packet header
//		packet - packet data
void Packet_Handler(u_char *useless,const struct pcap_pkthdr* pkthdr,const u_char* packet);

// Updates a suspect with evidence to be processed later
//		packet : Packet headers to used for the evidence
void UpdateSuspect(Packet packet);

// Masks the kill signals of a thread so they will get
// sent to the main thread's signal handler.
void MaskKillSignals();

// Updates suspect and stores it as a training data point
void UpdateAndStore(uint64_t suspect);

// Updates data and classification for a suspect
void UpdateAndClassify(uint64_t suspect);


}

#endif /* NOVAD_H_ */
