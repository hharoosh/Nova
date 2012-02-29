//============================================================================
// Name        : novagui.cpp
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
// Description : The main NovaGUI component, utilizes the auto-generated ui_novagui.h
//============================================================================
#include "novagui.h"
#include "NovaUtil.h"
#include "run_popup.h"
#include "novaconfig.h"
#include "nova_manual.h"
#include "classifierPrompt.h"

#include <boost/property_tree/xml_parser.hpp>
#include <boost/foreach.hpp>
#include <sys/socket.h>
#include <QFileDialog>
#include <sys/un.h>
#include <signal.h>
#include <syslog.h>
#include <errno.h>
#include <QDir>


using namespace std;
using namespace Nova;

//Socket communication variables
int NovadInSocket, NovadOutSocket;
struct sockaddr_un NovadInAddress, NovadOutAddress;
int len;

//GUI to Nova message variables
GUIMsg message = GUIMsg();
u_char msgBuffer[MAX_GUIMSG_SIZE];
int msgLen = 0;

//Receive Suspect variables
u_char buf[MAX_MSG_SIZE];
int bytesRead;

pthread_rwlock_t lock;
string homePath, readPath, writePath;


//General variables like tables, flags, locks, etc.
SuspectGUIHashTable SuspectTable;

// Defines the order of components in the process list and novaComponents array
#define COMPONENT_NOVAD 0
#define COMPONENT_DMH 1
#define COMPONENT_HSH 2

#define NOVA_COMPONENTS 3


struct novaComponent novaComponents[NOVA_COMPONENTS];

/************************************************
 * Constructors, Destructors and Closing Actions
 ************************************************/

//Called when process receives a SIGINT, like if you press ctrl+c
void sighandler(int param)
{
	param = param;
	StopNova();
	exit(EXIT_SUCCESS);
}

NovaGUI::NovaGUI(QWidget *parent)
    : QMainWindow(parent)
{
	signal(SIGINT, sighandler);
	pthread_rwlock_init(&lock, NULL);
	SuspectTable.set_empty_key(1);
	SuspectTable.set_deleted_key(5);
	m_subnets.set_empty_key("");
	m_ports.set_empty_key("");
	m_nodes.set_empty_key("");
	m_profiles.set_empty_key("");
	m_scripts.set_empty_key("");
	m_subnets.set_deleted_key("Deleted");
	m_nodes.set_deleted_key("Deleted");
	m_profiles.set_deleted_key("Deleted");
	m_ports.set_deleted_key("Deleted");
	m_scripts.set_deleted_key("Deleted");

	m_editingSuspectList = false;
	m_pathsFile = (char*)"/etc/nova/paths";


	ui.setupUi(this);

	//Pre-forms the suspect menu
	m_suspectMenu = new QMenu(this);
	m_systemStatMenu = new QMenu(this);

	m_isHelpUp = false;

	openlog("NovaGUI", OPEN_SYSL, LOG_AUTHPRIV);

	if( !NOVAConfiguration::InitUserConfigs(GetHomePath()) )
	{
		syslog(SYSL_ERR, "Error: InitUserConfigs failed. Your home folder and permissions may not have been configured properly");	
		//exit(EXIT_FAILURE);
	}

	InitSession();
	InitiateSystemStatus();

	// Create the dialog generator
	prompter= new DialogPrompter();

	// Register our desired error message types
	messageType t;
	t.action = CHOICE_SHOW;

	// Error prompts
	t.type = errorPrompt;

	t.descriptionUID = "Failure reading config files";
	CONFIG_READ_FAIL = prompter->RegisterDialog(t);

	t.descriptionUID = "Failure writing config files";
	CONFIG_WRITE_FAIL = prompter->RegisterDialog(t);

	t.descriptionUID = "Failure reading honeyd files";
	HONEYD_READ_FAIL = prompter->RegisterDialog(t);

	t.descriptionUID = "Failure loading honeyd config files";
	HONEYD_LOAD_FAIL = prompter->RegisterDialog(t);

	t.descriptionUID = "Unexpected file entries";
	UNEXPECTED_ENTRY = prompter->RegisterDialog(t);

	t.descriptionUID = "Honeyd subnets out of range";
	HONEYD_INVALID_SUBNET = prompter->RegisterDialog(t);

	t.descriptionUID = "Cannot delete the selected port";
	CANNOT_DELETE_PORT = prompter->RegisterDialog(t);

	// Action required notification prompts
	t.type = notifyActionPrompt;

	t.descriptionUID = "Request to merge CE capture into training Db";
	LAUNCH_TRAINING_MERGE = prompter->RegisterDialog(t);

	t.descriptionUID = "Cannot inherit the selected port";
	CANNOT_INHERIT_PORT = prompter->RegisterDialog(t);

	t.descriptionUID = "Cannot delete the selected item";
	CANNOT_DELETE_ITEM = prompter->RegisterDialog(t);

	// Preventable warnings
	t.type = warningPreventablePrompt;

	t.descriptionUID = "No Doppelganger could be found";
	NO_DOPP = prompter->RegisterDialog(t);

	// Misc other prompts
	t.descriptionUID = "Problem inheriting port";
	t.type = notifyPrompt;
	NO_ANCESTORS = prompter->RegisterDialog(t);

	t.descriptionUID = "Loading a Haystack Node Failed";
	t.type = warningPrompt;
	NODE_LOAD_FAIL = prompter->RegisterDialog(t);

	LoadAllTemplates();

	//This register meta type function needs to be called for any object types passed through a signal
	qRegisterMetaType<in_addr_t>("in_addr_t");
	qRegisterMetaType<QItemSelection>("QItemSelection");

	//Sets up the socket addresses
	InitSocketAddresses();

	//Create listening socket, listen thread and draw thread --------------
	pthread_t CEListenThread;
	pthread_t StatusUpdateThread;

	if((NovadInSocket = socket(AF_UNIX,SOCK_STREAM,0)) == -1)
	{
		syslog(SYSL_ERR, "File: %s Line: %d socket: %s", __FILE__, __LINE__, strerror(errno));
		CloseSocket(NovadInSocket);
		exit(EXIT_FAILURE);
	}

	len = strlen(NovadInAddress.sun_path) + sizeof(NovadInAddress.sun_family);

	if(bind(NovadInSocket,(struct sockaddr *)&NovadInAddress,len) == -1)
	{
		syslog(SYSL_ERR, "File: %s Line: %d bind: %s", __FILE__, __LINE__, strerror(errno));
		CloseSocket(NovadInSocket);
		exit(EXIT_FAILURE);
	}

	if(listen(NovadInSocket, SOCKET_QUEUE_SIZE) == -1)
	{
		syslog(SYSL_ERR, "File: %s Line: %d listen: %s", __FILE__, __LINE__, strerror(errno));
		CloseSocket(NovadInSocket);
		exit(EXIT_FAILURE);
	}

	//Sets initial view
	this->ui.stackedWidget->setCurrentIndex(0);
	this->ui.mainButton->setFlat(true);
	this->ui.suspectButton->setFlat(false);
	this->ui.doppelButton->setFlat(false);
	this->ui.haystackButton->setFlat(false);
	connect(this, SIGNAL(newSuspect(in_addr_t)), this, SLOT(DrawSuspect(in_addr_t)), Qt::BlockingQueuedConnection);
	connect(this, SIGNAL(refreshSystemStatus()), this, SLOT(UpdateSystemStatus()), Qt::BlockingQueuedConnection);

	pthread_create(&CEListenThread,NULL,NovadListenLoop, this);
	pthread_create(&StatusUpdateThread,NULL,StatusUpdate, this);
}

NovaGUI::~NovaGUI()
{

}


//Draws the suspect context menu
void NovaGUI::contextMenuEvent(QContextMenuEvent * event)
{
	if(ui.suspectList->hasFocus() || ui.suspectList->underMouse())
	{
		m_suspectMenu->clear();
		if(ui.suspectList->isItemSelected(ui.suspectList->currentItem()))
		{
			m_suspectMenu->addAction(ui.actionClear_Suspect);
			m_suspectMenu->addAction(ui.actionHide_Suspect);
		}

		m_suspectMenu->addSeparator();
		m_suspectMenu->addAction(ui.actionClear_All_Suspects);
		m_suspectMenu->addAction(ui.actionSave_Suspects);
		m_suspectMenu->addSeparator();

		m_suspectMenu->addAction(ui.actionShow_All_Suspects);
		m_suspectMenu->addAction(ui.actionHide_Old_Suspects);

		QPoint globalPos = event->globalPos();
		m_suspectMenu->popup(globalPos);
	}
	else if(ui.hostileList->hasFocus() || ui.hostileList->underMouse())
	{
		m_suspectMenu->clear();
		if(ui.hostileList->isItemSelected(ui.hostileList->currentItem()))
		{
			m_suspectMenu->addAction(ui.actionClear_Suspect);
			m_suspectMenu->addAction(ui.actionHide_Suspect);
		}

		m_suspectMenu->addSeparator();
		m_suspectMenu->addAction(ui.actionClear_All_Suspects);
		m_suspectMenu->addAction(ui.actionSave_Suspects);
		m_suspectMenu->addSeparator();

		m_suspectMenu->addAction(ui.actionShow_All_Suspects);
		m_suspectMenu->addAction(ui.actionHide_Old_Suspects);

		QPoint globalPos = event->globalPos();
		m_suspectMenu->popup(globalPos);
	}
	else if(ui.systemStatusTable->hasFocus() || ui.systemStatusTable->underMouse())
	{
		m_systemStatMenu->clear();

		int row = ui.systemStatusTable->currentRow();
		if (row < 0 || row > NOVA_COMPONENTS)
		{
			syslog(SYSL_ERR, "File: %s Line: %d Invalid System Status Selection Row, ignoring", __FILE__, __LINE__);
			return;
		}

		if (novaComponents[row].process != NULL && novaComponents[row].process->pid())
		{
			m_systemStatMenu->addAction(ui.actionSystemStatKill);

			if (row != COMPONENT_DMH && row != COMPONENT_HSH)
				m_systemStatMenu->addAction(ui.actionSystemStatStop);

			if (row == COMPONENT_NOVAD)
				m_systemStatMenu->addAction(ui.actionSystemStatReload);
		}
		else
		{
			m_systemStatMenu->addAction(ui.actionSystemStatStart);
		}

		QPoint globalPos = event->globalPos();
		m_systemStatMenu->popup(globalPos);
	}
	else
	{
		return;
	}
}

void NovaGUI::closeEvent(QCloseEvent * e)
{
	e = e;
	StopNova();
}

/************************************************
 * Gets preliminary information
 ************************************************/

void NovaGUI::InitSession()
{
	InitPaths();
	configuration = new NOVAConfiguration();
	LoadSettings();
	InitNovadCommands();
}

void NovaGUI::InitNovadCommands()
{
	novaComponents[COMPONENT_NOVAD].name = "NOVA Daemon";
	novaComponents[COMPONENT_NOVAD].terminalCommand = "xterm -geometry \"+0+600\" -e Novad";
	novaComponents[COMPONENT_NOVAD].noTerminalCommand = "nohup Novad";
	novaComponents[COMPONENT_NOVAD].shouldBeRunning = false;

	novaComponents[COMPONENT_DMH].name ="Doppelganger Honeyd";
	novaComponents[COMPONENT_DMH].terminalCommand ="xterm -geometry \"+500+0\" -e sudo honeyd -d -i lo -f "+homePath+"/Config/doppelganger.config -p "+readPath+"/nmap-os-db -s /var/log/honeyd/honeydDoppservice.log 10.0.0.0/8";
	novaComponents[COMPONENT_DMH].noTerminalCommand ="nohup sudo honeyd -d -i lo -f "+homePath+"/Config/doppelganger.config -p "+readPath+"/nmap-os-db -s /var/log/honeyd/honeydDoppservice.log 10.0.0.0/8";
	novaComponents[COMPONENT_DMH].shouldBeRunning = false;

	novaComponents[COMPONENT_HSH].name ="Haystack Honeyd";
	novaComponents[COMPONENT_HSH].terminalCommand ="xterm -geometry \"+0+0\" -e sudo honeyd -d -i " + configuration->getInterface() + " -f "+homePath+"/Config/haystack.config -p "+readPath+"/nmap-os-db -s /var/log/honeyd/honeydHaystackservice.log -t /var/log/honeyd/ipList";
	novaComponents[COMPONENT_HSH].noTerminalCommand ="nohup sudo honeyd -d -i " + configuration->getInterface() + " -f "+homePath+"/Config/haystack.config -p "+readPath+"/nmap-os-db -s /var/log/honeyd/honeydHaystackservice.log -t /var/log/honeyd/ipList";
	novaComponents[COMPONENT_HSH].shouldBeRunning = false;
}
void NovaGUI::InitPaths()
{
	homePath = GetHomePath();
	readPath = GetReadPath();
	writePath = GetWritePath();

	if((homePath == "") || (readPath == "") || (writePath == ""))
	{
		exit(EXIT_FAILURE);
	}

	QDir::setCurrent((QString)homePath.c_str());
}

void NovaGUI::LoadSettings()
{
	string line, prefix; //used for input checking

	//Get locations of nova files
	ifstream *settings =  new ifstream((homePath+"/settings").c_str());

	if(settings->is_open())
	{
		while(settings->good())
		{
			getline(*settings,line);
			prefix = "group";
			if(!line.substr(0,prefix.size()).compare(prefix))
			{
				line = line.substr(prefix.size()+1,line.size());
				m_group = line;
				continue;
			}
		}
	}
	settings->close();
	delete settings;
	settings = NULL;
}

void NovaGUI::SystemStatusRefresh()
{
	Q_EMIT refreshSystemStatus();
}

/************************************************
 * Save Honeyd XML Configuration Functions
 ************************************************/

//Saves the current configuration information to XML files

//**Important** this function assumes that unless it is a new item (ptree pointer == NULL) then
// all required fields exist and old fields have been removed. Ex: if a port previously used a script
// but now has a behavior of open, at that point the user should have erased the script field.
// inverserly if a user switches to script the script field must be created.

//To summarize this function only populates the xml data for the values it contains unless it is a new item,
// it does not clean up, and only creates if it's a new item and then only for the fields that are needed.
// it does not track profile inheritance either, that should be created when the heirarchy is modified.
void NovaGUI::SaveAllTemplates()
{
	using boost::property_tree::ptree;
	ptree pt;

	//Scripts
	m_scriptTree.clear();
	for(ScriptTable::iterator it = m_scripts.begin(); it != m_scripts.end(); it++)
	{
		pt = it->second.tree;
		pt.put<std::string>("name", it->second.name);
		pt.put<std::string>("path", it->second.path);
		m_scriptTree.add_child("scripts.script", pt);
	}

	//Ports
	m_portTree.clear();
	for(PortTable::iterator it = m_ports.begin(); it != m_ports.end(); it++)
	{
		pt = it->second.tree;
		pt.put<std::string>("name", it->second.portName);
		pt.put<std::string>("number", it->second.portNum);
		pt.put<std::string>("type", it->second.type);
		pt.put<std::string>("behavior", it->second.behavior);
		//If this port uses a script, save it.
		if(!it->second.behavior.compare("script") || !it->second.behavior.compare("internal"))
		{
			pt.put<std::string>("script", it->second.scriptName);
		}
		//If the port works as a proxy, save destination
		else if(!it->second.behavior.compare("proxy"))
		{
			pt.put<std::string>("IP", it->second.proxyIP);
			pt.put<std::string>("Port", it->second.proxyPort);
		}
		m_portTree.add_child("ports.port", pt);
	}

	m_subnetTree.clear();
	for(SubnetTable::iterator it = m_subnets.begin(); it != m_subnets.end(); it++)
	{
		pt = it->second.tree;

		//TODO assumes subnet is interface, need to discover and handle if virtual
		pt.put<std::string>("name", it->second.name);
		pt.put<bool>("enabled",it->second.enabled);
		pt.put<bool>("isReal", it->second.isRealDevice);

		//Remove /## format mask from the address then put it in the XML.
		stringstream ss;
		ss << "/" << it->second.maskBits;
		int i = ss.str().size();
		string temp = it->second.address.substr(0,(it->second.address.size()-i));
		pt.put<std::string>("IP", temp);

		//Gets the mask from mask bits then put it in XML
		in_addr_t mask = pow(2, 32-it->second.maskBits) - 1;
		//If maskBits is 24 then we have 2^8 -1 = 0x000000FF
		mask = ~mask; //After getting the inverse of this we have the mask in host addr form.
		//Convert to network order, put in in_addr struct
		//call ntoa to get char * and make string
		in_addr tempMask;
		tempMask.s_addr = htonl(mask);
		temp = string(inet_ntoa(tempMask));
		pt.put<std::string>("mask", temp);
		m_subnetTree.add_child("interface", pt);
	}

	//Nodes
	m_nodesTree.clear();
	for(NodeTable::iterator it = m_nodes.begin(); it != m_nodes.end(); it++)
	{
		pt = it->second.tree;
		//Required xml entires
		pt.put<std::string>("interface", it->second.interface);
		pt.put<std::string>("IP", it->second.IP);
		pt.put<bool>("enabled", it->second.enabled);
		pt.put<std::string>("name", it->second.name);
		if(it->second.MAC.size())
			pt.put<std::string>("MAC", it->second.MAC);
		pt.put<std::string>("profile.name", it->second.pfile);
		m_nodesTree.add_child("node",pt);
	}
	using boost::property_tree::ptree;
	BOOST_FOREACH(ptree::value_type &v, m_groupTree.get_child("groups"))
	{
		//Find the specified group
		if(!v.second.get<std::string>("name").compare(m_group))
		{
			//Load Subnets first, they are needed before we can load nodes
			v.second.put_child("subnets", m_subnetTree);
			v.second.put_child("nodes",m_nodesTree);
		}
	}
	m_profileTree.clear();
	for(ProfileTable::iterator it = m_profiles.begin(); it != m_profiles.end(); it++)
	{
		if(it->second.parentProfile == "")
		{
			pt = it->second.tree;
			m_profileTree.add_child("profiles.profile", pt);
		}
	}
	boost::property_tree::xml_writer_settings<char> settings('\t', 1);
	write_xml(homePath+"/scripts.xml", m_scriptTree, std::locale(), settings);
	write_xml(homePath+"/templates/ports.xml", m_portTree, std::locale(), settings);
	write_xml(homePath+"/templates/nodes.xml", m_groupTree, std::locale(), settings);
	write_xml(homePath+"/templates/profiles.xml", m_profileTree, std::locale(), settings);
}

//Writes the current configuration to honeyd configs
void NovaGUI::WriteHoneydConfiguration()
{
	stringstream out;
	stringstream doppelOut;

	vector<string> profilesParsed;

	for (ProfileTable::iterator it = m_profiles.begin(); it != m_profiles.end(); it++)
	{
		if (!it->second.parentProfile.compare(""))
		{
			string pString = ProfileToString(&it->second);
			out << pString;
			doppelOut << pString;
			profilesParsed.push_back(it->first);
		}
	}

	while (profilesParsed.size() < m_profiles.size())
	{
		for (ProfileTable::iterator it = m_profiles.begin(); it != m_profiles.end(); it++)
		{
			bool selfMatched = false;
			bool parentFound = false;
			for (uint i = 0; i < profilesParsed.size(); i++)
			{
				if(!it->second.parentProfile.compare(profilesParsed[i]))
				{
					parentFound = true;
					continue;
				}
				if (!it->first.compare(profilesParsed[i]))
				{
					selfMatched = true;
					break;
				}
			}

			if(!selfMatched && parentFound)
			{
				string pString = ProfileToString(&it->second);
				out << pString;
				doppelOut << pString;
				profilesParsed.push_back(it->first);

			}
		}
	}

	// Start node section
	out << endl << endl;
	for (NodeTable::iterator it = m_nodes.begin(); it != m_nodes.end(); it++)
	{
		if (!it->second.enabled)
		{
			continue;
		}
		else if(!it->second.name.compare("Doppelganger"))
		{
			doppelOut << "bind " << it->second.IP << " " << it->second.pfile << endl;
		}
		else switch (m_profiles[it->second.pfile].type)
		{
			case static_IP:
				out << "bind " << it->second.IP << " " << it->second.pfile << endl;
				if(it->second.MAC.compare(""))
					out << "set " << it->second.IP << " ethernet \"" << it->second.MAC << "\"" << endl;
				break;
			case staticDHCP:
				out << "dhcp " << it->second.pfile << " on " << it->second.interface << " ethernet \"" << it->second.MAC << "\"" << endl;
				break;
			case randomDHCP:
				out << "dhcp " << it->second.pfile << " on " << it->second.interface << endl;
				break;
		}
	}

	ofstream outFile(configuration->getPathConfigHoneydHs().data());
	outFile << out.str() << endl;
	outFile.close();

	ofstream doppelOutFile(configuration->getPathConfigHoneydDm().data());
	doppelOutFile << doppelOut.str() << endl;
	doppelOutFile.close();
}

string NovaGUI::ProfileToString(profile* p)
{
	stringstream out;

	if (!p->parentProfile.compare("default") || !p->parentProfile.compare(""))
		out << "create " << p->name << endl;
	else
		out << "clone " << p->parentProfile << " " << p->name << endl;

	out << "set " << p->name  << " default tcp action " << p->tcpAction << endl;
	out << "set " << p->name  << " default udp action " << p->udpAction << endl;
	out << "set " << p->name  << " default icmp action " << p->icmpAction << endl;

	if (p->personality.compare(""))
		out << "set " << p->name << " personality \"" << p->personality << '"' << endl;

	if (p->ethernet.compare(""))
		out << "set " << p->name << " ethernet \"" << p->ethernet << '"' << endl;

	if (p->uptime.compare(""))
		out << "set " << p->name << " uptime " << p->uptime << endl;

	if (p->dropRate.compare(""))
		out << "set " << p->name << " droprate in " << p->dropRate << endl;

	for (uint i = 0; i < p->ports.size(); i++)
	{
		// Only include non-inherited ports
		if (!p->ports[i].second)
		{
			out << "add " << p->name;
			if(!m_ports[p->ports[i].first].type.compare("TCP"))
				out << " tcp port ";
			else
				out << " udp port ";
			out << m_ports[p->ports[i].first].portNum << " ";

			if (!(m_ports[p->ports[i].first].behavior.compare("script")))
			{
				string scriptName = m_ports[p->ports[i].first].scriptName;

				if (m_scripts[scriptName].path.compare(""))
					out << '"' << m_scripts[scriptName].path << '"'<< endl;
				else
					syslog(SYSL_ERR, "File: %s Line: %d Error writing profile port script %s: Path to script is null", __FILE__, __LINE__, scriptName.c_str());
			}
			else
			{
				out << m_ports[p->ports[i].first].behavior << endl;
			}
		}
	}

	out << endl;
	return out.str();
}


/************************************************
 * Load Honeyd XML Configuration Functions
 ************************************************/

//Calls all load functions
void NovaGUI::LoadAllTemplates()
{
	m_scripts.clear_no_resize();
	m_ports.clear_no_resize();
	m_profiles.clear_no_resize();
	m_nodes.clear_no_resize();
	m_subnets.clear_no_resize();

	LoadScriptsTemplate();
	LoadPortsTemplate();
	LoadProfilesTemplate();
	LoadNodesTemplate();
}

//Loads scripts from file
void NovaGUI::LoadScriptsTemplate()
{
	using boost::property_tree::ptree;
	using boost::property_tree::xml_parser::trim_whitespace;
	m_scriptTree.clear();
	try
	{
		read_xml(homePath+"/scripts.xml", m_scriptTree, boost::property_tree::xml_parser::trim_whitespace);

		BOOST_FOREACH(ptree::value_type &v, m_scriptTree.get_child("scripts"))
		{
			script s;
			s.tree = v.second;
			//Each script consists of a name and path to that script
			s.name = v.second.get<std::string>("name");

			if (!s.name.compare(""))
			{
				syslog(SYSL_ERR, "File: %s Line: %d Problem loading honeyd XML files", __FILE__, __LINE__);
				prompter->DisplayPrompt(HONEYD_LOAD_FAIL, "Warning: the honeyd scripts XML file contains invalid (null) script names. Some scripts have failed to load.");
				continue;
			}

			s.path = v.second.get<std::string>("path");
			m_scripts[s.name] = s;
		}
	}
	catch(std::exception &e)
	{
		syslog(SYSL_ERR, "File: %s Line: %d Problem loading scripts: %s", __FILE__, __LINE__, string(e.what()).c_str());
		prompter->DisplayPrompt(HONEYD_READ_FAIL, "Problem loading scripts:  " + string(e.what()));
	}
}

void NovaGUI::InitiateSystemStatus()
{
	// Pull in the icons now that homePath is set
	string greenPath = "/usr/share/nova/icons/greendot.png";
	string yellowPath = "/usr/share/nova/icons/yellowdot.png";
	string redPath = "/usr/share/nova/icons/reddot.png";

	m_greenIcon = new QIcon(QPixmap(QString::fromStdString(greenPath)));
	m_yellowIcon = new QIcon(QPixmap(QString::fromStdString(yellowPath)));
	m_redIcon = new QIcon(QPixmap(QString::fromStdString(redPath)));

	// Populate the System Status table with empty widgets
	for (int i = 0; i < ui.systemStatusTable->rowCount(); i++)
		for (int j = 0; j < ui.systemStatusTable->columnCount(); j++)
			ui.systemStatusTable->setItem(i, j,  new QTableWidgetItem());

	// Add labels for our components
	ui.systemStatusTable->item(COMPONENT_NOVAD,0)->setText(QString::fromStdString(novaComponents[COMPONENT_NOVAD].name));
	ui.systemStatusTable->item(COMPONENT_DMH,0)->setText(QString::fromStdString(novaComponents[COMPONENT_DMH].name));
	ui.systemStatusTable->item(COMPONENT_HSH,0)->setText(QString::fromStdString(novaComponents[COMPONENT_HSH].name));
}


void NovaGUI::UpdateSystemStatus()
{
	QTableWidgetItem *item;
	QTableWidgetItem *pidItem;

	for (uint i = 0; i < NOVA_COMPONENTS; i++)
	{
		item = ui.systemStatusTable->item(i,0);
		pidItem = ui.systemStatusTable->item(i,1);

		if (novaComponents[i].process == NULL || !novaComponents[i].process->pid())
		{
			pidItem->setText("");

			// Restart processes that died for some reason
			if (novaComponents[i].shouldBeRunning)
			{
				syslog(SYSL_ERR, "File: %s Line: %d GUI has detected a dead process %s. Restarting it.", __FILE__, __LINE__, novaComponents[i].name.c_str());
				item->setIcon(*m_yellowIcon);
				StartComponent(&novaComponents[i]);
			}
			else
			{
				item->setIcon(*m_redIcon);
			}

		}
		else
		{
			// The process is running, but it shouldn't be. Make it yellow
			if (novaComponents[i].shouldBeRunning)
				item->setIcon(*m_greenIcon);
			else
				item->setIcon(*m_yellowIcon);

			pidItem->setText(QString::number(novaComponents[i].process->pid()));
		}
	}

	// Update the buttons if need be
	int row = ui.systemStatusTable->currentRow();
	if (row < 0 || row > NOVA_COMPONENTS)
		return;
	else
		on_systemStatusTable_itemSelectionChanged();
}


//Loads ports from file
void NovaGUI::LoadPortsTemplate()
{
	using boost::property_tree::ptree;
	using boost::property_tree::xml_parser::trim_whitespace;

	m_portTree.clear();
	try
	{
		read_xml(homePath+"/templates/ports.xml", m_portTree, boost::property_tree::xml_parser::trim_whitespace);

		BOOST_FOREACH(ptree::value_type &v, m_portTree.get_child("ports"))
		{
			port p;
			p.tree = v.second;
			//Required xml entries
			p.portName = v.second.get<std::string>("name");

			if (!p.portName.compare(""))
			{
				syslog(SYSL_ERR, "File: %s Line: %d Problem loading honeyd XML files", __FILE__, __LINE__);
				prompter->DisplayPrompt(HONEYD_LOAD_FAIL, "Warning: the honeyd XML files contain invalid port names. Some ports have failed to load.");
				continue;
			}

			p.portNum = v.second.get<std::string>("number");
			p.type = v.second.get<std::string>("type");
			p.behavior = v.second.get<std::string>("behavior");

			//If this port uses a script, find and assign it.
			if(!p.behavior.compare("script") || !p.behavior.compare("internal"))
			{
				p.scriptName = v.second.get<std::string>("script");
			}
			//If the port works as a proxy, find destination
			else if(!p.behavior.compare("proxy"))
			{
				p.proxyIP = v.second.get<std::string>("IP");
				p.proxyPort = v.second.get<std::string>("Port");
			}
			m_ports[p.portName] = p;
		}
	}
	catch(std::exception &e)
	{
		syslog(SYSL_ERR, "File: %s Line: %d Problem loading ports: %s", __FILE__, __LINE__, string(e.what()).c_str());
		prompter->DisplayPrompt(HONEYD_READ_FAIL, "Problem loading ports: " + string(e.what()));
	}
}


//Loads the subnets and nodes from file for the currently specified group
void NovaGUI::LoadNodesTemplate()
{
	using boost::property_tree::ptree;
	using boost::property_tree::xml_parser::trim_whitespace;

	m_groupTree.clear();
	ptree ptr;

	try
	{
		read_xml(homePath+"/templates/nodes.xml", m_groupTree, boost::property_tree::xml_parser::trim_whitespace);
		BOOST_FOREACH(ptree::value_type &v, m_groupTree.get_child("groups"))
		{
			//Find the specified group
			if(!v.second.get<std::string>("name").compare(m_group))
			{
				try //Null Check
				{
					//Load Subnets first, they are needed before we can load nodes
					m_subnetTree = v.second.get_child("subnets");
					LoadSubnets(&m_subnetTree);

					try //Null Check
					{
						//If subnets are loaded successfully, load nodes
						m_nodesTree = v.second.get_child("nodes");
						LoadNodes(&m_nodesTree);
					}
					catch(std::exception &e)
					{
						syslog(SYSL_ERR, "File: %s Line: %d Problem loading nodes: %s", __FILE__, __LINE__, string(e.what()).c_str());
						prompter->DisplayPrompt(HONEYD_READ_FAIL, "Problem loading nodes: " + string(e.what()));
					}
				}
				catch(std::exception &e)
				{
					syslog(SYSL_ERR, "File: %s Line: %d Problem loading subnets: %s", __FILE__, __LINE__, string(e.what()).c_str());
					prompter->DisplayPrompt(HONEYD_READ_FAIL, "Problem loading subnets: " + string(e.what()));
				}
			}
		}
	}
	catch(std::exception &e)
	{
		syslog(SYSL_ERR, "File: %s Line: %d Problem loading group: %s - %s", __FILE__, __LINE__, m_group.c_str(), string(e.what()).c_str());
		prompter->DisplayPrompt(HONEYD_READ_FAIL, "Problem loading group:  string(e.what())");
	}
}

//loads subnets from file for current group
void NovaGUI::LoadSubnets(ptree *ptr)
{
	try
	{
		BOOST_FOREACH(ptree::value_type &v, ptr->get_child(""))
		{
			//If real interface
			if(!string(v.first.data()).compare("interface"))
			{
				subnet sub;
				sub.tree = v.second;
				sub.isRealDevice =  v.second.get<bool>("isReal");
				//Extract the data
				sub.name = v.second.get<std::string>("name");
				sub.address = v.second.get<std::string>("IP");
				sub.mask = v.second.get<std::string>("mask");
				sub.enabled = v.second.get<bool>("enabled");

				//Gets the IP address in uint32 form
				in_addr_t baseTemp = ntohl(inet_addr(sub.address.c_str()));

				//Converting the mask to uint32 allows a simple bitwise AND to get the lowest IP in the subnet.
				in_addr_t maskTemp = ntohl(inet_addr(sub.mask.c_str()));
				sub.base = (baseTemp & maskTemp);
				//Get the number of bits in the mask
				sub.maskBits = GetMaskBits(maskTemp);
				//Adding the binary inversion of the mask gets the highest usable IP
				sub.max = sub.base + ~maskTemp;
				stringstream ss;
				ss << sub.address << "/" << sub.maskBits;
				sub.address = ss.str();

				//Save subnet
				m_subnets[sub.name] = sub;
			}
			//If virtual honeyd subnet
			else if(!string(v.first.data()).compare("virtual"))
			{
				//TODO Implement and test
				/*subnet sub;
				sub.tree = v.second;
				sub.isRealDevice = false;
				//Extract the data
				sub.name = v.second.get<std::string>("name");
				sub.address = v.second.get<std::string>("IP");
				sub.mask = v.second.get<std::string>("mask");
				sub.enabled = v.second.get<bool>("enabled");

				//Gets the IP address in uint32 form
				in_addr_t baseTemp = ntohl(inet_addr(sub.address.c_str()));

				//Converting the mask to uint32 allows a simple bitwise AND to get the lowest IP in the subnet.
				in_addr_t maskTemp = ntohl(inet_addr(sub.mask.c_str()));
				sub.base = (baseTemp & maskTemp);
				//Get the number of bits in the mask
				sub.maskBits = GetMaskBits(maskTemp);
				//Adding the binary inversion of the mask gets the highest usable IP
				sub.max = sub.base + ~maskTemp;
				stringstream ss;
				ss << sub.address << "/" << sub.maskBits;
				sub.address = ss.str();

				//Save subnet
				subnets[sub.name] = sub;*/
			}
			else
			{
				syslog(SYSL_ERR, "File: %s Line: %d Unexpected Entry in file: %s", __FILE__, __LINE__, string(v.first.data()).c_str());
				prompter->DisplayPrompt(UNEXPECTED_ENTRY, string(v.first.data()).c_str());
			}
		}
	}
	catch(std::exception &e)
	{
		syslog(SYSL_ERR, "File: %s Line: %d Problem loading subnets: %s", __FILE__, __LINE__, string(e.what()).c_str());
		prompter->DisplayPrompt(HONEYD_LOAD_FAIL, "Problem loading subnets: " + string(e.what()));
	}
}


//loads haystack nodes from file for current group
void NovaGUI::LoadNodes(ptree *ptr)
{
	profile p;
	//ptree * ptr2;
	try
	{
		BOOST_FOREACH(ptree::value_type &v, ptr->get_child(""))
		{
			if(!string(v.first.data()).compare("node"))
			{
				node n;
				int max = 0;
				bool unique = true;
				stringstream ss;
				uint i = 0, j = 0;
				j = ~j; // 2^32-1

				n.tree = v.second;
				//Required xml entires
				n.interface = v.second.get<std::string>("interface");
				n.IP = v.second.get<std::string>("IP");
				n.enabled = v.second.get<bool>("enabled");
				n.pfile = v.second.get<std::string>("profile.name");

				if (!n.pfile.compare(""))
				{
					syslog(SYSL_ERR, "File: %s Line: %d Problem loading honeyd XML files", __FILE__, __LINE__);
					prompter->DisplayPrompt(HONEYD_LOAD_FAIL, "Warning: the honeyd nodes XML file contains invalid node names. Some nodes have failed to load.");
					continue;
				}

				p = m_profiles[n.pfile];

				//Get mac if present
				try //Conditional: has "set" values
				{
					//ptr2 = &v.second.get_child("MAC");
					//pass 'set' subset and pointer to this profile
					n.MAC = v.second.get<std::string>("MAC");
				}
				catch(...){}
				if(!n.IP.compare(configuration->getDoppelIp()))
				{
					n.name = "Doppelganger";
					n.sub = n.interface;
					n.realIP = htonl(inet_addr(n.IP.c_str())); //convert ip to uint32
					//save the node in the table
					m_nodes[n.name] = n;

					//Put address of saved node in subnet's list of nodes.
					m_subnets[m_nodes[n.name].sub].nodes.push_back(n.name);
				}
				else switch(p.type)
				{

					//***** STATIC IP ********//
					case static_IP:

						n.name = n.IP;

						if (!n.name.compare(""))
						{
							syslog(SYSL_ERR, "File: %s Line: %d Problem loading honeyd XML files", __FILE__, __LINE__);
							prompter->DisplayPrompt(HONEYD_LOAD_FAIL, "Warning: the honeyd nodes XML file contains invalid node names. Some nodes have failed to load.");
							continue;
						}

						//intialize subnet to NULL and check for smallest bounding subnet
						n.sub = ""; //TODO virtual subnets will need to be handled when implemented
						n.realIP = htonl(inet_addr(n.IP.c_str())); //convert ip to uint32
						//Tracks the mask with smallest range by comparing num of bits used.

						//Check each subnet
						for(SubnetTable::iterator it = m_subnets.begin(); it != m_subnets.end(); it++)
						{
							//If node falls outside a subnets range skip it
							if((n.realIP < it->second.base) || (n.realIP > it->second.max))
								continue;
							//If this is the smallest range
							if(it->second.maskBits > max)
							{
								//If node isn't using host's address
								if(it->second.address.compare(n.IP))
								{
									max = it->second.maskBits;
									n.sub = it->second.name;
								}
							}
						}

						//Check that node has unique IP addr
						for(NodeTable::iterator it = m_nodes.begin(); it != m_nodes.end(); it++)
						{
							if(n.realIP == it->second.realIP)
							{
								unique = false;
							}
						}

						//If we have a subnet and node is unique
						if((n.sub != "") && unique)
						{
							//save the node in the table
							m_nodes[n.name] = n;

							//Put address of saved node in subnet's list of nodes.
							m_subnets[m_nodes[n.name].sub].nodes.push_back(n.name);
						}
						//If no subnet found, can't use node unless it's doppelganger.
						else
						{
							syslog(SYSL_ERR, "File: %s Line: %d Node at IP: %s is outside all valid subnet "
									"ranges", __FILE__, __LINE__, n.IP.c_str());
							prompter->DisplayPrompt(HONEYD_INVALID_SUBNET, " Node at IP is outside all "
									"valid subnet ranges: " + n.name);
						}
						break;


					//***** STATIC DHCP (static MAC) ********//
					case staticDHCP:

						//If no MAC is set, there's a problem
						if(!n.MAC.size())
						{
							syslog(SYSL_ERR, "File: %s Line: %d DHCP Enabled node using profile %s "
									"does not have a MAC Address.",
									__FILE__, __LINE__, string(n.pfile).c_str());
							prompter->DisplayPrompt(NODE_LOAD_FAIL, "DHCP Enabled node using profile "
									"" + n.pfile + " does not have a MAC Address.");
							continue;
						}

						//Associated MAC is already in use, this is not allowed, throw out the node
						if(m_nodes.find(n.MAC) != m_nodes.end())
						{
							syslog(SYSL_ERR, "File: %s Line: %d Duplicate MAC address detected "
									"in node: %s", __FILE__, __LINE__, n.MAC.c_str());
							prompter->DisplayPrompt(NODE_LOAD_FAIL, n.MAC +" is already in use.");
							continue;
						}
						n.name = n.MAC;

						if (!n.name.compare(""))
						{
							syslog(SYSL_ERR, "File: %s Line: %d Problem loading honeyd XML files", __FILE__, __LINE__);
							prompter->DisplayPrompt(HONEYD_LOAD_FAIL, "Warning: the honeyd nodes XML file contains invalid node names. Some nodes have failed to load.");
							continue;
						}

						n.sub = n.interface; //TODO virtual subnets will need to be handled when implemented
						// If no valid subnet/interface found
						if(!n.sub.compare(""))
						{
							syslog(SYSL_ERR, "File: %s Line: %d DHCP Enabled Node with MAC: %s "
									"is unable to resolve it's interface.",__FILE__, __LINE__, n.MAC.c_str());
							prompter->DisplayPrompt(NODE_LOAD_FAIL, "DHCP Enabled Node with MAC "
									+ n.MAC + " is unable to resolve it's interface.");
							continue;
						}

						//save the node in the table
						m_nodes[n.name] = n;

						//Put address of saved node in subnet's list of nodes.
						m_subnets[m_nodes[n.name].sub].nodes.push_back(n.name);
						break;

					//***** RANDOM DHCP (random MAC each time run) ********//
					case randomDHCP:

						n.name = n.pfile + " on " + n.interface;

						if (!n.name.compare(""))
						{
							syslog(SYSL_ERR, "File: %s Line: %d Problem loading honeyd XML files", __FILE__, __LINE__);
							prompter->DisplayPrompt(HONEYD_LOAD_FAIL, "Warning: the honeyd nodes XML file contains invalid node names. Some nodes have failed to load.");
							continue;
						}

						//Finds a unique identifier
						while((m_nodes.find(n.name) != m_nodes.end()) && (i < j))
						{
							i++;
							ss.str("");
							ss << n.pfile << " on " << n.interface << "-" << i;
							n.name = ss.str();
						}
						n.sub = n.interface; //TODO virtual subnets will need to be handled when implemented
						// If no valid subnet/interface found
						if(!n.sub.compare(""))
						{
							syslog(SYSL_ERR, "File: %s Line: %d DHCP Enabled Node is unable to resolve "
									"it's interface: %s.", __FILE__, __LINE__, n.interface.c_str());
							prompter->DisplayPrompt(NODE_LOAD_FAIL, "DHCP Enabled Node with MAC "
									+ n.MAC + " is unable to resolve it's interface.");
							continue;
						}
						//save the node in the table
						m_nodes[n.name] = n;

						//Put address of saved node in subnet's list of nodes.
						m_subnets[m_nodes[n.name].sub].nodes.push_back(n.name);
						break;
				}
			}
			else
			{
				syslog(SYSL_ERR, "File: %s Line: %d Unexpected Entry in file: %s", __FILE__, __LINE__, string(v.first.data()).c_str());
				prompter->DisplayPrompt(UNEXPECTED_ENTRY, "Unexpected Entry in file: " + string(v.first.data()));
			}
		}
	}
	catch(std::exception &e)
	{
		syslog(SYSL_ERR, "File: %s Line: %d Problem loading nodes: %s", __FILE__, __LINE__, string(e.what()).c_str());
		prompter->DisplayPrompt(HONEYD_LOAD_FAIL, "Problem loading nodes: " + string(e.what()));
	}
}

void NovaGUI::LoadProfilesTemplate()
{
	using boost::property_tree::ptree;
	using boost::property_tree::xml_parser::trim_whitespace;
	ptree * ptr;
	m_profileTree.clear();
	try
	{
		read_xml(homePath+"/templates/profiles.xml", m_profileTree, boost::property_tree::xml_parser::trim_whitespace);

		BOOST_FOREACH(ptree::value_type &v, m_profileTree.get_child("profiles"))
		{
			//Generic profile, essentially a honeyd template
			if(!string(v.first.data()).compare("profile"))
			{
				profile p;
				//Root profile has no parent
				p.parentProfile = "";
				p.tree = v.second;

				//Name required, DCHP boolean intialized (set in loadProfileSet)
				p.name = v.second.get<std::string>("name");

				if (!p.name.compare(""))
				{
					syslog(SYSL_ERR, "File: %s Line: %d Problem loading honeyd XML files", __FILE__, __LINE__);
					prompter->DisplayPrompt(HONEYD_LOAD_FAIL, "Warning: the honeyd profiles XML file contains invalid profile names. Some profiles have failed to load.");
					continue;
				}

				p.ports.clear();
				p.type = (profileType)v.second.get<int>("type");
				for(uint i = 0; i < INHERITED_MAX; i++)
				{
					p.inherited[i] = false;
				}

				try //Conditional: has "set" values
				{
					ptr = &v.second.get_child("set");
					//pass 'set' subset and pointer to this profile
					LoadProfileSettings(ptr, &p);
				}
				catch(...){}

				try //Conditional: has "add" values
				{
					ptr = &v.second.get_child("add");
					//pass 'add' subset and pointer to this profile
					LoadProfileServices(ptr, &p);
				}
				catch(...){}

				//Save the profile
				m_profiles[p.name] = p;

				try //Conditional: has children profiles
				{
					//start recurisive descent down profile tree with this profile as the root
					//pass subtree and pointer to parent
					LoadProfileChildren(p.name);
				}
				catch(...){}

			}

			//Honeyd's implementation of switching templates based on conditions
			else if(!string(v.first.data()).compare("dynamic"))
			{
				//TODO
			}
			else
			{
				syslog(SYSL_ERR, "File: %s Line: %d Invalid XML Path %s", __FILE__, __LINE__, string(v.first.data()).c_str());
			}
		}
	}
	catch(std::exception &e)
	{
		syslog(SYSL_ERR, "File: %s Line: %d Problem loading Profiles: %s", __FILE__, __LINE__, string(e.what()).c_str());
		prompter->DisplayPrompt(HONEYD_LOAD_FAIL, "Problem loading Profiles: " + string(e.what()));
	}
}

//Sets the configuration of 'set' values for profile that called it
void NovaGUI::LoadProfileSettings(ptree *ptr, profile *p)
{
	string prefix;
	try
	{
		BOOST_FOREACH(ptree::value_type &v, ptr->get_child(""))
		{
			prefix = "TCP";
			if(!string(v.first.data()).compare(prefix))
			{
				p->tcpAction = v.second.data();
				p->inherited[TCP_ACTION] = false;
				continue;
			}
			prefix = "UDP";
			if(!string(v.first.data()).compare(prefix))
			{
				p->udpAction = v.second.data();
				p->inherited[UDP_ACTION] = false;
				continue;
			}
			prefix = "ICMP";
			if(!string(v.first.data()).compare(prefix))
			{
				p->icmpAction = v.second.data();
				p->inherited[ICMP_ACTION] = false;
				continue;
			}
			prefix = "personality";
			if(!string(v.first.data()).compare(prefix))
			{
				p->personality = v.second.data();
				p->inherited[PERSONALITY] = false;
				continue;
			}
			prefix = "ethernet";
			if(!string(v.first.data()).compare(prefix))
			{
				p->ethernet = v.second.data();
				p->inherited[ETHERNET] = false;
				continue;
			}
			prefix = "uptime";
			if(!string(v.first.data()).compare(prefix))
			{
				p->uptime = v.second.data();
				p->inherited[UPTIME] = false;
				continue;
			}
			prefix = "uptimeRange";
			if(!string(v.first.data()).compare(prefix))
			{
				p->uptimeRange = v.second.data();
				continue;
			}
			prefix = "dropRate";
			if(!string(v.first.data()).compare(prefix))
			{
				p->dropRate = v.second.data();
				p->inherited[DROP_RATE] = false;
				continue;
			}
		}
	}
	catch(std::exception &e)
	{
		syslog(SYSL_ERR, "File: %s Line: %d Problem loading profile set parameters: %s", __FILE__, __LINE__, string(e.what()).c_str());
		prompter->DisplayPrompt(HONEYD_LOAD_FAIL, "Problem loading profile set parameters: " + string(e.what()));
	}
}

//Adds specified ports and subsystems
// removes any previous port with same number and type to avoid conflicts
void NovaGUI::LoadProfileServices(ptree *ptr, profile *p)
{
	string prefix;
	port * prt;

	try
	{
		for(uint i = 0; i < p->ports.size(); i++)
		{
			p->ports[i].second = true;
		}
		BOOST_FOREACH(ptree::value_type &v, ptr->get_child(""))
		{
			//Checks for ports
			prefix = "ports";
			if(!string(v.first.data()).compare(prefix))
			{
				//Iterates through the ports
				BOOST_FOREACH(ptree::value_type &v2, ptr->get_child("ports"))
				{
					prt = &m_ports[v2.second.data()];

					//Checks inherited ports for conflicts
					for(uint i = 0; i < p->ports.size(); i++)
					{
						//Erase inherited port if a conflict is found
						if(!prt->portNum.compare(m_ports[p->ports[i].first].portNum) && !prt->type.compare(m_ports[p->ports[i].first].type))
						{
							p->ports.erase(p->ports.begin()+i);
						}
					}
					//Add specified port
					pair<string, bool> portPair;
					portPair.first = prt->portName;
					portPair.second = false;
					if(!p->ports.size())
						p->ports.push_back(portPair);
					else
					{
						uint i = 0;
						for(i = 0; i < p->ports.size(); i++)
						{
							port * temp = &m_ports[p->ports[i].first];
							if((atoi(temp->portNum.c_str())) < (atoi(prt->portNum.c_str())))
							{
								continue;
							}
							break;
						}
						if(i < p->ports.size())
							p->ports.insert(p->ports.begin()+i, portPair);
						else
							p->ports.push_back(portPair);
					}
				}
				continue;
			}

			//Checks for a subsystem
			prefix = "subsystem"; //TODO
			if(!string(v.first.data()).compare(prefix))
			{
				continue;
			}
		}
	}
	catch(std::exception &e)
	{
		syslog(SYSL_ERR, "File: %s Line: %d Problem loading profile add parameters: %s", __FILE__, __LINE__, string(e.what()).c_str());
		prompter->DisplayPrompt(HONEYD_LOAD_FAIL, "Problem loading profile add parameters: " + string(e.what()));
	}
}

//Recursive descent down a profile tree, inherits parent, sets values and continues if not leaf.
void NovaGUI::LoadProfileChildren(string parent)
{
	ptree ptr = m_profiles[parent].tree;
	try
	{
		BOOST_FOREACH(ptree::value_type &v, ptr.get_child("profiles"))
		{
			ptree *ptr2;

			//Inherits parent,
			profile prof = m_profiles[parent];
			prof.tree = v.second;
			prof.parentProfile = parent;

			//Gets name, initializes DHCP
			prof.name = v.second.get<std::string>("name");

			if (!prof.name.compare(""))
			{
				syslog(SYSL_ERR, "File: %s Line: %d Problem loading honeyd XML files", __FILE__, __LINE__);
				prompter->DisplayPrompt(HONEYD_LOAD_FAIL, "Warning: the honeyd profiles XML file contains invalid profile names. Some profiles have failed to load.");
				continue;
			}

			for(uint i = 0; i < INHERITED_MAX; i++)
			{
				prof.inherited[i] = true;
			}

			try //Conditional: If profile overrides type
			{
				prof.type = (profileType)v.second.get<int>("type");
				prof.inherited[TYPE] = false;
			}
			catch(...){}

			try //Conditional: If profile has set configurations different from parent
			{
				ptr2 = &v.second.get_child("set");
				LoadProfileSettings(ptr2, &prof);
			}
			catch(...){}

			try //Conditional: If profile has port or subsystems different from parent
			{
				ptr2 = &v.second.get_child("add");
				LoadProfileServices(ptr2, &prof);
			}
			catch(...){}

			//Saves the profile
			m_profiles[prof.name] = prof;


			try //Conditional: if profile has children (not leaf)
			{
				LoadProfileChildren(prof.name);
			}
			catch(...){}
		}
	}
	catch(std::exception &e)
	{
		syslog(SYSL_ERR, "File: %s Line: %d Problem loading sub profiles: %s", __FILE__, __LINE__, string(e.what()).c_str());
		prompter->DisplayPrompt(HONEYD_LOAD_FAIL, "Problem loading sub profiles:" + string(e.what()));
	}
}


/************************************************
 * Suspect Functions
 ************************************************/

bool NovaGUI::ReceiveSuspectFromNovad(int socket)
{
	struct sockaddr_un remote;
	int socketSize, connectionSocket;
	socketSize = sizeof(remote);

	//Blocking call
	if ((connectionSocket = accept(socket, (struct sockaddr *)&remote, (socklen_t*)&socketSize)) == -1)
	{
		syslog(SYSL_ERR, "File: %s Line: %d accept: %s", __FILE__, __LINE__, strerror(errno));
		CloseSocket(connectionSocket);
		return false;
	}
	if((bytesRead = recv(connectionSocket, buf, MAX_MSG_SIZE, 0 )) == 0)
	{
		return false;
	}
	else if(bytesRead == -1)
	{
		syslog(SYSL_ERR, "File: %s Line: %d recv: %s", __FILE__, __LINE__, strerror(errno));
		CloseSocket(connectionSocket);
		return false;
	}
	CloseSocket(connectionSocket);

	Suspect* suspect = new Suspect();

	try
	{
		suspect->DeserializeSuspect(buf);
		bzero(buf, bytesRead);
	}
	catch(std::exception e)
	{
		syslog(SYSL_ERR, "File: %s Line: %d Error interpreting received Suspect: %s", __FILE__, __LINE__, string(e.what()).c_str());
		delete suspect;
		return false;
	}

	struct suspectItem sItem;
	sItem.suspect = suspect;
	sItem.item = NULL;
	sItem.mainItem = NULL;
	ProcessReceivedSuspect(sItem);
	return true;
}

void NovaGUI::ProcessReceivedSuspect(suspectItem suspectItem)
{

	pthread_rwlock_wrlock(&lock);
	//If the suspect already exists in our table
	if(SuspectTable.find(suspectItem.suspect->GetIpAddress()) != SuspectTable.end())
	{
		//We point to the same item so it doesn't need to be deleted.
		suspectItem.item = SuspectTable[suspectItem.suspect->GetIpAddress()].item;
		suspectItem.mainItem = SuspectTable[suspectItem.suspect->GetIpAddress()].mainItem;

		//Delete the old Suspect since we created and pointed to a new one
		delete SuspectTable[suspectItem.suspect->GetIpAddress()].suspect;
	}
	//We borrow the flag to show there is new information.
	suspectItem.suspect->SetNeedsFeatureUpdate(true);
	//Update the entry in the table
	SuspectTable[suspectItem.suspect->GetIpAddress()] = suspectItem;
	pthread_rwlock_unlock(&lock);
	Q_EMIT newSuspect(suspectItem.suspect->GetIpAddress());
}

/************************************************
 * Display Functions
 ************************************************/

void NovaGUI::DrawAllSuspects()
{
	m_editingSuspectList = true;
	ClearSuspectList();

	QListWidgetItem * item = NULL;
	QListWidgetItem * mainItem = NULL;
	Suspect * suspect = NULL;
	QString str;
	QBrush brush;
	QColor color;

	pthread_rwlock_wrlock(&lock);
	for (SuspectGUIHashTable::iterator it = SuspectTable.begin() ; it != SuspectTable.end(); it++)
	{
		str = (QString) string(inet_ntoa(it->second.suspect->GetInAddr())).c_str();
		suspect = it->second.suspect;
		//Create the colors for the draw

		if (suspect->GetClassification() < 0)
		{
			// In training mode, classification is never set and ends up at -1
			// Make it a nice blue so it's clear that it hasn't classified
			color = QColor(0,0,255);
		}
		else if(suspect->GetClassification() < 0.5)
		{
			//at 0.5 QBrush is 255,255 (yellow), from 0->0.5 include more red until yellow
			color = QColor((int)(200*2*suspect->GetClassification()),200, 50);
		}
		else
		{
			//at 0.5 QBrush is 255,255 (yellow), at from 0.5->1.0 remove more green until QBrush is Red
			color = QColor(200,200-(int)(200*2*(suspect->GetClassification()-0.5)), 50);
		}
		brush.setColor(color);
		brush.setStyle(Qt::NoBrush);

		//Create the Suspect in list with info set alignment and color
		item = new QListWidgetItem(str,0);
		item->setTextAlignment(Qt::AlignLeft|Qt::AlignBottom);
		item->setForeground(brush);

		in_addr_t addr;
		int i = 0;
		if(ui.suspectList->count())
		{
			for(i = 0; i < ui.suspectList->count(); i++)
			{
				addr = inet_addr(ui.suspectList->item(i)->text().toStdString().c_str());
				if(SuspectTable[addr].suspect->GetClassification() < suspect->GetClassification())
					break;
			}
		}
		ui.suspectList->insertItem(i, item);

		//If Hostile
		if(suspect->GetIsHostile())
		{
			//Copy the item and add it to the list
			mainItem = new QListWidgetItem(str,0);
			mainItem->setTextAlignment(Qt::AlignLeft|Qt::AlignBottom);
			mainItem->setForeground(brush);

			i = 0;
			if(ui.hostileList->count())
			{
				for(i = 0; i < ui.hostileList->count(); i++)
				{
					addr = inet_addr(ui.hostileList->item(i)->text().toStdString().c_str());
					if(SuspectTable[addr].suspect->GetClassification() < suspect->GetClassification())
						break;
				}
			}
			ui.hostileList->insertItem(i, mainItem);
			it->second.mainItem = mainItem;
		}
		//Point to the new items
		it->second.item = item;
		//Reset the flags
		suspect->SetNeedsFeatureUpdate(false);
		it->second.suspect = suspect;
	}
	UpdateSuspectWidgets();
	pthread_rwlock_unlock(&lock);
	m_editingSuspectList = false;
}

//Updates the UI with the latest suspect information
//*NOTE This slot is not thread safe, make sure you set appropriate locks before sending a signal to this slot
void NovaGUI::DrawSuspect(in_addr_t suspectAddr)
{
	m_editingSuspectList = true;
	QString str;
	QBrush brush;
	QColor color;
	in_addr_t addr;

	pthread_rwlock_wrlock(&lock);
	suspectItem * sItem = &SuspectTable[suspectAddr];
	//Extract Information
	str = (QString) string(inet_ntoa(sItem->suspect->GetInAddr())).c_str();

	//Create the colors for the draw
	if (sItem->suspect->GetClassification() < 0)
	{
		// In training mode, classification is never set and ends up at -1
		// Make it a nice blue so it's clear that it hasn't classified
		color = QColor(0,0,255);
	}
	else if(sItem->suspect->GetClassification() < 0.5)
	{
		//at 0.5 QBrush is 255,255 (yellow), from 0->0.5 include more red until yellow
		color = QColor((int)(200*2*sItem->suspect->GetClassification()),200, 50);
	}
	else
	{
		//at 0.5 QBrush is 255,255 (yellow), at from 0.5->1.0 remove more green until QBrush is Red
		color = QColor(200,200-(int)(200*2*(sItem->suspect->GetClassification()-0.5)), 50);
	}
	brush.setColor(color);
	brush.setStyle(Qt::NoBrush);

	//If the item exists
	if(sItem->item != NULL)
	{
		sItem->item->setText(str);
		sItem->item->setForeground(brush);
		bool selected = false;
		int current_row = ui.suspectList->currentRow();

		//If this is our current selection flag it so we can update the selection if we change the index
		if(current_row == ui.suspectList->row(sItem->item))
			selected = true;

		ui.suspectList->removeItemWidget(sItem->item);

		int i = 0;
		if(ui.suspectList->count())
		{
			for(i = 0; i < ui.suspectList->count(); i++)
			{
				addr = inet_addr(ui.suspectList->item(i)->text().toStdString().c_str());
				if(SuspectTable[addr].suspect->GetClassification() < sItem->suspect->GetClassification())
					break;
			}
		}
		ui.suspectList->insertItem(i, sItem->item);

		//If we need to update the selection
		if(selected)
		{
			ui.suspectList->setCurrentRow(i);
		}
	}
	//If the item doesn't exist
	else
	{
		//Create the Suspect in list with info set alignment and color
		sItem->item = new QListWidgetItem(str,0);
		sItem->item->setTextAlignment(Qt::AlignLeft|Qt::AlignBottom);
		sItem->item->setForeground(brush);

		int i = 0;
		if(ui.suspectList->count())
		{
			for(i = 0; i < ui.suspectList->count(); i++)
			{
				addr = inet_addr(ui.suspectList->item(i)->text().toStdString().c_str());
				if(SuspectTable[addr].suspect->GetClassification() < sItem->suspect->GetClassification())
					break;
			}
		}
		ui.suspectList->insertItem(i, sItem->item);
	}

	//If the mainItem exists and suspect is hostile
	if((sItem->mainItem != NULL) && sItem->suspect->GetIsHostile())
	{
		sItem->mainItem->setText(str);
		sItem->mainItem->setForeground(brush);
		bool selected = false;
		int current_row = ui.hostileList->currentRow();

		//If this is our current selection flag it so we can update the selection if we change the index
		if(current_row == ui.hostileList->row(sItem->mainItem))
			selected = true;

		ui.hostileList->removeItemWidget(sItem->mainItem);
		int i = 0;
		if(ui.hostileList->count())
		{
			for(i = 0; i < ui.hostileList->count(); i++)
			{
				addr = inet_addr(ui.hostileList->item(i)->text().toStdString().c_str());
				if(SuspectTable[addr].suspect->GetClassification() < sItem->suspect->GetClassification())
					break;
			}
		}
		ui.hostileList->insertItem(i, sItem->mainItem);

		//If we need to update the selection
		if(selected)
		{
			ui.hostileList->setCurrentRow(i);
		}
		sItem->mainItem->setToolTip(QString(sItem->suspect->ToString(m_featureEnabled).c_str()));
	}
	//Else if the mainItem exists and suspect is not hostile
	else if(sItem->mainItem != NULL)
	{
		ui.hostileList->removeItemWidget(sItem->mainItem);
	}
	//If the mainItem doesn't exist and suspect is hostile
	else if(sItem->suspect->GetIsHostile())
	{
		//Create the Suspect in list with info set alignment and color
		sItem->mainItem = new QListWidgetItem(str,0);
		sItem->mainItem->setTextAlignment(Qt::AlignLeft|Qt::AlignBottom);
		sItem->mainItem->setForeground(brush);

		sItem->mainItem->setToolTip(QString(sItem->suspect->ToString(m_featureEnabled).c_str()));

		int i = 0;
		if(ui.hostileList->count())
		{
			for(i = 0; i < ui.hostileList->count(); i++)
			{
				addr = inet_addr(ui.hostileList->item(i)->text().toStdString().c_str());
				if(SuspectTable[addr].suspect->GetClassification() < sItem->suspect->GetClassification())
					break;
			}
		}
		ui.hostileList->insertItem(i, sItem->mainItem);
	}
	sItem->item->setToolTip(QString(sItem->suspect->ToString(m_featureEnabled).c_str()));
	UpdateSuspectWidgets();
	pthread_rwlock_unlock(&lock);
	m_editingSuspectList = false;
}

void NovaGUI::UpdateSuspectWidgets()
{
	double hostileAcc = 0, benignAcc = 0, totalAcc = 0;

	for (SuspectGUIHashTable::iterator it = SuspectTable.begin() ; it != SuspectTable.end(); it++)
	{
		if(it->second.suspect->GetIsHostile())
		{
			hostileAcc += it->second.suspect->GetClassification();
			totalAcc += it->second.suspect->GetClassification();
		}
		else
		{
			benignAcc += 1-it->second.suspect->GetClassification();
			totalAcc += 1-it->second.suspect->GetClassification();
		}
	}

	int numBenign = ui.suspectList->count() - ui.hostileList->count();
	stringstream ss;
	ss << numBenign;
	ui.numBenignEdit->setText(QString(ss.str().c_str()));

	if(numBenign)
	{
		benignAcc /= numBenign;
		ui.benignClassificationBar->setValue((int)(benignAcc*100));
		ui.benignSuspectClassificationBar->setValue((int)(benignAcc*100));
	}
	else
	{
		ui.benignClassificationBar->setValue(100);
		ui.benignSuspectClassificationBar->setValue(100);
	}
	if(ui.hostileList->count())
	{
		hostileAcc /= ui.hostileList->count();
		ui.hostileClassificationBar->setValue((int)(hostileAcc*100));
		ui.hostileSuspectClassificationBar->setValue((int)(hostileAcc*100));
	}
	else
	{
		ui.hostileClassificationBar->setValue(100);
		ui.hostileSuspectClassificationBar->setValue(100);
	}
	if(ui.suspectList->count())
	{
		totalAcc /= ui.suspectList->count();
		ui.overallSuspectClassificationBar->setValue((int)(totalAcc*100));
	}
	else
	{
		ui.overallSuspectClassificationBar->setValue(100);
	}
}
void NovaGUI::SaveAllSuspects()
{
	 QString filename = QFileDialog::getSaveFileName(this,
			tr("Save Suspect List"), QDir::currentPath(),
			tr("Documents (*.txt)"));

	if (filename.isNull())
	{
		return;
	}


	message.SetMessage(WRITE_SUSPECTS, filename.toStdString());
	msgLen = message.SerialzeMessage(msgBuffer);

	//Sends the message to all Nova processes
	SendToNovad(msgBuffer, msgLen);
}

//Clears the suspect tables completely.
void NovaGUI::ClearSuspectList()
{
	pthread_rwlock_wrlock(&lock);
	this->ui.suspectList->clear();
	this->ui.hostileList->clear();
	//Since clearing permenantly deletes the items we need to make sure the suspects point to null
	for (SuspectGUIHashTable::iterator it = SuspectTable.begin() ; it != SuspectTable.end(); it++)
	{
		it->second.item = NULL;
		it->second.mainItem = NULL;
	}
	pthread_rwlock_unlock(&lock);
}

/************************************************
 * Menu Signal Handlers
 ************************************************/

void NovaGUI::on_actionRunNova_triggered()
{
	StartNova();
}

void NovaGUI::on_actionRunNovaAs_triggered()
{
	Run_Popup *w = new Run_Popup(this, homePath);
	w->show();
}

void NovaGUI::on_actionStopNova_triggered()
{
	StopNova();

	// Were we in training mode?
	if (configuration->getIsTraining())
	{
		prompter->DisplayPrompt(LAUNCH_TRAINING_MERGE,
			"ClassificationEngine was in training mode. Would you like to merge the capture file into the training database now?",
			ui.actionTrainingData, NULL);
	}
}

void NovaGUI::on_actionConfigure_triggered()
{
	NovaConfig *w = new NovaConfig(this, homePath);
	w->setWindowModality(Qt::WindowModal);
	w->show();
}

void  NovaGUI::on_actionExit_triggered()
{
	StopNova();
	exit(EXIT_SUCCESS);
}

void NovaGUI::on_actionClear_All_Suspects_triggered()
{
	m_editingSuspectList = true;
	ClearAllSuspects();
	QFile::remove(QString::fromStdString(configuration->getPathCESaveFile()));
	DrawAllSuspects();
	m_editingSuspectList = false;
}

void NovaGUI::on_actionClear_Suspect_triggered()
{
	QListWidget * list;
	if(ui.suspectList->hasFocus())
	{
		list = ui.suspectList;
	}
	else if(ui.hostileList->hasFocus())
	{
		list = ui.hostileList;
	}
	if(list->currentItem() != NULL && list->isItemSelected(list->currentItem()))
	{
		string suspectStr = list->currentItem()->text().toStdString();
		in_addr_t addr = inet_addr(suspectStr.c_str());
		HideSuspect(addr);
		ClearSuspect(suspectStr);
	}
}

void NovaGUI::on_actionHide_Suspect_triggered()
{
	QListWidget * list;
	if(ui.suspectList->hasFocus())
	{
		list = ui.suspectList;
	}
	else if(ui.hostileList->hasFocus())
	{
		list = ui.hostileList;
	}
	if(list->currentItem() != NULL && list->isItemSelected(list->currentItem()))
	{
		in_addr_t addr = inet_addr(list->currentItem()->text().toStdString().c_str());
		HideSuspect(addr);
	}
}

void NovaGUI::on_actionSave_Suspects_triggered()
{
	SaveAllSuspects();
}

void NovaGUI::on_actionMakeDataFile_triggered()
{
	 QString data = QFileDialog::getOpenFileName(this,
			 tr("File to select classifications from"), QString::fromStdString(configuration->getPathTrainingFile()), tr("NOVA Classification Database (*.db)"));

	if (data.isNull())
		return;

	trainingSuspectMap* map = ParseTrainingDb(data.toStdString());
	if (map == NULL)
	{
		prompter->DisplayPrompt(CONFIG_READ_FAIL, "Error parsing file " + data.toStdString());
		return;
	}


	classifierPrompt* classifier = new classifierPrompt(map);

	if (classifier->exec() == QDialog::Rejected)
		return;

	string dataFileContent = MakaDataFile(*map);

	ofstream out (configuration->getPathTrainingFile().data());
	out << dataFileContent;
	out.close();
}

void NovaGUI::on_actionTrainingData_triggered()
{
	 QString data = QFileDialog::getOpenFileName(this,
			 tr("Classification Engine Data Dump"), QString::fromStdString(configuration->getPathTrainingFile()), tr("NOVA Classification Dump (*.dump)"));

	if (data.isNull())
		return;

	trainingDumpMap* trainingDump = ParseEngineCaptureFile(data.toStdString());

	if (trainingDump == NULL)
	{
		prompter->DisplayPrompt(CONFIG_READ_FAIL, "Error parsing file " + data.toStdString());
		return;
	}

	ThinTrainingPoints(trainingDump, configuration->getThinningDistance());

	classifierPrompt* classifier = new classifierPrompt(trainingDump);

	if (classifier->exec() == QDialog::Rejected)
		return;

	trainingSuspectMap* headerMap = classifier->getStateData();

	QString outputFile = QFileDialog::getSaveFileName(this,
			tr("Classification Database File"), QString::fromStdString(configuration->getPathTrainingFile()), tr("NOVA Classification Database (*.db)"));

	if (outputFile.isNull())
		return;

	if (!CaptureToTrainingDb(outputFile.toStdString(), headerMap))
	{
		prompter->DisplayPrompt(CONFIG_READ_FAIL, "Error parsing the input files. Please see the logs for more details.");
	}
}

void  NovaGUI::on_actionHide_Old_Suspects_triggered()
{
	m_editingSuspectList = true;
	ClearSuspectList();
	m_editingSuspectList = false;
}

void  NovaGUI::on_actionShow_All_Suspects_triggered()
{
	m_editingSuspectList = true;
	DrawAllSuspects();
	m_editingSuspectList = false;
}

void NovaGUI::on_actionHelp_2_triggered()
{
	if(!m_isHelpUp)
	{
		Nova_Manual *wi = new Nova_Manual(this);
		wi->show();
		m_isHelpUp = true;
	}
}

void NovaGUI::on_actionLogger_triggered()
{
		LoggerWindow *wi = new LoggerWindow(this);
		wi->show();
}

/************************************************
 * View Signal Handlers
 ************************************************/

void NovaGUI::on_mainButton_clicked()
{
	this->ui.stackedWidget->setCurrentIndex(0);
	this->ui.mainButton->setFlat(true);
	this->ui.suspectButton->setFlat(false);
	this->ui.doppelButton->setFlat(false);
	this->ui.haystackButton->setFlat(false);
}

void NovaGUI::on_suspectButton_clicked()
{
	this->ui.stackedWidget->setCurrentIndex(1);
	this->ui.mainButton->setFlat(false);
	this->ui.suspectButton->setFlat(true);
	this->ui.doppelButton->setFlat(false);
	this->ui.haystackButton->setFlat(false);
}

void NovaGUI::on_doppelButton_clicked()
{
	this->ui.stackedWidget->setCurrentIndex(2);
	this->ui.mainButton->setFlat(false);
	this->ui.suspectButton->setFlat(false);
	this->ui.doppelButton->setFlat(true);
	this->ui.haystackButton->setFlat(false);
}

void NovaGUI::on_haystackButton_clicked()
{
	this->ui.stackedWidget->setCurrentIndex(3);
	this->ui.mainButton->setFlat(false);
	this->ui.suspectButton->setFlat(false);
	this->ui.doppelButton->setFlat(false);
	this->ui.haystackButton->setFlat(true);
}

/************************************************
 * Button Signal Handlers
 ************************************************/

void NovaGUI::on_runButton_clicked()
{
	// TODO: Put this back? It was really annoying if you had an existing
	// haystack.config you wanted to use, kept rewriting it on start.
	// Commented for now until the Node setup works in the GUI.
	//writeHoneyd();
	StartNova();
}
void NovaGUI::on_stopButton_clicked()
{
	Q_EMIT on_actionStopNova_triggered();
}

void NovaGUI::on_systemStatusTable_itemSelectionChanged()
{
	int row = ui.systemStatusTable->currentRow();

	if (row < 0 || row > NOVA_COMPONENTS)
	{
		syslog(SYSL_ERR, "File: %s Line: %d Invalid System Status Selection Row, ignoring", __FILE__, __LINE__);
		return;
	}

	if (novaComponents[row].process != NULL && novaComponents[row].process->pid())
	{
		ui.systemStatStartButton->setDisabled(true);
		ui.systemStatKillButton->setDisabled(false);

		// We can't send a stop signal to honeyd, force using the kill button
		if (row == COMPONENT_DMH || row == COMPONENT_HSH)
			ui.systemStatStopButton->setDisabled(true);
		else
			ui.systemStatStopButton->setDisabled(false);

	}
	else
	{
		ui.systemStatStartButton->setDisabled(false);
		ui.systemStatStopButton->setDisabled(true);
		ui.systemStatKillButton->setDisabled(true);
	}
}

void NovaGUI::on_actionSystemStatKill_triggered()
{
	int row = ui.systemStatusTable->currentRow();

	if (row < 0 || row > NOVA_COMPONENTS)
	{
		syslog(SYSL_ERR, "File: %s Line: %d Invalid System Status Selection Row, ignoring", __FILE__, __LINE__);
		return;
	}

	QProcess *process = novaComponents[row].process;
	novaComponents[row].shouldBeRunning = false;

	// Fix for honeyd not closing with gnome-terminal + sudo
	if (configuration->getUseTerminals() && process != NULL && process->pid() &&
			(ui.systemStatusTable->currentRow() == COMPONENT_DMH || ui.systemStatusTable->currentRow() == COMPONENT_HSH))
	{
		QString killString = QString("sudo pkill -TERM -P ") + QString::number(process->pid());
		system(killString.toStdString().c_str());

		killString = QString("sudo kill ") + QString::number(process->pid());
		system(killString.toStdString().c_str());
	}

	// Politely ask the process to die
	if(process != NULL && process->pid())
		process->terminate();

	// Tell the process to die in a stern voice
	if(process != NULL && process->pid())
		process->kill();

	// Give up telling it to die and kill it ourselves with the power of root
	if(process != NULL && process->pid())
	{
		QString killString = QString("sudo kill ") + QString::number(process->pid());
		system(killString.toStdString().c_str());
	}
}


void NovaGUI::on_actionSystemStatStop_triggered()
{
	int row = ui.systemStatusTable->currentRow();
	novaComponents[row].shouldBeRunning = false;

	//Sets the message
	message.SetMessage(EXIT);
	msgLen = message.SerialzeMessage(msgBuffer);

	switch (row)
	{
		case COMPONENT_NOVAD:
			SendToNovad(msgBuffer, msgLen);
			break;
		case COMPONENT_DMH:
			if (novaComponents[COMPONENT_DMH].process != NULL && novaComponents[COMPONENT_DMH].process->pid() != 0)
			{
				QString killString = QString("sudo pkill -TERM -P ") + QString::number(novaComponents[COMPONENT_DMH].process->pid());
				system(killString.toStdString().c_str());

				killString = QString("sudo kill ") + QString::number(novaComponents[COMPONENT_DMH].process->pid());
				system(killString.toStdString().c_str());
			}
			break;
		case COMPONENT_HSH:
			if (novaComponents[COMPONENT_HSH].process != NULL && novaComponents[COMPONENT_HSH].process->pid() != 0)
			{
				QString killString = QString("sudo pkill -TERM -P ") + QString::number(novaComponents[COMPONENT_HSH].process->pid());
				system(killString.toStdString().c_str());

				killString = QString("sudo kill ") + QString::number(novaComponents[COMPONENT_HSH].process->pid());
				system(killString.toStdString().c_str());
			}
			break;
	}
}


void NovaGUI::on_actionSystemStatStart_triggered()
{
	int row = ui.systemStatusTable->currentRow();

	switch (row) {
	case COMPONENT_NOVAD:
		StartComponent(&novaComponents[COMPONENT_NOVAD]);
		break;
	case COMPONENT_HSH:
		StartComponent(&novaComponents[COMPONENT_HSH]);
		break;
	case COMPONENT_DMH:
		StartComponent(&novaComponents[COMPONENT_DMH]);
		break;
	default:
		return;
	}

	UpdateSystemStatus();
}


void NovaGUI::on_actionSystemStatReload_triggered()
{
	//Sets the message
	message.SetMessage(RELOAD);
	msgLen = message.SerialzeMessage(msgBuffer);
	SendToNovad(msgBuffer, msgLen);
}

void NovaGUI::on_systemStatStartButton_clicked()
{
	on_actionSystemStatStart_triggered();
}

void NovaGUI::on_systemStatKillButton_clicked()
{
	on_actionSystemStatKill_triggered();
}

void NovaGUI::on_systemStatStopButton_clicked()
{
	on_actionSystemStatStop_triggered();
}

void NovaGUI::on_clearSuspectsButton_clicked()
{
	m_editingSuspectList = true;
	ClearAllSuspects();
	QFile::remove(QString::fromStdString(configuration->getPathCESaveFile()));
	DrawAllSuspects();
	m_editingSuspectList = false;
}


/************************************************
 * List Signal Handlers
 ************************************************/
void NovaGUI::on_suspectList_itemSelectionChanged()
{
	if(!m_editingSuspectList)
	{
		pthread_rwlock_wrlock(&lock);
		if(ui.suspectList->currentItem() != NULL)
		{
			in_addr_t addr = inet_addr(ui.suspectList->currentItem()->text().toStdString().c_str());
			ui.suspectFeaturesEdit->setText(QString(SuspectTable[addr].suspect->ToString(m_featureEnabled).c_str()));
			SetFeatureDistances(SuspectTable[addr].suspect);
		}
		pthread_rwlock_unlock(&lock);
	}
}

void NovaGUI::SetFeatureDistances(Suspect* suspect)
{
	int row = 0;
	ui.suspectDistances->clear();
	for (int i = 0; i < DIM; i++)
	{
		if (m_featureEnabled[i])
		{
			QString featureLabel;

			switch (i)
			{
			case IP_TRAFFIC_DISTRIBUTION:
				featureLabel = tr("IP Traffic Distribution");
				break;
			case PORT_TRAFFIC_DISTRIBUTION:
				featureLabel = tr("Port Traffic Distribution");
				break;
			case HAYSTACK_EVENT_FREQUENCY:
				featureLabel = tr("Haystack Event Frequency");
				break;
			case PACKET_SIZE_MEAN:
				featureLabel = tr("Packet Size Mean");
				break;
			case PACKET_SIZE_DEVIATION:
				featureLabel = tr("Packet Size Deviation");
				break;
			case DISTINCT_IPS:
				featureLabel = tr("IPs Contacted");
				break;
			case DISTINCT_PORTS:
				featureLabel = tr("Ports Contacted");
				break;
			case PACKET_INTERVAL_MEAN:
				featureLabel = tr("Packet Interval Mean");
				break;
			case PACKET_INTERVAL_DEVIATION:
				featureLabel = tr("Packet Interval Deviation");
				break;
			}

			ui.suspectDistances->insertItem(row, featureLabel + tr(" Accuracy"));
			QString formatString = "%p% | ";
			formatString.append(featureLabel);
			formatString.append(": ");

			row++;
			QProgressBar* bar = new QProgressBar();
			bar->setMinimum(0);
			bar->setMaximum(100);
			bar->setTextVisible(true);

			if (suspect->m_featureAccuracy[i] < 0)
			{
				syslog(SYSL_ERR, "File: %s Line: %d GUI got invalid feature accuracy (should be between 0 and 1), but is  %E", __FILE__, __LINE__, suspect->m_featureAccuracy[i]);
				suspect->m_featureAccuracy[i] = 0;
			}
			else if (suspect->m_featureAccuracy[i] > 1)
			{
				syslog(SYSL_ERR, "File: %s Line: %d GUI got invalid feature accuracy (should be between 0 and 1), but is  %E", __FILE__, __LINE__, suspect->m_featureAccuracy[i]);
				suspect->m_featureAccuracy[i] = 1;
			}

			bar->setValue((int)((1 - suspect->m_featureAccuracy[i]/1.0)*100));
			bar->setStyleSheet(
				"QProgressBar:horizontal {border: 1px solid gray;background: white;padding: 1px;} \
				QProgressBar::chunk:horizontal {margin: 0.5px; background: qlineargradient(x1: 0, y1: 0.5, x2: 1, y2: 0.5, stop: 0 yellow, stop: 1 green);}");

			formatString.append(QString::number(suspect->m_features.m_features[i]));
			bar->setFormat(formatString);

			QListWidgetItem* item = new QListWidgetItem();
			ui.suspectDistances->insertItem(row, item);
			ui.suspectDistances->setItemWidget(item, bar);

			row++;
		}
	}
}

/************************************************
 * IPC Functions
 ************************************************/
void NovaGUI::HideSuspect(in_addr_t addr)
{
	pthread_rwlock_wrlock(&lock);
	m_editingSuspectList = true;
	suspectItem * sItem = &SuspectTable[addr];
	if(!sItem->item->isSelected())
	{
		pthread_rwlock_unlock(&lock);
		m_editingSuspectList = false;
		return;
	}
	ui.suspectList->removeItemWidget(sItem->item);
	delete sItem->item;
	sItem->item = NULL;
	if(sItem->mainItem != NULL)
	{
		ui.hostileList->removeItemWidget(sItem->mainItem);
		delete sItem->mainItem;
		sItem->mainItem = NULL;
	}
	pthread_rwlock_unlock(&lock);
	m_editingSuspectList = false;
}

/*********************************************************
 ----------------- General Functions ---------------------
 *********************************************************/

namespace Nova
{

/************************************************
 * Thread Loops
 ************************************************/

void *StatusUpdate(void *ptr)
{
	while(true)
	{
		((NovaGUI*)ptr)->SystemStatusRefresh();

		sleep(2);
	}
	return NULL;
}

void *NovadListenLoop(void *ptr)
{
	while(true)
	{
		((NovaGUI*)ptr)->ReceiveSuspectFromNovad(NovadInSocket);
	}
	return NULL;
}

//Removes all information on a suspect
void ClearSuspect(string suspectStr)
{
	pthread_rwlock_wrlock(&lock);
	SuspectTable.erase(inet_addr(suspectStr.c_str()));
	message.SetMessage(CLEAR_SUSPECT, suspectStr);
	msgLen = message.SerialzeMessage(msgBuffer);
	SendToNovad(msgBuffer, msgLen);
	pthread_rwlock_unlock(&lock);
}

//Deletes all Suspect information for the GUI and Nova
void ClearAllSuspects()
{
	pthread_rwlock_wrlock(&lock);
	SuspectTable.clear();
	message.SetMessage(CLEAR_ALL);
	msgLen = message.SerialzeMessage(msgBuffer);
	SendToNovad(msgBuffer, msgLen);
	pthread_rwlock_unlock(&lock);
}

void StopNova()
{
	for (uint i = 0; i < NOVA_COMPONENTS; i++)
	{
		novaComponents[i].shouldBeRunning = false;
	}
	//Sets the message
	message.SetMessage(EXIT);
	msgLen = message.SerialzeMessage(msgBuffer);

	//Sends the message to all Nova processes
	SendToNovad(msgBuffer, msgLen);

	// Close Honeyd processes
	FILE * out = popen("pidof honeyd","r");
	if(out != NULL)
	{
		char buffer[1024];
		char * line = fgets(buffer, sizeof(buffer), out);

		if (line != NULL)
		{
			string cmd = "sudo kill " + string(line);
			if(cmd.size() > 5)
				system(cmd.c_str());
		}
	}
	pclose(out);
}


void StartNova()
{
	// Start and processes that aren't running already
	for (uint i = 0; i < NOVA_COMPONENTS; i++)
	{
		if(novaComponents[i].process == NULL || !novaComponents[i].process->pid())
			StartComponent(&novaComponents[i]);
	}

}

void StartComponent(novaComponent *component)
{
	QString program;
	NOVAConfiguration config;

	if (config.getUseTerminals())
		program = QString::fromStdString(component->terminalCommand);
	else
		program = QString::fromStdString(component->noTerminalCommand);

	syslog(SYSL_INFO, "Running start command: %s", program.toStdString().c_str());

	// Is the process already running?
	if (component->process != NULL)
	{
		component->process->kill();
		delete component->process;
	}

	component->shouldBeRunning = true;

	component->process = new QProcess();
	component->process->start(program);
}


/************************************************
 * Socket Functions
 ************************************************/

void InitSocketAddresses()
{

	//CE IN --------------------------------------------------
	//Builds the key path
	string key = NOVAD_IN_FILENAME;
	key = homePath + key;
	//Builds the address
	NovadInAddress.sun_family = AF_UNIX;
	strcpy(NovadInAddress.sun_path, key.c_str());
	unlink(NovadInAddress.sun_path);

	//CE OUT -------------------------------------------------
	//Builds the key path
	key = NOVAD_OUT_FILENAME;
	key = homePath + key;
	//Builds the address
	NovadOutAddress.sun_family = AF_UNIX;
	strcpy(NovadOutAddress.sun_path, key.c_str());
}

void SendToNovad(u_char * data, int size)
{
	//Opens the socket
	if ((NovadOutSocket = socket(AF_UNIX, SOCK_STREAM, 0)) == -1)
	{
		syslog(SYSL_ERR, "File: %s Line: %d socket: %s", __FILE__, __LINE__, strerror(errno));
		close(NovadOutSocket);
		exit(EXIT_FAILURE);
	}
	//Sends the message
	len = strlen(NovadOutAddress.sun_path) + sizeof(NovadOutAddress.sun_family);
	if (connect(NovadOutSocket, (struct sockaddr *)&NovadOutAddress, len) == -1)
	{
		syslog(SYSL_ERR, "File: %s Line: %d connect: %s", __FILE__, __LINE__, strerror(errno));
		close(NovadOutSocket);
		return;
	}

	else if (send(NovadOutSocket, data, size, 0) == -1)
	{
		syslog(SYSL_ERR, "File: %s Line: %d send: %s", __FILE__, __LINE__, strerror(errno));
		close(NovadOutSocket);
		return;
	}
	close(NovadOutSocket);
}

void CloseSocket(int sock)
{
	close(sock);
}

}
