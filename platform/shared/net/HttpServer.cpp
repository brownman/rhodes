#include "net/HttpServer.h"
#include "common/RhodesApp.h"
#include "ruby/ext/rho/rhoruby.h"

#if !defined(OS_WINCE)
#include <sys/stat.h>
#else
#include "CompatWince.h"

#ifdef EAGAIN
#undef EAGAIN
#endif
#define EAGAIN EWOULDBLOCK

#endif

#if defined(OS_WINDOWS) || defined(OS_WINCE)
typedef unsigned __int16 uint16_t;

#  ifndef S_ISDIR
#    define S_ISDIR(m) ((_S_IFDIR & m) == _S_IFDIR)
#  endif

#  ifndef S_ISREG
#    define S_ISREG(m) ((_S_IFREG & m) == _S_IFREG)
#  endif

#  ifndef EAGAIN
#    define EAGAIN WSAEWOULDBLOCK
#  endif

#endif

#undef DEFAULT_LOGCATEGORY
#define DEFAULT_LOGCATEGORY "HttpServer"

extern "C" void rho_sync_addobjectnotify_bysrcname(const char* szSrcName, const char* szObject);

namespace rho
{
namespace net
{

IMPLEMENT_LOGCLASS(CHttpServer, "HttpServer");
    
static bool isid(String const &s)
{
    return s.size() > 2 && s[0] == '{' && s[s.size() - 1] == '}';
}

static bool isdir(String const &path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

static bool isfile(String const &path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

static String decode(String const &url)
{
    String ret;
    for (const char *s = url.c_str(); *s != '\0'; ++s) {
        if (*s != '%') {
            ret.push_back(*s);
            continue;
        }
        
        unsigned int c1 = (unsigned char)*++s;
        if (c1 >= (unsigned char)'0' && c1 <= (unsigned char)'9')
            c1 = c1 - (unsigned char)'0';
        else if (c1 >= (unsigned char)'a' && c1 <= (unsigned char)'f')
            c1 = c1 - (unsigned char)'a' + 10;
        else if (c1 >= (unsigned char)'A' && c1 <= (unsigned char)'F')
            c1 = c1 - (unsigned char)'A' + 10;
        else
            break;
        unsigned int c2 = (unsigned char)*++s;
        if (c2 >= (unsigned char)'0' && c2 <= (unsigned char)'9')
            c2 = c2 - (unsigned char)'0';
        else if (c2 >= (unsigned char)'a' && c2 <= (unsigned char)'f')
            c2 = c2 - (unsigned char)'a' + 10;
        else if (c2 >= (unsigned char)'A' && c2 <= (unsigned char)'F')
            c2 = c2 - (unsigned char)'A' + 10;
        else
            break;
        
        char c = (char)((c1 << 4) | c2);
        ret.push_back(c);
    }
    return ret;
}

static bool isindex(String const &uri)
{
    static struct {
        const char *s;
        size_t len;
    } index_files[] = {
        {"index_erb.iseq", 14},
        {"index.html", 10},
        {"index.htm", 9},
        {"index.php", 9},
        {"index.cgi", 9}
    };
    
    for (size_t i = 0, lim = sizeof(index_files)/sizeof(index_files[0]); i != lim; ++i) {
        size_t pos = uri.find(index_files[i].s);
        if (pos == String::npos)
            continue;
        
        if (pos + index_files[i].len != uri.size())
            continue;
        
        return true;
    }
    
    return false;
}

static bool isknowntype(String const &uri)
{
    static struct {
        const char *s;
        size_t len;
    } ignored_exts[] = {
        {".css", 4},
        {".js", 3},
        {".html", 5},
        {".htm", 4},
        {".png", 4},
        {".bmp", 4},
        {".jpg", 4},
        {".jpeg", 5}
    };
    
    for (size_t i = 0, lim = sizeof(ignored_exts)/sizeof(ignored_exts[0]); i != lim; ++i) {
        size_t pos = uri.find(ignored_exts[i].s);
        if (pos == String::npos)
            continue;
        
        if (pos + ignored_exts[i].len != uri.size())
            continue;
        
        return true;
    }
    
    return false;
}
    

static String get_mime_type(String const &path)
{
    static const struct {
        const char	*extension;
        int		ext_len;
        const char	*mime_type;
    } builtin_mime_types[] = {
        {".html",       5,	"text/html"                     },
        {".htm",		4,	"text/html"                     },
        {".txt",		4,	"text/plain"                    },
        {".css",		4,	"text/css"                      },
        {".js",         3,  "text/javascript"               },
        {".ico",		4,	"image/x-icon"                  },
        {".gif",		4,	"image/gif"                     },
        {".jpg",		4,	"image/jpeg"                    },
        {".jpeg",       5,	"image/jpeg"                    },
        {".png",		4,	"image/png"                     },
        {".svg",		4,	"image/svg+xml"                 },
        {".torrent",	8,	"application/x-bittorrent"      },
        {".wav",		4,	"audio/x-wav"                   },
        {".mp3",		4,	"audio/x-mp3"                   },
        {".mid",		4,	"audio/mid"                     },
        {".m3u",		4,	"audio/x-mpegurl"               },
        {".ram",		4,	"audio/x-pn-realaudio"          },
        {".ra",         3,	"audio/x-pn-realaudio"          },
        {".doc",		4,	"application/msword",           },
        {".exe",		4,	"application/octet-stream"      },
        {".zip",		4,	"application/x-zip-compressed"	},
        {".xls",		4,	"application/excel"             },
        {".tgz",		4,	"application/x-tar-gz"          },
        {".tar.gz",     7,	"application/x-tar-gz"          },
        {".tar",		4,	"application/x-tar"             },
        {".gz",         3,	"application/x-gunzip"          },
        {".arj",		4,	"application/x-arj-compressed"	},
        {".rar",		4,	"application/x-arj-compressed"	},
        {".rtf",		4,	"application/rtf"               },
        {".pdf",		4,	"application/pdf"               },
        {".swf",		4,	"application/x-shockwave-flash"	},
        {".mpg",		4,	"video/mpeg"                    },
        {".mpeg",       5,	"video/mpeg"                    },
        {".asf",		4,	"video/x-ms-asf"                },
        {".avi",		4,	"video/x-msvideo"               },
        {".bmp",		4,	"image/bmp"                     },
    };
    
    String mime_type;
    for (int i = 0, lim = sizeof(builtin_mime_types)/sizeof(builtin_mime_types[0]); i != lim; ++i) {
        size_t pos = path.find(builtin_mime_types[i].extension);
        if (pos == String::npos)
            continue;
        
        if (pos + builtin_mime_types[i].ext_len != path.size())
            continue;
        
        mime_type = builtin_mime_types[i].mime_type;
        break;
    }
    
    if (mime_type.empty())
        mime_type = "text/plain";
    
    return mime_type;
}
    

static VALUE create_request_hash(String const &application, String const &model,
                                 String const &action, String const &id,
                                 String const &method, String const &uri, String const &query,
                                 HttpHeaderList const &headers, String const &body)
{
    VALUE hash = createHash();
    
    addStrToHash(hash, "application", application.c_str(), application.size());
	addStrToHash(hash, "model", model.c_str(), model.size());
    if (!action.empty())
        addStrToHash(hash, "action", action.c_str(), action.size());
    if (!id.empty())
        addStrToHash(hash, "id", id.c_str(), id.size());
	
	addStrToHash(hash, "request-method", method.c_str(), method.size());
	addStrToHash(hash, "request-uri", uri.c_str(), uri.size());
    addStrToHash(hash, "request-query", query.c_str(), query.size());
	
	VALUE hash_headers = createHash();
    for (HttpHeaderList::const_iterator it = headers.begin(), lim = headers.end(); it != lim; ++it)
        addStrToHash(hash_headers, it->name.c_str(), it->value.c_str(), it->value.size());
	addHashToHash(hash,"headers",hash_headers);
	
    if (!body.empty())
		addStrToHash(hash, "request-body", body.c_str(), body.size());
    
    return hash;
}

CHttpServer::CHttpServer(int port, String const &root)
    :m_exit(false), m_port(port), m_root(root)
{
    RAWTRACE("Open listening socket...");
    
    m_listener = socket(AF_INET, SOCK_STREAM, 0);
    if (m_listener == SOCKET_ERROR) {
        RAWLOG_ERROR1("Can not create listener: %d", RHO_NET_ERROR_CODE);
        return;
    }
    
    int enable = 1;
    if (setsockopt(m_listener, SOL_SOCKET, SO_REUSEADDR, (const char *)&enable, sizeof(enable)) == SOCKET_ERROR) {
        RAWLOG_ERROR1("Can not set socket option (SO_REUSEADDR): %d", RHO_NET_ERROR_CODE);
        return;
    }
    
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)m_port);
    sa.sin_addr.s_addr = INADDR_ANY;
    if (bind(m_listener, (const sockaddr *)&sa, sizeof(sa)) == SOCKET_ERROR) {
        RAWLOG_ERROR2("Can not bind to port %d: %d", m_port, RHO_NET_ERROR_CODE);
        return;
    }
    
    if (listen(m_listener, 128) == SOCKET_ERROR) {
        RAWLOG_ERROR1("Can not listen on socket: %d", RHO_NET_ERROR_CODE);
        return;
    }
    
    RAWTRACE1("Listen for connections on port %d", m_port);
}

CHttpServer::~CHttpServer()
{
}

void CHttpServer::stop()
{
	m_exit = true;
	RAWTRACE("Close listening socket");
	closesocket(m_listener);
}

void CHttpServer::register_uri(String const &uri, CHttpServer::callback_t const &callback)
{
    m_registered[uri] = callback;
}

CHttpServer::callback_t CHttpServer::registered(String const &uri)
{
    std::map<String, callback_t>::const_iterator it = m_registered.find(uri);
    if (it == m_registered.end())
        return (callback_t)0;
    return it->second;
}

bool CHttpServer::run()
{
    if (m_listener == INVALID_SOCKET)
        return false;
    
    for(;;) {
        RAWTRACE("Waiting for connections...");
        SOCKET conn = accept(m_listener, NULL, NULL);
		if (m_exit) {
			RAWTRACE("Stop HTTP server");
			return true;
		}
        if (conn == INVALID_SOCKET) {
#if !defined(OS_WINDOWS) && !defined(OS_WINCE)
            if (RHO_NET_ERROR_CODE == EINTR)
                continue;
#endif
            
            RAWLOG_ERROR1("Can not accept connection: %d", RHO_NET_ERROR_CODE);
            return false;
        }
        
        RAWTRACE("Connection accepted, process it...");
        process(conn);
        
        RAWTRACE("Close connected socket");
        closesocket(conn);
    }
    
    return true;
}

bool CHttpServer::receive_request(ByteVector &request)
{
    request.clear();

    RAWTRACE("Receiving request...");
    
    // First of all, make socket non-blocking
#if defined(OS_WINDOWS) || defined(OS_WINCE)
	unsigned long optval = 1;
	if(::ioctlsocket(m_sock, FIONBIO, &optval) == SOCKET_ERROR) {
		RAWLOG_ERROR1("Can not set non-blocking socket mode: %d", RHO_NET_ERROR_CODE);
		return false;
	}
#else
    int flags = fcntl(m_sock, F_GETFL);
    if (flags == -1) {
        RAWLOG_ERROR1("Can not get current socket mode: %d", errno);
        return false;
    }
    if (fcntl(m_sock, F_SETFL, flags | O_NONBLOCK) == -1) {
        RAWLOG_ERROR1("Can not set non-blocking socket mode: %d", errno);
        return false;
    }
#endif
    
    char buf[BUF_SIZE];
    for(;;) {
        RAWTRACE("Read portion of data from socket...");
        int n = recv(m_sock, &buf[0], sizeof(buf), 0);
        if (n == -1) {
            int e = RHO_NET_ERROR_CODE;
#if !defined(OS_WINDOWS) && !defined(OS_WINCE)
            if (e == EINTR)
                continue;
#endif
            if (e == EAGAIN) {
                if (!request.empty())
                    break;
                
                fd_set fds;
                FD_ZERO(&fds);
                FD_SET(m_sock, &fds);
                select(m_sock + 1, &fds, 0, 0, 0);
                continue;
            }
            
            RAWLOG_ERROR1("Error when receiving data from socket: %d", e);
            return false;
        }
        
        if (n == 0) {
            RAWLOG_ERROR("Connection gracefully closed before we send any data");
            return false;
        }
        
        RAWTRACE1("Actually read %d bytes", n);
        request.insert(request.end(), &buf[0], &buf[0] + n);
    }
    
    if (request.empty())
        return false;
    
    request.push_back('\0');
    RAWTRACE1("Received request:\n%s", &request[0]);
    return true;
}

bool CHttpServer::send_response(String const &response)
{
    RAWTRACE("Sending response...");
    // First of all, make socket blocking
#if defined(OS_WINDOWS) || defined(OS_WINCE)
	unsigned long optval = 0;
	if(::ioctlsocket(m_sock, FIONBIO, &optval) == SOCKET_ERROR) {
		RAWLOG_ERROR1("Can not set blocking socket mode: %d", RHO_NET_ERROR_CODE);
		return false;
	}
#else
    int flags = fcntl(m_sock, F_GETFL);
    if (flags == -1) {
        RAWLOG_ERROR1("Can not get current socket mode: %d", errno);
        return false;
    }
    if (fcntl(m_sock, F_SETFL, flags & ~O_NONBLOCK) == -1) {
        RAWLOG_ERROR1("Can not set blocking socket mode: %d", errno);
        return false;
    }
#endif
    
    size_t pos = 0;
    for(;;) {
        int n = send(m_sock, response.c_str() + pos, response.size() - pos, 0);
        if (n == -1) {
            int e = RHO_NET_ERROR_CODE;
#if !defined(OS_WINDOWS) && !defined(OS_WINCE)
            if (e == EINTR)
                continue;
#endif
            
            RAWLOG_ERROR1("Can not send response: %d", e);
            return false;
        }
        
        if (n == 0)
            break;
        
        pos += n;
    }
    
    //String dbg_response = response.size() > 100 ? response.substr(0, 100) : response;
    //RAWTRACE2("Sent response:\n%s%s", dbg_response.c_str(), response.size() > 100 ? "..." : "   ");
    RAWTRACE("Response sent");
    return true;
}

String CHttpServer::create_response(String const &reason)
{
    return create_response(reason, "");
}

String CHttpServer::create_response(String const &reason, HeaderList const &headers)
{
    return create_response(reason, headers, "");
}

String CHttpServer::create_response(String const &reason, String const &body)
{
    return create_response(reason, HeaderList(), body);
}

String CHttpServer::create_response(String const &reason, HeaderList const &hdrs, String const &body)
{
    String response = "HTTP/1.1 ";
    response += reason;
    response += "\r\n";
    
    char buf[50];
    snprintf(buf, sizeof(buf), "%d", m_port);
    
    HeaderList headers;
    headers.push_back(Header("Host", String("localhost:") + buf));
    headers.push_back(Header("Connection", "close"));
    std::copy(hdrs.begin(), hdrs.end(), std::back_inserter(headers));
    
    for(HeaderList::const_iterator it = headers.begin(), lim = headers.end();
        it != lim; ++it) {
        response += it->name;
        response += ": ";
        response += it->value;
        response += "\r\n";
    }
    
    response += "\r\n";
    
    response += body;
    
    return response;
}

bool CHttpServer::process(SOCKET sock)
{
    m_sock = sock;
    // Read request from socket
    ByteVector request;
    if (!receive_request(request))
        return false;
    
    if (request.empty() || (request.size() == 3 && memcmp(&request[0], "\r\n", 3) == 0)) {
        RAWTRACE("This is empty request, skip it");
        return true;
    }
    
    RAWTRACE("Parsing request...");
    String method, uri, query;
    HeaderList headers;
    String body;
    if (!parse_request(request, method, uri, query, headers, body)) {
        RAWLOG_ERROR("Parsing error");
        send_response(create_response("500 Internal Error"));
        return false;
    }
    
    return decide(method, uri, query, headers, body);
}

bool CHttpServer::parse_request(ByteVector &request, String &method, String &uri,
                                String &query, HeaderList &headers, String &body)
{
    method.clear();
    uri.clear();
    headers.clear();
    body.clear();
    
    char *s = &request[0];
    for(;;) {
        char *e;
        for(e = s; *e != '\r' && *e != '\0'; ++e);
        if (*e == '\0' || *(e + 1) != '\n')
            return false;
        *e = 0;
        
        String line = s;
        s = e + 2;
        
        if (!line.empty()) {
            if (uri.empty()) {
                // Parse start line
                if (!parse_startline(line, method, uri, query) || uri.empty())
                    return false;
            }
            else {
                Header hdr;
                if (!parse_header(line, hdr) || hdr.name.empty())
                    return false;
                headers.push_back(hdr);
            }
        }
        else {
            // Stop parsing
            body = s;
            return true;
        }
    }
}

bool CHttpServer::parse_startline(String const &line, String &method, String &uri, String &query)
{
    const char *s, *e;
    
    // Find first space
    for(s = line.c_str(), e = s; *e != ' ' && *e != '\0'; ++e);
    if (*e == '\0')
        return false;
    
    method.assign(s, e);
    
    // Skip spaces
    for(s = e; *s == ' '; ++s);
    
    for(e = s; *e != '?' && *e != ' ' && *e != '\0'; ++e);
    if (*e == '\0')
        return false;
    
    uri.assign(s, e);
    uri = decode(uri);
    
    query.clear();
    if (*e == '?') {
        s = ++e;
        for(e = s; *e != ' ' && *e != '\0'; ++e);
        if (*e != '\0')
            query.assign(s, e);
    }
    
    return true;
}

bool CHttpServer::parse_header(String const &line, Header &hdr)
{
    const char *s, *e;
    for(s = line.c_str(), e = s; *e != ' ' && *e != ':' && *e != '\0'; ++e);
    if (*e == '\0')
        return false;
    hdr.name.assign(s, e);
    
    // Skip spaces and colon
    for(s = e; *s == ' ' || *s == ':'; ++s);
    
    hdr.value = s;
    return true;
}

bool CHttpServer::parse_route(String const &line, Route &route)
{
    if (line.empty())
        return false;
    
    const char *s = line.c_str();
    if (*s == '/')
        ++s;
    
    const char *application_begin = s;
    for(; *s != '/' && *s != '\0'; ++s);
    if (*s == '\0')
        return false;
    const char *application_end = s;
    
    const char *model_begin = ++s;
    for(; *s != '/' && *s != '\0'; ++s);
    const char *model_end = s;
    
    route.application.assign(application_begin, application_end);
    route.model.assign(model_begin, model_end);
    
    if (*s == '\0')
        return true;
    
    const char *actionorid_begin = ++s;
    for (; *s != '/' && *s != '\0'; ++s);
    const char *actionorid_end = s;
    
    if (*s == '/')
        ++s;
    
    String aoi(actionorid_begin, actionorid_end);
    if (isid(aoi)) {
        route.id = aoi;
        route.action = s;
    }
    else {
        route.id = s;
        route.action = aoi;
    }
    
    return true;
}

bool CHttpServer::dispatch(String const &uri, Route &route)
{
    if (isknowntype(uri))
        return false;
    
    // Trying to parse route
    if (!parse_route(uri, route))
        return false;
    
    struct stat st;
    //is this an actual file or folder
    if (stat((m_root + "/" + uri).c_str(), &st) != 0)
        return true;      
    
    //is this a folder
    if (!S_ISDIR(st.st_mode))
        return false;
    
    //check if there is controller.rb to run
    size_t len = uri.size();
    String slash = uri[len-1] == '\\' || uri[len-1] == '/' ? "" : "/";
    
    String filename = m_root + "/" + uri + slash + "controller.iseq";
    if (stat(filename.c_str(), &st) != 0 || S_ISDIR(st.st_mode))
        return false;
    
    RAWLOG_INFO1("Run controller on this url: %s", uri.c_str());
    return true;
}

bool CHttpServer::send_file(String const &path)
{
    // TODO:
    //static const char *resp = "<html><title>TEST</title><body><h1>This is test</h1></body></html>";
    //send_response(sock, create_response("200 OK", resp));
    
    String fullPath = m_root + "/" + path;
    
    struct stat st;
    if (stat(fullPath.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        send_response(create_response("404 Not Found"));
        return false;
    }
    
    FILE *fp = fopen(fullPath.c_str(), "rb");
    if (!fp) {
        send_response(create_response("404 Not Found"));
        return false;
    }
    
    HeaderList headers;
    
    // Detect mime type
    headers.push_back(Header("Content-Type", get_mime_type(path)));
    
    // Content length
    char buf[4096];
    
    size_t fileSize = st.st_size;
    snprintf(buf, sizeof(buf), "%d", fileSize);
    headers.push_back(Header("Content-Length", buf));
    
    // Send headers
    if (!send_response(create_response("200 OK", headers))) {
        fclose(fp);
        return false;
    }
    
    // Send body
    while (!feof(fp)) {
        size_t n = fread(buf, 1, sizeof(buf), fp);
        if (!send_response(String(buf, n))) {
            fclose(fp);
            return false;
        }
    }
    
    fclose(fp);
    return true;
}

bool CHttpServer::decide(String const &method, String const &uri, String const &query,
                         HeaderList const &headers, String const &body)
{
    callback_t callback = registered(uri);
    if (callback) {
        callback(this, query);
        return true;
    }
    
    String fullPath = m_root + "/" + uri;
    
    Route route;
    if (dispatch(uri, route)) {
        if (method == "GET")
            rho_rhodesapp_keeplastvisitedurl(uri.c_str());
        
        VALUE req = create_request_hash(route.application, route.model, route.action, route.id,
                                        method, uri, query, headers, body);
        VALUE data = callFramework(req);
        
        String reply(getStringFromValue(data), getStringLenFromValue(data));
        if (!send_response(reply))
            return false;
        
        if (!route.id.empty())
            rho_sync_addobjectnotify_bysrcname(route.model.c_str(), route.id.c_str());
        
        return true;
    }
    
    if (isdir(fullPath)) {
        String slash = !uri.empty() && uri[uri.size() - 1] == '/' ? "" : "/";
        String q = query.empty() ? "" : "?" + query;
        
        HeaderList headers;
        headers.push_back(Header("Location", uri + slash + "index_erb.iseq" + q));
        
        send_response(create_response("301 Moved Permanently", headers));
        return false;
    }
    
    if (isindex(uri)) {
        if (!isfile(fullPath)) {
            send_response(create_response("404 Not Found"));
            return false;
        }
        
        if (method == "GET")
            rho_rhodesapp_keeplastvisitedurl(uri.c_str());
        
        VALUE data = callServeIndex((char *)fullPath.c_str());
        String reply(getStringFromValue(data), getStringLenFromValue(data));
        return send_response(reply);
    }
    
    // Try to send requested file
    return send_file(uri);
}

} // namespace net
} // namespace rho