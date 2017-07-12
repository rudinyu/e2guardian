// http://e2guardian.org/
// Released under the GPL v2, with the OpenSSL exception described in the README file.

// INCLUDES
#ifdef HAVE_CONFIG_H
#include "dgconfig.h"
#endif
//#include "NaughtyFilter.hpp"
//#include "StoryBoard.hpp"
#include "ConnectionHandler.hpp"
#include "DataBuffer.hpp"
#include "UDSocket.hpp"
//#include "Auth.hpp"
#include "FDTunnel.hpp"
#include "BackedStore.hpp"
#include "Queue.hpp"
#include "ImageContainer.hpp"
#include "FDFuncs.hpp"
#include <signal.h>
#include <arpa/nameser.h>
#include <resolv.h>

#ifdef __SSLMITM
#include "CertificateAuthority.hpp"
#endif //__SSLMITM

#include <syslog.h>
#include <cerrno>
#include <cstdio>
#include <ctime>
#include <algorithm>
#include <netdb.h>
#include <cstdlib>
#include <unistd.h>
#include <sys/time.h>
#include <strings.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <istream>
#include <sstream>
#include <memory>

#ifdef ENABLE_ORIG_IP
#include <linux/types.h>
#include <linux/netfilter_ipv4.h>
#endif

#ifdef __SSLMITM
#include "openssl/ssl.h"
#include "openssl/x509v3.h"
#include "String.hpp"
#endif

// GLOBALS
extern OptionContainer o;
extern bool is_daemonised;
extern std::atomic<int> ttg;
//bool reloadconfig = false;
// If a specific debug line is needed
thread_local int dbgPeerPort = 0;


// IMPLEMENTATION

ConnectionHandler::ConnectionHandler()
    : clienthost(NULL) {
     ch_isiphost.comp(",*[a-z|A-Z].*");

    // initialise SBauth structure
    SBauth.filter_group = 0;
    SBauth.is_authed = false;
    SBauth.user_name = "";
}


// Custom exception class for POST filtering errors
class postfilter_exception : public std::runtime_error
{
    public:
    postfilter_exception(const char *const &msg)
        : std::runtime_error(msg){};
};

//
// URL cache funcs
//

// check the URL cache to see if we've already flagged an address as clean
bool wasClean(HTTPHeader &header, String &url, const int fg)
{
   return false;   // this function needs rewriting always return false
}

// add a known clean URL to the cache
void addToClean(String &url, const int fg)
{
    return ;   // this function needs rewriting
}

//
// ConnectionHandler class
//


void ConnectionHandler::cleanThrow(const char *message, Socket &peersock, Socket &proxysock)
{
if (o.logconerror)
{
   int peerport = peersock.getPeerSourcePort();

    std::string peer_ip = peersock.getPeerIP();

    int err = peersock.getErrno();

    if (peersock.isTimedout())
        syslog(LOG_INFO, "%d %s Client at %s Connection timedout - errno: %d", peerport, message, peer_ip.c_str(), err);
    else if (peersock.isHup())
        syslog(LOG_INFO, "%d %s Client at %s has disconnected - errno: %d", peerport, message, peer_ip.c_str(), err);
     else if (peersock.sockError())
        syslog(LOG_INFO, "%d %s Client at %s Connection socket error - errno: %d", peerport, message, peer_ip.c_str(),err);
    else if (peersock.isNoRead())
        syslog(LOG_INFO, "%d %s cant read Client Connection at %s - errno: %d ", peerport, message, peer_ip.c_str(),err);
    else if (peersock.isNoWrite())
        syslog(LOG_INFO, "%d %s cant write Client Connection  at %s - errno: %d ", peerport, message, peer_ip.c_str(),err);
    else if (peersock.isNoOpp())
        syslog(LOG_INFO, "%d %s Client Connection at %s is no-op - errno: %d", peerport, message, peer_ip.c_str(),err);

    err = proxysock.getErrno();
    if (proxysock.isTimedout())
        syslog(LOG_INFO, "%d %s proxy timedout - errno: %d", peerport, message, err);
    else if (proxysock.isHup())
        syslog(LOG_INFO, "%d %s proxy has disconnected - errno: %d", peerport, message, err);
    else if (proxysock.sockError())
        syslog(LOG_INFO, "%d %s proxy socket error - errno: %d", peerport, message, err);
    else if (proxysock.isNoRead())
        syslog(LOG_INFO, "%d %s cant read proxy Connection - errno: %d ", peerport, message, err);
    else if (proxysock.isNoWrite())
        syslog(LOG_INFO, "%d %s cant write proxy Connection  - errno: %d", peerport, message, err);
    else if (proxysock.isNoOpp())
        syslog(LOG_INFO, "%d %s proxy Connection s no-op - errno: %d", peerport, message, err);
}
    if (proxysock.isNoOpp())
        proxysock.close();
    throw std::exception();
}

void ConnectionHandler::cleanThrow(const char *message, Socket &peersock ) {
    if (o.logconerror)
    {
        int peerport = peersock.getPeerSourcePort();
        std::string peer_ip = peersock.getPeerIP();
        int err = peersock.getErrno();

        if (peersock.isTimedout())
            syslog(LOG_INFO, "%d %s Client at %s Connection timedout - errno: %d", peerport, message, peer_ip.c_str(), err);
        else if (peersock.isHup())
            syslog(LOG_INFO, "%d %s Client at %s has disconnected - errno: %d", peerport, message, peer_ip.c_str(), err);
        else if (peersock.sockError())
            syslog(LOG_INFO, "%d %s Client at %s Connection socket error - errno: %d", peerport, message, peer_ip.c_str(),err);
        else if (peersock.isNoRead())
            syslog(LOG_INFO, "%d %s cant read Client Connection at %s - errno: %d ", peerport, message, peer_ip.c_str(),err);
        else if (peersock.isNoWrite())
            syslog(LOG_INFO, "%d %s cant write Client Connection  at %s - errno: %d ", peerport, message, peer_ip.c_str(),err);
        else if (peersock.isNoOpp())
            syslog(LOG_INFO, "%d %s proxy Connection s no-op - errno: %d", peerport, message, err);
    }
    throw std::exception();
}

// strip the URL down to just the IP/hostname, then do an isIPHostname on the result
bool ConnectionHandler::isIPHostnameStrip(String url)
{
    url = url.getHostname();
    if(ch_isiphost.match(url.toCharArray(), Rch_isiphost))
        return false;
    else
        return true;
//    return ldl->fg[0]->isIPHostname(url);
}

// perform URL encoding on a string
std::string ConnectionHandler::miniURLEncode(const char *s)
{
    std::string encoded;
    char *buf = new char[3];
    unsigned char c;
    for (int i = 0; i < (signed)strlen(s); i++) {
        c = s[i];
        // allowed characters in a url that have non special meaning
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            encoded += c;
            continue;
        }
        // all other characters get encoded
        sprintf(buf, "%02x", c);
        encoded += "%";
        encoded += buf;
    }
    delete[] buf;
    return encoded;
}

// create a temporary bypass URL for the banned page
String ConnectionHandler::hashedURL(String *url, int filtergroup, std::string *clientip, bool infectionbypass)
{
    // filter/virus bypass hashes last for a certain time only
    //String timecode(time(NULL) + (infectionbypass ? (*ldl->fg[filtergroup]).infection_bypass_mode : (*ldl->fg[filtergroup]).bypass_mode));
    String timecode(time(NULL) + (infectionbypass ? (*ldl->fg[filtergroup]).infection_bypass_mode : (*ldl->fg[filtergroup]).bypass_mode));
    // use the standard key in normal bypass mode, and the infection key in infection bypass mode
    String magic(infectionbypass ? ldl->fg[filtergroup]->imagic.c_str() : ldl->fg[filtergroup]->magic.c_str());
    magic += clientip->c_str();
    magic += timecode;
    String res(infectionbypass ? "GIBYPASS=" : "GBYPASS=");
    if (!url->after("://").contains("/")) {
        String newurl((*url));
        newurl += "/";
        res += newurl.md5(magic.toCharArray());
    } else {
        res += url->md5(magic.toCharArray());
    }
    res += timecode;
    return res;
}

// create temporary bypass cookie
String ConnectionHandler::hashedCookie(String *url, const char *magic, std::string *clientip, int bypasstimestamp)
{
    String timecode(bypasstimestamp);
    String data(magic);
    data += clientip->c_str();
    data += timecode;
    String res(url->md5(data.toCharArray()));
    res += timecode;

#ifdef DGDEBUG
    std::cout << dbgPeerPort << " -hashedCookie=" << res << std::endl;
#endif
    return res;
}


// send a file to the client - used during bypass of blocked downloads
off_t ConnectionHandler::sendFile(Socket *peerconn, String &filename, String &filemime, String &filedis, String &url)
{
    int fd = open(filename.toCharArray(), O_RDONLY);
    if (fd < 0) { // file access error
        syslog(LOG_ERR, "Error reading file to send");
#ifdef DGDEBUG
        std::cout << dbgPeerPort << " -Error reading file to send:" << filename << std::endl;
#endif
        String fnf(o.language_list.getTranslation(1230));
        String message("HTTP/1.0 404 " + fnf + "\nContent-Type: text/html\n\n<HTML><HEAD><TITLE>" + fnf + "</TITLE></HEAD><BODY><H1>" + fnf + "</H1></BODY></HTML>\n");
        peerconn->writeString(message.toCharArray());
        return 0;
    }

    off_t filesize = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    String message("HTTP/1.0 200 OK\nContent-Type: " + filemime + "\nContent-Length: " + String(filesize));
    if (filedis.length() == 0) {
        filedis = url.before("?");
        while (filedis.contains("/"))
            filedis = filedis.after("/");
    }
    message += "\nContent-disposition: attachment; filename=" + filedis;
    message += "\n\n";
        //peerconn->writeString(message.toCharArray());
    if(!peerconn->writeString(message.toCharArray())) {
        close(fd);
        return 0;
    }

    // perform the actual sending
    off_t sent = 0;
    int rc;
    char *buffer = new char[250000];
    while (sent < filesize) {
        rc = readEINTR(fd, buffer, 250000);
#ifdef DGDEBUG
        std::cout << dbgPeerPort << " -reading send file rc:" << rc << std::endl;
#endif
        if (rc < 0) {
#ifdef DGDEBUG
            std::cout << dbgPeerPort << " -error reading send file so throwing exception" << std::endl;
#endif
            delete[] buffer;
//            throw std::exception();
            cleanThrow("error reading send file", *peerconn);
        }
        if (rc == 0) {
#ifdef DGDEBUG
            std::cout << dbgPeerPort << " -got zero bytes reading send file" << std::endl;
#endif
            break; // should never happen
        }
        // as it's cached to disk the buffer must be reasonably big
        if (!peerconn->writeToSocket(buffer, rc, 0, 100)) {
            delete[] buffer;
            cleanThrow("Error sending file to client", *peerconn);
           // throw std::exception();
        }
        sent += rc;
#ifdef DGDEBUG
        std::cout << dbgPeerPort << " -total sent from temp:" << sent << std::endl;
#endif
    }
    delete[] buffer;
    close(fd);
    return sent;
}

// pass data between proxy and client, filtering as we go.
// this is the only public function of ConnectionHandler
int ConnectionHandler::handlePeer(Socket &peerconn, String &ip, stat_rec* &dystat)
{
    persistent_authed = false;
    is_real_user = false;
    int rc = 0;
//#ifdef DGDEBUG
    // for debug info only - TCP peer port
    dbgPeerPort = peerconn.getPeerSourcePort();
//#endif
    Socket proxysock;

    rc = handleConnection(peerconn, ip, false, proxysock, dystat);
    //if ( ldl->reload_id != load_id)
   //     rc = -1;
    return rc;
}

// all content blocking/filtering is triggered from calls inside here
int ConnectionHandler::handleConnection(Socket &peerconn, String &ip, bool ismitm, Socket &proxysock,
stat_rec* &dystat)
{
    struct timeval thestart;
    gettimeofday(&thestart, NULL);

    //peerconn.setTimeout(o.proxy_timeout);
    peerconn.setTimeout(o.pcon_timeout);

   // ldl = o.currentLists();

    HTTPHeader docheader(__HEADER_RESPONSE); // to hold the returned page header from proxy
    HTTPHeader header(__HEADER_REQUEST); // to hold the incoming client request headeri(ldl)

    // set a timeout as we don't want blocking 4 eva
    // this also sets how long a peerconn will wait for other requests
    header.setTimeout(o.pcon_timeout);
    docheader.setTimeout(o.exchange_timeout);

    // to hold the returned page
//    DataBuffer docbody;
//    docbody.setTimeout(o.exchange_timeout);

    // flags
    //bool waschecked = false;
    //bool wasrequested = false;
    //bool isexception = false;
//    bool isourwebserver = false;
//    bool wasclean = false;
//    bool cachehit = false;
//    bool isbypass = false;
//    bool iscookiebypass = false;
//    bool isvirusbypass = false;
//    bool isscanbypass = false;
    //bool ispostblock = false;
//    bool pausedtoobig = false;
//    bool wasinfected = false;
//    bool wasscanned = false;
//    bool contentmodified = false;
//    bool urlmodified = false;
//    bool headermodified = false;
//    bool headeradded = false;
//    bool isconnect;
//    bool ishead;
//    bool scanerror;
//    bool ismitmcandidate = false;
//    bool do_mitm = false;
//    bool is_ssl = false;
    int bypasstimestamp = 0;
//    bool urlredirect = false;
    // Remove some results from log: eg: 302 requests
//    bool nolog = false;

    // 0=none,1=first line,2=all
//    int headersent = 0;
//    int message_no = 0;

    // Content scanning plugins to use for request (POST) & response data
    std::deque<CSPlugin *> requestscanners;
    std::deque<CSPlugin *> responsescanners;

//    std::string mimetype("-");

    //String url;
//    String logurl;
//    String urld;
//    String urldomain;

//    std::string exceptionreason; // to hold the reason for not blocking
///    std::string exceptioncat;

//    off_t docsize = 0; // to store the size of the returned document for logging

    std::string clientip(ip.toCharArray()); // hold the clients ip

    if (clienthost) delete clienthost;

    clienthost = NULL; // and the hostname, if available
    matchedip = false;

    // clear list of parameters extracted from URL
    urlparams.clear();

    // clear out info about POST data
    postparts.clear();

#ifdef DGDEBUG // debug stuff surprisingly enough
    std::cout << dbgPeerPort << " -got peer connection" << std::endl;
    std::cout << dbgPeerPort << clientip << std::endl;
#endif
    // proxysock now passed to function
    // Socket proxysock;

    try {
        int rc;

#ifdef DGDEBUG
        int pcount = 0;
#endif

        // assume all requests over the one persistent connection are from
        // the same user. means we only need to query the auth plugin until
        // we get credentials, then assume they are valid for all reqs. on
        // the persistent connection.
        std::string oldclientuser;
        std::string room;

        int oldfg = 0;
        bool authed = false;
        bool isbanneduser = false;

//        FDTunnel fdt;
//        NaughtyFilter checkme(header, docheader);
        AuthPlugin *auth_plugin = NULL;

        // RFC states that connections are persistent
        bool persistOutgoing = true;
        bool persistPeer = true;
        bool persistProxy = true;

        bool firsttime = true;
        if(!header.in(&peerconn, true, true)) {     // get header from client, allowing persistency
           if (o.logconerror) {
               if (peerconn.getFD() > -1) {

                   int err = peerconn.getErrno();
                   int pport = peerconn.getPeerSourcePort();
                   std::string peerIP = peerconn.getPeerIP();

                   syslog(LOG_INFO, "%d No header recd from client at %s - errno: %d", pport, peerIP.c_str(), err);
#ifdef DGDEBUG
            	   std::cout << "pport" << " No header recd from client - errno: " << err << std::endl;
#endif
               } else {
                   syslog(LOG_INFO, "Client connection closed early - no request header received");
               }
           }
            firsttime = false;
            persistPeer = false;
        }
        else {
            ++dystat->reqs;
// TODO  addXFor... into HTTPHeader::in or checkheader()
            if (o.forwarded_for && !ismitm) {
                header.addXForwardedFor(clientip); // add squid-like entry
            }
#ifdef DGDEBUG
            //header.dbshowheader(&checkme.logurl, clientip.c_str());
#endif
        }
        //
        // End of set-up section
        //
        // Start of main loop
        //

        // maintain a persistent connection
        while ((firsttime || persistPeer) && !ttg)
        //    while ((firsttime || persistPeer) && !reloadconfig)
        {
#ifdef DGDEBUG
            std::cout << " firsttime =" << firsttime << "ismitm =" << ismitm << " clientuser =" << clientuser << " group = " << filtergroup << std::endl;
#endif
            ldl = o.currentLists();
            NaughtyFilter checkme(header, docheader);
            DataBuffer docbody;
            docbody.setTimeout(o.exchange_timeout);
            FDTunnel fdt;

            if (firsttime) {
                // reset flags & objects next time round the loop
                firsttime = false;

                // quick trick for the very first connection :-)
                if (!ismitm)
                    persistProxy = false;
            } else {
// another round...
#ifdef DGDEBUG
                std::cout << dbgPeerPort << " -persisting (count " << ++pcount << ")" << std::endl;
                syslog(LOG_ERR, "Served %d requests on this connection so far - ismitm=%d", pcount, ismitm);
                std::cout << dbgPeerPort << " - " << clientip << std::endl;
#endif
                header.reset();
                if(!header.in(&peerconn, true, true)) {
#ifdef DGDEBUG
                    std::cout << dbgPeerPort << " -Persistent connection closed" << std::endl;
#endif
                    break;
                }
                ++dystat->reqs;
                if (o.forwarded_for && !ismitm) {
                    header.addXForwardedFor(clientip); // add squid-like entry
                }
#ifdef DGDEBUG
                header.dbshowheader(&checkme.logurl, clientip.c_str());
#endif
                // we will actually need to do *lots* of resetting of flags etc. here for pconns to work
                gettimeofday(&thestart, NULL);

                //waschecked = false; // flags
                ///
                ///wasrequested = false;
                //isexception = false;
                //isourwebserver = false;
                //wasclean = false;
                //isbypass = false;
                //iscookiebypass = false;
                //isvirusbypass = false;
                bypasstimestamp = 0;
                //isscanbypass = false;
                //ispostblock = false;
                //checkme.pausedtoobig = false;
                ///wasinfected = false;
                //wasscanned = false;
                //contentmodified = false;
                //urlmodified = false;
                //headermodified = false;
                //headeradded = false;
                //urlredirect = false;

                authed = false;
                isbanneduser = false;

                requestscanners.clear();
                responsescanners.clear();

                checkme.headersent = 0; // 0=none,1=first line,2=all
                //delete clienthost;
                //clienthost = NULL; // and the hostname, if available
                matchedip = false;
                urlparams.clear();
                postparts.clear();
                //docsize = 0; // to store the size of the returned document for logging
                checkme.message_no = 0;
                checkme.mimetype = "-";
                //exceptionreason = "";
                //exceptioncat = "";
                room = "";

                // reset docheader & docbody
                // headers *should* take care of themselves on the next in()
                // actually not entirely true for docheader - we may read
                // certain properties of it (in denyAccess) before we've
                // actually performed the next in(), so make sure we do a full
                // reset now.
                docheader.reset();
                docbody.reset();

                // our filter
                //checkme.reset();
            }
//
            // do all of this normalisation etc just the once at the start.
            checkme.url = header.getUrl(false, ismitm);
            checkme.baseurl = checkme.url;
            checkme.baseurl.removeWhiteSpace();
            checkme.baseurl.toLower();
            checkme.baseurl.removePTP();
            checkme.logurl = header.getLogUrl(false, ismitm);
            checkme.urld = header.decode(checkme.url);
            checkme.urldomain = checkme.url.getHostname();
            checkme.urldomain.toLower();
            checkme.isiphost = isIPHostnameStrip(checkme.urldomain);
            checkme.is_ssl = header.requestType().startsWith("CONNECT");
            checkme.isconnect = checkme.is_ssl;
            checkme.ismitm = ismitm;

            //If proxy connction is not persistent...
            if (!persistProxy) {
                try {
                    // ...connect to proxy
                    proxysock.setTimeout(1000);
                    for (int i = 0; i < o.proxy_timeout_sec; i++) {
                        rc = proxysock.connect(o.proxy_ip, o.proxy_port);

                        if (!rc) {
                            if (i > 0) {
                                syslog(LOG_ERR, "Proxy responded after %d retrys", i);
                            }
                            break;
                        } else {
                            if (!proxysock.isTimedout())
                                  std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                        }
                    }
                    if (rc) {
#ifdef DGDEBUG
                        std::cerr << dbgPeerPort << " -Error connecting to proxy" << std::endl;
#endif
                        syslog(LOG_ERR, "Proxy not responding after %d trys - ip client: %s destination: %s - %d::%s", o.proxy_timeout_sec, clientip.c_str(), checkme.urldomain.c_str(), proxysock.getErrno(),strerror(proxysock.getErrno()));
                        if (proxysock.isTimedout()) {
                            checkme.message_no = 201;
                            peerconn.writeString("HTTP/1.0 504 Gateway Time-out\nContent-Type: text/html\n\n");
                            peerconn.writeString(
                                    "<HTML><HEAD><TITLE>e2guardian - 504 Gateway Time-out</TITLE></HEAD><BODY><H1>e2guardian - 504 Gateway Time-out</H1>");
                            peerconn.writeString(o.language_list.getTranslation(201));
                            peerconn.writeString(o.language_list.getTranslation(204));
                            peerconn.writeString("</BODY></HTML>\n");
                        } else {
                            checkme.message_no = 202;
                            peerconn.writeString("HTTP/1.0 502 Gateway Error\nContent-Type: text/html\n\n");
                            peerconn.writeString(
                                    "<HTML><HEAD><TITLE>e2guardian - 502 Gateway Error</TITLE></HEAD><BODY><H1>e2guardian - 502 Gateway Error</H1>");
                            peerconn.writeString(o.language_list.getTranslation(202));
                            peerconn.writeString(o.language_list.getTranslation(204));
                            peerconn.writeString("</BODY></HTML>\n");
                        }
                        return 3;
                    }
                } catch (std::exception &e) {
#ifdef DGDEBUG
                    std::cerr << dbgPeerPort << " -exception while creating proxysock: " << e.what() << std::endl;
#endif
                }
            }

#ifdef DGDEBUG
            std::cerr << getpid() << "Start URL " << checkme.url.c_str() << "is_ssl=" << checkme.is_ssl << "ismitm=" << ismitm << std::endl;
#endif

            // checks for bad URLs to prevent security holes/domain obfuscation.
            if (header.malformedURL(checkme.url)) {
                    // The requested URL is malformed.
                    checkme.message_no = 200;
                    peerconn.writeString("HTTP/1.0 400 Bad Request\nContent-Type: text/html\n\n");
                    peerconn.writeString("<HTML><HEAD><TITLE>e2guardian - 400 Bad Request</TITLE></HEAD><BODY><H1>e2guardian - 400 Bad Request</H1>");
                    peerconn.writeString(o.language_list.getTranslation(200));
                    peerconn.writeString("</BODY></HTML>\n");
                proxysock.close(); // close connection to proxy
                break;
            }

            checkme.urld = header.decode(checkme.url);

            if (checkme.urldomain == "internal.test.e2guardian.org") {
                    peerconn.writeString("HTTP/1.0 200 \nContent-Type: text/html\n\n<HTML><HEAD><TITLE>e2guardian internal test</TITLE></HEAD><BODY><H1>e2guardian internal test OK</H1> ");
                    peerconn.writeString("</BODY></HTML>\n");
                proxysock.close(); // close connection to proxy
                break;
            }

            // do total block list checking here
            if (o.use_total_block_list && o.inTotalBlockList(checkme.urld)) {
                if (checkme.is_ssl) {
                        peerconn.writeString("HTTP/1.0 404 Banned Site\nContent-Type: text/html\n\n<HTML><HEAD><TITLE>Protex - Banned Site</TITLE></HEAD><BODY><H1>Protex - Banned Site</H1> ");
                        peerconn.writeString(checkme.logurl.c_str());
                        // The requested URL is malformed.
                        peerconn.writeString("</BODY></HTML>\n");
                } else { // write blank graphic
                        peerconn.writeString("HTTP/1.0 200 OK\n");
                    o.banned_image.display(&peerconn);
                }
                proxysock.close(); // close connection to proxy
                break;
            }

            // don't let the client connection persist if the client doesn't want it to.
            persistOutgoing = header.isPersistent();
            //
            //
            // Start of Authentication Checks
            //
            //
            // don't have credentials for this connection yet? get some!
            overide_persist = false;
            if (!persistent_authed) {
#ifdef DGDEBUG
                std::cout << dbgPeerPort << " -Not got persistent credentials for this connection - querying auth plugins" << std::endl;
#endif
                bool dobreak = false;
                if (o.authplugins.size() != 0) {
                    // We have some auth plugins load
                    int authloop = 0;
                    String tmp;

                    for (std::deque<Plugin *>::iterator i = o.authplugins_begin; i != o.authplugins_end; i++) {
#ifdef DGDEBUG
                        std::cout << dbgPeerPort << " -Querying next auth plugin..." << std::endl;
#endif
                        // try to get the username & parse the return value
                        auth_plugin = (AuthPlugin *)(*i);

                        // auth plugin selection for multi ports
                        //
                        //
                        // Logic changed to allow auth scan with multiple ports as option to auth-port
                        //       fixed mapping
                        //
                        if (o.map_auth_to_ports) {
                            if (o.filter_ports.size() > 1) {
                                tmp = o.auth_map[peerconn.getPort()];
                            } else {
                                // auth plugin selection for one port
                                tmp = o.auth_map[authloop];
                                authloop++;
                            }

                            if (tmp.compare(auth_plugin->getPluginName().toCharArray()) == 0) {
                                rc = auth_plugin->identify(peerconn, proxysock, header, clientuser, is_real_user);
                            } else {
                                rc = DGAUTH_NOMATCH;
                            }
                        } else {
                            rc = auth_plugin->identify(peerconn, proxysock, header, clientuser, is_real_user);
                        }

                        if (rc == DGAUTH_NOMATCH) {
#ifdef DGDEBUG
                            std::cout << "Auth plugin did not find a match; querying remaining plugins" << std::endl;
#endif
                            continue;
                        } else if (rc == DGAUTH_REDIRECT) {
#ifdef DGDEBUG
                            std::cout << "Auth plugin told us to redirect client to \"" << clientuser << "\"; not querying remaining plugins" << std::endl;
#endif
                            // ident plugin told us to redirect to a login page
                            String writestring("HTTP/1.0 302 Redirect\r\nLocation: ");
                            writestring += clientuser;
                            writestring += "\r\n\r\n";
                            peerconn.writeString(writestring.toCharArray());   // no action on failure
                            dobreak = true;
                            break;
                        } else if (rc == DGAUTH_OK_NOPERSIST) {
#ifdef DGDEBUG
                            std::cout << "Auth plugin  returned OK but no persist not setting persist auth" << std::endl;
#endif
                            overide_persist = true;
                        } else if (rc < 0) {
                            if (!is_daemonised)
                                std::cerr << "Auth plugin returned error code: " << rc << std::endl;
                            syslog(LOG_ERR, "Auth plugin returned error code: %d", rc);
                            dobreak = true;
                            break;
                        }
#ifdef DGDEBUG
                        std::cout << dbgPeerPort << " -Auth plugin found username " << clientuser << " (" << oldclientuser << "), now determining group" << std::endl;
#endif
                        if (clientuser == oldclientuser) {
#ifdef DGDEBUG
                            std::cout << dbgPeerPort << " -Same user as last time, re-using old group no." << std::endl;
#endif
                            authed = true;
                            filtergroup = oldfg;
                            break;
                        }
                        // try to get the filter group & parse the return value
                        rc = auth_plugin->determineGroup(clientuser, filtergroup,ldl->filter_groups_list);
                        if (rc == DGAUTH_OK) {
#ifdef DGDEBUG
                            std::cout << "Auth plugin found username & group; not querying remaining plugins" << std::endl;
#endif
                            authed = true;
                            break;
                        } else if (rc == DGAUTH_NOMATCH) {
#ifdef DGDEBUG
                            std::cout << "Auth plugin did not find a match; querying remaining plugins" << std::endl;
#endif
                            continue;
                        } else if (rc == DGAUTH_NOUSER) {
#ifdef DGDEBUG
                            std::cout << "Auth plugin found username \"" << clientuser << "\" but no associated group; not querying remaining plugins" << std::endl;
#endif
                            if (o.auth_requires_user_and_group)
                                continue;
                            filtergroup = 0; //default group - one day configurable?
                            authed = true;
                            break;
                        } else if (rc < 0) {
                            if (!is_daemonised)
                                std::cerr << "Auth plugin returned error code: " << rc << std::endl;
                            syslog(LOG_ERR, "Auth plugin returned error code: %d", rc);
                            dobreak = true;
                            break;
                        }
                    } // end of querying all plugins (for)

                    // break the peer loop
                    if (dobreak)
                        break;

                    if ((!authed) || (filtergroup < 0) || (filtergroup >= o.numfg)) {
#ifdef DGDEBUG
                        if (!authed)
                            std::cout << dbgPeerPort << " -No identity found; using defaults" << std::endl;
                        else
                            std::cout << dbgPeerPort << " -Plugin returned out-of-range filter group number; using defaults" << std::endl;
#endif
                        // If none of the auth plugins currently loaded rely on querying the proxy,
                        // such as 'ident' or 'ip', then pretend we're authed. What this flag
                        // actually controls is whether or not the query should be forwarded to the
                        // proxy (without pre-emptive blocking); we don't want this for 'ident' or
                        // 'ip', because Squid isn't necessarily going to return 'auth required'.
                        authed = !o.auth_needs_proxy_query;
#ifdef DGDEBUG
                        if (!o.auth_needs_proxy_query)
                            std::cout << dbgPeerPort << " -No loaded auth plugins require parent proxy queries; enabling pre-emptive blocking despite lack of authentication" << std::endl;
#endif
                        clientuser = "-";
                        filtergroup = 0; //default group - one day configurable?
                    } else {
#ifdef DGDEBUG
                        std::cout << dbgPeerPort << " -Identity found; caching username & group" << std::endl;
#endif
                        if (auth_plugin->is_connection_based && !overide_persist) {
#ifdef DGDEBUG
                            std::cout << "Auth plugin is for a connection-based auth method - keeping credentials for entire connection" << std::endl;
#endif
                            persistent_authed = true;
                        }
                        oldclientuser = clientuser;
                        oldfg = filtergroup;
                    }
                } else {
// We don't have any auth plugins loaded
#ifdef DGDEBUG
                    std::cout << dbgPeerPort << " -No auth plugins loaded; using defaults & feigning persistency" << std::endl;
#endif
                    authed = true;
                    clientuser = "-";
                    filtergroup = 0;
                    persistent_authed = true;
                }
            } else {
// persistent_authed == true
#ifdef DGDEBUG
                std::cout << dbgPeerPort << " -Already got credentials for this connection - not querying auth plugins" << std::endl;
#endif
                authed = true;
            }

#ifdef DGDEBUG
            std::cout << dbgPeerPort << " -username: " << clientuser << std::endl;
            std::cout << dbgPeerPort << " -filtergroup: " << filtergroup << std::endl;
#endif
//
//
// End of Authentication Checking
//
//

#ifdef __SSLMITM
            //			Set if candidate for MITM
            //			(Exceptions will not go MITM)
            checkme.ismitmcandidate = checkme.is_ssl && ldl->fg[filtergroup]->ssl_mitm && (header.port == 443);
#endif

            //
            //
            // Now check if user or machine is banned and room-based checking
            //
            //
            // filter group modes are: 0 = banned, 1 = filtered, 2 = exception.
            // is this user banned?
            isbanneduser = false;
            if (o.use_xforwardedfor) {
                bool use_xforwardedfor;
                if (o.xforwardedfor_filter_ip.size() > 0) {
                    use_xforwardedfor = false;
                    for (unsigned int i = 0; i < o.xforwardedfor_filter_ip.size(); i++) {
                        if (strcmp(clientip.c_str(), o.xforwardedfor_filter_ip[i].c_str()) == 0) {
                            use_xforwardedfor = true;
                            break;
                        }
                    }
                } else {
                    use_xforwardedfor = true;
                }
                if (use_xforwardedfor == 1) {
                    std::string xforwardip(header.getXForwardedForIP());
                    if (xforwardip.length() > 6) {
                        clientip = xforwardip;
                    }
#ifdef DGDEBUG
                    std::cout << dbgPeerPort << " -using x-forwardedfor:" << clientip << std::endl;
#endif
                }
            }

            // is this machine banned?
            bool isbannedip = ldl->inBannedIPList(&clientip, clienthost);
            bool part_banned;
            if (isbannedip)
                matchedip = clienthost == NULL;
            else {
                if (ldl->inRoom(clientip, room, clienthost, &isbannedip, &part_banned, &checkme.isexception, checkme.urld)) {
#ifdef DGDEBUG
                    std::cout << " isbannedip = " << isbannedip << "ispart_banned = " << part_banned << " isexception = " << checkme.isexception << std::endl;
#endif
                    if (isbannedip) {
                        matchedip = clienthost == NULL;
                    }
                    if (checkme.isexception) {
                        // do reason codes etc
                        checkme.exceptionreason = o.language_list.getTranslation(630);
                        checkme.exceptionreason.append(room);
                        checkme.exceptionreason.append(o.language_list.getTranslation(631));
                        checkme.message_no = 632;
                    }
                }
            }

//            if (o.forwarded_for) {
//                header.addXForwardedFor(clientip); // add squid-like entry
//            }

#ifdef ENABLE_ORIG_IP
            // if working in transparent mode and grabbing of original IP addresses is
            // enabled, does the original IP address match one of those that the host
            // we are going to resolves to?
            // Resolves http://www.kb.cert.org/vuls/id/435052
            if (o.get_orig_ip) {
                // XXX This will currently only work on Linux/Netfilter.
                sockaddr_in origaddr;
                socklen_t origaddrlen(sizeof(sockaddr_in));
                // Note: we assume that for non-redirected connections, this getsockopt call will
                // return the proxy server's IP, and not -1.  Hence, comparing the result with
                // the return value of Socket::getLocalIP() should tell us that the client didn't
                // connect transparently, and we can assume they aren't vulnerable.
                if (getsockopt(peerconn.getFD(), SOL_IP, SO_ORIGINAL_DST, &origaddr, &origaddrlen) < 0) {
                    syslog(LOG_ERR, "Failed to get client's original destination IP: %s", strerror(errno));
                    break;
                }

                std::string orig_dest_ip(inet_ntoa(origaddr.sin_addr));
                if (orig_dest_ip == peerconn.getLocalIP()) {
// The destination IP before redirection is the same as the IP the
// client has actually been connected to - they aren't connecting transparently.
#ifdef DGDEBUG
                    std::cout << dbgPeerPort << " -SO_ORIGINAL_DST and getLocalIP are equal; client not connected transparently" << std::endl;
#endif
                } else {
                    // Look up domain from request URL, and check orig IP against resolved IPs
                    addrinfo hints;
                    memset(&hints, 0, sizeof(hints));
                    hints.ai_family = AF_INET;
                    hints.ai_socktype = SOCK_STREAM;
                    hints.ai_protocol = IPPROTO_TCP;
                    addrinfo *results;
                    int result = getaddrinfo(checkme.urldomain.c_str(), NULL, &hints, &results);
                    if (result) {
                        freeaddrinfo(results);
                        syslog(LOG_ERR, "Cannot resolve hostname for host header checks: %s", gai_strerror(errno));
                        break;
                    }
                    addrinfo *current = results;
                    bool matched = false;
                    while (current != NULL) {
                        if (orig_dest_ip == inet_ntoa(((sockaddr_in *)(current->ai_addr))->sin_addr)) {
#ifdef DGDEBUG
                            std::cout << dbgPeerPort << checkme.urldomain << " matched to original destination of " << orig_dest_ip << std::endl;
#endif
                            matched = true;
                            break;
                        }
                        current = current->ai_next;
                    }
                    freeaddrinfo(results);
                    if (!matched) {
// Host header/URL said one thing, but the original destination IP said another.
// This is exactly the vulnerability we want to prevent.
#ifdef DGDEBUG
                        std::cout << dbgPeerPort << checkme.urldomain << " DID NOT MATCH original destination of " << orig_dest_ip << std::endl;
#endif
                        syslog(LOG_ERR, "Destination host of %s did not match the original destination IP of %s", checkme.urldomain.c_str(), orig_dest_ip.c_str());
                            // writestring throws exception on error/timeout
                            peerconn.writeString("HTTP/1.0 400 Bad Request\nContent-Type: text/html\n\n");
                            peerconn.writeString("<HTML><HEAD><TITLE>e2guardian - 400 Bad Request</TITLE></HEAD><BODY><H1>e2guardian - 400 Bad Request</H1>");

                            // The requested URL is malformed.
                            peerconn.writeString(o.language_list.getTranslation(200));
                            peerconn.writeString("</BODY></HTML>\n");
                        break;
                    }
                }
            }
#endif

            //
            // Start of by pass
            //

            if (header.isScanBypassURL(&checkme.logurl, ldl->fg[filtergroup]->magic.c_str(), clientip.c_str())) {
#ifdef DGDEBUG
                std::cout << dbgPeerPort << " -Scan Bypass URL match" << std::endl;
#endif
                checkme.isscanbypass = true;
                checkme.isbypass = true;
                checkme.exceptionreason = o.language_list.getTranslation(608);
            } else if ((ldl->fg[filtergroup]->bypass_mode != 0) || (ldl->fg[filtergroup]->infection_bypass_mode != 0)) {
#ifdef DGDEBUG
                std::cout << dbgPeerPort << " -About to check for bypass..." << std::endl;
#endif
                if (ldl->fg[filtergroup]->bypass_mode != 0)
                    bypasstimestamp = header.isBypassURL(&checkme.logurl, ldl->fg[filtergroup]->magic.c_str(), clientip.c_str(), NULL);
                if ((bypasstimestamp == 0) && (ldl->fg[filtergroup]->infection_bypass_mode != 0))
                    bypasstimestamp = header.isBypassURL(&checkme.logurl, ldl->fg[filtergroup]->imagic.c_str(), clientip.c_str(), &checkme.isvirusbypass);
                if (bypasstimestamp > 0) {
#ifdef DGDEBUG
                    if (checkme.isvirusbypass)
                        std::cout << dbgPeerPort << " -Infection bypass URL match" << std::endl;
                    else
                        std::cout << dbgPeerPort << " -Filter bypass URL match" << std::endl;
#endif
                    header.chopBypass(checkme.logurl, checkme.isvirusbypass);
                    if (bypasstimestamp > 1) { // not expired
                        checkme.isbypass = true;
                        // checkme: need a TR string for virus bypass
                        checkme.exceptionreason = o.language_list.getTranslation(606);
                    }
                } else if (ldl->fg[filtergroup]->bypass_mode != 0) {
                    if (header.isBypassCookie(checkme.urldomain, ldl->fg[filtergroup]->cookie_magic.c_str(), clientip.c_str())) {
#ifdef DGDEBUG
                        std::cout << dbgPeerPort << " -Bypass cookie match" << std::endl;
#endif
                        checkme.iscookiebypass = true;
                        checkme.isbypass = true;
                        checkme.isexception = true;
                        checkme.exceptionreason = o.language_list.getTranslation(607);
                    }
                }
#ifdef DGDEBUG
                std::cout << dbgPeerPort << " -Finished bypass checks." << std::endl;
#endif
            }

#ifdef DGDEBUG
            if (checkme.isbypass) {
                std::cout << dbgPeerPort << " -Bypass activated!" << std::endl;
            }
#endif
            //
            // End of bypass
            //
            // Start of scan by pass
            //

            if (checkme.isscanbypass) {
                //we need to decode the URL and send the temp file with the
                //correct header to the client then delete the temp file
                String tempfilename(checkme.url.after("GSBYPASS=").after("&N="));
                String tempfilemime(tempfilename.after("&M="));
                String tempfiledis(header.decode(tempfilemime.after("&D="), true));
#ifdef DGDEBUG
                std::cout << dbgPeerPort << " -Original filename: " << tempfiledis << std::endl;
#endif
                String rtype(header.requestType());
                tempfilemime = tempfilemime.before("&D=");
                tempfilename = o.download_dir + "/tf" + tempfilename.before("&M=");
                try {
                    checkme.docsize = sendFile(&peerconn, tempfilename, tempfilemime, tempfiledis, checkme.url);
                    header.chopScanBypass(checkme.url);
                    checkme.url = header.getLogUrl();
                    //urld = header.decode(url);  // unneeded really

                    doLog(clientuser, clientip, checkme.logurl, header.port, checkme.exceptionreason,
                        rtype, checkme.docsize, NULL, false, 0, checkme.isexception, false, &thestart,
                        checkme.cachehit, 200, checkme.mimetype, checkme.wasinfected, checkme.wasscanned, 0, filtergroup,
                        &header);

                    if (o.delete_downloaded_temp_files) {
                        unlink(tempfilename.toCharArray());
                    }
                } catch (std::exception &e) {
                }
                persistProxy = false;
                proxysock.close(); // close connection to proxy
                break;
            }
            //
            // End of scan by pass
            //

            char *retchar;

            //
            // Start of exception checking
            //
            // being a banned user/IP overrides the fact that a site may be in the exception lists
            // needn't check these lists in bypass modes
            if (!(isbanneduser || isbannedip || checkme.isbypass || checkme.isexception)) {
                //bool is_ssl = header.requestType() == "CONNECT";
                bool is_ip = isIPHostnameStrip(checkme.urld);
                    // Exception client user match.
                if (ldl->inExceptionIPList(&clientip, clienthost)) { // admin pc
                    matchedip = clienthost == NULL;
                    checkme.isexception = true;
                    checkme.exceptionreason = o.language_list.getTranslation(600);
                    // Exception client IP match.
                }

// Replace this with storyboard call - local exception checks
                String funct = "checkrequest";
                ldl->fg[filtergroup]->StoryB.runFunct( funct, checkme);
                std::cerr << "After StoryB checkrequest " << checkme.isexception << " mess_no " << checkme.message_no   << std::endl;
            }

// Replace this with storyboard call - local requestLocalChecks

            // orginal section only now called if local list not matched

// Replace this with storyboard call - main exception checks


//
// End of main exception checking
//

#ifdef DGDEBUG
            std::cout << dbgPeerPort << " -extracted url:" << checkme.urld << std::endl;
#endif

            // don't run willScanRequest if content scanning is disabled, or on exceptions if contentscanexceptions is off,
            // or on SSL (CONNECT) requests, or on HEAD requests, or if in AV bypass mode

            // POST scanning code
            // Query request and response scanners to see which is interested in scanning data for this request
            // TODO - Should probably block if willScanRequest returns error
            bool multipart = false;
            if (!isbannedip && !isbanneduser && !checkme.isconnect && !checkme.ishead
                && (ldl->fg[filtergroup]->disable_content_scan != 1)
                && !(checkme.isexception && !o.content_scan_exceptions)) {
                for (std::deque<Plugin *>::iterator i = o.csplugins_begin; i != o.csplugins_end; ++i) {
                    int csrc = ((CSPlugin *)(*i))->willScanRequest(header.getUrl(), clientuser.c_str(), ldl->fg[filtergroup], clientip.c_str(), false, false, checkme.isexception, checkme.isbypass);
                    if (csrc > 0)
                        responsescanners.push_back((CSPlugin *)(*i));
                    else if (csrc < 0)
                        syslog(LOG_ERR, "willScanRequest returned error: %d", csrc);
                }
#ifdef DGDEBUG
                std::cout << dbgPeerPort << " -Content scanners interested in response data: " << responsescanners.size() << std::endl;
#endif

                // Only query scanners regarding outgoing data if we are actually sending data in the request
                if (header.contentLength() > 0) {
                    // POST data log entry - fill in for single-part posts,
                    // and fill in overall "guess" for multi-part posts;
                    // latter will be overwritten with more detail about
                    // individual parts, if part-by-part filtering occurs.
                    String mtype(header.getContentType());
                    postparts.push_back(postinfo());
                    postparts.back().mimetype.assign(mtype);
                    postparts.back().size = header.contentLength();

                    if (mtype == "application/x-www-form-urlencoded" || (multipart = (mtype == "multipart/form-data"))) {
                        // Don't bother if it's a single part POST and is above max_content_ramcache_scan_size
                        if (!multipart && header.contentLength() > o.max_content_ramcache_scan_size) {
#ifdef DGDEBUG
                            std::cout << dbgPeerPort << " -Not running willScanRequest for POST data: single-part POST with content length above size limit" << std::endl;
#endif
                        } else {
                            for (std::deque<Plugin *>::iterator i = o.csplugins_begin; i != o.csplugins_end; ++i) {
                                int csrc = ((CSPlugin *)(*i))->willScanRequest(header.getUrl(), clientuser.c_str(), ldl->fg[filtergroup], clientip.c_str(), true, !multipart, checkme.isexception, checkme.isbypass);
                                if (csrc > 0)
                                    requestscanners.push_back((CSPlugin *)(*i));
                                else if (csrc < 0)
                                    syslog(LOG_ERR, "willScanRequest returned error: %d", csrc);
                            }
                        }
#ifdef DGDEBUG
                        std::cout << dbgPeerPort << " -Content scanners interested in request data: " << requestscanners.size() << std::endl;
#endif
                    }
                }
            }
            // END of POST scanning code

            if (((checkme.isexception || checkme.iscookiebypass || checkme.isvirusbypass)
                    // don't filter exception and local web server
                    // Cookie bypass so don't need to add cookie so just CONNECT (unless should content scan)
                    && !isbannedip // bad users pc
                    && !isbanneduser // bad user
                    && requestscanners.empty() && responsescanners.empty()) // doesn't need content scanning
                // bad people still need to be able to access the banned page
                || checkme.isourwebserver) {
                if(!proxysock.breadyForOutput(o.proxy_timeout))
                    cleanThrow("Unable to write to proxy",peerconn, proxysock);
#ifdef DGDEBUG
                std::cerr << dbgPeerPort << "  got past line 1257 rfo " << std::endl;
#endif

                if(!( header.out(&peerconn, &proxysock, __DGHEADER_SENDALL, true) // send proxy the request
                    && (docheader.in(&proxysock, persistOutgoing)) )) {
                    if (proxysock.isTimedout()) {
                        checkme.message_no = 203;
                        peerconn.writeString("HTTP/1.0 504 Gateway Time-out\nContent-Type: text/html\n\n");
                        peerconn.writeString(
                                "<HTML><HEAD><TITLE>e2guardian - 504 Gateway Time-out</TITLE></HEAD><BODY><H1>e2guardian - 504 Gateway Time-out</H1>");
                        peerconn.writeString(o.language_list.getTranslation(203));
                        peerconn.writeString(o.language_list.getTranslation(204));
                        peerconn.writeString("</BODY></HTML>\n");
                        break;
                    } else {
                        checkme.message_no = 205;
                        peerconn.writeString("HTTP/1.0 502 Gateway Error\nContent-Type: text/html\n\n");
                        peerconn.writeString(
                                "<HTML><HEAD><TITLE>e2guardian - 502 Gateway Error</TITLE></HEAD><BODY><H1>e2guardian - 502 Gateway Error</H1>");
                        peerconn.writeString(o.language_list.getTranslation(205));
                        peerconn.writeString(o.language_list.getTranslation(206));
                        peerconn.writeString("</BODY></HTML>\n");
                        break;
                //        cleanThrow("Unable to read header from proxy", peerconn, proxysock);
                    }
                }
                persistProxy = docheader.isPersistent();
                persistPeer = persistOutgoing && docheader.wasPersistent();
#ifdef DGDEBUG
                std::cout << dbgPeerPort << " -persistPeer: " << persistPeer << std::endl;
                docheader.dbshowheader(&checkme.logurl, clientip.c_str());
#endif
                if(!docheader.out(NULL, &peerconn, __DGHEADER_SENDALL))
                      cleanThrow("Unable to send return header to client",peerconn, proxysock);
                // only open a two-way tunnel on CONNECT if the return code indicates success
                if (!(docheader.returnCode() == 200)) {
                    checkme.isconnect = false;
                }

                if (checkme.isconnect) {
                    persistProxy = false;
                    persistOutgoing = false;
                    persistPeer = false;
                }

                //try
                    fdt.reset(); // make a tunnel object
                    // tunnel from client to proxy and back
                    // two-way if SSL
                //syslog(LOG_INFO, "before tunnel 1324");
                     if(!fdt.tunnel(proxysock, peerconn, checkme.isconnect, docheader.contentLength(), true))
                         persistProxy = false;
//                         cleanThrow("Error Connect tunnel", peerconn,proxysock);
                //syslog(LOG_INFO, "after tunnel 1327");
                    checkme.docsize = fdt.throughput;
                try {
                    if (!checkme.isourwebserver) { // don't log requests to the web server
                        String rtype(header.requestType());
                        doLog(clientuser, clientip, checkme.logurl, header.port, checkme.exceptionreason, rtype, checkme.docsize, (checkme.exceptioncat.length() ? &checkme.exceptioncat : NULL), false, 0, checkme.isexception,
                            false, &thestart, checkme.cachehit, ((!checkme.isconnect && persistPeer) ? docheader.returnCode() : 200),
                            checkme.mimetype, checkme.wasinfected, checkme.wasscanned, 0, filtergroup, &header, checkme.message_no);
                    }
                } catch (std::exception &e) {
                }

                if (!persistProxy)
                    proxysock.close(); // close connection to proxy

                if (persistPeer)
                    continue;

                break;
            }

            // URL regexp search and redirect
            if (!checkme.is_ssl)
                checkme.urlredirect = header.urlRedirectRegExp(ldl->fg[filtergroup]);
            if (checkme.urlredirect) {
                checkme.url = header.redirecturl();
#ifdef DGDEBUG
        //        std::cout << "urlRedirectRegExp told us to redirect client to \"" << url << std::endl;
#endif
                proxysock.close();
                String writestring("HTTP/1.0 302 Redirect\nLocation: ");
                writestring += checkme.url;
                writestring += "\n\n";
                peerconn.writeString(writestring.toCharArray());
                break;
            }

            if (!checkme.is_ssl)
                checkme.headeradded = header.isHeaderAdded(ldl->fg[filtergroup]);

            // URL regexp search and replace
            checkme.urlmodified = header.urlRegExp(ldl->fg[filtergroup]);
            if (checkme.urlmodified) {
                checkme.url = header.getUrl();
                checkme.urld = header.decode(checkme.url);
                checkme.urldomain = checkme.url.getHostname();

                // if the user wants, re-check the exception site, URL and regex lists after modification.
                // this allows you to, for example, force safe search on Google URLs, then flag the
                // request as an exception, to prevent questionable language in returned site summaries
                // from blocking the entire request.
                // this could be achieved with exception phrases (which are, of course, always checked
                // after the URL) too, but there are cases for both, and flexibility is good.
                if (o.recheck_replaced_urls && !(isbanneduser || isbannedip)) {
                    //bool is_ssl = header.requestType() == "CONNECT";
                    bool is_ip = isIPHostnameStrip(checkme.urld);
                    if (ldl->fg[filtergroup]->inExceptionSiteList(checkme.urld, true, is_ip, checkme.is_ssl, lastcategory)) { // allowed site
                        if (ldl->fg[0]->isOurWebserver(checkme.url)) {
                            checkme.isourwebserver = true;
                        } else {
                            checkme.isexception = true;
                            checkme.exceptionreason = o.language_list.getTranslation(602);
                            checkme.message_no = 602;
                            // Exception site match.
                            checkme.exceptioncat = lastcategory.toCharArray();
                        }
                    } else if (ldl->fg[filtergroup]->inExceptionURLList(checkme.urld, true, is_ip, checkme.is_ssl, lastcategory)) { // allowed url
                        checkme.isexception = true;
                        checkme.exceptionreason = o.language_list.getTranslation(603);
                        checkme.message_no = 603;
                        // Exception url match.
                        checkme.exceptioncat = lastcategory.toCharArray();
                    } else if ((rc = ldl->fg[filtergroup]->inExceptionRegExpURLList(checkme.urld, lastcategory)) > -1) {
                        checkme.isexception = true;
                        // exception regular expression url match:
                        checkme.exceptionreason = o.language_list.getTranslation(609);
                        checkme.message_no = 609;
                        checkme.exceptionreason += ldl->fg[filtergroup]->exception_regexpurl_list_source[rc].toCharArray();
                        checkme.exceptioncat = lastcategory.toCharArray();
                    }
                    // don't filter exception and local web server
                    if ((checkme.isexception
                            // even after regex URL replacement, we still don't want banned IPs/users viewing exception sites
                            && !isbannedip // bad users pc
                            && !isbanneduser // bad user
                            && requestscanners.empty() && responsescanners.empty())
                        || checkme.isourwebserver) {
                        if(!proxysock.breadyForOutput(o.proxy_timeout))
                            cleanThrow("Unable to write to proxy 1415",peerconn, proxysock);
#ifdef DGDEBUG
                        std::cerr << dbgPeerPort << "  got past line 1391 rfo " << std::endl;
#endif
                        if(!header.out(&peerconn, &proxysock, __DGHEADER_SENDALL, true)) // send proxy the request
                            cleanThrow("Unable to write header to proxy",peerconn, proxysock);
                        if(!docheader.in(&proxysock, persistOutgoing))
                            cleanThrow("Unable to read header from proxy",peerconn, proxysock);
                        persistProxy = docheader.isPersistent();
                        persistPeer = persistOutgoing && docheader.wasPersistent();
#ifdef DGDEBUG
                        std::cout << dbgPeerPort << " -persistPeer: " << persistPeer << std::endl;
#endif
                        if(!docheader.out(NULL, &peerconn, __DGHEADER_SENDALL))
                            cleanThrow("Unable to send return header to client",peerconn, proxysock);

                        // only open a two-way tunnel on CONNECT if the return code indicates success
                        if (!(docheader.returnCode() == 200)) {
                            checkme.isconnect = false;
                        }
                        if (checkme.isconnect) {
                            persistProxy = false;
                            persistOutgoing = false;
                            persistPeer = false;
                        }
                        //try
                            fdt.reset(); // make a tunnel object
                            // tunnel from client to proxy and back
                            // two-way if SSL
                        if(!fdt.tunnel(proxysock, peerconn, checkme.isconnect, docheader.contentLength(), true) )
                            persistProxy = false;
//                            cleanThrow("Error Connect tunnel", peerconn,proxysock);
                            checkme.docsize = fdt.throughput;
                        try {
                            if (!checkme.isourwebserver) { // don't log requests to the web server
                                String rtype(header.requestType());
                                doLog(clientuser, clientip, checkme.logurl, header.port, checkme.exceptionreason, rtype, checkme.docsize, (checkme.exceptioncat.length() ? &checkme.exceptioncat : NULL),
                                    false, 0, checkme.isexception, false, &thestart, checkme.cachehit, ((!checkme.isconnect && persistPeer) ? docheader.returnCode() : 200),
                                    checkme.mimetype, checkme.wasinfected, checkme.wasscanned, checkme.naughtiness, filtergroup, &header, checkme.message_no,
                                    // content wasn't modified, but URL was
                                    false, true, checkme.headermodified, checkme.headeradded);
                            }

                        } catch (std::exception &e) {
                        }
                        if (!persistProxy)
                            proxysock.close(); // close connection to proxy

                        if (persistPeer && proxysock.readyForOutput())
                            continue;
                        proxysock.close();
                        break;
                    }
                }
            }

            // Outgoing header modifications
            checkme.headermodified = header.headerRegExp(ldl->fg[filtergroup]);

            // if o.content_scan_exceptions is on then exceptions have to
            // pass on until later for AV scanning too.
            // Bloody annoying feature that adds mess and complexity to the code
            if (checkme.isexception) {
                checkme.isException = true;
                checkme.whatIsNaughtyLog = checkme.exceptionreason;
                checkme.whatIsNaughtyCategories = checkme.exceptioncat;
            }

            if (checkme.isconnect && !checkme.isbypass && !checkme.isexception) {
                if (!authed) {
#ifdef DGDEBUG
                    std::cout << dbgPeerPort << " -CONNECT: user not authed - getting response to see if it's auth required" << std::endl;
#endif
                    // send header to proxy
                    if(!proxysock.breadyForOutput(o.proxy_timeout))
                        cleanThrow("Unable to send header to proxy 1439",peerconn, proxysock);


                    //proxysock.readyForOutput(o.proxy_timeout);
#ifdef DGDEBUG
                    std::cerr << dbgPeerPort << "  got past line 1465 rfo " << std::endl;
#endif
                    if(!header.out(NULL, &proxysock, __DGHEADER_SENDALL, true))
                    cleanThrow("Unable to send header to proxy 1524",peerconn, proxysock);

                    // get header from proxy
                    if(!proxysock.bcheckForInput(o.exchange_timeout))
                        cleanThrow("Unable to get header from proxy 1527",peerconn, proxysock);
                    if(!docheader.in(&proxysock, persistOutgoing))
                        cleanThrow("Unable to getr return header from proxy 1530",peerconn, proxysock);
                    persistProxy = docheader.isPersistent();
                    persistPeer = persistOutgoing && docheader.wasPersistent();
#ifdef DGDEBUG
                    std::cout << dbgPeerPort << " -persistPeer: " << persistPeer << std::endl;
#endif
                    checkme.wasrequested = true;

                    if (docheader.returnCode() != 200) {
#ifdef DGDEBUG
                        std::cout << dbgPeerPort << " -CONNECT: user not authed - doing standard filtering on auth required response" << std::endl;
#endif
                        checkme.isconnect = false;
                    }
                }
#ifdef DGDEBUG
                std::cout << dbgPeerPort << "isconnect=" << checkme.isconnect << " ismitmcandidate=" << checkme.ismitmcandidate << " only_mitm_ssl_grey=" << ldl->fg[filtergroup]->only_mitm_ssl_grey << std::endl;
#endif

                if (checkme.isconnect && ((!checkme.ismitmcandidate) || ldl->fg[filtergroup]->only_mitm_ssl_grey)) {
                    persistProxy = false;
                    persistPeer = false;
                    persistOutgoing = false;
#ifdef DGDEBUG
                    std::cout << dbgPeerPort << " -CONNECT: user is authed/auth not required - attempting pre-emptive ban" << std::endl;
#endif
                    // if its a connect and we don't do filtering on it now then
                    // it will get tunneled and not filtered.  We can't tunnel later
                    // as its ssl so we can't see the return header etc
                    // So preemptive banning is forced on with ssl unfortunately.
                    // It is unlikely to cause many problems though.
                    requestChecks(&header, &checkme, &checkme.urld, &checkme.url, &clientip, &clientuser, filtergroup, isbanneduser, isbannedip, room);
                    checkme.message_no = checkme.message_no;
#ifdef DGDEBUG
                    std::cout << dbgPeerPort << " -done checking" << std::endl;
#endif
                }
            }

#ifdef __SSLMITM
            //https mitm but only on port 443
            if (ldl->fg[filtergroup]->only_mitm_ssl_grey && !checkme.isSSLGrey)
                checkme.ismitmcandidate = false;
            if (!checkme.isexception && checkme.ismitmcandidate) {
#ifdef DGDEBUG
                std::cout << dbgPeerPort << " -Intercepting HTTPS connection" << std::endl;
#endif

                //Do the connect request
                if (!checkme.isItNaughty && !checkme.wasrequested) {
#ifdef DGDEBUG
                    std::cout << dbgPeerPort << " -Forwarding connect request" << std::endl;
#endif
                    if(!proxysock.breadyForOutput(o.proxy_timeout))
                        cleanThrow("Unable to send header to proxy 1584",peerconn, proxysock);
                    if (checkme.isconnect)
                        header.sslsiteRegExp(ldl->fg[filtergroup]);
                    if(!header.out(NULL, &proxysock, __DGHEADER_SENDALL, true)) // send proxy the request
                        cleanThrow("Unable to send header to proxy 1586",peerconn, proxysock);
                    //check the response headers so we can go ssl
                    if(!proxysock.bcheckForInput(120000))
                        cleanThrow("Unable to get response header from proxy 1589",peerconn, proxysock);
                    if(!docheader.in(&proxysock, persistOutgoing))
                        cleanThrow("Unable to get response header from proxy 1591",peerconn, proxysock);
                    persistProxy = docheader.isPersistent();
                    persistPeer = persistOutgoing && docheader.wasPersistent();
#ifdef DGDEBUG
                    std::cout << dbgPeerPort << " -persistPeer: " << persistPeer << std::endl;
#endif
                }

                //take care of connect fails / proxy auth requests
                if (!checkme.isItNaughty && !(docheader.returnCode() == 200)) {
#ifdef DGDEBUG
                    std::cout << dbgPeerPort << " -Connect request failed / proxy auth required. Returning data to client" << std::endl;
#endif
                    //send data to the client and let it deal with it
                    if(!docheader.out(NULL, &peerconn, __DGHEADER_SENDALL, true))
                        cleanThrow("Unable to send response header to client 1606",peerconn, proxysock);

#ifdef DGDEBUG
                        std::cout << dbgPeerPort << " -Forwarding body to client" << std::endl;
#endif
                        fdt.reset(); // make a tunnel object
                        // tunnel from proxy to client
                        if(!fdt.tunnel(proxysock, peerconn, checkme.isconnect, docheader.contentLength(), true) )
                            persistProxy = false;
//                            cleanThrow("Error forwarding body to client", peerconn,proxysock);
                        checkme.docsize = fdt.throughput;
                    try {
                        String rtype(header.requestType());
                        doLog(clientuser, clientip, checkme.logurl, header.port, checkme.exceptionreason, rtype, checkme.docsize, &checkme.whatIsNaughtyCategories, false, 0,
                            checkme.isexception, false, &thestart,
                            checkme.cachehit, header.returnCode(), checkme.mimetype, checkme.wasinfected,
                            checkme.wasscanned, checkme.naughtiness, filtergroup, &header, checkme.message_no, false, checkme.urlmodified, checkme.headermodified, checkme.headeradded);

                    } catch (std::exception &e) {
                    }
                        if (!persistProxy)
                            proxysock.close(); // close connection to proxy

                    if (persistPeer) {
                        continue;
                    }

                    break;
                }

                //  CA intialisation now Moved into OptionContainer so now done once on start-up
                //  instead off on every request

                X509 *cert = NULL;
                struct ca_serial caser;
                EVP_PKEY *pkey = NULL;
                bool certfromcache = false;
                //generate the cert
                if (!checkme.isItNaughty) {
#ifdef DGDEBUG
                    std::cout << dbgPeerPort << " -Getting ssl certificate for client connection" << std::endl;
#endif

                    pkey = o.ca->getServerPkey();

                    //generate the certificate but dont write it to disk (avoid someone
                    //requesting lots of places that dont exist causing the disk to fill
                    //up / run out of inodes
                    certfromcache = o.ca->getServerCertificate(checkme.urldomain.CN().c_str(), &cert,
                        &caser);
#ifdef DGDEBUG
                    if (caser.asn == NULL) {
                        std::cout << "caser.asn is NULL" << std::endl;
                    }
//				std::cout << "serials are: " << (char) *caser.asn << " " < caser.charhex  << std::endl;
#endif

                    //check that the generated cert is not null and fillin checkme if it is
                    if (cert == NULL) {
                        checkme.isItNaughty = true;
                        //checkme.whatIsNaughty = "Failed to get ssl certificate";
                        checkme.message_no = 151;
                        checkme.whatIsNaughty = o.language_list.getTranslation(151);
                        checkme.whatIsNaughtyLog = checkme.whatIsNaughty;
                        checkme.whatIsNaughtyCategories = o.language_list.getTranslation(70);
                    } else if (pkey == NULL) {
                        checkme.isItNaughty = true;
                        //checkme.whatIsNaughty = "Failed to load ssl private key";
                        checkme.message_no = 153;
                        checkme.whatIsNaughty = o.language_list.getTranslation(153);
                        checkme.whatIsNaughtyLog = checkme.whatIsNaughty;
                        checkme.whatIsNaughtyCategories = o.language_list.getTranslation(70);
                    }
                }

                //startsslserver on the connection to the client
                if (!checkme.isItNaughty) {
#ifdef DGDEBUG
                    std::cout << dbgPeerPort << " -Going SSL on the peer connection" << std::endl;
#endif
                    //send a 200 to the client no matter what because they managed to get a connection to us
                    //and we can use it for a blockpage if nothing else
                    std::string msg = "HTTP/1.0 200 Connection established\r\n\r\n";
                    if(!peerconn.writeString(msg.c_str()))
                        cleanThrow("Unable to send to client 1670",peerconn,proxysock);

                    if (peerconn.startSslServer(cert, pkey, o.set_cipher_list) < 0) {
                        //make sure the ssl stuff is shutdown properly so we display the old ssl blockpage
                        peerconn.stopSsl();

                        checkme.isItNaughty = true;
                        //checkme.whatIsNaughty = "Failed to negotiate ssl connection to client";
                        checkme.message_no = 154;
                        checkme.whatIsNaughty = o.language_list.getTranslation(154);
                        checkme.whatIsNaughtyLog = checkme.whatIsNaughty;
                        checkme.whatIsNaughtyCategories = o.language_list.getTranslation(70);
                    }
                }

                //startsslclient connected to the proxy and check the certificate of the server
                bool badcert = false;
                if (!checkme.isItNaughty) {
#ifdef DGDEBUG
                    std::cout << dbgPeerPort << " -Going SSL on connection to proxy" << std::endl;
#endif
                    std::string certpath = std::string(o.ssl_certificate_path);
                    if (proxysock.startSslClient(certpath,checkme.urldomain)) {
                        //make sure the ssl stuff is shutdown properly so we display the old ssl blockpage
                    //    proxysock.stopSsl();

                        checkme.isItNaughty = true;
                        //checkme.whatIsNaughty = "Failed to negotiate ssl connection to server";
                        checkme.message_no = 160;
                        checkme.whatIsNaughty = o.language_list.getTranslation(160);
                        checkme.whatIsNaughtyLog = checkme.whatIsNaughty;
                        checkme.whatIsNaughtyCategories = o.language_list.getTranslation(70);
                    }
		}

                if (!checkme.isItNaughty) {

#ifdef DGDEBUG
                    std::cout << dbgPeerPort << " -Checking certificate" << std::endl;
#endif
                    //will fill in checkme of its own accord
                    if (ldl->fg[filtergroup]->mitm_check_cert && !ldl->fg[filtergroup]->inNoCheckCertSiteList(checkme.urldomain, false)) {
                        checkCertificate(checkme.urldomain, &proxysock, &checkme);
                        badcert = checkme.isItNaughty;
                    }
                }

                //handleConnection inside the ssl tunnel
                if (!checkme.isItNaughty) {
                    bool writecert = true;
                    if (!certfromcache) {
                        writecert = o.ca->writeCertificate(checkme.urldomain.c_str(), cert,
                            &caser);
                    }

                    //if we cant write the certificate its not the end of the world but it is slow
                    if (!writecert) {
#ifdef DGDEBUG
                        std::cout << dbgPeerPort << " -Couldn't save certificate to on disk cache" << std::endl;
#endif
                        syslog(LOG_ERR, "Couldn't save certificate to on disk cache");
                    }
#ifdef DGDEBUG
                    std::cout << dbgPeerPort << " -Handling connections inside ssl tunnel" << std::endl;
#endif

                    if (authed) {
                        persistent_authed = true;
                    }

                    handleConnection(peerconn, ip, true, proxysock, dystat);
#ifdef DGDEBUG
                    std::cout << dbgPeerPort << " -Handling connections inside ssl tunnel: done" << std::endl;
#endif
                }
                o.ca->free_ca_serial(&caser);

                //stopssl on the proxy connection
                //if it was marked as naughty then show a deny page and close the connection
                if (checkme.isItNaughty) {
#ifdef DGDEBUG
                    std::cout << dbgPeerPort << " -SSL Interception failed " << checkme.whatIsNaughty << std::endl;
#endif

                    String rtype(header.requestType());
                    doLog(clientuser, clientip, checkme.logurl, header.port, checkme.whatIsNaughtyLog, rtype, checkme.docsize, &checkme.whatIsNaughtyCategories, true, checkme.blocktype,
                        checkme.isexception, false, &thestart,
                        checkme.cachehit, (checkme.wasrequested ? docheader.returnCode() : 200), checkme.mimetype, checkme.wasinfected,
                        checkme.wasscanned, checkme.naughtiness, filtergroup, &header, checkme.message_no, false, checkme.urlmodified, checkme.headermodified, checkme.headeradded);

                    denyAccess(&peerconn, &proxysock, &header, &docheader, &checkme.logurl, &checkme, &clientuser,
                        &clientip, filtergroup, checkme.ispostblock, checkme.headersent, checkme.wasinfected, checkme.scanerror, badcert);
                }
#ifdef DGDEBUG
                std::cout << dbgPeerPort << " -Shutting down ssl to proxy" << std::endl;
#endif
                proxysock.stopSsl();

#ifdef DGDEBUG
                std::cout << dbgPeerPort << " -Shutting down ssl to client" << std::endl;
#endif

                peerconn.stopSsl();

                //tidy up key and cert
                X509_free(cert);
                EVP_PKEY_free(pkey);

                persistProxy = false;
                proxysock.close();
                break;
            }
//if not mitm then just do the tunneling
//else
//
#endif //__SSLMITM
            // Banned rewrite SSL denied page
            if ((checkme.is_ssl == true) && (checkme.isItNaughty == true) && (ldl->fg[filtergroup]->ssl_denied_rewrite == true)) {
                header.DenySSL(ldl->fg[filtergroup]);
                String rtype(header.requestType());
                doLog(clientuser, clientip, checkme.logurl, header.port, checkme.whatIsNaughtyLog, rtype, checkme.docsize, &checkme.whatIsNaughtyCategories, true, checkme.blocktype, checkme.isexception, false, &thestart, checkme.cachehit, (checkme.wasrequested ? docheader.returnCode() : 200), checkme.mimetype, checkme.wasinfected, checkme.wasscanned, checkme.naughtiness, filtergroup, &header, checkme.message_no, false, checkme.urlmodified, checkme.headermodified, checkme.headeradded);
                checkme.isItNaughty = false;
            }

            if (!checkme.isItNaughty && checkme.isconnect) {
                // can't filter content of CONNECT
                if (!checkme.wasrequested) {
                   if(! proxysock.breadyForOutput(o.proxy_timeout))
                    cleanThrow("Error sending header to proxy", peerconn,proxysock);
#ifdef DGDEBUG
                    std::cerr << dbgPeerPort << "  got past line 1759 rfo " << std::endl;
#endif
                    header.out(NULL, &proxysock, __DGHEADER_SENDALL, true); // send proxy the request
                } else {
                    docheader.out(NULL, &peerconn, __DGHEADER_SENDALL);
                }
#ifdef DGDEBUG
                    std::cout << dbgPeerPort << " -Opening tunnel for CONNECT" << std::endl;
#endif
                    fdt.reset(); // make a tunnel object
                    // tunnel from client to proxy and back - *true* two-way tunnel
                if(!fdt.tunnel(proxysock, peerconn, true) )
                    persistProxy = false;
                    //cleanThrow("Error from 2 way tunnel", peerconn,proxysock);
                    checkme.docsize = fdt.throughput;
                try {
                    String rtype(header.requestType());
                    doLog(clientuser, clientip, checkme.logurl, header.port, checkme.exceptionreason, rtype, checkme.docsize, &checkme.whatIsNaughtyCategories, false,
                        0, checkme.isexception, false, &thestart,
                        checkme.cachehit, (checkme.wasrequested ? docheader.returnCode() : 200), checkme.mimetype, checkme.wasinfected,
                        checkme.wasscanned, checkme.naughtiness, filtergroup, &header, checkme.message_no, false, checkme.urlmodified, checkme.headermodified, checkme.headeradded);

                } catch (std::exception &e) {
                }

                if (!persistProxy)
                    proxysock.close(); // close connection to proxy

                if (persistPeer)
                    continue;

                break;
            }

            // check header sent to proxy - this is done before the send, so that pre-emptive banning
            // can be used for authenticated users. this gets around the problem of Squid fetching content
            // from sites when they're just going to get banned: not too big an issue in most cases, but
            // not good if blocking sites it would be illegal to retrieve, and allows web bugs/tracking
            // links not to be requested.
            if (authed && !checkme.isbypass && !checkme.isexception && !checkme.isItNaughty) {
                requestChecks(&header, &checkme, &checkme.urld, &checkme.url, &clientip, &clientuser, filtergroup,
                    isbanneduser, isbannedip, room);
            }

            // TODO - This post code is too big
            // Filtering of POST data
            off_t cl = header.contentLength();
            if (authed && !checkme.isItNaughty && cl > 0) {
                // Check for POST upload size blocking, unless request is an exception
                // MIME type test is just an approximation, but probably good enough

                long max_upload_size;
                max_upload_size = (*ldl->fg[filtergroup]).max_upload_size;

#ifdef DGDEBUG
                std::cout << dbgPeerPort << " max upload size general: " << max_upload_size << " filtergroup " << filtergroup << ": " << (*ldl->fg[filtergroup]).max_upload_size << std::endl;

#endif
                if (!checkme.isbypass && !checkme.isexception
                    && ((max_upload_size >= 0) && (cl > max_upload_size))
                    && multipart) {
#ifdef DGDEBUG
                    std::cout << dbgPeerPort << " -Detected POST upload violation by Content-Length header - discarding rest of POST data..." << max_upload_size << std::endl;
#endif
                    header.discard(&peerconn);
                    checkme.whatIsNaughty = (*ldl->fg[filtergroup]).max_upload_size == 0 ? o.language_list.getTranslation(700) : o.language_list.getTranslation(701);
                    // Web upload is banned.
                    checkme.whatIsNaughtyLog = checkme.whatIsNaughty;
                    checkme.whatIsNaughtyCategories = "Web upload";
                    checkme.isItNaughty = true;
                    checkme.ispostblock = true;
                } else if (!requestscanners.empty()) {
                    // POST scanning by content scanning plugins
                    if (multipart) {
#ifdef DGDEBUG
                        std::cout << dbgPeerPort << " -Filtering multi-part POST data" << std::endl;
#endif
                        // multi-part POST, possibly including file upload
                        // retrieve each part in turn and filter it on the fly

                        // network retrieval buffer
                        char buffer[2048];
                        size_t bytes_remaining = cl;

                        // determine boundary between MIME parts
                        // limit boundary to a sensible maximum to prevent DoS
                        String boundary("--");
                        boundary.append(header.getMIMEBoundary());
                        // include trailing "\r\n" or "--" in length
                        // later on, will also include leading "\r\n"
                        // need to make sure boundary fits in half our network buffer,
                        // or we won't be able to locate instances of it reliably
                        if ((boundary.length() + 2) == 0 || (boundary.length() + 2) > 1022)
                            throw postfilter_exception("Could not determine boundary for multi-part POST");

#ifdef DGDEBUG
                        std::cout << dbgPeerPort << " -Boundary: " << boundary << std::endl;
#endif

                        // Grab remaining data, including trailing boundary
                        // Split into parts and process each as we go
                        std::unique_ptr<BackedStore> part;
                        std::string rolling_buffer;
                        std::string trailer;
                        rolling_buffer.reserve(2048);
                        bool first = true;
                        bool last = false;
                        // Iterate over all parts.  Stop filtering after the first blocked part,
                        // for performance, but keep processing so that we can store all parts
                        // if necessary.
                        while (bytes_remaining > 0 && !last /*&& !checkme.isItNaughty*/) {
                            // Grab the next chunk of data
                            int bytes_this_time = bytes_remaining > (2048 - rolling_buffer.length())
                                ? (2048 - rolling_buffer.length())
                                : bytes_remaining;
                            int rc = peerconn.readFromSocketn(buffer, bytes_this_time, 0, 10000);
                            if (rc < bytes_this_time)
                                throw postfilter_exception("Could not retrieve POST data from browser");

                            // Put up to (chunk size * 2) in rolling buffer
                            rolling_buffer.append(buffer, bytes_this_time);
                            bytes_remaining -= bytes_this_time;

                            bool foundb = false;
                            do {
                                // Process data from left of buffer
                                std::string::size_type loc = rolling_buffer.find(boundary);
                                if ((loc == std::string::npos) || (rolling_buffer.length() - (loc + (boundary.length() + 2)) < 0)) {
                                    // Didn't contain the boundary, or wasn't long enough
                                    // to contain boundary plus trailer - append up to
                                    // the first half of the rolling buffer to the
                                    // current part, then discard it
                                    loc = 1024 < rolling_buffer.length() ? 1024 : rolling_buffer.length();
                                    foundb = false;
                                } else {
                                    // Contained the boundary - append data up to the
                                    // boundary, discard that data plus boundary
                                    foundb = true;
                                    // See what the two trailing bytes of the boundary are
                                    trailer.assign(rolling_buffer.substr(loc + boundary.length(), 2));
                                    if (trailer == "--")
                                        last = true;
                                    else if (trailer != "\r\n")
                                        throw postfilter_exception("Unrecognised multi-part POST boundary trailer");
                                }

                                // Store data from left-hand half of buffer
                                // Don't bother storing the preamble
                                if (!first) {
                                    if (part.get() != NULL && part->append(rolling_buffer.substr(0, loc).c_str(), loc)) {
                                        if (foundb) {
                                            // Determine where the headers end and the data begins
                                            part->finalise();
                                            const char *data = part->getData();
                                            size_t offset = 0;
                                            bool foundend = false;
                                            do {
                                                void *headend = memchr((void *)(data + offset), '\r', part->getLength() - offset);
                                                if (headend == NULL)
                                                    // not found
                                                    break;
                                                offset = (size_t)headend - (size_t)(data);
                                                if ((part->getLength() - offset) >= 4
                                                    && strncmp(data + offset, "\r\n\r\n", 4) == 0) {
                                                    // found
                                                    foundend = true;
                                                    break;
                                                }
                                                // not found, but keep looking
                                                ++offset;
                                            } while (offset < (ssize_t)(part->getLength() - 4));

                                            if (!foundend)
                                                throw postfilter_exception("End of POST data part headers not found");
#ifdef DGDEBUG
                                            std::cout << dbgPeerPort << " -POST data headers: " << std::string(data, offset) << std::endl;
#endif
                                            // Extract pertinent info from part's headers
                                            String mimetype;
                                            String disposition;
                                            size_t hdr_offset = 0;
                                            do {
                                                // Look for the end of the next header line in the section of the part
                                                // that we know consists of headers (plus the last '\r')
                                                void *headend = memchr((void *)(data + hdr_offset), '\r', (offset - hdr_offset) + 1);
                                                if (headend == NULL)
                                                    // not found
                                                    break;
                                                size_t new_hdr_offset = (size_t)headend - (size_t)(data);
                                                if ((new_hdr_offset - hdr_offset > 14)
                                                    && strncasecmp(data + hdr_offset + 9, "ype: ", 5) == 0) {
                                                    // found Content-Type
                                                    mimetype.assign(data + (hdr_offset + 14), new_hdr_offset - (hdr_offset + 14));
                                                } else if ((new_hdr_offset - hdr_offset > 21)
                                                    && strncasecmp(data + hdr_offset + 9, "isposition: ", 12) == 0) {
                                                    // found Content-Disposition
                                                    disposition.assign(data + (hdr_offset + 21), new_hdr_offset - (hdr_offset + 21));
                                                }
                                                // Restart from end of current header (also skip '\n')
                                                hdr_offset = new_hdr_offset + 2;
                                            } while (hdr_offset < offset);
#ifdef DGDEBUG
                                            std::cout << dbgPeerPort << " -POST part MIME type: " << mimetype << std::endl;
                                            std::cout << dbgPeerPort << " -POST part disposition: " << disposition << std::endl;
#endif
                                            // Put info about the part in the POST parts list, for logging
                                            if (mimetype.empty())
                                                mimetype.assign("text/plain");
                                            postparts.push_back(postinfo());
                                            postparts.back().mimetype.assign(mimetype);
                                            std::string::size_type start = disposition.find("filename=");
                                            if (start != std::string::npos) {
                                                start += 9;
                                                char endchar = ';';
                                                if (disposition[start] == '"') {
                                                    endchar = '"';
                                                    ++start;
                                                }
                                                std::string::size_type end = disposition.find(endchar, start);
                                                if (end != std::string::npos)
                                                    postparts.back().filename = disposition.substr(start, end - start);
                                                else
                                                    postparts.back().filename = disposition.substr(start);
                                            }
                                            // Don't include "\r\n\r\n" in part's body data
                                            offset += 4;
                                            postparts.back().size = part->getLength();
                                            postparts.back().bodyoffset = offset;

                                            // Pre-emptively store the data part if storage is enabled.
                                            // If, when we get to the end of the filtering, the request
                                            // is not blocked/marked for storage, all parts will then
                                            // be deleted.  We need all parts to give decent context.
                                            if (!o.blocked_content_store.empty()) {
                                                postparts.back().storedname = part->store(o.blocked_content_store.c_str());
#ifdef DGDEBUG
                                                std::cout << dbgPeerPort << " -Pre-emptively stored POST data part: " << postparts.back().storedname << std::endl;
#endif
                                            }

                                            // Run part through interested request scanning plugins
                                            if (!checkme.isItNaughty) {
                                                for (std::deque<CSPlugin *>::iterator i = requestscanners.begin(); i != requestscanners.end(); ++i) {
                                                    int csrc = (*i)->willScanData(header.getUrl(), clientuser.c_str(), ldl->fg[filtergroup], clientip.c_str(),
                                                        true, false, checkme.isexception, checkme.isbypass, disposition, mimetype, part->getLength() - offset);
#ifdef DGDEBUG
                                                    std::cerr << dbgPeerPort << " -willScanData returned: " << csrc << std::endl;
#endif
                                                    if (csrc > 0) {
                                                        csrc = (*i)->scanMemory(&header, NULL, clientuser.c_str(), ldl->fg[filtergroup], clientip.c_str(),
                                                            data + offset, part->getLength() - offset, &checkme,
                                                            &disposition, &mimetype);
                                                        if (csrc != DGCS_CLEAN && csrc != DGCS_WARNING) {
                                                            checkme.blocktype = 1;
                                                            postparts.back().blocked = true;
                                                            // Don't delete part (yet) if in stealth mode - need to send the data upstream
                                                            if (ldl->fg[filtergroup]->reporting_level != -1)
                                                                part.reset();
                                                        }
                                                        if (csrc == DGCS_BLOCKED) {
                                                            // Send part upstream anyway if in stealth mode
                                                            if (ldl->fg[filtergroup]->reporting_level != -1)
                                                                break;
                                                        } else if (csrc == DGCS_INFECTED) {
                                                            checkme.wasinfected = true;
                                                            // Send part upstream anyway if in stealth mode
                                                            if (ldl->fg[filtergroup]->reporting_level != -1)
                                                                break;
                                                        }
                                                        //if its not clean / we errored then treat it as infected
                                                        else if (csrc != DGCS_CLEAN && csrc != DGCS_WARNING) {
                                                            if (csrc < 0) {
                                                                syslog(LOG_ERR, "Return code from content scanner: %d", csrc);
                                                            } else {
                                                                syslog(LOG_ERR, "scanFile/Memory returned error: %d", csrc);
                                                            }
                                                            //TODO: have proper error checking/reporting here?
                                                            //at the very least, integrate with the translation system.
                                                            //checkme.whatIsNaughty = "WARNING: Could not perform content scan!";
                                                            checkme.message_no = 1203;
                                                            checkme.whatIsNaughty = o.language_list.getTranslation(1203);
                                                            checkme.whatIsNaughtyLog = (*i)->getLastMessage().toCharArray();
                                                            //checkme.whatIsNaughtyCategories = "Content scanning";
                                                            checkme.whatIsNaughtyCategories = o.language_list.getTranslation(72);
                                                            checkme.isItNaughty = true;
                                                            checkme.isException = false;
                                                            checkme.scanerror = true;
                                                            break;
                                                        }
                                                    } else if (csrc < 0)
                                                        // TODO - Should probably block here
                                                        syslog(LOG_ERR, "willScanData returned error: %d", csrc);
                                                }
                                            }
                                            // Send whole part upstream
                                            if (!checkme.isItNaughty || ldl->fg[filtergroup]->reporting_level == -1)
                                                if(!proxysock.writeToSocket(data, part->getLength(), 0, 20000))
                                                    cleanThrow("Error sending post data to proxy", peerconn,proxysock);
                                        }
                                    } else {
                                        // Data could not be appended to the buffered POST part
                                        // - length must have exceeded maxcontentfilecachescansize,
                                        // so send the part directly upstream instead
                                        if (part.get() != NULL) {
#ifdef DGDEBUG
                                            std::cout << dbgPeerPort << " -POST data part too large, sending upstream" << std::endl;
#endif
                                            // Send what we've buffered so far, then delete the buffer
                                            part->finalise();
                                            if(!proxysock.writeToSocket(part->getData(), part->getLength(), 0, 20000))
                                                cleanThrow("Error sending post data to proxy", peerconn,proxysock);
                                            part.reset();
                                        }
                                        // Send current chunk upstream directly
                                        if(!proxysock.writeToSocket(rolling_buffer.substr(0, loc).c_str(), loc, 0, 20000))
                                           cleanThrow("Error sending post data to proxy", peerconn,proxysock);
                                    }
                                    if (foundb) {
                                        if (!checkme.isItNaughty || ldl->fg[filtergroup]->reporting_level == -1) {
                                            // Regardless of whether we were buffering or streaming, send the
                                            // boundary and trailers upstream if this was the last chunk of a part
                                            if(!proxysock.writeToSocket(boundary.c_str(), boundary.length(), 0, 10000))
                                                cleanThrow("Error sending post data to proxy", peerconn,proxysock);
                                            if(!proxysock.writeToSocket(trailer.c_str(), trailer.length(), 0, 10000))
                                                cleanThrow("Error sending post data to proxy", peerconn,proxysock);
                                            // Include final CRLF (after the trailer) after last boundary
                                            if (last)
                                                if(!proxysock.writeToSocket("\r\n", 2, 0, 10000))
                                                    cleanThrow("Error sending post data to proxy", peerconn,proxysock);
                                        }
                                        part.reset(new BackedStore(o.max_content_ramcache_scan_size, o.max_content_filecache_scan_size));
                                    }
                                }

                                // If we found the boundary, include boundary size
                                // in the length of data we will discard
                                if (foundb) {
                                    loc += boundary.length() + 2;
                                    if (first) {
// We just past the preamble/first boundary
// Send request headers and first boundary upstream
#ifdef DGDEBUG
                                        std::cout << dbgPeerPort << " -Preamble/first boundary passed; sending headers & first boundary upstream" << std::endl;
#endif
                                        if (!checkme.wasrequested && (!checkme.isItNaughty || ldl->fg[filtergroup]->reporting_level == -1)) {
                                            if(!proxysock.breadyForOutput(o.proxy_timeout))
                                            cleanThrow("Error sending headers to proxy", peerconn,proxysock);
#ifdef DGDEBUG
                                            std::cerr << dbgPeerPort << "  got past line 2098 rfo " << std::endl;
#endif
                                            // sent *without* POST data, so cannot retrieve headers yet
                                            header.out(NULL, &proxysock, __DGHEADER_SENDALL, true);
                                            checkme.wasrequested = true;
                                            if(!proxysock.writeToSocket(boundary.c_str(), boundary.length(), 0, 10000))
                                                cleanThrow("Error sending headers to proxy", peerconn,proxysock);
                                            if(!proxysock.writeToSocket(trailer.c_str(), trailer.length(), 0, 10000))
                                                cleanThrow("Error sending headers  to proxy", peerconn,proxysock);
                                        }
                                        first = false;
                                        // Clear out dummy log data so it can be filled in/
                                        // with info about each POST part individually
                                        postparts.clear();
                                        // For all boundaries after the first, include the leading CRLF
                                        boundary.insert(0, "\r\n");
                                        // Create BackedStore for first data part
                                        part.reset(new BackedStore(o.max_content_ramcache_scan_size, o.max_content_filecache_scan_size));
                                    }
                                }
                                rolling_buffer.erase(0, loc);
                            } while (foundb /*&& !checkme.isItNaughty*/);
                        } // while bytes_remaining > 0 && !last /* && not blocked */

                        // If the request is not blocked or storage has not been requested,
                        // delete all the (possibly) pre-emptively stored data parts
                        if (!o.blocked_content_store.empty() && (!checkme.isItNaughty || !checkme.store)) {
#ifdef DGDEBUG
                            std::cout << dbgPeerPort << " -Request was not blocked/marked for storage. Deleting data parts:" << std::endl;
#endif
                            for (std::list<postinfo>::iterator i = postparts.begin(); i != postparts.end(); ++i) {
                                if (i->storedname.empty())
                                    continue;
#ifdef DGDEBUG
                                std::cout << dbgPeerPort << " -Part " << i->storedname << std::endl;
#endif
                                unlink(i->storedname.c_str());
                                i->storedname.clear();
                            }
#ifdef DGDEBUG
                            std::cout << dbgPeerPort << " -All parts deleted" << std::endl;
#endif
                        }

                        if (!checkme.isItNaughty) {
                            // Were we still within a part when the data came to an end?
                            // Did we not find a correctly-formatted last part boundary?
                            // Was there data (other than a CRLF) remaining after the final boundary?
                            if (rolling_buffer.length() > 2 || !last || bytes_remaining > 2) {
                                std::ostringstream ss;
                                ss << "Last part of multi-part POST was not correctly terminated.  Part length: ";
                                ss << part->getLength() << ", bytes remaining: " << bytes_remaining << ", last part found: " << last;
                                throw postfilter_exception(ss.str().c_str());
                            }
// get header from proxy
// wasrequested will have been set to true (we have had to send out
// the request headers & POST data by the time we get here), so none
// of the code below here will do this for us.
#ifdef DGDEBUG
                            std::cout << dbgPeerPort << " -All parts sent upstream; retrieving response headers" << std::endl;
#endif
                            proxysock.bcheckForInput(120000);
                            docheader.in(&proxysock, persistOutgoing);
                            persistProxy = docheader.isPersistent();
                            persistPeer = persistOutgoing && docheader.wasPersistent();

#ifdef DGDEBUG
                            std::cout << dbgPeerPort << " -persistPeer: " << persistPeer << std::endl;
#endif

                        } else {
// Was blocked - discard rest of POST data before we show the block page
#ifdef DGDEBUG
                            std::cout << dbgPeerPort << " -POST data part blocked; discarding remaining POST data" << std::endl;
#endif
                            // Send rest of data upstream anyway if in stealth mode
                            if (ldl->fg[filtergroup]->reporting_level == -1) {
                                if(!proxysock.writeToSocket(rolling_buffer.c_str(), rolling_buffer.length(), 0, 10000))
                                    cleanThrow("Error sending headers  to proxy", peerconn,proxysock);
                                fdt.reset();
                                if(!fdt.tunnel(peerconn, proxysock, false, bytes_remaining, false) )
                                     cleanThrow("Error in tunneling data", peerconn,proxysock);
                                //persistProxy = false;
                                // Also retrieve response headers, if wasrequested was set to true,
                                // because nothing else will do so later on
                                if (checkme.wasrequested) {
                                    docheader.in(&proxysock, persistOutgoing);
                                    persistProxy = docheader.isPersistent();
                                    persistPeer = persistOutgoing && docheader.wasPersistent();
#ifdef DGDEBUG
                                    std::cout << dbgPeerPort << " -persistPeer: " << persistPeer << std::endl;
#endif
                                }
                            } else
                                header.discard(&peerconn, bytes_remaining);
                        }

                    } else // if (mtype == "application/x-www-form-urlencoded")
                    {
#ifdef DGDEBUG
                        std::cout << dbgPeerPort << " -Filtering single-part POST data" << std::endl;
#endif
                        // single-part POST (plain-text form data)
                        // we know the size for the part has already been checked by this point
                        // TODO Change this to use a BackedStore for consistency, and so that we
                        // don't have to have cut-and-pasted code in the blocked content storage
                        // implementation?  Should possibly make this a loop around a more
                        // light-weight socket read function, as even if we won't get data into the
                        // BackedStore in a zero-copy fashion, there is no reason to have *too*
                        // much copied data sat around in RAM.
                        // Also a "reserve()"-alike for BackedStore wouldn't go amiss, as we know
                        // the data size in advance.
                        char buffer[cl];
                        int rc = peerconn.readFromSocketn(buffer, cl, 0, 10000);

                        if (rc < 0)
                            throw postfilter_exception("Could not retrieve POST data from browser");

                        // Set the POST data buffer on the request, so that it
                        // does not block indefinitely trying to tunnel data that
                        // the browser has already sent
                        header.setPostData(buffer, cl);

                        // data looks like "name=value+1&name2=value+2".
                        // parse the text to remove variable names and pad with
                        // spaces at beginning & end.
                        String result(" ");
                        bool inname = true;
                        for (off_t i = 1; i < cl; ++i) {
                            if (inname) {
                                if (buffer[i] == '=')
                                    inname = false;
                            } else {
                                if (buffer[i] == '&') {
                                    inname = true;
                                    result.append(" ");
                                } else
                                    result.append(1, buffer[i]);
                            }
                        }
                        result.append(" ");

                        // turn '+' back into ' '
                        result.replaceall("+", " ");

                        // decode %xx
                        result = HTTPHeader::decode(result, true);

// Run the result through request scanners which are happy to deal with reconstituted data
#ifdef DGDEBUG
                        std::cout << dbgPeerPort << " -Form data: " << result.c_str() << std::endl;
#endif
                        for (std::deque<CSPlugin *>::iterator i = requestscanners.begin(); i != requestscanners.end(); ++i) {
                            int csrc = (*i)->willScanData(header.getUrl(), clientuser.c_str(), ldl->fg[filtergroup], clientip.c_str(),
                                true, true, checkme.isexception, checkme.isbypass, "", "text/plain", result.length());
#ifdef DGDEBUG
                            std::cerr << dbgPeerPort << " -willScanData returned: " << csrc << std::endl;
#endif
                            if (csrc > 0) {
                                String mimetype("text/plain");
                                csrc = (*i)->scanMemory(&header, NULL, clientuser.c_str(), ldl->fg[filtergroup], clientip.c_str(),
                                    result.c_str(), result.length(), &checkme, NULL, &mimetype);
                                if (csrc != DGCS_CLEAN && csrc != DGCS_WARNING) {
                                    checkme.blocktype = 1;
                                    postparts.back().blocked = true;
                                    if (checkme.store && !o.blocked_content_store.empty()) {
                                        // Write original encoded buffer to disk
                                        std::ostringstream timedprefix;
                                        timedprefix << o.blocked_content_store << '-' << time(NULL) << '-' << std::flush;
                                        std::string pfx(timedprefix.str());
                                        char storedname[pfx.length() + 7];
                                        strncpy(storedname, pfx.c_str(), pfx.length());
                                        strncpy(storedname + pfx.length(), "XXXXXX", 6);
                                        storedname[pfx.length() + 6] = '\0';
#ifdef DGDEBUG
                                        std::cout << dbgPeerPort << " -Single-part POST: storedname template: " << storedname << std::endl;
#endif
                                        int storefd;
                                        if ((storefd = mkstemp(storedname)) < 0) {
                                            std::ostringstream ss;
                                            ss << "Could not create file for single-part POST data: " << strerror(errno);
                                            throw std::runtime_error(ss.str().c_str());
                                        }
#ifdef DGDEBUG
                                        std::cout << dbgPeerPort << " -Single-part POST: storedname: " << storedname << std::endl;
#endif
                                        postparts.back().storedname = storedname;
                                        ssize_t bytes_written = 0;
                                        ssize_t rc = 0;
                                        do {
                                            rc = write(storefd, buffer + bytes_written, cl - bytes_written);
                                            if (rc > 0)
                                                bytes_written += rc;
                                        } while (bytes_written < cl && (rc > 0 || errno == EINTR));
                                        if (rc < 0 && errno != EINTR) {
                                            std::ostringstream ss;
                                            ss << "Could not write single-part POST data to file: " << strerror(errno);
                                            do {
                                                rc = close(storefd);
                                            } while (rc < 0 && errno == EINTR);
                                            throw std::runtime_error(ss.str().c_str());
                                        }
                                        do {
                                            rc = close(storefd);
                                        } while (rc < 0 && errno == EINTR);
                                    }
                                }
                                if (csrc == DGCS_BLOCKED) {
                                    break;
                                } else if (csrc == DGCS_INFECTED) {
                                    checkme.wasinfected = true;
                                    break;
                                }
                                //if its not clean / we errored then treat it as infected
                                else if (csrc != DGCS_CLEAN && csrc != DGCS_WARNING) {
                                    if (csrc < 0) {
                                        syslog(LOG_ERR, "Unknown return code from content scanner: %d", csrc);
                                    } else {
                                        syslog(LOG_ERR, "scanFile/Memory returned error: %d", csrc);
                                    }
                                    //at the very least, integrate with the translation system.
                                    //checkme.whatIsNaughty = "WARNING: Could not perform content scan!";
                                    checkme.message_no = 1203;
                                    checkme.whatIsNaughty = o.language_list.getTranslation(1203);
                                    checkme.whatIsNaughtyLog = (*i)->getLastMessage().toCharArray();
                                    checkme.whatIsNaughtyCategories = "Content scanning";
                                    checkme.whatIsNaughtyCategories = o.language_list.getTranslation(72);
                                    checkme.isItNaughty = true;
                                    checkme.isException = false;
                                    checkme.scanerror = true;
                                    break;
                                }
                            } else if (csrc < 0)
                                // TODO - Should probably block here
                                syslog(LOG_ERR, "willScanData returned error: %d", csrc);
                        }
                    }
                    // Cannot be other, unknown MIME type because MIME type
                    // is checked before CS plugins are queried (so plugin lists
                    // will be empty for other MIME types)
                }
            }
#ifdef DGDEBUG
            // Banning POST requests for unauthed users (when auth is enabled) could potentially prevent users from authenticating.
            else if (!authed)
                std::cout << dbgPeerPort << " -Skipping POST filtering because user is unauthed." << std::endl;
#endif

            if (!checkme.isItNaughty) {
                // the request is ok, so we can	now pass it to the proxy, and check the returned header
                // temp char used in various places here
                char *i;

                // send header to proxy
                if (!checkme.wasrequested) {
#ifdef DGDEBUG
                    std::cerr << dbgPeerPort << " before 2352 rfo " << std::endl;
#endif
                    if(!proxysock.breadyForOutput(o.proxy_timeout))
                        cleanThrow("Error sending headers to proxy", peerconn,proxysock);
                    //proxysock.readyForOutput(o.proxy_timeout);
#ifdef DGDEBUG
                    std::cerr << dbgPeerPort << "  got past line 2352 rfo " << std::endl;
#endif
                    header.out(&peerconn, &proxysock, __DGHEADER_SENDALL, true);
#ifdef DGDEBUG
                    std::cerr << dbgPeerPort << "  got past line 2350 proxy header out " << std::endl;
                    std::cerr << dbgPeerPort << "  exchange_timeout is " << o.exchange_timeout << std::endl;
#endif

                    // get header from proxy
                    if (proxysock.bcheckForInput(o.exchange_timeout)) {
#ifdef DGDEBUG
                        std::cout << dbgPeerPort << " before docheader in 2371: "  << std::endl;
#endif
                        docheader.in(&proxysock, persistOutgoing);
                        persistProxy = docheader.isPersistent();
                        persistPeer = persistOutgoing && docheader.wasPersistent();
#ifdef DGDEBUG
                        std::cout << dbgPeerPort << " -persistPeer: " << persistPeer << std::endl;
#endif
                    }  else {
#ifdef DGDEBUG
                        std::cout << dbgPeerPort << " -error/timeout on header in from proxy: " << persistPeer << std::endl;
                        if (proxysock.isHup())
                        std::cout << dbgPeerPort << " -proxy hung up " << std::endl;
                        if (proxysock.isTimedout())
                        std::cout << dbgPeerPort << " -proxy timedout" << std::endl;
                        if (proxysock.sockError())
                        std::cout << dbgPeerPort << " -proxy socket error" << std::endl;
#endif
                        if (proxysock.isTimedout()) {
                            checkme.message_no = 200;
                            peerconn.writeString("HTTP/1.0 504 Gateway Time-out\nContent-Type: text/html\n\n");
                            peerconn.writeString(
                                    "<HTML><HEAD><TITLE>e2guardian - 504 Gateway Time-out</TITLE></HEAD><BODY><H1>e2guardian - 504 Gateway Time-out</H1>");
                            peerconn.writeString(o.language_list.getTranslation(201));
                            peerconn.writeString("</BODY></HTML>\n");
                            break;
                        } else {
                            checkme.message_no = 200;
                            peerconn.writeString("HTTP/1.0 502 Gateway Error\nContent-Type: text/html\n\n");
                            peerconn.writeString(
                                    "<HTML><HEAD><TITLE>e2guardian - 502 Gateway Error</TITLE></HEAD><BODY><H1>e2guardian - 502 Gateway Error</H1>");
                            peerconn.writeString(o.language_list.getTranslation(202));
                            peerconn.writeString("</BODY></HTML>\n");
                            break;
                            //        cleanThrow("Unable to read header from proxy", peerconn, proxysock);
                        }
                    }

                    checkme.wasrequested = true; // so we know where we are later
                }

// TODO: Ugly Temporary: we must remove from filtering many unused HTTP code (V4 ?)

                if ((docheader.returnCode() == 302) || (docheader.returnCode() == 301) || (docheader.returnCode() == 307) || (docheader.returnCode() == 308)) {
#ifdef DGDEBUG
                  std::cout << " -Filtering exception: " << checkme.urldomain << " code: " << docheader.returnCode() << std::endl;
#endif
                    checkme.nolog = true;
                    checkme.isexception = true;
                }
#ifdef DGDEBUG
                std::cout << dbgPeerPort << " -got header from proxy" << std::endl;
                if (!persistProxy)
                    std::cout << dbgPeerPort << " -header says close, so not persisting" << std::endl;
#endif

                // if we're not careful, we can end up accidentally setting the bypass cookie twice.
                // because of the code flow, this second cookie ends up with timestamp 0, and is always disallowed.
                if (checkme.isbypass && !checkme.isvirusbypass && !checkme.iscookiebypass && !checkme.isexception) {
#ifdef DGDEBUG
                    std::cout << "Setting GBYPASS cookie; bypasstimestamp = " << bypasstimestamp << std::endl;
#endif
                    String ud(checkme.urldomain);
                    if (ud.startsWith("www.")) {
                        ud = ud.after("www.");
                    }

                    if(!docheader.header.empty()) {
                        docheader.setCookie("GBYPASS", ud.toCharArray(),
                                            hashedCookie(&ud, ldl->fg[filtergroup]->cookie_magic.c_str(), &clientip,
                                                         bypasstimestamp).toCharArray());

                        // redirect user to URL with GBYPASS parameter no longer appended
                        docheader.header[0] = "HTTP/1.0 302 Redirect";
                        String loc("Location: ");
                        loc += header.getUrl(true);
                        docheader.header.push_back(loc);
                        docheader.setContentLength(0);

                        persistOutgoing = false;
                        docheader.out(NULL, &peerconn, __DGHEADER_SENDALL);
                    }

                    if (!persistProxy)
                        proxysock.close(); // close connection to proxy

                    break;
                }

                // don't even bother scan testing if the content-length header indicates the file is larger than the maximum size we'll scan
                // - based on patch supplied by cahya (littlecahya@yahoo.de)
                // be careful: contentLength is signed, and max_content_filecache_scan_size is unsigned
                off_t cl = docheader.contentLength();
                if (!responsescanners.empty()) {
                    if (cl == 0)
                        responsescanners.clear();
                    else if ((cl > 0) && (cl > o.max_content_filecache_scan_size))
                        responsescanners.clear();
                }

                // now that we have the proxy's header too, we can make a better informed decision on whether or not to scan.
                // this used to be done before we'd grabbed the proxy's header, rendering exceptionvirusmimetypelist useless,
                // and exceptionvirusextensionlist less effective, because we didn't have a Content-Disposition header.
                if (!responsescanners.empty()) {
#ifdef DGDEBUG
                    std::cerr << dbgPeerPort << " -Number of response CS plugins in candidate list: " << responsescanners.size() << std::endl;
#endif
//send header to plugin here needed
//also send user and group
#ifdef DGDEBUG
                    int j = 0;
#endif
                    std::deque<CSPlugin *> newplugins;
                    for (std::deque<CSPlugin *>::iterator i = responsescanners.begin(); i != responsescanners.end(); ++i) {
                        int csrc = (*i)->willScanData(header.getUrl(), clientuser.c_str(), ldl->fg[filtergroup], clientip.c_str(),
                            false, false, checkme.isexception, checkme.isbypass, docheader.disposition(), docheader.getContentType(), docheader.contentLength());
#ifdef DGDEBUG
                        std::cerr << dbgPeerPort << " -willScanData for plugin " << j << " returned: " << csrc << std::endl;
#endif
                        if (csrc > 0)
                            newplugins.push_back(*i);
                        else if (csrc < 0)
                            // TODO Should probably block on error
                            syslog(LOG_ERR, "willScanData returned error: %d", csrc);
#ifdef DGDEBUG
                        j++;
#endif
                    }

                    // Store only those plugins which responded positively to willScanData
                    responsescanners.swap(newplugins);
                }

                // no need to check bypass mode, exception mode, auth required headers, redirections, or banned ip/user (the latter get caught by requestChecks later)
                if (!checkme.isexception && !checkme.isbypass && !(isbannedip || isbanneduser) && !docheader.isRedirection() && !docheader.authRequired()) {
                    bool download_exception = false;

                    // Check the exception file site and MIME type lists.
                    checkme.mimetype = docheader.getContentType().toCharArray();
                    if (ldl->fg[filtergroup]->inExceptionFileSiteList(checkme.urld))
                        download_exception = true;
                    else {
                        if (o.lm.l[ldl->fg[filtergroup]->exception_mimetype_list]->findInList(checkme.mimetype.c_str(), lastcategory))
                            download_exception = true;
                    }

                    // Perform banned MIME type matching
                    if (!download_exception) {
                        // If downloads are blanket blocked, block outright.
                        if (ldl->fg[filtergroup]->block_downloads) {
                            // did not match the exception list
                            checkme.whatIsNaughty = o.language_list.getTranslation(750);
                            // Blanket file download is active
                            checkme.whatIsNaughty += checkme.mimetype;
                            checkme.whatIsNaughtyLog = checkme.whatIsNaughty;
                            checkme.isItNaughty = true;
                            checkme.whatIsNaughtyCategories = "Blanket download block";
                        } else if ((i = o.lm.l[ldl->fg[filtergroup]->banned_mimetype_list]->findInList(checkme.mimetype.c_str(), lastcategory)) != NULL) {
                            // matched the banned list
                            checkme.whatIsNaughty = o.language_list.getTranslation(800);
                            // Banned MIME Type:
                            checkme.whatIsNaughty += i;
                            checkme.whatIsNaughtyLog = checkme.whatIsNaughty;
                            checkme.isItNaughty = true;
                            checkme.whatIsNaughtyCategories = "Banned MIME Type";
                        }

#ifdef DGDEBUG
                        std::cout << dbgPeerPort << checkme.mimetype.length() << std::endl;
                        std::cout << dbgPeerPort << " -:" << checkme.mimetype;
                        std::cout << dbgPeerPort << " -:" << std::endl;
#endif
                    }

                    // Perform extension matching - if not already matched the exception MIME or site lists
                    if (!download_exception) {
                        // Can't ban file extensions of URLs that just redirect
                        String tempurl(checkme.urld);
                        String tempdispos(docheader.disposition());
                        unsigned int elist, blist;
                        elist = ldl->fg[filtergroup]->exception_extension_list;
                        blist = ldl->fg[filtergroup]->banned_extension_list;
                        char *e = NULL;
                        char *b = NULL;
                        if (tempdispos.length() > 1) {
// dispos filename must take presidense
#ifdef DGDEBUG
                            std::cout << dbgPeerPort << " -Disposition filename:" << tempdispos << ":" << std::endl;
#endif
                            // The function expects a url so we have to
                            // generate a pseudo one.
                            tempdispos = "http://foo.bar/" + tempdispos;
                            e = ldl->fg[filtergroup]->inExtensionList(elist, tempdispos);
                            // Only need to check banned list if not blanket blocking
                            if ((e == NULL) && !(ldl->fg[filtergroup]->block_downloads))
                                b = ldl->fg[filtergroup]->inExtensionList(blist, tempdispos);
                        } else {
                            if (!tempurl.contains("?")) {
                                e = ldl->fg[filtergroup]->inExtensionList(elist, tempurl);
                                if ((e == NULL) && !(ldl->fg[filtergroup]->block_downloads))
                                    b = ldl->fg[filtergroup]->inExtensionList(blist, tempurl);
                            } else if (String(checkme.mimetype.c_str()).contains("application/")) {
                                while (tempurl.endsWith("?")) {
                                    tempurl.chop();
                                }
                                while (tempurl.contains("/")) { // no slash no url
                                    e = ldl->fg[filtergroup]->inExtensionList(elist, tempurl);
                                    if (e != NULL)
                                        break;
                                    if (!(ldl->fg[filtergroup]->block_downloads))
                                        b = ldl->fg[filtergroup]->inExtensionList(blist, tempurl);
                                    while (tempurl.contains("/") && !tempurl.endsWith("?")) {
                                        tempurl.chop();
                                    }
                                    tempurl.chop(); // get rid of the ?
                                }
                            }
                        }

                        // If downloads are blanket blocked, block unless matched the exception list.
                        // If downloads are not blanket blocked, block if matched the banned list and not the exception list.
                        if (ldl->fg[filtergroup]->block_downloads && (e == NULL)) {
                            // did not match the exception list
                            checkme.whatIsNaughty = o.language_list.getTranslation(751);
                            // Blanket file download is active
                            checkme.whatIsNaughtyLog = checkme.whatIsNaughty;
                            checkme.isItNaughty = true;
                            checkme.whatIsNaughtyCategories = "Blanket download block";
                        } else if (!(ldl->fg[filtergroup]->block_downloads) && (e == NULL) && (b != NULL)) {
                            // matched the banned list
                            checkme.whatIsNaughty = o.language_list.getTranslation(900);
                            // Banned extension:
                            checkme.whatIsNaughty += b;
                            checkme.whatIsNaughtyLog = checkme.whatIsNaughty;
                            checkme.isItNaughty = true;
                            checkme.whatIsNaughtyCategories = "Banned extension";
                        } else if (e != NULL) {
                            // intention is to match either/or of the MIME & extension lists
                            // so if it gets this far, un-naughty it (may have been naughtied by the MIME type list)
                            checkme.isItNaughty = false;
                        }
                    }
                }

                // check header sent to proxy - this could be done before the send, but we
                // want to wait until after the MIME type & extension checks, because they may
                // act as a quicker rejection. also so as not to pre-emptively ban currently
                // un-authed users.
                if (!authed && !checkme.isbypass && !checkme.isexception && !checkme.isItNaughty && !docheader.authRequired()) {
                    requestChecks(&header, &checkme, &checkme.urld, &checkme.url, &clientip, &clientuser, filtergroup,
                        isbanneduser, isbannedip, room);
                }

                // check body from proxy
                // can't do content filtering on HEAD or redirections (no content)
                // actually, redirections CAN have content
                if (!checkme.isItNaughty && (cl != 0) && !checkme.ishead) {
                    if (((docheader.isContentType("text",ldl->fg[filtergroup]) || docheader.isContentType("-",ldl->fg[filtergroup])) && !checkme.isexception) || !responsescanners.empty()) {
                        // despite the debug note above, we do still go through contentFilter for cached non-exception HTML,
                        // as content replacement rules need to be applied.
                        checkme.waschecked = true;
                        if (!responsescanners.empty()) {
#ifdef DGDEBUG
                            std::cout << dbgPeerPort << " -Filtering with expectation of a possible csmessage" << std::endl;
#endif
                            String csmessage;
                            contentFilter(&docheader, &header, &docbody, &proxysock, &peerconn, &checkme.headersent, &checkme.pausedtoobig,
                                &checkme.docsize, &checkme, checkme.wasclean, filtergroup, responsescanners, &clientuser, &clientip,
                                &checkme.wasinfected, &checkme.wasscanned, checkme.isbypass, checkme.urld, checkme.urldomain, &checkme.scanerror, checkme.contentmodified, &csmessage);
                            if (csmessage.length() > 0) {
#ifdef DGDEBUG
                                std::cout << dbgPeerPort << " -csmessage found: " << csmessage << std::endl;
#endif
                                checkme.exceptionreason = csmessage.toCharArray();
                            }
                        } else {
                            contentFilter(&docheader, &header, &docbody, &proxysock, &peerconn, &checkme.headersent, &checkme.pausedtoobig,
                                &checkme.docsize, &checkme, checkme.wasclean, filtergroup, responsescanners, &clientuser, &clientip,
                                &checkme.wasinfected, &checkme.wasscanned, checkme.isbypass, checkme.urld, checkme.urldomain, &checkme.scanerror, checkme.contentmodified, NULL);
                        }
                    }
                }
 // End of "if (!checkme.isItNaughty) {"
            }

            if (!checkme.isexception && checkme.isException) {
                checkme.isexception = true;
                checkme.exceptionreason = checkme.whatIsNaughtyLog;
            }

            // then we deny. previously, this che/ipcsockcked the isbypass flag too; now, since bypass requests only undergo the same checking
            // as exceptions, it needn't. and in fact it mustn't, if bypass requests are to be virus scanned/blocked in the same manner as exceptions.
            // make sure we keep track of whether or not logging has been performed, as we may be in stealth mode and don't want to double log.
            bool logged = false;
            if (!authed) {
                logged = true;
                String temp;
                bool is_ip = isIPHostnameStrip(temp);

		if (ldl->fg[filtergroup]->inExceptionSiteList(checkme.urld, true, is_ip, checkme.is_ssl, lastcategory)) { // allowed site
			if (ldl->fg[0]->isOurWebserver(checkme.url)) {
				checkme.isourwebserver = true;
		        } else {
				checkme.isexception = true;
		                    checkme.exceptionreason = o.language_list.getTranslation(602);
		                    checkme.message_no = 602;
		                    // Exception site match.
		                    checkme.exceptioncat = lastcategory.toCharArray();
		                }
		            } else if (ldl->fg[filtergroup]->inExceptionURLList(checkme.urld, true, is_ip, checkme.is_ssl, lastcategory)) { // allowed url
		                checkme.isexception = true;
		                checkme.exceptionreason = o.language_list.getTranslation(603);
		                checkme.message_no = 603;
		                // Exception url match.
		                checkme.exceptioncat = lastcategory.toCharArray();
		            } else if ((rc = ldl->fg[filtergroup]->inExceptionRegExpURLList(checkme.urld, lastcategory)) > -1) {
		                checkme.isexception = true;
		                // exception regular expression url match:
		                checkme.exceptionreason = o.language_list.getTranslation(609);
		                checkme.message_no = 609;
		                checkme.exceptionreason += ldl->fg[filtergroup]->exception_regexpurl_list_source[rc].toCharArray();
		                checkme.exceptioncat = lastcategory.toCharArray();
		          }
		logged = false;
		}

            if (checkme.isItNaughty && !checkme.isexception) {
                String rtype(header.requestType());
#ifdef DGDEBUG
                std::cout << "Category: " << checkme.whatIsNaughtyCategories << std::endl;
#endif
                logged = true;
                doLog(clientuser, clientip, checkme.logurl, header.port, checkme.whatIsNaughtyLog,
                    rtype, checkme.docsize, &checkme.whatIsNaughtyCategories, true, checkme.blocktype, false, false, &thestart,
                    checkme.cachehit, 403, checkme.mimetype, checkme.wasinfected, checkme.wasscanned, checkme.naughtiness, filtergroup,
                    &header, checkme.message_no, checkme.contentmodified, checkme.urlmodified, checkme.headermodified, checkme.headeradded);
                if (denyAccess(&peerconn, &proxysock, &header, &docheader, &checkme.logurl, &checkme, &clientuser, &clientip, filtergroup, checkme.ispostblock, checkme.headersent, checkme.wasinfected, checkme.scanerror)) {
                    return 0; // not stealth mode
                }

                // if get here in stealth mode
            }

            if (!checkme.wasrequested) {
                if(!proxysock.breadyForOutput(o.proxy_timeout))
                    cleanThrow("Error sending headers to proxy", peerconn,proxysock);
                //proxysock.readyForOutput(o.proxy_timeout); // exceptions on error/timeout
#ifdef DGDEBUG
                std::cerr << dbgPeerPort << "  got past line 2659 rfo " << std::endl;
#endif
                header.out(&peerconn, &proxysock, __DGHEADER_SENDALL, true); // exceptions on error/timeout
                proxysock.bcheckForInput(o.exchange_timeout); // exceptions on error/timeout
                docheader.in(&proxysock, persistOutgoing); // get reply header from proxy
                persistProxy = docheader.isPersistent();
                persistPeer = persistOutgoing && docheader.wasPersistent();
#ifdef DGDEBUG
                std::cout << dbgPeerPort << " -persistPeer: " << persistPeer << std::endl;
#endif
            }

//TODO: need to change connection: close if there is plugin involved.
#ifdef DGDEBUG
            std::cout << dbgPeerPort << " -sending header to client" << std::endl;
#endif
            if(!peerconn.breadyForOutput(o.proxy_timeout))
                cleanThrow("Error sending headers to client", peerconn,proxysock);
            //peerconn.readyForOutput(o.proxy_timeout); // exceptions on error/timeout
#ifdef DGDEBUG
            std::cerr << dbgPeerPort << "  got past line 2677 rfo " << std::endl;
#endif
            if (checkme.headersent == 1) {
                docheader.out(NULL, &peerconn, __DGHEADER_SENDREST); // send rest of header to client
#ifdef DGDEBUG
                std::cout << dbgPeerPort << " -sent rest header to client" << std::endl;
#endif
            } else if (checkme.headersent == 0) {
               if(!docheader.out(NULL, &peerconn, __DGHEADER_SENDALL)) { // send header to client
#ifdef DGDEBUG
                   std::cout << dbgPeerPort << " -sent all header failed to client" << std::endl;
                      } else {
                   std::cout << dbgPeerPort << " -sent all header to client" << std::endl;
                   std::cout << dbgPeerPort << " -waschecked:" << checkme.waschecked << std::endl;
#endif
               }
            }

            if (checkme.waschecked) {
                if (!docheader.authRequired() && !checkme.pausedtoobig) {
                    String rtype(header.requestType());
                    if (!logged && !checkme.nolog) {
                        doLog(clientuser, clientip, checkme.logurl, header.port, checkme.exceptionreason,
                            rtype, checkme.docsize, &checkme.whatIsNaughtyCategories, false, 0, checkme.isexception,
                            docheader.isContentType("text",ldl->fg[filtergroup]), &thestart, checkme.cachehit, docheader.returnCode(), checkme.mimetype,
                            checkme.wasinfected, checkme.wasscanned, checkme.naughtiness, filtergroup, &header, checkme.message_no,
                            checkme.contentmodified, checkme.urlmodified, checkme.headermodified, checkme.headeradded);
                    }
                }

                //if(!peerconn.breadyForOutput(o.proxy_timeout))
                 //   cleanThrow("Error sending body to client 2784", peerconn,proxysock);
                //peerconn.readyForOutput(o.proxy_timeout); // check for error/timeout needed
//#ifdef DGDEBUG
  //              std::cerr << dbgPeerPort << "  got past line 2705 rfo " << std::endl;
//#endif

                // it must be clean if we got here
                if (docbody.dontsendbody && docbody.tempfilefd > -1) {
                    // must have been a 'fancy'
                    // download manager so we need to send a special link which
                    // will get recognised and cause DG to send the temp file to
                    // the browser.  The link will be the original URL with some
                    // magic appended to it like the bypass system.

                    // format is:
                    // GSBYPASS=hash(ip+url+tempfilename+mime+disposition+secret)
                    // &N=tempfilename&M=mimetype&D=dispos

                    String ip(clientip);
                    String tempfilename(docbody.tempfilepath.after("/tf"));
                    String tempfilemime(docheader.getContentType());
                    String tempfiledis(miniURLEncode(docheader.disposition().toCharArray()).c_str());
                    String secret(ldl->fg[filtergroup]->magic.c_str());
                    String magic(ip + checkme.url + tempfilename + tempfilemime + tempfiledis + secret);
                    String hashed(magic.md5());
#ifdef DGDEBUG
                    std::cout << dbgPeerPort << " -sending magic link to client: " << ip << " " << checkme.url << " " << tempfilename << " " << tempfilemime << " " << tempfiledis << " " << secret << " " << hashed << std::endl;
#endif
                    String sendurl(checkme.url);
                    if (!sendurl.after("://").contains("/")) {
                        sendurl += "/";
                    }
                    if (sendurl.contains("?")) {
                        sendurl = sendurl + "&GSBYPASS=" + hashed + "&N=";
                    } else {
                        sendurl = sendurl + "?GSBYPASS=" + hashed + "&N=";
                    }
                    sendurl += tempfilename + "&M=" + tempfilemime + "&D=" + tempfiledis;
                    docbody.dm_plugin->sendLink(peerconn, sendurl, checkme.url);

                    // can't persist after this - DM plugins don't generally send a Content-Length.
                    //TODO: need to change connection: close if there is plugin involved.
                    persistOutgoing = false;
                } else {
#ifdef DGDEBUG
                    std::cout << dbgPeerPort << " -sending body to client" << std::endl;
#endif
       //             syslog(LOG_INFO, " -sending body to client %d", dbgPeerPort);
                    try {docbody.out(&peerconn);} // send doc body to client
                         catch (std::exception &e) {
                             //syslog(LOG_INFO, " -problem sending body to client %d", dbgPeerPort);
                             checkme.pausedtoobig = false;
                         }
                }
//                if (pausedtoobig)
                    //syslog(LOG_INFO, " -sent PARTIAL body to client %d", dbgPeerPort);
                // else
                    //syslog(LOG_INFO, " -sent body to client d", dbgPeerPort);
                //
#ifdef DGDEBUG
                if (checkme.pausedtoobig) {
                    std::cout << dbgPeerPort << " -sent PARTIAL body to client" << std::endl;
                } else {
                    std::cout << dbgPeerPort << " -sent body to client" << std::endl;
                }
#endif
                if (checkme.pausedtoobig && !docbody.dontsendbody) {
#ifdef DGDEBUG
                    std::cout << dbgPeerPort << " -about to start tunnel to send the rest" << std::endl;
#endif
                    fdt.reset();
#ifdef DGDEBUG
                    std::cout << dbgPeerPort << " -1tunnel activated" << std::endl;
#endif
                    if(!fdt.tunnel(proxysock, peerconn, false, docheader.contentLength() - checkme.docsize, true) )
                        persistProxy = false;
                        //  cleanThrow("Error in tunnel 1", peerconn,proxysock);
                    checkme.docsize += fdt.throughput;
                    String rtype(header.requestType());
                    if (!logged && !checkme.nolog) {
                        doLog(clientuser, clientip, checkme.logurl, header.port, checkme.exceptionreason,
                            rtype, checkme.docsize, &checkme.whatIsNaughtyCategories, false, 0, checkme.isexception,
                            docheader.isContentType("text",ldl->fg[filtergroup]), &thestart, checkme.cachehit, docheader.returnCode(), checkme.mimetype,
                            checkme.wasinfected, checkme.wasscanned, checkme.naughtiness, filtergroup, &header, checkme.message_no,
                            checkme.contentmodified, checkme.urlmodified, checkme.headermodified, checkme.headeradded);
                    }
                }
            } else if (!checkme.ishead) {
                // was not supposed to be checked
                fdt.reset();
#ifdef DGDEBUG
                std::cout << dbgPeerPort << " -2tunnel activated" << std::endl;
#endif
                if(!fdt.tunnel(proxysock, peerconn, checkme.isconnect, docheader.contentLength(), true))
                    persistProxy = false;
                   // cleanThrow("Error in tunnel 1", peerconn,proxysock);
#ifdef DGDEBUG
                std::cout << dbgPeerPort << " -2tunnel returned from" << std::endl;
#endif
                checkme.docsize = fdt.throughput;
#ifdef DGDEBUG
                //std::cout << dbgPeerPort << " -docsize after 2tunnel s " << docsize << std::endl;
#endif
                String rtype(header.requestType());
                if (!logged && !checkme.nolog) {
                    doLog(clientuser, clientip, checkme.logurl, header.port, checkme.exceptionreason,
                        rtype, checkme.docsize, &checkme.whatIsNaughtyCategories, false, 0, checkme.isexception,
                        docheader.isContentType("text",ldl->fg[filtergroup]), &thestart, checkme.cachehit, docheader.returnCode(), checkme.mimetype,
                        checkme.wasinfected, checkme.wasscanned, checkme.naughtiness, filtergroup, &header, checkme.message_no,
                        checkme.contentmodified, checkme.urlmodified, checkme.headermodified, checkme.headeradded);
                }
            }

            if (!persistProxy)
                proxysock.close();

        } // while persistOutgoing
    } catch (postfilter_exception &e) {
#ifdef DGDEBUG
        std::cerr << dbgPeerPort << " -connection handler caught a POST filtering exception: " << e.what() << std::endl;
#endif
        if(o.logconerror)
        syslog(LOG_ERR, "POST filtering exception: %s", e.what());

        // close connection to proxy
        proxysock.close();

        return 0;
    } catch (std::exception &e) {
#ifdef DGDEBUG
        std::cerr << dbgPeerPort << " -connection handler caught an exception: " << e.what() << std::endl;
#endif
        if(o.logconerror)
        syslog(LOG_ERR, " -connection handler caught an exception %s" , e.what());

        // close connection to proxy
        proxysock.close();

        return -1;   // to allow calling function to re-create connection handler and hopefully clear any mem errors
    }

    if (!ismitm)
        try {
#ifdef DGDEBUG
            std::cout << dbgPeerPort << " -Attempting graceful connection close" << std::endl;
#endif
            //syslog(LOG_INFO, " -Attempting graceful connection close" );
            int fd = peerconn.getFD();
            if (fd > -1) {
                if (shutdown(fd, SHUT_WR) == 0) {
                    char buff[2];
                    peerconn.readFromSocket(buff, 2, 0, 5000);
                };
            };

            // close connection to the client
            peerconn.close();
            proxysock.close();
        } catch (std::exception &e) {
#ifdef DGDEBUG
            std::cerr << dbgPeerPort << " -connection handler caught an exception on connection closedown: " << e.what() << std::endl;
#endif
            // close connection to the client
            peerconn.close();
            proxysock.close();
        }

    return 0;
}

// decide whether or not to perform logging, categorise the log entry, and write it.
void ConnectionHandler::doLog(std::string &who, std::string &from, String &where, unsigned int &port,
    std::string &what, String &how, off_t &size, std::string *cat, bool isnaughty, int naughtytype,
    bool isexception, bool istext, struct timeval *thestart, bool cachehit,
    int code, std::string &mimetype, bool wasinfected, bool wasscanned, int naughtiness, int filtergroup,
    HTTPHeader *reqheader, int message_no, bool contentmodified, bool urlmodified, bool headermodified, bool headeradded)
{

    // don't log if logging disabled entirely, or if it's an ad block and ad logging is disabled,
    // or if it's an exception and exception logging is disabled
    if (
        (o.ll == 0) || ((cat != NULL) && !o.log_ad_blocks && (strstr(cat->c_str(), "ADs") != NULL)) || ((o.log_exception_hits == 0) && isexception)) {
#ifdef DGDEBUG
        if (o.ll != 0) {
            if (isexception)
                std::cout << dbgPeerPort << " -Not logging exceptions" << std::endl;
            else
                std::cout << dbgPeerPort << " -Not logging 'ADs' blocks" << std::endl;
        }
#endif
        return;
    }

    std::string data, cr("\n");

    if ((isexception && (o.log_exception_hits == 2))
        || isnaughty || o.ll == 3 || (o.ll == 2 && istext)) {
        // put client hostname in log if enabled.
        // for banned & exception IP/hostname matches, we want to output exactly what was matched against,
        // be it hostname or IP - therefore only do lookups here when we don't already have a cached hostname,
        // and we don't have a straight IP match agaisnt the banned or exception IP lists.
        if (o.log_client_hostnames && (clienthost == NULL) && !matchedip && !o.anonymise_logs) {
#ifdef DGDEBUG
            std::cout << "logclienthostnames enabled but reverseclientiplookups disabled; lookup forced." << std::endl;
#endif
            std::deque<String> *names = ipToHostname(from.c_str());
            if (names->size() > 0)
                clienthost = new std::string(names->front().toCharArray());
            delete names;
        }

        // Search 'log-only' domain, url and regexp url lists
        std::string *newcat = NULL;
        if (!cat || cat->length() == 0) {
#ifdef DGDEBUG
            std::cout << dbgPeerPort << " -Checking for log-only categories" << std::endl;
#endif
            const char *c = ldl->fg[filtergroup]->inLogSiteList(where, lastcategory);
#ifdef DGDEBUG
            if (c)
                std::cout << dbgPeerPort << " -Found log-only domain category: " << c << std::endl;
#endif
            if (!c) {
                c = ldl->fg[filtergroup]->inLogURLList(where, lastcategory);
#ifdef DGDEBUG
                if (c)
                    std::cout << dbgPeerPort << " -Found log-only URL category: " << c << std::endl;
#endif
            }
            if (!c) {
                c = ldl->fg[filtergroup]->inLogRegExpURLList(where);
#ifdef DGDEBUG
                if (c)
                    std::cout << dbgPeerPort << " -Found log-only regexp URL category: " << c << std::endl;
#endif
            }
            if (c) {
                newcat = new std::string(c);
                cat = newcat;
            }
        }
#ifdef DGDEBUG
        else
            std::cout << dbgPeerPort << " -Not looking for log-only category; current cat string is: " << *cat << " (" << cat->length() << ")" << std::endl;
#endif

        // Build up string describing POST data parts, if any
        std::ostringstream postdata;
        for (std::list<postinfo>::iterator i = postparts.begin(); i != postparts.end(); ++i) {
            // Replace characters which would break log format with underscores
            std::string::size_type loc = 0;
            while ((loc = i->filename.find_first_of(",;\t ", loc)) != std::string::npos)
                i->filename[loc] = '_';
            // Build up contents of log column
            postdata << i->mimetype << "," << i->filename << "," << i->size
                     << "," << i->blocked << "," << i->storedname << "," << i->bodyoffset << ";";
        }
        postdata << std::flush;

        // Formatting code moved into log_listener in FatController.cpp
        // Original patch by J. Gauthier

        // Item length limit put back to avoid log listener
        // overload with very long urls Philip Pearce Jan 2014
        if ((cat != NULL) && (cat->length() > o.max_logitem_length))
            cat->resize(o.max_logitem_length);
        if (what.length() > o.max_logitem_length)
            what.resize(o.max_logitem_length);
        if (where.length() > o.max_logitem_length)
            where.limitLength(o.max_logitem_length);
        if (o.dns_user_logging && !is_real_user) {
            String user;
            if (getdnstxt(from,user)) {
                who = who + ":" + user;
            };
            is_real_user = true;    // avoid looping on persistent connections
        };

#ifdef DGDEBUG
        std::cout << dbgPeerPort << " -Building raw log data string... ";
#endif

        data = String(isexception) + cr;
        data += (cat ? (*cat) + cr : cr);
        data += String(isnaughty) + cr;
        data += String(naughtytype) + cr;
        data += String(naughtiness) + cr;
        data += where + cr;
        data += what + cr;
        data += how + cr;
        data += who + cr;
        data += from + cr;
        data += String(port) + cr;
        data += String(wasscanned) + cr;
        data += String(wasinfected) + cr;
        data += String(contentmodified) + cr;
        data += String(urlmodified) + cr;
        data += String(headermodified) + cr;
        data += String(size) + cr;
        data += String(filtergroup) + cr;
        data += String(code) + cr;
        data += String(cachehit) + cr;
        data += String(mimetype) + cr;
        data += String((*thestart).tv_sec) + cr;
        data += String((*thestart).tv_usec) + cr;
        data += (clienthost ? (*clienthost) + cr : cr);
        if (o.log_user_agent)
            data += (reqheader ? reqheader->userAgent() + cr : cr);
        else
            data += cr;
        data += urlparams + cr;
        data += postdata.str().c_str() + cr;
        data += String(message_no) + cr;
        data += String(headeradded) + cr;

#ifdef DGDEBUG
        std::cout << dbgPeerPort << " -...built" << std::endl;
#endif

        delete newcat;
// push on log queue
        o.log_Q->push(data);
        // connect to dedicated logging proc
    }
}

// check the request header is OK (client host/user/IP allowed to browse, site not banned, upload not too big)
void ConnectionHandler::requestChecks(HTTPHeader *header, NaughtyFilter *checkme, String *urld, String *url,
    std::string *clientip, std::string *clientuser, int filtergroup,
    bool &isbanneduser, bool &isbannedip, std::string &room)
{
#ifdef DGDEBUG
    std::cout << dbgPeerPort << " -starting request checks" << std::endl;
#endif

    if (isbannedip) {
        (*checkme).isItNaughty = true;
        (*checkme).whatIsNaughtyLog = o.language_list.getTranslation(100);
        (*checkme).message_no = 100;
        // Your IP address is not allowed to web browse:
        (*checkme).whatIsNaughtyLog += clienthost ? *clienthost : *clientip;
        (*checkme).whatIsNaughty = o.language_list.getTranslation(101);
        (*checkme).message_no = 101;
        // Your IP address is not allowed to web browse.
        if (room.empty())
            (*checkme).whatIsNaughtyCategories = o.language_list.getTranslation(103);
        else {
            checkme->whatIsNaughtyCategories = o.language_list.getTranslation(104);
            checkme->whatIsNaughtyLog.append(o.language_list.getTranslation(51));
            checkme->whatIsNaughtyLog.append(room);
        }
        return;
    } else if (isbanneduser) {
        (*checkme).isItNaughty = true;
        (*checkme).whatIsNaughtyLog = o.language_list.getTranslation(102);
        (*checkme).message_no = 102;
        // Your username is not allowed to web browse:
        (*checkme).whatIsNaughtyLog += (*clientuser);
        (*checkme).whatIsNaughty = (*checkme).whatIsNaughtyLog;
        (*checkme).whatIsNaughtyCategories = o.language_list.getTranslation(105);
        return;
    }

    char *i;
    int j;
    String temp;
    temp = (*urld);
    bool is_ssl = header->requestType() == "CONNECT";
    bool is_ip = isIPHostnameStrip(temp);

    // search term blocking - MOVED to after Banned checks

    if (ldl->fg[filtergroup]->enable_regex_grey) {
            if ((j = (*ldl->fg[filtergroup]).inBannedRegExpURLList(temp, lastcategory)) >= 0) {
                (*checkme).isItNaughty = true;
                (*checkme).whatIsNaughtyLog = o.language_list.getTranslation(503);
                (*checkme).message_no = 503;
                // Banned Regular Expression URL:
                (*checkme).whatIsNaughtyLog += (*ldl->fg[filtergroup]).banned_regexpurl_list_source[j].toCharArray();
                (*checkme).whatIsNaughty = o.language_list.getTranslation(504);
                // Banned Regular Expression URL found.
                (*checkme).whatIsNaughtyCategories = (*o.lm.l[(*ldl->fg[filtergroup]).banned_regexpurl_list_ref[j]]).category.toCharArray();
                return;
            } else if ((j = (*ldl->fg[filtergroup]).inBannedRegExpHeaderList(header->header, lastcategory)) >= 0) {
                checkme->isItNaughty = true;
                checkme->whatIsNaughtyLog = o.language_list.getTranslation(508);
                checkme->message_no = 508;
                checkme->whatIsNaughtyLog += ldl->fg[filtergroup]->banned_regexpheader_list_source[j].toCharArray();
                checkme->whatIsNaughty = o.language_list.getTranslation(509);
                checkme->whatIsNaughtyCategories = (*o.lm.l[(*ldl->fg[filtergroup]).banned_regexpheader_list_ref[j]]).category.toCharArray();
                return;
            }
    }

    if (checkme->isItNaughty) { // why bother with checking anything else!!!!
        return;
    }

    if (!(*checkme).isGrey
        && ((*ldl->fg[filtergroup]).inGreySiteList(temp, true, is_ip, is_ssl) || (*ldl->fg[filtergroup]).inGreyURLList(temp, true, is_ip, is_ssl))) {
        (*checkme).isGrey = true;
        if (!(*ldl->fg[filtergroup]).enable_ssl_legacy_logic)
            if (is_ssl)
                (*checkme).isSSLGrey = true;
    }

    // only apply bans to things not in the grey lists
    if (!(*checkme).isGrey) {
        if ((i = (*ldl->fg[filtergroup]).inBannedSiteList(temp, true, is_ip, is_ssl,lastcategory)) != NULL) {
            // need to reintroduce ability to produce the blanket block messages
            (*checkme).whatIsNaughty = o.language_list.getTranslation(500); // banned site
            (*checkme).message_no = 500;
            (*checkme).whatIsNaughty += i;
            (*checkme).whatIsNaughtyLog = (*checkme).whatIsNaughty;
            (*checkme).isItNaughty = true;
            (*checkme).whatIsNaughtyCategories = lastcategory.toCharArray();
            return;
        }
        if ((i = (*ldl->fg[filtergroup]).inBannedURLList(temp, true, is_ip, is_ssl, lastcategory)) != NULL) {
            (*checkme).whatIsNaughty = o.language_list.getTranslation(501);
            (*checkme).message_no = 501;
            // Banned URL:
            (*checkme).whatIsNaughty += i;
            (*checkme).whatIsNaughtyLog = (*checkme).whatIsNaughty;
            (*checkme).isItNaughty = true;
            (*checkme).whatIsNaughtyCategories = lastcategory.toCharArray();
            return;
        }
        // when enable_regex_grey is false no urls will be test against regex
        if (!ldl->fg[filtergroup]->enable_regex_grey) {
            if ((j = (*ldl->fg[filtergroup]).inBannedRegExpURLList(temp, lastcategory)) >= 0) {
                (*checkme).isItNaughty = true;
                (*checkme).whatIsNaughtyLog = o.language_list.getTranslation(503);
                (*checkme).message_no = 503;
                // Banned Regular Expression URL:
                (*checkme).whatIsNaughtyLog += (*ldl->fg[filtergroup]).banned_regexpurl_list_source[j].toCharArray();
                (*checkme).whatIsNaughty = o.language_list.getTranslation(504);
                // Banned Regular Expression URL found.
                (*checkme).whatIsNaughtyCategories = (*o.lm.l[(*ldl->fg[filtergroup]).banned_regexpurl_list_ref[j]]).category.toCharArray();
                return;
            } else if ((j = (*ldl->fg[filtergroup]).inBannedRegExpHeaderList(header->header, lastcategory)) >= 0) {
                checkme->isItNaughty = true;
                checkme->whatIsNaughtyLog = o.language_list.getTranslation(508);
                checkme->message_no = 508;
                checkme->whatIsNaughtyLog += ldl->fg[filtergroup]->banned_regexpheader_list_source[j].toCharArray();
                checkme->whatIsNaughty = o.language_list.getTranslation(509);
                checkme->whatIsNaughtyCategories = (*o.lm.l[(*ldl->fg[filtergroup]).banned_regexpheader_list_ref[j]]).category.toCharArray();
                return;
            }
        }
        // look for URLs within URLs - ban, for example, images originating from banned sites during a Google image search.
        if ((*ldl->fg[filtergroup]).deep_url_analysis) {
#ifdef DGDEBUG
            std::cout << dbgPeerPort << " -starting deep analysis" << std::endl;
#endif
            String deepurl(temp.after("p://"));
            deepurl = header->decode(deepurl, true);
            while (deepurl.contains(":")) {
                deepurl = deepurl.after(":");
                while (deepurl.startsWith(":") || deepurl.startsWith("/")) {
                    deepurl.lop();
                }
#ifdef DGDEBUG
                std::cout << dbgPeerPort << " -deep analysing: " << deepurl << std::endl;
#endif

                if ((*ldl->fg[filtergroup]).enable_local_list) {
                    if (ldl->fg[filtergroup]->inLocalExceptionSiteList(deepurl,false,false,false, lastcategory)
                        || ldl->fg[filtergroup]->inLocalGreySiteList(deepurl)
                        || ldl->fg[filtergroup]->inLocalExceptionURLList(deepurl,false,false, false, lastcategory)
                        || ldl->fg[filtergroup]->inLocalGreyURLList(deepurl)) {
#ifdef DGDEBUG
                        std::cout << "deep site found in exception/grey list; skipping" << std::endl;
#endif
                        continue;
                    }
                    if ((i = (*ldl->fg[filtergroup]).inLocalBannedSiteList(deepurl, false, false, false, lastcategory)) != NULL) {
                        (*checkme).whatIsNaughty = o.language_list.getTranslation(500); // banned site
                        (*checkme).message_no = 500;
                        (*checkme).whatIsNaughty += i;
                        (*checkme).whatIsNaughtyLog = (*checkme).whatIsNaughty;
                        (*checkme).isItNaughty = true;
                        (*checkme).whatIsNaughtyCategories = lastcategory.toCharArray();
#ifdef DGDEBUG
                        std::cout << "deep site: " << deepurl << std::endl;
#endif
                    } else if ((i = (*ldl->fg[filtergroup]).inLocalBannedURLList(deepurl, false, false, false, lastcategory)) != NULL) {
                        (*checkme).whatIsNaughty = o.language_list.getTranslation(501);
                        (*checkme).message_no = 501;
                        // Banned URL:
                        (*checkme).whatIsNaughty += i;
                        (*checkme).whatIsNaughtyLog = (*checkme).whatIsNaughty;
                        (*checkme).isItNaughty = true;
                        (*checkme).whatIsNaughtyCategories = lastcategory.toCharArray();
#ifdef DGDEBUG
                        std::cout << "deep url: " << deepurl << std::endl;
#endif
                    }
                }
                if ((!(*checkme).isItNaughty)) {
                    if (ldl->fg[filtergroup]->inExceptionSiteList(deepurl, false,false,false,lastcategory) || ldl->fg[filtergroup]->inGreySiteList(deepurl)
                        || ldl->fg[filtergroup]->inExceptionURLList(deepurl, false, false, false, lastcategory) || ldl->fg[filtergroup]->inGreyURLList(deepurl))

                    {
#ifdef DGDEBUG
                        std::cout << dbgPeerPort << " -deep site found in exception/grey list; skipping" << std::endl;
#endif
                        continue;
                    } else if ((i = (*ldl->fg[filtergroup]).inBannedSiteList(deepurl, false, false, false, lastcategory)) != NULL) {
                        (*checkme).whatIsNaughty = o.language_list.getTranslation(500); // banned site
                        (*checkme).whatIsNaughty += i;
                        (*checkme).whatIsNaughtyLog = (*checkme).whatIsNaughty;
                        (*checkme).message_no = 500;
                        (*checkme).isItNaughty = true;
                        (*checkme).whatIsNaughtyCategories = lastcategory.toCharArray();
#ifdef DGDEBUG
                        std::cout << dbgPeerPort << " -deep site: " << deepurl << std::endl;
#endif
                    } else if ((i = (*ldl->fg[filtergroup]).inBannedURLList(deepurl, false, false, false, lastcategory)) != NULL) {
                        (*checkme).whatIsNaughty = o.language_list.getTranslation(501);
                        (*checkme).message_no = 501;
                        // Banned URL:
                        (*checkme).whatIsNaughty += i;
                        (*checkme).whatIsNaughtyLog = (*checkme).whatIsNaughty;
                        (*checkme).isItNaughty = true;
                        (*checkme).whatIsNaughtyCategories = lastcategory.toCharArray();
#ifdef DGDEBUG
                        std::cout << dbgPeerPort << " -deep url: " << deepurl << std::endl;
#endif
                    }
                }
            }
#ifdef DGDEBUG
            std::cout << dbgPeerPort << " -done deep analysis" << std::endl;
#endif

            if ((*checkme).isItNaughty) {
                return;
            }
        }
    }

    // if we get here it's to be content checked (so far!)
    // So now check for search etc.

    // search term blocking - apply even to things in grey lists, as it's a form of content filtering
    // don't bother with SSL sites, though.  note that we must pass in the non-hex-decoded URL in
    // order for regexes to be able to split up parameters reliably.
    if (!is_ssl) {

        (*checkme).isSearch = (*header).isSearch(ldl->fg[filtergroup]);
        if ((*checkme).isSearch) {
            if ((i = (*ldl->fg[filtergroup]).inBannedSearchList((*header).searchwords(), lastcategory)) != NULL) {
                (*checkme).whatIsNaughty = o.language_list.getTranslation(521);
                (*checkme).message_no = 521;
                // Banned search term:
                (*checkme).whatIsNaughty += i;
                (*checkme).whatIsNaughtyLog = (*checkme).whatIsNaughty;
                (*checkme).isItNaughty = true;
                (*checkme).whatIsNaughtyCategories = lastcategory.toCharArray();
                return;
            }
        }

        if ((*checkme).isSearch) {
            String terms;
            terms = (*header).searchterms();
            // search terms are URL parameter type "0"
            urlparams.append("0=").append(terms).append(";");
            if (ldl->fg[filtergroup]->searchterm_limit > 0) {
                // Add spaces at beginning and end of block before filtering, so
                // that the quick & dirty trick of putting spaces around words
                // (Scunthorpe problem) can still be used, bearing in mind the block
                // of text here is usually very small.
                terms.insert(terms.begin(), ' ');
                terms.append(" ");
                checkme->checkme(terms.c_str(), terms.length(), NULL, NULL, ldl->fg[filtergroup],
                    (ldl->fg[filtergroup]->searchterm_flag ? ldl->fg[filtergroup]->searchterm_list : ldl->fg[filtergroup]->banned_phrase_list),
                    ldl->fg[filtergroup]->searchterm_limit, true);
                if (checkme->isItNaughty) {
                    checkme->blocktype = 2;
                    return;
                }
            }
        }
    }

#ifdef __SSLMITM
    //check the certificate if
    //its a connect,
    //ssl cert checking is enabled,
    //ssl mitm is disabled (will get checked by that anyway)
    //its a connection to port 443
    if (is_ssl && !((*checkme).isItNaughty) && (*ldl->fg[filtergroup]).ssl_check_cert && !(*ldl->fg[filtergroup]).ssl_mitm && (*header).port == 443) {

#ifdef DGDEBUG
        std::cout << dbgPeerPort << " -checking SSL certificate" << std::endl;
#endif

        Socket ssl_sock;
        //connect to the local proxy then do a connect
        //to make sure we go through any upstream proxys
        if (ssl_sock.connect(o.proxy_ip.c_str(), o.proxy_port) < 0) {
            //(*checkme).whatIsNaughty = "Could not connect to proxy server" ;
            (*checkme).message_no = 159;
            (*checkme).whatIsNaughty = o.language_list.getTranslation(159);
            (*checkme).whatIsNaughtyLog = (*checkme).whatIsNaughty;
            (*checkme).isItNaughty = true;
            (*checkme).whatIsNaughtyCategories = o.language_list.getTranslation(70);
#ifdef DGDEBUG
            syslog(LOG_ERR, "error opening socket\n");
            std::cout << dbgPeerPort << " -couldnt connect to proxy for ssl certificate checks. failed with error " << strerror(errno) << std::endl;
#endif
            return;
        }

#ifdef DGDEBUG
        std::cout << dbgPeerPort << " -connected to proxy" << std::endl;
#endif

        //create tunnel to destination
        String hostname(temp);
        hostname.removePTP();
        int rc = sendProxyConnect(hostname, &ssl_sock, checkme);
        if (rc < 0) {
            return;
        }
#ifdef DGDEBUG
        std::cout << dbgPeerPort << " -created tunnel through proxy with rc: " << rc << std::endl;
#endif
        //start an ssl client
        std::string certpath(o.ssl_certificate_path.c_str());
        if (ssl_sock.startSslClient(certpath,hostname) < 0) {
            (*checkme).whatIsNaughty = "Could not open ssl connection";
            (*checkme).whatIsNaughtyLog = (*checkme).whatIsNaughty;
            (*checkme).isItNaughty = true;
            (*checkme).whatIsNaughtyCategories = "SSL Site";
#ifdef DGDEBUG
            syslog(LOG_ERR, "error opening ssl connection\n");
            std::cout << dbgPeerPort << " -couldnt connect ssl server to check certificate. failed with error " << strerror(errno) << std::endl;
#endif
            return;
        }
        checkCertificate(hostname, &ssl_sock, checkme);

#ifdef DGDEBUG
        std::cout << dbgPeerPort << " -done checking SSL certificate" << std::endl;
#endif
    }
#endif //__SSLMITM
}

// check the request header is OK (client host/user/IP allowed to browse, site not banned, upload not too big)
void ConnectionHandler::requestLocalChecks(HTTPHeader *header, NaughtyFilter *checkme, String *urld, String *url,
    std::string *clientip, std::string *clientuser, int filtergroup,
    bool &isbanneduser, bool &isbannedip, std::string &room)
{
#ifdef DGDEBUG
    std::cout << dbgPeerPort << " -starting local checks" << std::endl;
#endif

    if (isbannedip) {
        (*checkme).isItNaughty = true;
        (*checkme).whatIsNaughtyLog = o.language_list.getTranslation(100);
        (*checkme).message_no = 100;
        // Your IP address is not allowed to web browse:
        (*checkme).whatIsNaughtyLog += clienthost ? *clienthost : *clientip;
        (*checkme).whatIsNaughty = o.language_list.getTranslation(101);
        (*checkme).message_no = 101;
        // Your IP address is not allowed to web browse.
        if (room.empty())
            (*checkme).whatIsNaughtyCategories = "Banned Client IP";
        else {
            checkme->whatIsNaughtyCategories = "Banned Room";
            checkme->whatIsNaughtyLog.append(" in ");
            checkme->whatIsNaughtyLog.append(room);
        }
        return;
    } else if (isbanneduser) {
        (*checkme).isItNaughty = true;
        (*checkme).whatIsNaughtyLog = o.language_list.getTranslation(102);
        (*checkme).message_no = 102;
        // Your username is not allowed to web browse:
        (*checkme).whatIsNaughtyLog += (*clientuser);
        (*checkme).whatIsNaughty = (*checkme).whatIsNaughtyLog;
        (*checkme).whatIsNaughtyCategories = "Banned User";
        return;
    }

    char *i;
    int j;
    String temp;
    temp = (*urld);
    bool is_ssl = header->requestType() == "CONNECT";
    bool is_ip = isIPHostnameStrip(temp);

    // search term blocking - MOVED to after Banned checks

    if (!(*checkme).isGrey
        && ((*ldl->fg[filtergroup]).inLocalGreySiteList(temp, true, is_ip, is_ssl) || (*ldl->fg[filtergroup]).inLocalGreyURLList(temp, true, is_ip, is_ssl))) {
        (*checkme).isGrey = true;
        if (is_ssl)
            (*checkme).isSSLGrey = true;
    }

    // only apply bans to things not in the grey lists
    if (!(*checkme).isGrey) {
        if ((i = (*ldl->fg[filtergroup]).inLocalBannedSiteList(temp, true, is_ip, is_ssl, lastcategory)) != NULL) {
            // need to reintroduce ability to produce the blanket block messages
            (*checkme).whatIsNaughty = o.language_list.getTranslation(560); // banned site
            (*checkme).message_no = 560;
            (*checkme).whatIsNaughty += i;
            (*checkme).whatIsNaughtyLog = (*checkme).whatIsNaughty;
            (*checkme).isItNaughty = true;
            (*checkme).whatIsNaughtyCategories = lastcategory.toCharArray();
            return;
        }
        if ((i = (*ldl->fg[filtergroup]).inLocalBannedURLList(temp, true, is_ip, is_ssl, lastcategory)) != NULL) {
            (*checkme).whatIsNaughty = o.language_list.getTranslation(561);
            (*checkme).message_no = 561;
            // Banned URL:
            (*checkme).whatIsNaughty += i;
            (*checkme).whatIsNaughtyLog = (*checkme).whatIsNaughty;
            (*checkme).isItNaughty = true;
            (*checkme).whatIsNaughtyCategories = lastcategory.toCharArray();
            return;
        }

        // look for URLs within URLs - ban, for example, images originating from banned sites during a Google image search.
        // done in main requestChecks
    }

    // if we get here it's to be content checked (so far!)
    // So now check for search etc.

    // NOTE dg/protex search term stuff needs combining!!!!

    // search term blocking - apply even to things in grey lists, as it's a form of content filtering
    // don't bother with SSL sites, though.  note that we must pass in the non-hex-decoded URL in
    // order for regexes to be able to split up parameters reliably.
    if (!is_ssl) {
        (*checkme).isSearch = (*header).isSearch(ldl->fg[filtergroup]);
        if ((*checkme).isSearch) {
            if ((i = (*ldl->fg[filtergroup]).inLocalBannedSearchList((*header).searchwords(), lastcategory)) != NULL) {
                (*checkme).whatIsNaughty = o.language_list.getTranslation(581);
                (*checkme).message_no = 581;
                // Banned search term:
                (*checkme).whatIsNaughty += i;
                (*checkme).whatIsNaughtyLog = (*checkme).whatIsNaughty;
                (*checkme).isItNaughty = true;
                (*checkme).whatIsNaughtyCategories = lastcategory.toCharArray();
                return;
            }
        }

        // dg code
        String terms;
        if ((*checkme).isSearch) {
            terms = (*header).searchterms();
            // search terms are URL parameter type "0"
            urlparams.append("0=").append(terms).append(";");
            if (ldl->fg[filtergroup]->searchterm_limit > 0) {
                // Add spaces at beginning and end of block before filtering, so
                // that the quick & dirty trick of putting spaces around words
                // (Scunthorpe problem) can still be used, bearing in mind the block
                // of text here is usually very small.
                terms.insert(terms.begin(), ' ');
                terms.append(" ");
                checkme->checkme(terms.c_str(), terms.length(), NULL, NULL, ldl->fg[filtergroup],
                    (ldl->fg[filtergroup]->searchterm_flag ? ldl->fg[filtergroup]->searchterm_list : ldl->fg[filtergroup]->banned_phrase_list),
                    ldl->fg[filtergroup]->searchterm_limit, true);
                if (checkme->isItNaughty) {
                    checkme->blocktype = 2;
                    return;
                }
            }
        }
    }
}

// check if embeded url trusted referer
bool ConnectionHandler::embededRefererChecks(HTTPHeader *header, String *urld, String *url,
    int filtergroup)
{

    char *i;
    int j;
    String temp;
    temp = (*urld);
    temp.hexDecode();

    if (ldl->fg[filtergroup]->inRefererExceptionLists(header->getReferer())) {
        return true;
    }
#ifdef DGDEBUG
    std::cout << dbgPeerPort << " -checking for embed url in " << temp << std::endl;
#endif

    if (ldl->fg[filtergroup]->inEmbededRefererLists(temp)) {

// look for referer URLs within URLs
#ifdef DGDEBUG
        std::cout << dbgPeerPort << " -starting embeded referer deep analysis" << std::endl;
#endif
        String deepurl(temp.after("p://"));
        deepurl = header->decode(deepurl, true);
        while (deepurl.contains(":")) {
            deepurl = deepurl.after(":");
            while (deepurl.startsWith(":") || deepurl.startsWith("/")) {
                deepurl.lop();
            }

            if (ldl->fg[filtergroup]->inRefererExceptionLists(deepurl)) {
#ifdef DGDEBUG
                std::cout << "deep site found in trusted referer list; " << std::endl;
#endif
                return true;
            }
        }
#ifdef DGDEBUG
        std::cout << dbgPeerPort << " -done embdeded referer deep analysis" << std::endl;
#endif
    }
    return false;
}

// based on patch by Aecio F. Neto (afn@harvest.com.br) - Harvest Consultoria (http://www.harvest.com.br)
// show the relevant banned page/image/CGI based on report level setting, request type etc.
bool ConnectionHandler::denyAccess(Socket *peerconn, Socket *proxysock, HTTPHeader *header, HTTPHeader *docheader,
    String *url, NaughtyFilter *checkme, std::string *clientuser, std::string *clientip, int filtergroup,
    bool ispostblock, int headersent, bool wasinfected, bool scanerror, bool forceshow)
{
    int reporting_level = ldl->fg[filtergroup]->reporting_level;
#ifdef DGDEBUG

    std::cout << dbgPeerPort << " -reporting level is " << reporting_level << std::endl;

#endif

    try { // writestring throws exception on error/timeout

        // flags to enable filter/infection bypass hash generation
        bool filterhash = false;
        bool virushash = false;
        // flag to enable internal generation of hashes (i.e. obey the "-1" setting; to allow the modes but disable hash generation)
        // (if disabled, just output '1' or '2' to show that the CGI should generate a filter/virus bypass hash;
        // otherwise, real hashes get put into substitution variables/appended to the ban CGI redirect URL)
        bool dohash = false;
        if (reporting_level > 0) {
            // generate a filter bypass hash
            if (!wasinfected && ((*ldl->fg[filtergroup]).bypass_mode != 0) && !ispostblock) {
#ifdef DGDEBUG
                std::cout << dbgPeerPort << " -Enabling filter bypass hash generation" << std::endl;
#endif
                filterhash = true;
                if (ldl->fg[filtergroup]->bypass_mode > 0)
                    dohash = true;
            }
            // generate an infection bypass hash
            else if (wasinfected && (*ldl->fg[filtergroup]).infection_bypass_mode != 0) {
                // only generate if scanerror (if option to only bypass scan errors is enabled)
                if ((*ldl->fg[filtergroup]).infection_bypass_errors_only ? scanerror : true) {
#ifdef DGDEBUG
                    std::cout << dbgPeerPort << " -Enabling infection bypass hash generation" << std::endl;
#endif
                    virushash = true;
                    if (ldl->fg[filtergroup]->infection_bypass_mode > 0)
                        dohash = true;
                }
            }
        }

// the user is using the full whack of custom banned images and/or HTML templates
#ifdef __SSLMITM
        if (reporting_level == 3 || (headersent > 0 && reporting_level > 0) || forceshow)
#else
        if (reporting_level == 3 || (headersent > 0 && reporting_level > 0))
#endif
        {
            // if reporting_level = 1 or 2 and headersent then we can't
            // send a redirect so we have to display the template instead

            (*proxysock).close(); // finished with proxy
            if(!(*peerconn).breadyForOutput(o.proxy_timeout))
                cleanThrow("Error sending to client 3648", *peerconn,*proxysock);
            //(*peerconn).readyForOutput(o.proxy_timeout);

#ifdef __SSLMITM
            if ((*header).requestType().startsWith("CONNECT") && !(*peerconn).isSsl())
#else
            if ((*header).requestType().startsWith("CONNECT"))
#endif
                {
                // if it's a CONNECT then headersent can't be set
                // so we don't need to worry about it

                // if preemptive banning is not in place then a redirect
                // is not guaranteed to ban the site so we have to write
                // an access denied page.  Unfortunately IE does not
                // work with access denied pages on SSL more than a few
                // hundred bytes so we have to use a crap boring one
                // instead.  Nothing can be done about it - blame
                // mickysoft.
                //
                // FredB 2013
                // Wrong Microsoft is right, no data will be accepted without hand shake
                // This is a Man in the middle problem with Firefox and IE (can't rewrite a ssl page)
                // 307 redirection Fix the problem for Firefox - only ? -
                // TODO: I guess the right thing to do should be a - SSL - DENIED Webpage 307 redirect and direct"

                if (ldl->fg[filtergroup]->sslaccess_denied_address.length() != 0) {
                    // grab either the full category list or the thresholded list
                    std::string cats;
                    cats = checkme->usedisplaycats ? checkme->whatIsNaughtyDisplayCategories : checkme->whatIsNaughtyCategories;
                    String hashed;
                    // generate valid hash locally if enabled
                    if (dohash) {
                        hashed = hashedURL(url, filtergroup, clientip, virushash);
                    }
                    // otherwise, just generate flags showing what to generate
                    else if (filterhash) {
                        hashed = "1";
                    } else if (virushash) {
                        hashed = "2";
                    }

                    String writestring("HTTP/1.1 307 Temporary Redirect\n");
                    writestring += "Location: ";
                    writestring += ldl->fg[filtergroup]->sslaccess_denied_address; // banned site for ssl
                    if (ldl->fg[filtergroup]->non_standard_delimiter) {
                        writestring += "?DENIEDURL==";
                        writestring += miniURLEncode((*url).toCharArray()).c_str();
                        writestring += "::IP==";
                        writestring += (*clientip).c_str();
                        writestring += "::USER==";
                        writestring += (*clientuser).c_str();
                        if (clienthost != NULL) {
                            writestring += "::HOST==";
                            writestring += clienthost->c_str();
                        }
                        writestring += "::CATEGORIES==";
                        writestring += miniURLEncode(cats.c_str()).c_str();
                        writestring += "::REASON==";
                    } else {
                        writestring += "?DENIEDURL=";
                        writestring += miniURLEncode((*url).toCharArray()).c_str();
                        writestring += "&IP=";
                        writestring += (*clientip).c_str();
                        writestring += "&USER=";
                        writestring += (*clientuser).c_str();
                        if (clienthost != NULL) {
                            writestring += "&HOST=";
                            writestring += clienthost->c_str();
                        }
                        writestring += "&CATEGORIES=";
                        writestring += miniURLEncode(cats.c_str()).c_str();
                        writestring += "&REASON=";
                    }

                    if (reporting_level == 1) {
                        writestring += miniURLEncode((*checkme).whatIsNaughty.c_str()).c_str();
                    } else {
                        writestring += miniURLEncode((*checkme).whatIsNaughtyLog.c_str()).c_str();
                    }
                    writestring += "\nContent-Length: 0";
                    writestring += "\nCache-control: no-cache";
                    writestring += "\nConnection: close\n";
                        (*peerconn).writeString(writestring.toCharArray());   // ignore errors
#ifdef DGDEBUG // debug stuff surprisingly enough
                    std::cout << dbgPeerPort << " -******* redirecting to:" << std::endl;
                    std::cout << dbgPeerPort << writestring << std::endl;
                    std::cout << dbgPeerPort << " -*******" << std::endl;
#endif
                } else {
                    // Broken, sadly blank page for user
                    // See comment above HTTPS
                    String writestring("HTTP/1.0 403 ");
                    writestring += o.language_list.getTranslation(500); // banned site
                    writestring += "\nContent-Type: text/html\n\n<HTML><HEAD><TITLE>e2guardian - ";
                    writestring += o.language_list.getTranslation(500); // banned site
                    writestring += "</TITLE></HEAD><BODY><H1>e2guardian - ";
                    writestring += o.language_list.getTranslation(500); // banned site
                    writestring += "</H1>";
                    writestring += (*url);
                    writestring += "</BODY></HTML>\n";
                        (*peerconn).writeString(writestring.toCharArray());     // ignore errors
                }

            } else {
                // we're dealing with a non-SSL'ed request, and have the option of using the custom banned image/page directly
                bool replaceimage = false;
                bool replaceflash = false;
                if (o.use_custom_banned_image) {

                    // It would be much nicer to do a mime comparison
                    // and see if the type is image/* but the header
                    // never (almost) gets back from squid because
                    // it gets denied before then.
                    // This method is prone to over image replacement
                    // but will work most of the time.

                    String lurl((*url));
                    lurl.toLower();
                    if (lurl.endsWith(".gif") || lurl.endsWith(".jpg") || lurl.endsWith(".jpeg") || lurl.endsWith(".jpe")
                        || lurl.endsWith(".png") || lurl.endsWith(".bmp") || (*docheader).isContentType("image/",ldl->fg[filtergroup])) {
                        replaceimage = true;
                    }
                }

                if (o.use_custom_banned_flash) {
                    String lurl((*url));
                    lurl.toLower();
                    if (lurl.endsWith(".swf") || (*docheader).isContentType("application/x-shockwave-flash",ldl->fg[filtergroup])) {
                        replaceflash = true;
                    }
                }

                // if we're denying an image request, show the image; otherwise, show the HTML page.
                // (or advanced ad block page, or HTML page with bypass URLs)
                if (replaceimage) {
                    if (headersent == 0) {
                        (*peerconn).writeString("HTTP/1.0 200 OK\n");  // ignore errors
                    }
                    o.banned_image.display(peerconn);
                } else if (replaceflash) {
                    if (headersent == 0) {
                        (*peerconn).writeString("HTTP/1.0 200 OK\n");  // ignore errors
                    }
                    o.banned_flash.display(peerconn);
                } else {
                    // advanced ad blocking - if category contains ADs, wrap ad up in an "ad blocked" message,
                    // which provides a link to the original URL if you really want it. primarily
                    // for IFRAMEs, which will end up containing this link instead of the ad (standard non-IFRAMEd
                    // ad images still get image-replaced.)
                    if (strstr(checkme->whatIsNaughtyCategories.c_str(), "ADs") != NULL) {
                        String writestring("HTTP/1.0 200 ");
                        writestring += o.language_list.getTranslation(1101); // advert blocked
                        writestring += "\nContent-Type: text/html\n\n<HTML><HEAD><TITLE>E2guardian - ";
                        writestring += o.language_list.getTranslation(1101); // advert blocked
                        writestring += "</TITLE></HEAD><BODY><CENTER><FONT SIZE=\"-1\"><A HREF=\"";
                        writestring += (*url);
                        writestring += "\" TARGET=\"_BLANK\">";
                        writestring += o.language_list.getTranslation(1101); // advert blocked
                        writestring += "</A></FONT></CENTER></BODY></HTML>\n";
                            (*peerconn).writeString(writestring.toCharArray());  // ignore errors
                    }

                    // Mod by Ernest W Lessenger Mon 2nd February 2004
                    // Other bypass code mostly written by Ernest also
                    // create temporary bypass URL to show on denied page
                    else {
                        String hashed;
                        // generate valid hash locally if enabled
                        if (dohash) {
                            hashed = hashedURL(url, filtergroup, clientip, virushash);
                        }
                        // otherwise, just generate flags showing what to generate
                        else if (filterhash) {
                            hashed = "HASH=1";
                        } else if (virushash) {
                            hashed = "HASH=2";
                        }

                        if (headersent == 0) {
                            (*peerconn).writeString("HTTP/1.0 200 OK\n");  // ignore errors
                        }
                        if (headersent < 2) {
                            (*peerconn).writeString("Content-type: text/html\n\n");  // ignore errors
                        }
                        // if the header has been sent then likely displaying the
                        // template will break the download, however as this is
                        // only going to be happening if the unsafe trickle
                        // buffer method is used and we want the download to be
                        // broken we don't mind too much
                        String fullurl = header->getLogUrl(true);
                        ldl->fg[filtergroup]->getHTMLTemplate()->display(peerconn,
                            &fullurl, (*checkme).whatIsNaughty, (*checkme).whatIsNaughtyLog,
                            // grab either the full category list or the thresholded list
                            (checkme->usedisplaycats ? checkme->whatIsNaughtyDisplayCategories : checkme->whatIsNaughtyCategories),
                            clientuser, clientip, clienthost, filtergroup, ldl->fg[filtergroup]->name, hashed);
                    }
                }
            }
        }

        // the user is using the CGI rather than the HTML template - so issue a redirect with parameters filled in on GET string
        else if (reporting_level > 0) {
            // grab either the full category list or the thresholded list
            std::string cats;
            cats = checkme->usedisplaycats ? checkme->whatIsNaughtyDisplayCategories : checkme->whatIsNaughtyCategories;

            String hashed;
            // generate valid hash locally if enabled
            if (dohash) {
                hashed = hashedURL(url, filtergroup, clientip, virushash);
            }
            // otherwise, just generate flags showing what to generate
            else if (filterhash) {
                hashed = "1";
            } else if (virushash) {
                hashed = "2";
            }

//            (*proxysock).close(); // finshed with proxy
            if(!(*peerconn).breadyForOutput(o.proxy_timeout))
                cleanThrow("Error sending to client 3877", *peerconn,*proxysock);
            //(*peerconn).readyForOutput(o.proxy_timeout);
            if ((*checkme).whatIsNaughty.length() > 2048) {
                (*checkme).whatIsNaughty = String((*checkme).whatIsNaughty.c_str()).subString(0, 2048).toCharArray();
            }
            if ((*checkme).whatIsNaughtyLog.length() > 2048) {
                (*checkme).whatIsNaughtyLog = String((*checkme).whatIsNaughtyLog.c_str()).subString(0, 2048).toCharArray();
            }
            String writestring("HTTP/1.0 302 Redirect\n");
            writestring += "Location: ";
            writestring += ldl->fg[filtergroup]->access_denied_address;

            if (ldl->fg[filtergroup]->non_standard_delimiter) {
                writestring += "?DENIEDURL==";
                writestring += miniURLEncode((*url).toCharArray()).c_str();
                writestring += "::IP==";
                writestring += (*clientip).c_str();
                writestring += "::USER==";
                writestring += (*clientuser).c_str();
                if (clienthost != NULL) {
                    writestring += "::HOST==";
                    writestring += clienthost->c_str();
                }
                writestring += "::CATEGORIES==";
                writestring += miniURLEncode(cats.c_str()).c_str();
                if (virushash || filterhash) {
                    // output either a genuine hash, or just flags
                    if (dohash) {
                        writestring += "::";
                        writestring += hashed.before("=").toCharArray();
                        writestring += "==";
                        writestring += hashed.after("=").toCharArray();
                    } else {
                        writestring += "::HASH==";
                        writestring += hashed.toCharArray();
                    }
                }
                writestring += "::REASON==";
            } else {
                writestring += "?DENIEDURL=";
                writestring += miniURLEncode((*url).toCharArray()).c_str();
                writestring += "&IP=";
                writestring += (*clientip).c_str();
                writestring += "&USER=";
                writestring += (*clientuser).c_str();
                if (clienthost != NULL) {
                    writestring += "&HOST=";
                    writestring += clienthost->c_str();
                }
                writestring += "&CATEGORIES=";
                writestring += miniURLEncode(cats.c_str()).c_str();
                if (virushash || filterhash) {
                    // output either a genuine hash, or just flags
                    if (dohash) {
                        writestring += "&";
                        writestring += hashed.toCharArray();
                    } else {
                        writestring += "&HASH=";
                        writestring += hashed.toCharArray();
                    }
                }
                writestring += "&REASON=";
            }
            if (reporting_level == 1) {
                writestring += miniURLEncode((*checkme).whatIsNaughty.c_str()).c_str();
            } else {
                writestring += miniURLEncode((*checkme).whatIsNaughtyLog.c_str()).c_str();
            }
            writestring += "\n\n";
            if(!(*peerconn).writeString(writestring.toCharArray()))
                cleanThrow("Error sending block screen to client", *peerconn, *proxysock);
#ifdef DGDEBUG // debug stuff surprisingly enough
            std::cout << dbgPeerPort << " -******* redirecting to:" << std::endl;
            std::cout << dbgPeerPort << writestring << std::endl;
            std::cout << dbgPeerPort << " -*******" << std::endl;
#endif
        }

        // the user is using the barebones banned page
        else if (reporting_level == 0) {
            (*proxysock).close(); // finshed with proxy
            String writestring("HTTP/1.0 200 OK\n");
            writestring += "Content-type: text/html\n\n";
            writestring += "<HTML><HEAD><TITLE>e2guardian - ";
            writestring += o.language_list.getTranslation(1); // access denied
            writestring += "</TITLE></HEAD><BODY><CENTER><H1>e2guardian - ";
            writestring += o.language_list.getTranslation(1); // access denied
            writestring += "</H1></CENTER></BODY></HTML>";
            if(!(*peerconn).breadyForOutput(o.proxy_timeout))
                cleanThrow("Error sending to client 3965", *peerconn,*proxysock);
            //(*peerconn).readyForOutput(o.proxy_timeout);
            if(!(*peerconn).writeString(writestring.toCharArray()))
                cleanThrow("Error sending to client 3974", *peerconn,*proxysock);
#ifdef DGDEBUG // debug stuff surprisingly enough
            std::cout << dbgPeerPort << " -******* displaying:" << std::endl;
            std::cout << dbgPeerPort << writestring << std::endl;
            std::cout << dbgPeerPort << " -*******" << std::endl;
#endif
        }

        // stealth mode
        else if (reporting_level == -1) {
            (*checkme).isItNaughty = false; // dont block
        }
    } catch (std::exception &e) {
    }

    // we blocked the request, so flush the client connection & close the proxy connection.
    if ((*checkme).isItNaughty) {
            (*peerconn).breadyForOutput(o.proxy_timeout); //as best a flush as I can
        (*proxysock).close(); // close connection to proxy
        // we said no to the request, so return true, indicating exit the connhandler
        ldl.reset();
        return true;
    }
    ldl.reset();
    return false;
}    // end of deny request loop

// do content scanning (AV filtering) and naughty filtering
void ConnectionHandler::contentFilter(HTTPHeader *docheader, HTTPHeader *header, DataBuffer *docbody,
    Socket *proxysock, Socket *peerconn, int *headersent, bool *pausedtoobig, off_t *docsize, NaughtyFilter *checkme,
    bool wasclean, int filtergroup, std::deque<CSPlugin *> &responsescanners,
    std::string *clientuser, std::string *clientip, bool *wasinfected, bool *wasscanned, bool isbypass,
    String &url, String &domain, bool *scanerror, bool &contentmodified, String *csmessage)
{
    int rc = 0;

    proxysock->bcheckForInput(120000);
    bool compressed = docheader->isCompressed();
    if (compressed) {
#ifdef DGDEBUG
        std::cout << dbgPeerPort << " -Decompressing as we go....." << std::endl;
#endif
        docbody->setDecompress(docheader->contentEncoding());
    }
#ifdef DGDEBUG
    std::cout << dbgPeerPort << docheader->contentEncoding() << std::endl;
    std::cout << dbgPeerPort << " -about to get body from proxy" << std::endl;
#endif
    (*pausedtoobig) = docbody->in(proxysock, peerconn, header, docheader, !responsescanners.empty(), headersent); // get body from proxy
// checkme: surely if pausedtoobig is true, we just want to break here?
// the content is larger than max_content_filecache_scan_size if it was downloaded for scanning,
// and larger than max_content_filter_size if not.
// in fact, why don't we check the content length (when it's not -1) before even triggering the download managers?
#ifdef DGDEBUG
    if ((*pausedtoobig)) {
        std::cout << dbgPeerPort << " -got PARTIAL body from proxy" << std::endl;
    } else {
        std::cout << dbgPeerPort << " -got body from proxy" << std::endl;
    }
#endif
    off_t dblen;
    bool isfile = false;
    if (docbody->tempfilesize > 0) {
        dblen = docbody->tempfilesize;
        isfile = true;
    } else {
        dblen = docbody->buffer_length;
    }
    // don't scan zero-length buffers (waste of AV resources, especially with external scanners (ICAP)).
    // these were encountered browsing opengroup.org, caused by a stats script. (PRA 21/09/2005)
    // if we wanted to honour a hypothetical min_content_scan_size, we'd do it here.
    if (((*docsize) = dblen) == 0) {
#ifdef DGDEBUG
        std::cout << dbgPeerPort << " -Not scanning zero-length body" << std::endl;
#endif
        // it's not inconceivable that we received zlib or gzip encoded content
        // that is, after decompression, zero length. we need to cater for this.
        // seen on SW's internal MediaWiki.
        docbody->swapbacktocompressed();
        return;
    }

    if (!wasclean) { // was not clean or no urlcache

        // fixed to obey maxcontentramcachescansize
        if (!responsescanners.empty() && (isfile ? dblen <= o.max_content_filecache_scan_size : dblen <= o.max_content_ramcache_scan_size)) {
            int csrc = 0;
#ifdef DGDEBUG
            int k = 0;
#endif
            for (std::deque<CSPlugin *>::iterator i = responsescanners.begin(); i != responsescanners.end(); i++) {
                (*wasscanned) = true;
                if (isfile) {
#ifdef DGDEBUG
                    std::cout << dbgPeerPort << " -Running scanFile" << std::endl;
#endif
                    csrc = (*i)->scanFile(header, docheader, clientuser->c_str(), ldl->fg[filtergroup], clientip->c_str(), docbody->tempfilepath.toCharArray(), checkme);
                    if ((csrc != DGCS_CLEAN) && (csrc != DGCS_WARNING)) {
                        unlink(docbody->tempfilepath.toCharArray());
                        // delete infected (or unscanned due to error) file straight away
                    }
                } else {
#ifdef DGDEBUG
                    std::cout << dbgPeerPort << " -Running scanMemory" << std::endl;
#endif
                    csrc = (*i)->scanMemory(header, docheader, clientuser->c_str(), ldl->fg[filtergroup], clientip->c_str(), docbody->data, docbody->buffer_length, checkme);
                }
#ifdef DGDEBUG
                std::cerr << dbgPeerPort << " -AV scan " << k << " returned: " << csrc << std::endl;
#endif
                if (csrc == DGCS_WARNING) {
                    // Scanner returned a warning. File wasn't infected, but wasn't scanned properly, either.
                    (*wasscanned) = false;
                    (*scanerror) = false;
#ifdef DGDEBUG
                    std::cout << dbgPeerPort << (*i)->getLastMessage() << std::endl;
#endif
                    (*csmessage) = (*i)->getLastMessage();
                } else if (csrc == DGCS_BLOCKED) {
                    (*wasscanned) = true;
                    (*scanerror) = false;
                    break;
                } else if (csrc == DGCS_INFECTED) {
                    (*wasinfected) = true;
                    (*scanerror) = false;
                    break;
                }
                //if its not clean / we errored then treat it as infected
                else if (csrc != DGCS_CLEAN) {
                    if (csrc < 0) {
                        syslog(LOG_ERR, "Unknown return code from content scanner: %d", csrc);
                    } else {
                        syslog(LOG_ERR, "scanFile/Memory returned error: %d", csrc);
                    }
                    //TODO: have proper error checking/reporting here?
                    //at the very least, integrate with the translation system.
                    //checkme->whatIsNaughty = "WARNING: Could not perform content scan!";
                    checkme->message_no = 1203;
                    checkme->whatIsNaughty = o.language_list.getTranslation(1203);
                    checkme->whatIsNaughtyLog = (*i)->getLastMessage().toCharArray();
                    //checkme->whatIsNaughtyCategories = "Content scanning";
                    checkme->whatIsNaughtyCategories = o.language_list.getTranslation(72);
                    checkme->isItNaughty = true;
                    checkme->isException = false;
                    (*scanerror) = true;
                    break;
                }
#ifdef DGDEBUG
                k++;
#endif
            }

#ifdef DGDEBUG
            std::cout << dbgPeerPort << " -finished running AV" << std::endl;
//            rc = system("date");
#endif
        }
#ifdef DGDEBUG
        else if (!responsescanners.empty()) {
            std::cout << dbgPeerPort << " -content length large so skipping content scanning (virus) filtering" << std::endl;
        }
//        rc = system("date");
#endif
        if (!checkme->isItNaughty && !checkme->isException && !isbypass && (dblen <= o.max_content_filter_size)
            && !docheader->authRequired() && (docheader->isContentType("text",ldl->fg[filtergroup]) || docheader->isContentType("-",ldl->fg[filtergroup]))) {
#ifdef DGDEBUG
            std::cout << dbgPeerPort << " -Start content filtering: ";
#endif
            checkme->checkme(docbody->data, docbody->buffer_length, &url, &domain,
                ldl->fg[filtergroup], ldl->fg[filtergroup]->banned_phrase_list, ldl->fg[filtergroup]->naughtyness_limit);
#ifdef DGDEBUG
            std::cout << dbgPeerPort << " -Done content filtering: ";
#endif
        }
#ifdef DGDEBUG
        else {
            std::cout << dbgPeerPort << " -Skipping content filtering: ";
            if (dblen > o.max_content_filter_size)
                std::cout << dbgPeerPort << " -Content too large";
            else if (checkme->isException)
                std::cout << dbgPeerPort << " -Is flagged as an exception";
            else if (checkme->isItNaughty)
                std::cout << dbgPeerPort << " -Is already flagged as naughty (content scanning)";
            else if (isbypass)
                std::cout << dbgPeerPort << " -Is flagged as a bypass";
            else if (docheader->authRequired())
                std::cout << dbgPeerPort << " -Is a set of auth required headers";
            else if (!docheader->isContentType("text",ldl->fg[filtergroup]))
                std::cout << dbgPeerPort << " -Not text";
            std::cout << dbgPeerPort << std::endl;
        }
#endif
    }

    // don't do phrase filtering or content replacement on exception/bypass accesses
    if (checkme->isException || isbypass) {
        // don't forget to swap back to compressed!
        docbody->swapbacktocompressed();
        return;
    }

    if ((dblen <= o.max_content_filter_size) && !checkme->isItNaughty && docheader->isContentType("text",ldl->fg[filtergroup])) {
        contentmodified = docbody->contentRegExp(ldl->fg[filtergroup]);
        // content modifying uses global variable
    }
#ifdef DGDEBUG
    else {
        std::cout << dbgPeerPort << " -Skipping content modification: ";
        if (dblen > o.max_content_filter_size)
            std::cout << dbgPeerPort << " -Content too large";
        else if (!docheader->isContentType("text",ldl->fg[filtergroup]))
            std::cout << dbgPeerPort << " -Not text";
        else if (checkme->isItNaughty)
            std::cout << dbgPeerPort << " -Already flagged as naughty";
        std::cout << dbgPeerPort << std::endl;
    }
    //rc = system("date");
#endif

    if (contentmodified) { // this would not include infected/cured files
// if the content was modified then it must have fit in ram so no
// need to worry about swapped to disk stuff
#ifdef DGDEBUG
        std::cout << dbgPeerPort << " -content modification made" << std::endl;
#endif
        if (compressed) {
            docheader->removeEncoding(docbody->buffer_length);
            // need to modify header to mark as not compressed
            // it also modifies Content-Length as well
        } else {
            docheader->setContentLength(docbody->buffer_length);
        }
    } else {
        docbody->swapbacktocompressed();
        // if we've not modified it might as well go back to
        // the original compressed version (if there) and send
        // that to the browser
    }
#ifdef DGDEBUG
    std::cout << dbgPeerPort << " Returning from content checking"  << std::endl;
#endif
    }

#ifdef __SSLMITM
int ConnectionHandler::sendProxyConnect(String &hostname, Socket *sock, NaughtyFilter *checkme)
{
    String connect_request = "CONNECT " + hostname + ":";
    connect_request += "443 HTTP/1.0\r\n\r\n";

#ifdef DGDEBUG
    std::cout << dbgPeerPort << " -creating tunnel through proxy to " << hostname << std::endl;
#endif

    //somewhere to hold the header from the proxy
    HTTPHeader header;
    //header.setTimeout(o.pcon_timeout);
    header.setTimeout(o.proxy_timeout);

        if(! (sock->writeString(connect_request.c_str())  &&  header.in(sock, true, true)) )  {

#ifdef DGDEBUG
        syslog(LOG_ERR, "Error creating tunnel through proxy\n");
        std::cout << dbgPeerPort << " -Error creating tunnel through proxy" << strerror(errno) << std::endl;
#endif
        //(*checkme).whatIsNaughty = "Unable to create tunnel through local proxy";
        checkme->message_no = 157;
        (*checkme).whatIsNaughty = o.language_list.getTranslation(157);
        (*checkme).whatIsNaughtyLog = (*checkme).whatIsNaughty;
        (*checkme).isItNaughty = true;
        (*checkme).whatIsNaughtyCategories = o.language_list.getTranslation(70);

        return -1;
    }
    //do http connect
    if (header.returnCode() != 200) {
        //connection failed
        //(*checkme).whatIsNaughty = "Opening tunnel failed";
        checkme->message_no = 158;
        (*checkme).whatIsNaughty = o.language_list.getTranslation(158);
        (*checkme).whatIsNaughtyLog = (*checkme).whatIsNaughty + " with errror code " + String(header.returnCode());
        (*checkme).isItNaughty = true;
        (*checkme).whatIsNaughtyCategories = o.language_list.getTranslation(70);

#ifdef DGDEBUG
        syslog(LOG_ERR, "Tunnel status not 200 ok aborting\n");
        std::cout << dbgPeerPort << " -Tunnel status was " << header.returnCode() << " expecting 200 ok" << std::endl;
#endif

        return -1;
    }

    return 0;
}

void ConnectionHandler::checkCertificate(String &hostname, Socket *sslsock, NaughtyFilter *checkme)
{

#ifdef DGDEBUG
    std::cout << dbgPeerPort << " -checking SSL certificate is valid" << std::endl;
#endif

    long rc = sslsock->checkCertValid();
    //check that everything in this certificate is correct appart from the hostname
    if (rc < 0) {
        //no certificate
        if ( ldl->fg[filtergroup]->allow_empty_host_certs)
            return;
        checkme->isItNaughty = true;
        //(*checkme).whatIsNaughty = "No SSL certificate supplied by server";
        checkme->message_no = 155;
        (*checkme).whatIsNaughty = o.language_list.getTranslation(155);
        (*checkme).whatIsNaughtyLog = (*checkme).whatIsNaughty;
        (*checkme).whatIsNaughtyCategories = o.language_list.getTranslation(70);
        return;
    } else if (rc != X509_V_OK) {
        //something was wrong in the certificate
        checkme->isItNaughty = true;
        //		(*checkme).whatIsNaughty = "Certificate supplied by server was not valid";
        checkme->message_no = 150;
        (*checkme).whatIsNaughty = o.language_list.getTranslation(150);
        (*checkme).whatIsNaughtyLog = (*checkme).whatIsNaughty + ": " + X509_verify_cert_error_string(rc);
        (*checkme).whatIsNaughtyCategories = o.language_list.getTranslation(70);
        return;
    }

#ifdef DGDEBUG
    std::cout << dbgPeerPort << " -checking SSL certificate hostname" << std::endl;
#endif

    //check the common name and altnames of a certificate against hostname
    if (sslsock->checkCertHostname(hostname) < 0) {
        //hostname was not matched by the certificate
        checkme->isItNaughty = true;
        //(*checkme).whatIsNaughty = "Server's SSL certificate does not match domain name";
        checkme->message_no = 156;
        (*checkme).whatIsNaughty = o.language_list.getTranslation(156);
        (*checkme).whatIsNaughtyLog = (*checkme).whatIsNaughty;
        (*checkme).whatIsNaughtyCategories = o.language_list.getTranslation(70);
        return;
    }
}
#endif //__SSLMITM


bool ConnectionHandler::getdnstxt(std::string &clientip, String &user)
{

    String ippath;

    ippath = clientip;
//    if (o.use_xforwardedfor) {
//        // grab the X-Forwarded-For IP if available
//        p2 = header.getXForwardedForIP();
//        if (p2.length() > 0) {
//            ippath = p1 + "-" + p2;
//        } else {
//            ippath = p1;
//        }
//    } else {
//        ippath = p1;
//    }

#ifdef DGDEBUG
    std::cout << "IPPath is " << ippath << std::endl;
#endif

    // change '.' to '-'
    ippath.swapChar('.', '-');
#ifdef DGDEBUG
    std::cout << "IPPath is " << ippath << std::endl;
#endif

    // get info from DNS
    union {
        HEADER hdr;
        u_char buf[NS_PACKETSZ];
    } response;
    int responseLen;
#ifdef PRT_DNSAUTH
    ns_msg handle; /* handle for response message */
    responseLen = res_querydomain(ippath.c_str(), o.dns_user_logging_domain.c_str(), ns_c_in, ns_t_txt, (u_char *)&response, sizeof(response));
    if (responseLen < 0) {
#ifdef DGDEBUG
        std::cout << "DNS query returned error " << dns_error(h_errno) << std::endl;
#endif
        return false;
    }
    if (ns_initparse(response.buf, responseLen, &handle) < 0) {
#ifdef DGDEBUG
        std::cout << "ns_initparse returned error " << strerror(errno) << std::endl;
#endif
        return false;
    }
    int rrnum; /* resource record number */
    ns_rr rr; /* expanded resource record */
    u_char *cp;
    char ans[MAXDNAME];

    int i = ns_msg_count(handle, ns_s_an);
    if (i > 0) {
        if (ns_parserr(&handle, ns_s_an, 0, &rr)) {
#ifdef DGDEBUG
            std::cout << "ns_paserr returned error " << strerror(errno) << std::endl;
#endif
            return false;
        } else {
            if (ns_rr_type(rr) == ns_t_txt) {
#ifdef DGDEBUG
                std::cout << "ns_rr_rdlen returned " << ns_rr_rdlen(rr) << std::endl;
#endif
                u_char *k = (u_char *)ns_rr_rdata(rr);
                char p[400];
                unsigned int j = 0;
                for (unsigned int j1 = 1; j1 < ns_rr_rdlen(rr); j1++) {
                    p[j++] = k[j1];
                }
                p[j] = (char)NULL;
#ifdef DGDEBUG
                std::cout << "ns_rr_data returned " << p << std::endl;
#endif
                String dnstxt(p);
                user = dnstxt.before(",");
                return true;
            }
        }
    }
#endif
    return false;
}

String ConnectionHandler::dns_error(int herror)
{

    String s;

    switch (herror) {
        case HOST_NOT_FOUND:
            s = "HOST_NOT_FOUND";
            break;
        case TRY_AGAIN:
            s = "TRY_AGAIN - DNS server failure";
            break;
        case NO_DATA:
            s = "NO_DATA - unexpected DNS error";
            break;
        default:
            String S2(herror);
            s = "DNS - Unexpected error number " + S2;
            break;
    }
    return s;
}