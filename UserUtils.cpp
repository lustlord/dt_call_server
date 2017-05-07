/*
 * UserUtils.cpp
 *
 *  Created on: May 1, 2017
 *      Author: Daniel
 */

#include "const.h"
#include "UserUtils.hpp"

using namespace std;

//static user utils instance
UserUtils* UserUtils::instance;

UserUtils* UserUtils::getInstance()
{
	if(instance == NULL)
	{
		instance = new UserUtils();
	}
	return instance;
}

UserUtils::UserUtils()
{
	//generate all user objects and have them accessible by name
	ifstream usersfile(USERSFILE);
	string line;

	while(getline(usersfile, line))
	{
		//skip blank lines and comment lines
		if(line.length() == 0 || line.at(0) == '#')
		{
			continue;
		}

		//read the name and password
		string name, publicKey;
		stringstream ss(line);
		getline(ss, name, ' ');
		getline(ss, publicKey, ' ');

		//cleanup the surrounding whitespace and strip the end of line comment
		name = Utils::trim(name);
		publicKey = Utils::trim(publicKey);

		//need both a name and a public key to continue
		if(name == "" || publicKey == "")
		{
			cout << "Account '" << name << "' is misconfigured\n";
			continue;
		}

		//open the public key file
		FILE *publicKeyFile = fopen(publicKey.c_str(), "r");
		if(publicKeyFile == NULL)
		{
			cout << "Having problems opening " << name << "'s public key file\n";
			continue;
		}

		//turn the file into a useable public key
		RSA *rsaPublic = PEM_read_RSA_PUBKEY(publicKeyFile, NULL, NULL, NULL);
		if(rsaPublic == NULL)
		{
			cout << "Could not generate a public key from file for: " << name << "\n";
			continue;
		}

		User *user = new User(name, rsaPublic);

		//in case the same person has ???2 entries??? get rid of the old one
		if(nameMap.count(name) > 0)
		{
			delete nameMap[name];
			nameMap.erase(name);
			cout << "Duplicate account entry for: " << name << "\n";
		}
		nameMap[name] = user;
	}
	usersfile.close();

	//setup the log output
	logTimeT = time(NULL);
	string nowString = string(ctime(&logTimeT));
	string logName = string(LOGPREFIX) + nowString.substr(0, nowString.length()-1);
	logfile.open(LOGFOLDER+logName);
}

UserUtils::~UserUtils()
{
	//only thing that matters is to remove all user objects in the heap
	//	no need to undo all maps, they will be killed automatically
	for(auto it = nameMap.begin(); it != nameMap.end(); ++it)
	{
		delete nameMap[it->first];
		nameMap[it->first] = NULL;
	}
}

RSA* UserUtils::getUserPublicKey(string username)
{
	if(nameMap.count(username) > 0)
	{
		return nameMap[username]->getPublicKey();
	}
	return NULL;
}

string UserUtils::getUserChallenge(string username)
{
	if(nameMap.count(username) > 0)
	{
		return nameMap[username]->getChallenge();
	}
	return "";
}

void UserUtils::setUserChallenge(string username, string challenge)
{
	if(nameMap.count(username) > 0)
	{
		nameMap[username]->setChallenge(challenge);
	}
}

void UserUtils::setUserSession(string username, uint64_t sessionid)
{
	if(nameMap.count(username) > 0)
	{
		User *user = nameMap[username];
		user->setSessionkey(sessionid);
		sessionkeyMap[sessionid] = user;
	}
}

void UserUtils::setFd(uint64_t sessionid, int fd, int which)
{

	User *user = sessionkeyMap[sessionid];

	if(which == COMMAND)
	{
		user->setCommandfd(fd);
		commandfdMap.erase(fd);
		commandfdMap[fd] = user;
	}
	else if (which == MEDIA)
	{
		user->setMediafd(fd);
		mediafdMap.erase(fd);
		mediafdMap[fd] = user;
	}
}

void UserUtils::clearSession(string username)
{
	if(nameMap.count(username) > 0)
	{
		User *user = nameMap[username];

		//remove session key
		sessionkeyMap.erase(user->getSessionkey());
		user->setSessionkey(0);

		//remove command and media fds
		commandfdMap.erase(user->getCommandfd());
		user->setCommandfd(0);
		mediafdMap.erase(user->getMediafd());
		user->setMediafd(0);

		//remove challenge
		user->setChallenge("");
	}
}

bool UserUtils::verifySessionid(uint64_t sessionid, int fd)
{
	if(sessionkeyMap.count(sessionid) == 0)
	{
		return false;
	}

	User *user = sessionkeyMap[sessionid];
	return user->getCommandfd() == fd;
}

string UserUtils::userFromFd(int fd, int which)
{
	if(which == COMMAND)
	{
		if(commandfdMap.count(fd) > 0)
		{
			return commandfdMap[fd]->getUname();
		}
	}
	else if (which == MEDIA)
	{
		if(mediafdMap.count(fd) > 0)
		{
			return mediafdMap[fd]->getUname();
		}
	}
	return "";
}

string UserUtils::userFromSessionid(uint64_t sessionid)
{
	if(sessionkeyMap.count(sessionid) > 0)
	{
		return sessionkeyMap[sessionid]->getUname();
	}

	return "";
}

int UserUtils::userFd(string user, int which)
{
	if(nameMap.count(user) > 0)
	{
		User *userObj = nameMap[user];
		if(which == COMMAND)
		{
			return userObj->getCommandfd();
		}
		else if (which == MEDIA)
		{
			return userObj->getMediafd();
		}
	}
	return 0;
}

bool UserUtils::doesUserExist(string name)
{
	return nameMap.count(name) > 0;
}

uint64_t UserUtils::userSessionId(string uname)
{
	if(nameMap.count(uname) > 0)
	{
		return nameMap[uname]->getSessionkey();
	}
	return 0;
}

void UserUtils::killInstance()
{
	delete instance;
}

void UserUtils::insertLog(Log dbl)
{
	//figure out if the current log is over 1 day old
	time_t now = time(NULL);
	if((now - logTimeT) > 60*60*24)
	{//if the log is too old, close it and start another one
		logfile.close();
		logTimeT = now;
		string nowString = string(ctime(&logTimeT));
		string logName = string(LOGPREFIX) + nowString.substr(0, nowString.length()-1);
		logfile.open(LOGFOLDER+logName);
	}
	logfile << dbl.toString() << "\n";
	logfile.flush(); // write immediately to the file
	cout << dbl.toString() << "\n";
}
