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
void write2Client(string response, SSL *respSsl);

//for sig alarm: set the boolean flag that sig alarm was issued so a log can be written
void alarm_handler(int signum);

//used for parsing the configuration file: remove whitespace preceding/trailing and comments
string trim (string str);

//get the ip address of a socket descriptor in human readable 192.168.1.1 format
string ipFromSd(int sd);

//get the time now in milliseconds
uint64_t millisNow();
