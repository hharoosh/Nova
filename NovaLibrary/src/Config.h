//============================================================================
// Name        : NOVAConfiguration.h
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
// Description : Class to load and parse the NOVA configuration file
//============================================================================/*

#ifndef NOVACONFIGURATION_H_
#define NOVACONFIGURATION_H_

#include "HashMapStructs.h"
#include "Defines.h"

namespace Nova {

class Config {

public:
	// This is a singleton class, use this to access it
	static Config* Inst();


	~Config();

	// Loads and parses a NOVA configuration file
	//      module - added s.t. rsyslog  will output NovaConfig messages as the parent process that called LoadConfig
	void LoadConfig();
	bool SaveConfig();

	bool LoadUserConfig();
	// TODO: SaveUserConfig();
	// We don't have any GUI stuff to edit this.. but we should

	// Loads the PATH file (usually in /etc)
	bool LoadPaths();

	// Loads default values for all variables
	void SetDefaults();

	// Checks to see if the current user has a ~/.nova directory, and creates it if not, along with default config files
	//	Returns: True if (after the function) the user has all necessary ~/.nova config files
	//		IE: Returns false only if the user doesn't have configs AND we weren't able to make them
    static bool InitUserConfigs(std::string homeNovaPath);

    std::string toString();

    // Getters
    std::string getConfigFilePath() ;
    std::string getDoppelInterface() ;
    std::string getDoppelIp() ;
    std::string getEnabledFeatures() ;
    bool isFeatureEnabled(int i) ;
    uint getEnabledFeatureCount() ;
    std::string getInterface() ;
    std::string getPathCESaveFile() ;
    std::string getPathConfigHoneydDm() ;
    std::string getPathConfigHoneydHs() ;
    std::string getPathPcapFile() ;
    std::string getPathTrainingCapFolder() ;
    std::string getPathTrainingFile() ;
    std::string getKey() ;
    std::vector<in_addr_t> getNeighbors() ;

    bool getReadPcap() ;
    bool getUseTerminals() ;
    bool getIsDmEnabled() ;
    bool getIsTraining() ;
    bool getGotoLive() ;

    int getClassificationTimeout() ;
    int getDataTTL() ;
    int getK() ;
    int getSaMaxAttempts() ;
    int getSaPort() ;
    int getSaveFreq() ;
    int getTcpCheckFreq() ;
    int getTcpTimout() ;
    int getThinningDistance() ;

    double getClassificationThreshold() ;
    double getSaSleepDuration() ;
    double getEps() ;

    std::string getGroup() ;

    // Setters
    void setClassificationThreshold(double classificationThreshold);
    void setClassificationTimeout(int classificationTimeout);
    void setConfigFilePath(std::string configFilePath);
    void setDataTTL(int dataTTL);
    void setDoppelInterface(std::string doppelInterface);
    void setDoppelIp(std::string doppelIp);
    void setEnabledFeatures(std::string enabledFeatureMask);
    void setEps(double eps);
    void setGotoLive(bool gotoLive);
    void setInterface(std::string interface);
    void setIsDmEnabled(bool isDmEnabled);
    void setIsTraining(bool isTraining);
    void setK(int k);
    void setPathCESaveFile(std::string pathCESaveFile);
    void setPathConfigHoneydDm(std::string pathConfigHoneydDm);
    void setPathConfigHoneydHs(std::string pathConfigHoneydHs);
    void setPathPcapFile(std::string pathPcapFile);
    void setPathTrainingCapFolder(std::string pathTrainingCapFolder);
    void setPathTrainingFile(std::string pathTrainingFile);
    void setReadPcap(bool readPcap);
    void setSaMaxAttempts(int saMaxAttempts);
    void setSaPort(int saPort);
    void setSaSleepDuration(double saSleepDuration);
    void setSaveFreq(int saveFreq);
    void setTcpCheckFreq(int tcpCheckFreq);
    void setTcpTimout(int tcpTimout);
    void setThinningDistance(int thinningDistance);
    void setUseTerminals(bool useTerminals);
    void setKey(std::string key);
    void setNeigbors(std::vector<in_addr_t> neighbors);
    void setGroup(std::string group);
    std::string getLoggerPreferences() ;
    std::string getSMTPAddr() ;
    std::string getSMTPDomain() ;
    std::vector<std::string> getSMTPEmailRecipients() ;
    in_port_t getSMTPPort() ;
    void setLoggerPreferences(std::string loggerPreferences);
    void setSMTPAddr(std::string SMTPAddr);
    void setSMTPDomain(std::string SMTPDomain);
	void setSMTPPort(in_port_t SMTPPort);

	double getSqurtEnabledFeatures();

    // Set with a vector of email addresses
    void setSMTPEmailRecipients(std::vector<std::string> SMTPEmailRecipients);
    // Set with a CSV std::string from the config file
    void setSMTPEmailRecipients(std::string SMTPEmailRecipients);

    // Getters for the paths stored in /etc
    std::string getPathBinaries();
    std::string getPathWriteFolder();
    std::string getPathReadFolder();
    std::string getPathHome();
    std::string getPathIcon();

protected:
	Config();

private:
	static Config *m_instance;

	__attribute__ ((visibility ("hidden"))) static std::string m_prefixes[];
	__attribute__ ((visibility ("hidden"))) static std::string m_requiredFiles[];

	std::string m_interface;
	std::string m_doppelIp;
	std::string m_doppelInterface;

	// Enabled feature stuff, we provide a few formats and helpers
	std::string m_enabledFeatureMask;
	bool m_isFeatureEnabled[DIM];
	uint m_enabledFeatureCount;
	double m_squrtEnabledFeatures;


	std::string m_pathConfigHoneydHs;
	std::string m_pathConfigHoneydDm;
	std::string m_pathPcapFile;
	std::string m_pathTrainingFile;
	std::string m_pathTrainingCapFolder;
	std::string m_pathCESaveFile;

	std::string m_group;

	int m_tcpTimout;
	int m_tcpCheckFreq;
	int m_classificationTimeout;
	int m_saPort;
	int m_k;
	int m_thinningDistance;
	int m_saveFreq;
	int m_dataTTL;
	int m_saMaxAttempts;

	double m_saSleepDuration;
	double m_eps;
	double m_classificationThreshold;

	bool m_readPcap;
	bool m_gotoLive;
	bool m_isTraining;
	bool m_isDmEnabled;

	// the SMTP server domain name for display purposes
	std::string m_SMTPDomain;
	// the email address that will be set as sender
	std::string m_SMTPAddr;
	// the port for SMTP send; normally 25 if I'm not mistaken, may take this out
	in_port_t m_SMTPPort;

	std::string m_loggerPreferences;
	// a vector containing the email recipients; may move this into the actual classes
	// as opposed to being in this struct
	std::vector<std::string> m_SMTPEmailRecipients;

	// User config options
	std::vector<in_addr_t> m_neighbors;
	std::string m_key;

	std::string m_configFilePath;
	std::string m_userConfigFilePath;


	// Options from the PATHS file (currently /etc/nova/paths)
	std::string m_pathBinaries;
	std::string m_pathWriteFolder;
	std::string m_pathReadFolder;
	std::string m_pathHome;
	std::string m_pathIcon;

	pthread_rwlock_t m_lock;

	// Used for loading the nova path file, resolves paths with env vars to full paths
	static std::string ResolvePathVars(std::string path);

	// Non-locking versions of some functions for internal use
	void setEnabledFeatures_noLocking(std::string enabledFeatureMask);

    // Set with a CSV std::string from the config file
    void setSMTPEmailRecipients_noLocking(std::string SMTPEmailRecipients);

};
}

#endif /* NOVACONFIGURATION_H_ */
