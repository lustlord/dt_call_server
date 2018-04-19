/*
 * server.cpp
 *
 *  Created on: December 8, 2015 (according to testing public.pem)
 *      Author: Daniel
 */

#include "server.hpp"

//associates socket descriptors to their ssl structs
std::unordered_map<int, SSL*>clientssl;

UserUtils *userUtils = UserUtils::getInstance();
Logger *logger = Logger::getInstance();

int main(int argc, char *argv[])
{

	std::string start = "starting call operator V" + VERSION();
	logger->insertLog(Log(Log::TAG::STARTUP, start, Log::SELF(), Log::TYPE::SYSTEM, Log::SELFIP()));

	//initialize library
	if(sodium_init() < 0)
	{
		logger->insertLog(Log(Log::TAG::STARTUP, "couldn't initialize sodium library", Log::SELF(), Log::TYPE::SYSTEM, Log::SELFIP()));
		exit(1);
	}

	int cmdPort = DEFAULTCMD; //command port stuff
	int mediaPort = DEFAULTMEDIA;

	std::string publicKeyFile = "";
	std::string privateKeyFile = "";
	std::string ciphers = DEFAULTCIPHERS();
	std::string dhfile = "";
	std::string sodiumPublic = "";
	std::string sodiumPrivate = "";

	//use a helper function to read the config file
	readServerConfig(cmdPort, mediaPort, publicKeyFile, privateKeyFile, ciphers, dhfile, sodiumPublic, sodiumPrivate, logger);

	//helper to setup the ssl context
	SSL_CTX *sslcontext = setupOpenSSL(ciphers, privateKeyFile, publicKeyFile, dhfile);
	if(sslcontext == NULL)
	{
		logger->insertLog(Log(Log::TAG::STARTUP, "could not establish ssl context", Log::SELF(), Log::TYPE::SYSTEM, Log::SELFIP()));
		exit(1);
	}

	//socket read timeout option
	struct timeval unauthTimeout; //for new sockets
	unauthTimeout.tv_sec = 0;
	unauthTimeout.tv_usec = UNAUTHTIMEOUT;

	//helper to setup the command socket
	int cmdFD;
	struct sockaddr_in serv_cmd;
	setupListeningSocket(SOCK_STREAM, &unauthTimeout, &cmdFD, &serv_cmd, cmdPort);

	//sigpipe is thrown for closing the broken connection. it's gonna happen for a voip server handling mobile clients
	//what're you gonna do about it... IGNORE IT!!
	sigset_t set;
	sigemptyset(&set);
	sigaddset(&set, SIGPIPE);
	pthread_sigmask(SIG_BLOCK, &set, NULL);

	//setup sodium keys
	unsigned char sodiumPublicKey[crypto_box_PUBLICKEYBYTES] = {0};
	Utils::destringify(sodiumPublic, sodiumPublicKey);
	unsigned char sodiumPrivateKey[crypto_box_SECRETKEYBYTES] = {0};
	Utils::destringify(sodiumPrivate, sodiumPrivateKey);

	//package the stuff to start the udp thread and start it
	struct UdpArgs *args = (struct UdpArgs*)malloc(sizeof(struct UdpArgs));
	memset(args, 0, sizeof(struct UdpArgs));
	args->port = mediaPort;
	memcpy(args->sodiumPrivateKey, sodiumPrivateKey, crypto_box_SECRETKEYBYTES);
	memcpy(args->sodiumPublicKey, sodiumPublicKey, crypto_box_PUBLICKEYBYTES);
	pthread_t callThread;
	if(pthread_create(&callThread, NULL, udpThread, args) != 0)
	{
		std::string error = "cannot create the udp thread (" + std::to_string(errno) + ") " + std::string(strerror(errno));
		logger->insertLog(Log(Log::TAG::STARTUP, error, Log::SELF(), Log::TYPE::ERROR, Log::SELFIP()));
		exit(1); //with no udp thread the server cannot handle any calls
	}

	while(true) //forever
	{
#ifdef VERBOSE
		std::cout << "------------------------------------------\n----------------------------------------\n";
#endif
		fd_set readfds;
		FD_ZERO(&readfds);
		FD_SET(cmdFD, &readfds);
		int maxsd = cmdFD;

		//build the fd watch list of command fds
		for(auto sslMapping : clientssl)
		{
			int sd = sslMapping.first;
			FD_SET(sd, &readfds);
			maxsd = (sd > maxsd) ? sd : maxsd;
		}

		//wait for somebody to send something to the server
		int sockets = select(maxsd+1, &readfds, NULL, NULL, NULL);
		if(sockets < 0)
		{
			std::string error = "read fds select system call error (" + std::to_string(errno) + ") " + std::string(strerror(errno));
			logger->insertLog(Log(Log::TAG::STARTUP, error, Log::SELF(), Log::TYPE::ERROR, Log::SELFIP()));
			exit(1); //see call thread fx for why
		}
#ifdef VERBOSE
		std::cout << "select has " << sockets << " sockets ready for reading\n";
#endif

		//check for a new incoming connection on command port
		if(FD_ISSET(cmdFD, &readfds))
		{
			sslAccept(cmdFD, sslcontext, &unauthTimeout);
		}

		std::vector<int> removals;

		//check for new commands
		for(auto sslMapping : clientssl)
		{

			//get the socket descriptor and associated ssl struct from the iterator round
			int sd = sslMapping.first;
			SSL *sdssl = sslMapping.second;
			if(FD_ISSET(sd, &readfds))
			{
#ifdef VERBOSE
				std::cout << "socket descriptor: " << sd << " was marked as set\n";
#endif

				//read the socket and make sure it wasn't just a socket death notice
				unsigned char inputBuffer[COMMANDSIZE + 1];
				int amountRead = readSSL(sdssl, inputBuffer);
				if(amountRead == 0)
				{
					removals.push_back(sd);

					//check if this person was in a call. send call drop to the other person
					std::string user = userUtils->userFromCommandFd(sd);
					std::string other = userUtils->getCallWith(user);
					if(other != "")
					{
						sendCallEnd(other);
					}
					continue;
				}

				//check if the bytes sent are valid ascii like c#
				if (!legitimateAscii(inputBuffer, amountRead))
				{
					std::string unexpected = "unexpected byte in string";
					std::string user = userUtils->userFromCommandFd(sd);
					std::string ip = ipFromFd(sd);
					logger->insertLog(Log(Log::TAG::BADCMD, unexpected, user, Log::TYPE::ERROR, ip));
					continue;
				}

				//what was previously a workaround now has an official purpose: heartbeat/ping ignore byte
				//this byte is just sent to keep the socket and its various nat tables it takes to get here alive
				std::string bufferString((char*)inputBuffer);
				if(bufferString == JBYTE())
				{
#ifdef VERBOSE
					std::cout << "Got a heartbeat byte on " << sd << "\n";
#endif
					continue;
				}
				std::string originalBufferCmd = std::string((char*)inputBuffer); //save original command string before it gets mutilated by strtok
				std::vector<std::string> commandContents = parse(inputBuffer);
				std::string ip = ipFromFd(sd);
				std::string user=userUtils->userFromCommandFd(sd);
				std::string error = " (" + originalBufferCmd + ")";
				time_t now = time(NULL);

				bool timestampOK = checkTimestamp(commandContents.at(0), Log::TAG::BADCMD, error, user, ip);
				if (!timestampOK)
				{
					continue;
				}
				std::string command = commandContents.at(1);

				if (command == "login1") //you can do string comparison like this in c++
				{ //timestamp|login1|username
					std::string username = commandContents.at(2);
					logger->insertLog(Log(Log::TAG::LOGIN, originalBufferCmd, username, Log::TYPE::INBOUND, ip));

					//don't immediately remove old command fd. this would allow anyone
					//	to send a login1 command and kick out a legitimately logged in person.

					//get the user's public key
					unsigned char userSodiumPublic[crypto_box_PUBLICKEYBYTES] = {0};
					bool exists = userUtils->getSodiumPublicKey(username, userSodiumPublic);
					if (!exists)
					{
						//not a real user. send login rejection
						std::string invalid = std::to_string(now) + "|invalid";
						logger->insertLog(Log(Log::TAG::LOGIN, invalid, username, Log::TYPE::OUTBOUND, ip));
						write2Client(invalid, sdssl);
						removals.push_back(sd); //nothing useful can come from this socket
						continue;
					}

					//generate the challenge gibberish
					std::string challenge = Utils::randomString(CHALLENGE_LENGTH);
					userUtils->setChallenge(username, challenge);
#ifdef VERBOSE
					std::cout << "challenge: " << challenge << "\n";
#endif
					int encLength = 0;
					std::unique_ptr<unsigned char> enc;
					sodiumAsymEncrypt((unsigned char*) (challenge.c_str()), challenge.length(), sodiumPrivateKey, userSodiumPublic, enc, encLength);
					if (encLength < 1)
					{
						logger->insertLog(Log(Log::TAG::LOGIN, "sodium encryption of the challenge failed", username, Log::TYPE::ERROR, ip));
						continue;
					}
					std::string encString = Utils::stringify(enc.get(), encLength);

					//send the challenge
					std::string resp = std::to_string(now) + "|login1resp|" + encString;
					write2Client(resp, sdssl);
					logger->insertLog(Log(Log::TAG::LOGIN, resp, username, Log::TYPE::OUTBOUND, ip));
					continue; //login command, no session key to verify, continue to the next fd after proccessing login1
				}
				else if (command == "login2")
				{ //timestamp|login2|username|challenge

					//ok to store challenge answer in the log. challenge is single use, disposable
					std::string username = commandContents.at(2);
					logger->insertLog(Log(Log::TAG::LOGIN, originalBufferCmd, username, Log::TYPE::INBOUND, ip));
					std::string triedChallenge = commandContents.at(3);

					//check the challenge
					//	an obvious loophole: send "" as the challenge since that's the default value
					//	DON'T accept the default ""
					std::string answer = userUtils->getChallenge(username);
#ifdef VERBOSE
					std::cout << "@username: " << username << " answer: " << answer << " attempt: " << triedChallenge << "\n";
#endif
					if (answer == "" || triedChallenge != answer) //no challenge registered for this person or wrong answer
					{
						//person doesn't have a challenge to answer or isn't supposed to be
						std::string invalid = std::to_string(now) + "|invalid";
						logger->insertLog(Log(Log::TAG::LOGIN, invalid, username, Log::TYPE::OUTBOUND, ip));
						write2Client(invalid, sdssl);
						removals.push_back(sd); //nothing useful can come from this socket

						//reset challenge in case it was wrong
						userUtils->setChallenge(username, "");
						continue;
					}

					//for authenticated connections, allow more timeout in case of bad internet
					struct timeval authTimeout;
					authTimeout.tv_sec = AUTHTIMEOUT;
					authTimeout.tv_usec = 0;
					if (setsockopt(sd, SOL_SOCKET, SO_RCVTIMEO, (char*) &authTimeout, sizeof(authTimeout)) < 0)
					{
						std::string error = "cannot set timeout for authenticated command socket (" + std::to_string(errno) + ") " + std::string(strerror(errno));
						logger->insertLog(Log(Log::TAG::LOGIN, error, Log::SELF(), Log::TYPE::ERROR, ip));
					}

					//now that the person has successfully logged in, remove the old information.
					//	this person has established new connections so it's 100% sure the old ones aren't
					//	needed anymore.
					int oldcmd = userUtils->getCommandFd(username);
					if (oldcmd > 0)
					{						//remove old SSL structs to prevent memory leak
#ifdef VERBOSE
					std::cout << "previous command socket/SSL* exists, will remove\n";
#endif
						removals.push_back(oldcmd);
					}

					//for cases where you were in a call but your connection died. call record will still be there
					//	since you didn't formally send a call end
					std::string other = userUtils->getCallWith(username);
					if (other != "")
					{
						sendCallEnd(other);
					}

					//dissociate old fd from user otherwise the person will have 2 commandfds listed in
					//	comamandfdMap. remove client will see the old fd pointing to the user and will clear
					//	the user's session key and fds. don't want them cleared as they're now the new ones.
					//	immediately clean up this person's records before all the new stuff goes in
					userUtils->clearSession(username);

					//challenge was correct and wasn't "", set the info
					std::string sessionkey = Utils::randomString(SESSION_KEY_LENGTH);
					userUtils->setSessionKey(username, sessionkey);
					userUtils->setCommandFd(sessionkey, sd);
					userUtils->setChallenge(username, ""); //reset after successful completion

					//send an ok
					std::string resp = std::to_string(now) + "|login2resp|" + sessionkey;
					write2Client(resp, sdssl);
#ifndef VERBOSE
					resp = std::to_string(now) + "|login2resp|" + SESSION_KEY_PLACEHOLDER();
#endif
					logger->insertLog(Log(Log::TAG::LOGIN, resp, username, Log::TYPE::OUTBOUND, ip));
					continue; //login command, no session key to verify, continue to the next fd after proccessing login2
				}

				//done processing login commands.
				//all (non login) commands have the format timestamp|COMMAND|(stuff)...sessionkey
				std::string sessionkey = commandContents.at(commandContents.size() - 1);

#ifndef VERBOSE //unless needed, don't log session keys as they're still in use
				originalBufferCmd.replace(originalBufferCmd.find(sessionkey), SESSION_KEY_LENGTH, SESSION_KEY_PLACEHOLDER());
#endif
				if (!userUtils->verifySessionKey(sessionkey, sd))
				{
					std::string error = "INVALID SESSION ID. refusing command (" + originalBufferCmd + ")";
					logger->insertLog(Log(Log::TAG::BADCMD, error, user, Log::TYPE::ERROR, ip));

					std::string invalid = std::to_string(now) + "|invalid";
					write2Client(invalid, sdssl);
					logger->insertLog(Log(Log::TAG::BADCMD, invalid, user, Log::TYPE::OUTBOUND, ip));
					continue;
				}

				//variables written from touma calling zapper perspective
				//command will come from touma's cmd fd
				if (command == "call")
				{					//timestamp|call|zapper|toumakey

					std::string zapper = commandContents.at(2);
					std::string touma = user;
					logger->insertLog(Log(Log::TAG::CALL, originalBufferCmd, touma, Log::TYPE::INBOUND, ip));
					int zapperCmdFd = userUtils->getCommandFd(zapper);

					//find out if zapper has a command fd (signed in)
					bool offline = (zapperCmdFd == 0);
					//make sure zapper isn't already in a call or waiting for one to connect
					bool busy = (userUtils->getCallWith(zapper) != "");
					//make sure touma didn't accidentally dial himself
					bool selfDial = (touma == zapper);

					if (offline || busy || selfDial)
					{
						std::string na = std::to_string(now) + "|end|" + zapper;
						write2Client(na, sdssl);
						logger->insertLog(Log(Log::TAG::CALL, na, touma, Log::TYPE::OUTBOUND, ip));
						continue; //nothing more to do
					}

					//setup the user statuses and register the call with user utils
					userUtils->setUserState(zapper, INIT);
					userUtils->setUserState(touma, INIT);
					userUtils->setCallPair(touma, zapper);

					//tell touma that zapper is being rung
					std::string notifyTouma = std::to_string(now) + "|available|" + zapper;
					write2Client(notifyTouma, sdssl);
					logger->insertLog(Log(Log::TAG::CALL, notifyTouma, touma, Log::TYPE::OUTBOUND, ip));

					//tell zapper touma wants to call her
					std::string notifyZapper = std::to_string(now) + "|incoming|" + touma;
					SSL *zapperssl = clientssl[zapperCmdFd];
					write2Client(notifyZapper, zapperssl);
					std::string zapperip = ipFromFd(zapperCmdFd);
					logger->insertLog(Log(Log::TAG::CALL, notifyZapper, zapper, Log::TYPE::OUTBOUND, zapperip));
				}
				//variables written when zapper accepets touma's call
				//command will come from zapper's cmd fd
				else if (command == "accept")
				{					//timestamp|accept|touma|zapperkey
					std::string zapper = user;
					std::string touma = commandContents.at(2);
					logger->insertLog(Log(Log::TAG::ACCEPT, originalBufferCmd, zapper, Log::TYPE::INBOUND, ip));

					if (!isRealCall(zapper, touma, Log::TAG::ACCEPT))
					{
						continue;
					}

					//arbitrarily chosen that the one who makes the call (touma) gets to generate the aes key
					int toumaCmdFd = userUtils->getCommandFd(touma);
					SSL *toumaCmdSsl = clientssl[toumaCmdFd];
					std::string toumaResp = std::to_string(now) + "|prepare|" + userUtils->getSodiumKeyDump(zapper) + "|" + zapper;
					write2Client(toumaResp, toumaCmdSsl);
					logger->insertLog(Log(Log::TAG::ACCEPT, toumaResp, touma, Log::TYPE::OUTBOUND, ipFromFd(toumaCmdFd)));

					//send zapper touma's public key to be able to verify that the aes256 passthrough is actually from him
					std::string zapperResp = std::to_string(now) + "|prepare|" + userUtils->getSodiumKeyDump(touma) + "|" + touma;
					write2Client(zapperResp, sdssl);
					logger->insertLog(Log(Log::TAG::ACCEPT, zapperResp, zapper, Log::TYPE::OUTBOUND, ip));
				}
				else if (command == "passthrough")
				{					//timestamp|passthrough|zapper|encrypted aes key|toumakey
					std::string zapper = commandContents.at(2);
					std::string touma = user;
					std::string aes = commandContents.at(3);
					originalBufferCmd.replace(originalBufferCmd.find(aes), aes.length(), AES_PLACEHOLDER());
					logger->insertLog(Log(Log::TAG::PASSTHROUGH, originalBufferCmd, user, Log::TYPE::INBOUND, ip));

					if (!isRealCall(touma, zapper, Log::TAG::PASSTHROUGH))
					{
						continue;
					}

					int zapperfd = userUtils->getCommandFd(zapper);
					SSL *zapperssl = clientssl[zapperfd];
					std::string direct = std::to_string(now) + "|direct|" + aes + "|" + touma;					//as in "directly" from touma, not from the server
					write2Client(direct, zapperssl);
					direct.replace(direct.find(aes), aes.length(), AES_PLACEHOLDER());
					logger->insertLog(Log(Log::TAG::PASSTHROUGH, direct, zapper, Log::TYPE::OUTBOUND, ipFromFd(zapperfd)));

				}
				else if (command == "ready")
				{					//timestamp|ready|touma|zapperkey
					std::string zapper = user;
					std::string touma = commandContents.at(2);
					logger->insertLog(Log(Log::TAG::READY, originalBufferCmd, user, Log::TYPE::INBOUND, ip));
					if (!isRealCall(zapper, touma, Log::TAG::READY))
					{
						continue;
					}

					userUtils->setUserState(zapper, INCALL);
					if (userUtils->getUserState(touma) == INCALL)
					{					//only if both people are ready can  you start the call

						//tell touma zapper accepted his call request
						//	AND confirm to touma, it's zapper he's being connected with
						int toumaCmdFd = userUtils->getCommandFd(touma);
						SSL *toumaCmdSsl = clientssl[toumaCmdFd];
						std::string toumaResp = std::to_string(now) + "|start|" + zapper;
						write2Client(toumaResp, toumaCmdSsl);
						logger->insertLog(Log(Log::TAG::ACCEPT, toumaResp, touma, Log::TYPE::OUTBOUND, ipFromFd(toumaCmdFd)));

						//confirm to zapper she's being connected to touma
						std::string zapperResp = std::to_string(now) + "|start|" + touma;
						write2Client(zapperResp, sdssl);
						logger->insertLog(Log(Log::TAG::ACCEPT, zapperResp, zapper, Log::TYPE::OUTBOUND, ip));
					}
				}
				//whether it's a call end or call timeout or call reject, the result is the same
				else if (command == "end")
				{ //timestamp|end|zapper|toumakey
					std::string zapper = commandContents.at(2);
					std::string touma = user;
					logger->insertLog(Log(Log::TAG::END, originalBufferCmd, touma, Log::TYPE::INBOUND, ip));

					if (!isRealCall(touma, zapper, Log::TAG::END))
					{
						continue;
					}

					sendCallEnd(zapper);
				}
				else //commandContents[1] is not a known command... something fishy???
				{
					logger->insertLog(Log(Log::TAG::BADCMD, originalBufferCmd, userUtils->userFromCommandFd(sd), Log::TYPE::INBOUND, ip));
				}
			} // if FD_ISSET : figure out command or voice and handle appropriately
		}// for loop going through the fd set

		//now that all fds are finished inspecting, remove any of them that are dead.
		//don't mess with the map contents while the iterator is live.
		//removing while runnning causes segfaults because if the removed item gets iterated over after removal
		//it's no longer there so you get a segfault
		if(removals.size() > 0)
		{
#ifdef VERBOSE
			std::cout << "Removing " << removals.size() << " dead/leftover sockets\n";
#endif
			for(int deadSock : removals)
			{
				if(clientssl.count(deadSock) > 0)
				{
					removeClient(deadSock);
				}
			}
			removals.clear();
		}
#ifdef VERBOSE
		std::cout << "_____________________________________\n_________________________________\n";
#endif
	}

	//stop user utilities
	UserUtils *instance = UserUtils::getInstance();
	instance->killInstance();

	//openssl stuff
	SSL_CTX_free(sslcontext);
	ERR_free_strings();
	EVP_cleanup();
	
	//close ports
	close(cmdFD);
	return 0; 
}

void* udpThread(void *ptr)
{
	//unpackage media thread args
	struct UdpArgs *receivedArgs = (struct UdpArgs*)ptr;
	unsigned char sodiumPublicKey[crypto_box_PUBLICKEYBYTES];
	unsigned char sodiumPrivateKey[crypto_box_SECRETKEYBYTES];
	memcpy(sodiumPublicKey, receivedArgs->sodiumPublicKey, crypto_box_PUBLICKEYBYTES);
	memcpy(sodiumPrivateKey, receivedArgs->sodiumPrivateKey, crypto_box_SECRETKEYBYTES);
	int mediaPort = receivedArgs->port;
	free(ptr);

	//establish the udp socket for voice data
	int mediaFd;
	struct sockaddr_in mediaInfo;
	setupListeningSocket(SOCK_DGRAM, NULL, &mediaFd, &mediaInfo, mediaPort);

	//make the socket an expedited one
	int express = IPTOS_DSCP_EF;
	if(setsockopt(mediaFd, IPPROTO_IP, IP_TOS, (char*)&express, sizeof(int)) < 0)
	{
		std::string error="cannot set udp socket dscp expedited (" + std::to_string(errno) + ") " + std::string(strerror(errno));
		logger->insertLog(Log(Log::TAG::UDPTHREAD, error, Log::SELF(), Log::TYPE::ERROR, Log::SELFIP()));
	}

	while(true)
	{
		//setup buffer to receive on udp socket
		unsigned char mediaBuffer[MEDIASIZE+1] = {0};
		struct sockaddr_in sender;
		socklen_t senderLength = sizeof(struct sockaddr_in);

		//read encrypted voice data or registration
		int receivedLength = recvfrom(mediaFd, mediaBuffer, MEDIASIZE, 0, (struct sockaddr*)&sender, &senderLength);
		if(receivedLength < 0)
		{
			std::string error = "udp read error with errno " + std::to_string(errno) + ": " + std::string(strerror(errno));
			logger->insertLog(Log(Log::TAG::UDPTHREAD, error, Log::SELF(), Log::TYPE::ERROR, Log::SELFIP()));
			continue; //received nothing, this round is a write off
		}

		//ip:port, glue address and port together
		std::string summary = std::string(inet_ntoa(sender.sin_addr)) + ":" + std::to_string(ntohs(sender.sin_port));
		std::string user = userUtils->userFromUdpSummary(summary);
		ustate state = userUtils->getUserState(user);
		std::cout << "summary: " << summary << " indicates: " << user << "\n";

		//need to send an ack whether it's for the first time or because the first one went missing.
		if((user == "") || (state == INIT))
		{
#ifdef VERBOSE
			std::cout << "sending ack for summary: " << summary << " belonging to " << user << "/\n";
#endif

			std::string ip = std::string(inet_ntoa(sender.sin_addr));

			//figure out who sent this registration
			unsigned char userLengthDisassembled[JAVA_MAX_PRECISION_INT] = {0};
			memcpy(userLengthDisassembled, mediaBuffer, JAVA_MAX_PRECISION_INT);
			int userLength = reassembleInt(userLengthDisassembled, JAVA_MAX_PRECISION_INT);
			int maxUserLength = receivedLength - JAVA_MAX_PRECISION_INT - 1; //-1 for at least 1 byte of sodium asym encrytion
			if(userLength > maxUserLength)
			{//actually check the user name length makes sense
				continue;
			}

			unsigned char userBytes[userLength] = {0}; //+1: null terminate
			memcpy(userBytes, mediaBuffer+JAVA_MAX_PRECISION_INT, userLength);
			if(!legitimateAscii(userBytes, userLength))
			{
				continue;
			}

			//get the claimed user's sodium public key
			std::string claimedUser = std::string((char*)userBytes, userLength);
			unsigned char userPublicSodiumKey[crypto_box_PUBLICKEYBYTES] = {0};
			bool exists = userUtils->getSodiumPublicKey(claimedUser, userPublicSodiumKey);
			if(!exists)
			{
				continue; //user doesn't exist
			}

			//decrypt media port register command
			int decLength = 0;
			int inputLength = receivedLength - JAVA_MAX_PRECISION_INT - userLength;
			unsigned char input[inputLength] = {0};
			memcpy(input, mediaBuffer+JAVA_MAX_PRECISION_INT+userLength, inputLength);
			std::unique_ptr<unsigned char> decryptedArrayHeap;
			sodiumAsymDecrypt(input, inputLength, sodiumPrivateKey, userPublicSodiumKey, decryptedArrayHeap, decLength);

			//check if the decryption was successful
			if(decLength == 0)
			{
				continue; //decryption failed
			}


			//check the decrypted contents don't have unwanted junk like c#
			if(!legitimateAscii(decryptedArrayHeap.get(), decLength-1))
			{
				std::string unexpected = "unexpected byte in string";
				logger->insertLog(Log(Log::TAG::UDPTHREAD, unexpected, claimedUser, Log::TYPE::ERROR, ip));
				continue;
			}

			//check the udp registration timestamp
			std::string timestampString((char*)decryptedArrayHeap.get(), decLength);
			bool timestampOK = checkTimestamp(timestampString, Log::TAG::UDPTHREAD, timestampString, claimedUser, ip);
			if(!timestampOK)
			{
				continue;
			}

			//if the decryption(with authentication) passed and timestamp is ok. it's the real deal
			if(user == "")
			{
				std::string sessionkey = userUtils->getSessionKey(claimedUser);
				userUtils->setUdpSummary(sessionkey, summary);
				userUtils->setUdpInfo(sessionkey, sender);
				user = claimedUser;
			}

			//if the person is not in a call, there is no need to register a media port
			if(userUtils->getCallWith(user) == "")
			{
				userUtils->clearUdpInfo(user);
				continue;
			}

			//create and encrypt ack
			time_t now=time(NULL);
			std::string ack = std::to_string(now);
			std::unique_ptr<unsigned char> ackEnc;
			int encLength = 0;
			sodiumAsymEncrypt((unsigned char*)ack.c_str(), ack.length(), sodiumPrivateKey, userPublicSodiumKey, ackEnc, encLength);

			//encryption failed??
			if(encLength == 0)
			{
				std::string error = "failed to sodium encrypt udp ack???\n";
				logger->insertLog(Log(Log::TAG::UDPTHREAD, error, user , Log::TYPE::ERROR, ip));
				continue;
			}

			//send udp ack: no time like the present to test the 2 way udp connection
			int sent = sendto(mediaFd, ackEnc.get(), encLength, 0, (struct sockaddr*)&sender, senderLength);
			if(sent < 0)
			{
				std::string error = "udp sendto failed during media port registration with errno (" + std::to_string(errno) + ") " + std::string(strerror(errno));
				logger->insertLog(Log(Log::TAG::UDPTHREAD, error, user , Log::TYPE::ERROR, ip));
			}
		}
		else if(state == INCALL)
		{//in call, passthrough audio untouched (end to end encryption if only to avoid touching more openssl apis)
			std::string otherPerson = userUtils->getCallWith(user);

			//if the other person disappears midway through, calling clear session on his socket will cause
			//	you to have nobody listed in User.callWith (or "" default value). getUdpInfo("") won't end well
			if(otherPerson == "")
			{
				continue;
			}


			struct sockaddr_in otherSocket = userUtils->getUdpInfo(otherPerson);
			std::cout << "call with " << otherPerson << " sending to " << std::to_string(otherSocket.sin_addr.s_addr) << ":" << std::to_string(otherSocket.sin_port) << "\n";

			int sent = sendto(mediaFd, mediaBuffer, receivedLength, 0, (struct sockaddr*)&otherSocket, sizeof(otherSocket));
			if(sent < 0)
			{
				std::string error = "udp sendto failed during live call with errno (" + std::to_string(errno) + ") " + std::string(strerror(errno));
				std::string ip = std::string(inet_ntoa(otherSocket.sin_addr));
				logger->insertLog(Log(Log::TAG::UDPTHREAD, error, user , Log::TYPE::ERROR, ip));
			}
		}
	}
	return NULL;
}


//use a vector to prevent reading out of bounds
std::vector<std::string> parse(unsigned char command[])
{
//timestamp|login1|username
//timestamp|login2|username|challenge_decrypted

//session key is always the last one for easy censoring in the logs
//timestamp|call|otheruser|sessionkey
//timestamp|lookup|otheruser|sessionkey
//timestamp|reject|otheruser|sessionkey
//timestamp|accept|otheruser|sessionkey
//timestamp|end|otheruser|sessionkey
//timestamp|passthrough|otheruser|(aes key encrypted)|sessionkey
//timestamp|ready|otheruser|sessionkey

	char *token;
	char* save;
	int i = 0;
	std::vector<std::string> result;
	token = strtok_r((char*)command, "|", &save);
	while(token != NULL && i < COMMAND_MAX_SEGMENTS)
	{
		result.push_back(std::string(token));
		token = strtok_r(NULL, "|", &save);
		i++;
	}
	return result;
}

// sd: a client's socket descriptor
void removeClient(int sd)
{
	std::string uname = userUtils->userFromCommandFd(sd);

	SSL_shutdown(clientssl[sd]);
	SSL_free(clientssl[sd]);
	shutdown(sd, 2);
	close(sd);
	clientssl.erase(sd);

	//clean up the live list if needed
	userUtils->clearSession(uname);
}

//before doing an accept, reject, end command check to see if it's for a real call
//	or someone trying to get smart with the server
bool isRealCall(std::string persona, std::string personb, Log::TAG tag)
{
	bool real = true;

	std::string awith = userUtils->getCallWith(persona);
	std::string bwith = userUtils->getCallWith(personb);
	if((awith == "") || (bwith == ""))
	{
		real = false;
	}

	if((persona != bwith) || (personb != awith))
	{
		real = false;
	}

	if(!real)
	{
		int fd = userUtils->getCommandFd(persona);
		std::string ip = ipFromFd(fd);
		std::string error = persona + " sent a command for a nonexistant call";
		logger->insertLog(Log(tag, error, persona, Log::TYPE::ERROR, ip));

		time_t now = time(NULL);
		std::string invalid = std::to_string(now) + "|invalid";
		if(fd > 0)
		{
			SSL *ssl = clientssl[fd];
			write2Client(invalid, ssl);
			logger->insertLog(Log(tag, invalid, persona, Log::TYPE::OUTBOUND, ip));
		}
	}
	return real;
}

// write a message to a client
void write2Client(std::string response, SSL *respSsl)
{
	int errValue = SSL_write(respSsl, response.c_str(), response.size());

	if(errValue <= 0)
	{
		int socket = SSL_get_fd(respSsl);
		std::string user = userUtils->userFromCommandFd(socket);
		std::string ip = ipFromFd(socket);
		std::string error = "ssl_write returned an error of " + std::string(ERR_error_string(ERR_get_error(), NULL));
		logger->insertLog(Log(Log::TAG::SSL, error, user, Log::TYPE::ERROR, ip));
	}
}

std::string ipFromFd(int sd)
{
	struct sockaddr_in thisfd;
	socklen_t thisfdSize = sizeof(struct sockaddr_in);
	int result = getpeername(sd, (struct sockaddr*) &thisfd, &thisfdSize);
	if(result == 0)
	{
		return std::string(inet_ntoa(thisfd.sin_addr));
	}
	else
	{
		return "(" +std::to_string(errno) + ": " + std::string(strerror(errno)) + ")";
	}
}

int readSSL(SSL *sdssl, unsigned char inputBuffer[])
{
	//read from the socket into the buffer
	int bufferRead=0, totalRead=0;
	bool waiting;
	memset(inputBuffer, 0, COMMANDSIZE+1);
	do
	{//wait for the input chunk to come in first before doing something
		totalRead = SSL_read(sdssl, inputBuffer, COMMANDSIZE-bufferRead);
		if(totalRead > 0)
		{
			bufferRead = bufferRead + totalRead;
		}
		int sslerr = SSL_get_error(sdssl, totalRead);
		switch (sslerr)
		{
			case SSL_ERROR_NONE:
				waiting = false;
				break;
			//other cases when necessary. right now only no error signals a successful read
		}
	} while(waiting && SSL_pending(sdssl));

	///SSL_read return 0 = dead socket
	if(totalRead == 0)
	{
		int sd = SSL_get_fd(sdssl);
		std::string user = userUtils->userFromCommandFd(sd);
		std::string ip = ipFromFd(sd);
		std::string error = "socket has died";
		logger->insertLog(Log(Log::TAG::DEADSOCK, error, user, Log::TYPE::ERROR, ip));
	}
	return totalRead;
}

bool legitimateAscii(unsigned char* buffer, int length)
{
	for (int i = 0; i < length; i++)
	{
		unsigned char byte = buffer[i];

		bool isSign = ((byte == 43) || (byte == 45));
		bool isNumber = ((byte >= 48) && (byte <= 57));
		bool isUpperCase = ((byte >= 65) && (byte <= 90));
		bool isLowerCase = ((byte >= 97) && (byte <= 122));
		bool isDelimiter = (byte == 124);

		if (!isSign && !isNumber && !isUpperCase && !isLowerCase && !isDelimiter)
		{//actually only checking for ascii of interest
			return false;
		}
	}
	return true;
}

void sendCallEnd(std::string user)
{
	//reset both peoples's states and remove the call pair record
	std::string other = userUtils->getCallWith(user);
	userUtils->setUserState(user, NONE);
	userUtils->setUserState(other, NONE);
	userUtils->removeCallPair(user);

	//send the call end
	std::string resp = std::to_string(time(NULL)) + "|end|" + other;
	int cmdFd = userUtils->getCommandFd(user);
	SSL *ssl = clientssl[cmdFd];
	write2Client(resp, ssl);
	logger->insertLog(Log(Log::TAG::END, resp, user, Log::TYPE::OUTBOUND, ipFromFd(cmdFd)));
}

void sslAccept(int cmdFD, SSL_CTX* sslcontext, struct timeval* unauthTimeout)
{
	struct sockaddr_in cli_addr;
	socklen_t clilen = sizeof(cli_addr);

	int incomingCmd = accept(cmdFD, (struct sockaddr *) &cli_addr, &clilen);
	if(incomingCmd < 0)
	{
		std::string error = "accept system call error (" + std::to_string(errno) + ") " + std::string(strerror(errno));
		logger->insertLog(Log(Log::TAG::INCOMINGCMD, error, Log::SELF(), Log::TYPE::ERROR, Log::DONTKNOW()));
		return;
	}
	std::string ip = inet_ntoa(cli_addr.sin_addr);

	//for new sockets that nobody owns, don't give much leniency for timeouts
	if(setsockopt(incomingCmd, SOL_SOCKET, SO_RCVTIMEO, (char*)unauthTimeout, sizeof(struct timeval)) < 0)
	{
		std::string error = "cannot set timeout for incoming command socket (" + std::to_string(errno) + ") " + std::string(strerror(errno));
		logger->insertLog(Log(Log::TAG::INCOMINGCMD, error, Log::SELF(), Log::TYPE::ERROR, ip));
		shutdown(incomingCmd, 2);
		close(incomingCmd);
		return;
	}

	//disable nagle delay for heartbeat which is a 1 char payload
	int nagle = 0;
	if(setsockopt(incomingCmd, IPPROTO_TCP, TCP_NODELAY, (char*)&nagle, sizeof(int)))
	{
		std::string error = "cannot disable nagle delay (" + std::to_string(errno) + ") " + std::string(strerror(errno));
		logger->insertLog(Log(Log::TAG::INCOMINGCMD, error, Log::SELF(), Log::TYPE::ERROR, ip));
	}

	//setup ssl connection
	SSL *connssl = SSL_new(sslcontext);
	SSL_set_fd(connssl, incomingCmd);

	//give 10 tries to get an ssl connection because first try isn't always successful
	int sslerr = SSL_ERROR_NONE;
	bool proceed = false;
	int retries = DT_SSL_ACCEPT_RETRIES;
	while(retries > 0)
	{
		int result = SSL_accept(connssl);
		sslerr = SSL_get_error(connssl, result);
		if(sslerr == SSL_ERROR_NONE) //everything ok, proceed
		{
			proceed = true;
			break;
		}
		else if (sslerr == SSL_ERROR_WANT_READ)
		{//incomplete handshake, try again
			retries--;
		}
		else
		{//some other error. stop
			break;
		}
	}

	if(proceed)
	{
		std::string message = "new command socket from " + ip;
		logger->insertLog(Log(Log::TAG::INCOMINGCMD, message, Log::SELF(), Log::TYPE::INBOUND, ip));
		clientssl[incomingCmd] = connssl;
	}
	else
	{
		std::string error = "Problem initializing new command tls connection" + std::string(ERR_error_string(ERR_get_error(), NULL));
		logger->insertLog(Log(Log::TAG::INCOMINGCMD, error, Log::SELF(), Log::TYPE::ERROR, ip));
		SSL_shutdown(connssl);
		SSL_free(connssl);
		shutdown(incomingCmd, 2);
		close(incomingCmd);
	}
}

bool checkTimestamp(const std::string& tsString, Log::TAG tag, const std::string& errorMessage, const std::string& user, const std::string& ip)
{
	try
	{
		uint64_t timestamp = (uint64_t) std::stoull(tsString); //catch is for this
		uint64_t maxError = 60 * MARGIN_OF_ERROR;
		time_t now=time(NULL);
		uint64_t timeDifference = std::max((uint64_t) now, timestamp) - std::min((uint64_t) now, timestamp);
		if (timeDifference > maxError)
		{
			//only bother processing the command if the timestamp was valid

			//prepare the error log
			uint64_t mins = timeDifference / 60;
			uint64_t seconds = timeDifference - mins * 60;
			std::string error = "timestamp received was outside the " + std::to_string(MARGIN_OF_ERROR) + " minute margin of error: " + std::to_string(mins) + "mins, " + std::to_string(seconds) + "seconds";
			error = error + errorMessage;
			logger->insertLog(Log(tag, error, user, Log::TYPE::ERROR, ip));
			return false;
		}
	}
	catch(std::invalid_argument &badarg)
	{ //timestamp couldn't be parsed. assume someone is trying something fishy
		logger->insertLog(Log(tag, "invalid_argument: " + errorMessage, user, Log::TYPE::INBOUND, ip));

		std::string error="INVALID ARGUMENT EXCEPTION: " + errorMessage;
		logger->insertLog(Log(tag, error, user, Log::TYPE::ERROR, ip));

		return false;
	}
	catch(std::out_of_range &exrange)
	{
		logger->insertLog(Log(tag, "out_of_range: " + errorMessage, user, Log::TYPE::INBOUND, ip));

		std::string error="OUT OF RANGE: " + errorMessage;
		logger->insertLog(Log(tag, error, user, Log::TYPE::ERROR, ip));

		return false;
	}

	return true;
}
