#include "PersonalityTable.h"

using namespace std;

namespace Nova
{
PersonalityTable::PersonalityTable()
{
	m_personalities.set_empty_key("");
	m_num_of_hosts = 0;
	m_host_addrs_avail = 0;
}

PersonalityTable::~PersonalityTable()
{}

void PersonalityTable::ListInfo()
{
	for(Personality_Table::iterator it = m_personalities.begin(); it != m_personalities.end(); it++)
	{
		std::cout << std::endl;

		std::cout << it->second->m_personalityClass[0] << ": ";

		for(uint16_t i = it->second->m_personalityClass.size() - 1; i > 1 ; i--)
		{
			std::cout << it->second->m_personalityClass[i] << " | ";
		}

		std::cout << it->second->m_personalityClass[1];

		std::cout << std::endl;

		std::cout << "Appeared " << it->second->m_count << " time(s) on the network" << std::endl;

		std::cout << "Associated IPs: " << std::endl;
		for(uint16_t i = 0; i < it->second->m_addresses.size(); i++)
		{
			std::cout << "\t" << it->second->m_addresses[i] << std::endl;
		}

		std::cout << "Associated MACs: " << std::endl;

		for(uint16_t j = 0; j < it->second->m_macs.size(); j++)
		{
			std::cout << "\t" << it->second->m_macs[j] << std::endl;
		}

		std::cout << "All MAC vendors associated with this Personality: " << std::endl;

		for(MAC_Table::iterator it2 = it->second->m_vendors.begin(); it2 != it->second->m_vendors.end(); it2++)
		{
			std::cout << "\t" << it2->first << " occurred " << it2->second << " time(s)." << std::endl;
		}

		std::cout << "Ports for this Personality: " << std::endl;

		for(Port_Table::iterator it2 = it->second->m_ports.begin(); it2 != it->second->m_ports.end(); it2++)
		{
			std::cout << "\t" << it2->first << " occurred " << it2->second << " time(s)." << std::endl;
		}

		std::cout << std::endl;
	}
}

// Add a single Host
void PersonalityTable::AddHost(Personality * add)
{
	m_num_of_hosts++;
	m_host_addrs_avail--;

	if(m_personalities.find(add->m_personalityClass[0]) == m_personalities.end())
	{
		m_personalities[add->m_personalityClass[0]] = add;
	}
	else
	{
		Personality * cur = m_personalities[add->m_personalityClass[0]];
		cur->m_macs.push_back(add->m_macs[0]);
		cur->m_addresses.push_back(add->m_addresses[0]);
		if(!add->m_addresses[0].compare("192.168.3.112"))
		{
			std::cout << "failbox found" << std::endl;
		}
		cur->m_count++;

		for(Port_Table::iterator it = add->m_ports.begin(); it != add->m_ports.end(); it++)
		{
			if(cur->m_ports.find(it->first) == cur->m_ports.end())
			{
				cur->m_ports[it->first] = 1;
			}
			else
			{
				cur->m_ports[it->first]++;
			}
			cur->m_port_count++;
		}


		for(MAC_Table::iterator it = add->m_vendors.begin(); it != add->m_vendors.end(); it++)
		{
			if(cur->m_vendors.find(it->first) == cur->m_vendors.end())
			{
				cur->m_vendors[it->first] = 1;
			}
			else
			{
				cur->m_vendors[it->first]++;
			}
		}
		// rest of stuff

		delete add;
	}
}
}
