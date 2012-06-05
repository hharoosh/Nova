/*
 * PersonalityTable.h
 *
 *  Created on: Jun 1, 2012
 *      Author: victim
 */

#include "HashMapStructs.h"
#include "Personality.h"
#include "HashMap.h"

//total num_hosts is the total num of unique hosts counted.
//total avail_addrs is the total num of ip addresses avail on the subnet

//Mapping of Personality objects using the OS name as the key
// contains:
/* map of occuring ports (Number_Protocol for key Ex: 22_TCP)
 *  - all behaviors assumed to be open if we have an entry.
 *  - determine default behaviors for TCP, UDP & ICMP somehow.
 * m_count of number of hosts w/ this OS.
 * m_port_count - number of open ports counted for hosts w/ this OS
 * map of occuring MAC addr vendors, so we know what types of NIC's are used for machines of a similar type on a network.
 */

typedef Nova::HashMap<std::string, Nova::Personality *, std::tr1::hash<std::string>, eqstr > Personality_Table;

namespace Nova
{
class PersonalityTable
{
public:
	PersonalityTable();
	~PersonalityTable();

	//Dummy function def -> implement to Add host into the table
	void AddHost(Personality *add);

	//Dummy function def -> implement to produce fuzzy output from populated table
	void* GenerateFuzzyOutput();

	// Dummy function def ->
	// Generate a haystack that matches only what is seen and to near exact ratios, essentially duplicating the network n times until it's full.
	void* GenerateExactOutput();

	void ListInfo();

	//Increment every time a host is added
	unsigned long int m_num_of_hosts;

	//Start with range of the subnets, decrement every time host is added
	unsigned long int m_host_addrs_avail;

	//HashMAP[std::string key]; key == Personality, val == ptr to Personality object
	Personality_Table m_personalities;
};
}
