#include <string>
#include <vector>
#include <openssl/ssl.h>

using namespace std;

//parse incoming server commands (split the incoming command string by the | character)
vector<string> parse(char command[]);

//remove a client's command and media or only media depending what kind of sd is given
void removeClient(int sd);

//verify the call is real and not a malicious hand crafted command
bool isRealCall(string persona, string personb);

//convert the string to c char[] and send it by ssl* (when sending, send only as many bytes as there are characters
// and not the whole command string buffer [] size
void write2Client(string response, SSL *respSsl, uint64_t relatedKey);

//get the ip address of a socket descriptor in human readable 192.168.1.1 format
string ipFromSd(int sd);

//turn unsigned char array into string of #s
string stringify(unsigned char *bytes, int length);

//read an SSL socket into param inputBuffer. maximum read size in const.h
int readSSL(SSL *sdssl, char inputBuffer[], uint64_t iterationKey);
