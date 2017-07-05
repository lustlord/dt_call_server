/*
 * server_init.c
 *
 *  Created on: May 2, 2017
 *      Author: Daniel
 *
 *  Various intialization functions used by the server.
 *  Moved here for readability.
 */
#include "server_init.hpp"

void readServerConfig(int *cmdPort, int *mediaPort, std::string *publicKeyFile, std::string *privateKeyFile, std::string *ciphers, std::string *dhfile, UserUtils *userUtils)
{
	std::ifstream conffile(CONFFILE);
	std::string line;
	bool gotPublicKey = false, gotPrivateKey = false, gotCiphers = false, gotCmdPort = false, gotMediaPort = false, gotDhFile = false;

	while(getline(conffile, line))
	{
		//skip blank lines and comment lines
		if(line.length() == 0 || line.at(0) == '#')
		{
			continue;
		}

		//read the variable and its value
		std::string var, value;
		std::stringstream ss(line);
		std::getline(ss, var, '=');
		std::getline(ss, value, '=');

		//cleanup the surrounding whitespace and strip the end of line comment
		var = Utils::trim(var);
		value = Utils::trim(value);

		//if there is no value then go on to the next line
		if(value == "")
		{
			continue;
		}

		if(var == "command")
		{
			*cmdPort = atoi(value.c_str());
			gotCmdPort = true;
			continue;
		}
		else if (var == "media")
		{
			*mediaPort = atoi(value.c_str());
			gotMediaPort = true;
			continue;
		}
		else if (var == "public")
		{
			*publicKeyFile = value;
			gotPublicKey = true;
			continue;
		}
		else if (var == "private")
		{
			*privateKeyFile = value;
			gotPrivateKey = true;
			continue;
		}
		else if (var == "ciphers")
		{
			*ciphers = value;
			gotCiphers = true;
			continue;
		}
		else if (var == "dhfile")
		{
			*dhfile = value;
			gotDhFile = true;
			continue;
		}
		else
		{
			std::string unknown = "unknown variable parsed: " + line;
			userUtils->insertLog(Log(TAG_INIT, unknown, SELF, SYSTEMLOG, SELFIP));
		}
	}

	//at the minimum a public and private key must be specified. everything else has a default value
	if (!gotPublicKey || !gotPrivateKey || !gotDhFile)
	{
		std::string conffile = CONFFILE;
		if(!gotPublicKey)
		{
			std::string error = "Your did not specify a PUBLIC key pem in: " + conffile + "\n";
			userUtils->insertLog(Log(TAG_INIT, error, SELF, ERRORLOG, SELFIP));
		}
		if(!gotPublicKey)
		{
			std::string error = "Your did not specify a PRIVATE key pem in: " + conffile + "\n";
			userUtils->insertLog(Log(TAG_INIT, error, SELF, ERRORLOG, SELFIP));
		}
		if(!gotDhFile)
		{
			std::string error = "Your did not specify a DH file for DHE ciphers in: " + conffile + "\n";
			userUtils->insertLog(Log(TAG_INIT, error, SELF, ERRORLOG, SELFIP));
		}
		exit(1);
	}

	//warn of default values if they're being used
	if(!gotCmdPort)
	{
		std::string message =  "Using default command port of: " + std::to_string(*cmdPort);
		userUtils->insertLog(Log(TAG_INIT, message, SELF, SYSTEMLOG, SELFIP));
	}
	if(!gotMediaPort)
	{
		std::string message= "Using default media port of: " + std::to_string(*mediaPort);
		userUtils->insertLog(Log(TAG_INIT, message, SELF, SYSTEMLOG, SELFIP));
	}
	if(!gotCiphers)
	{
		std::string message = "Using default ciphers (no ECDHE): " + *ciphers;
		userUtils->insertLog(Log(TAG_INIT, message, SELF, SYSTEMLOG, SELFIP));
	}

}

SSL_CTX* setupOpenSSL(std::string ciphers, std::string privateKeyFile, std::string publicKeyFile, std::string dhfile, UserUtils *userUtils)
{
	//openssl setup
	SSL_load_error_strings();
	SSL_library_init();
	OpenSSL_add_all_algorithms();

	SSL_CTX *result = SSL_CTX_new(TLSv1_2_method());

	//set ssl properties
	if(result <= 0)
	{
		std::string error = "ssl initialization problem";
		userUtils->insertLog(Log(TAG_INIT, error, SELF, ERRORLOG, SELFIP));
		perror(error.c_str());
		exit(1);
	}

	//ciphers
	SSL_CTX_set_cipher_list(result, ciphers.c_str());

	//private key
	if(SSL_CTX_use_PrivateKey_file(result, privateKeyFile.c_str(), SSL_FILETYPE_PEM) <= 0)
	{
		std::string error = "problems with the private key";
		userUtils->insertLog(Log(TAG_INIT, error, SELF, ERRORLOG, SELFIP));
		perror(error.c_str());
		exit(1);
	}

	//public key
	if(SSL_CTX_use_certificate_file(result, publicKeyFile.c_str(), SSL_FILETYPE_PEM) <= 0)
	{
		std::string error = "problems with the public key";
		userUtils->insertLog(Log(TAG_INIT, error, SELF, ERRORLOG, SELFIP));
		perror(error.c_str());
		exit(1);
	}

	//dh params to make dhe ciphers work
	//https://www.openssl.org/docs/man1.0.1/ssl/SSL_CTX_set_tmp_dh.html
	DH *dh = NULL;
	FILE *paramfile;
	paramfile = fopen(dhfile.c_str(), "r");
	if(!paramfile)
	{
		std::string error = "problems opening dh param file at: " +  dhfile;
		userUtils->insertLog(Log(TAG_INIT, error, SELF, ERRORLOG, SELFIP));
		perror(error.c_str());
		exit(1);
	}
	dh = PEM_read_DHparams(paramfile, NULL, NULL, NULL);
	fclose(paramfile);
	if(dh == NULL)
	{
		std::string error = "dh param file opened but openssl could not use dh param file at: " + dhfile;
		userUtils->insertLog(Log(TAG_INIT, error, SELF, ERRORLOG, SELFIP));
		perror(error.c_str());
		exit(1);
	}
	if(SSL_CTX_set_tmp_dh(result, dh) != 1)
	{
		std::string error = "dh param file opened and interpreted but reject by context: " + dhfile;
		userUtils->insertLog(Log(TAG_INIT, error, SELF, ERRORLOG, SELFIP));
		perror(error.c_str());
		exit(1);
	}
	//for ecdhe see SSL_CTX_set_tmp_ecdh
	return result;
}

void setupListeningSocket(int type, struct timeval *timeout, int *fd, struct sockaddr_in *info, int port, UserUtils *userUtils)
{
	//setup command port to accept new connections
	*fd = socket(AF_INET, type, 0); //tcp socket
	if(*fd < 0)
	{
		std::string error = "cannot establish socket";
		userUtils->insertLog(Log(TAG_INIT, error, SELF, ERRORLOG, SELFIP));
		perror(error.c_str());
		exit(1);
	}
	memset((char *) info, 0, sizeof(struct sockaddr_in));
	info->sin_family = AF_INET; //ipv4
	info->sin_addr.s_addr = INADDR_ANY; //listen on any nic
	info->sin_port = htons(port);
	if(bind(*fd, (struct sockaddr *)info, sizeof(struct sockaddr_in)) < 0)
	{
		std::string error = "cannot bind socket to a nic";
		userUtils->insertLog(Log(TAG_INIT, error, SELF, ERRORLOG, SELFIP));
		perror(error.c_str());
		exit(1);
	}

	if(type == SOCK_STREAM)
	{
		if(setsockopt(*fd, SOL_SOCKET, SO_RCVTIMEO, (char*)timeout, sizeof(struct timeval)) < 0)
		{
			std::string error="cannot set tcp socket options";
			userUtils->insertLog(Log(TAG_INIT, error, SELF, ERRORLOG, SELFIP));
			perror(error.c_str());
			exit(1);
		}
		listen(*fd, MAXLISTENWAIT);
	}
}
