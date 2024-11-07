#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <ifaddrs.h>
#include <net/route.h>
#include <sys/ioctl.h>
#include <signal.h>

#include "ssdp.h"

#define SSDP_PORT 1900
#define SSDP_ADDR "239.255.255.250"
#define NOTIFY_INTERVAL 30
#define BUFFER_SIZE 4096
#define CHUNK_SIZE 65536

// SSDP NOTIFY message template for MediaServer
const char *notify_template =
    "NOTIFY * HTTP/1.1\r\n"
    "HOST: 239.255.255.250:1900\r\n"
    "CACHE-CONTROL: max-age=1800\r\n"
    "LOCATION: http://%s/description.xml\r\n"
    "NT: urn:schemas-upnp-org:device:MediaServer:1\r\n"
    "NTS: ssdp:alive\r\n"
    "SERVER: %s/1.0\r\n"
    "USN: uuid:%s::urn:schemas-upnp-org:device:MediaServer:1\r\n"
    "X-DLNADOC: DMS-1.50\r\n\r\n";

// M-SEARCH response template for MediaServer
const char *search_response_template =
    "HTTP/1.1 200 OK\r\n"
    "CACHE-CONTROL: max-age=1800\r\n"
    "DATE: %s\r\n"
    "EXT:\r\n"
    "LOCATION: http://%s/description.xml\r\n"
    "SERVER: %s/1.0\r\n"
    "ST: urn:schemas-upnp-org:device:MediaServer:1\r\n"
    "USN: uuid:%s::urn:schemas-upnp-org:device:MediaServer:1\r\n"
    "X-DLNADOC: DMS-1.50\r\n\r\n";

// Device description XML template for MediaServer
const char *device_description_template =
    "<?xml version=\"1.0\"?>\r\n"
    "<root xmlns=\"urn:schemas-upnp-org:device-1-0\" xmlns:dlna=\"urn:schemas-dlna-org:device-1-0\">\r\n"
    "  <specVersion>\r\n"
    "    <major>1</major>\r\n"
    "    <minor>0</minor>\r\n"
    "  </specVersion>\r\n"
    "  <device>\r\n"
    "    <deviceType>urn:schemas-upnp-org:device:MediaServer:1</deviceType>\r\n"
    "    <friendlyName>%s</friendlyName>\r\n"
    "    <manufacturer>Marko Vitez</manufacturer>\r\n"
    "    <manufacturerURL>https://www.vitez.it</manufacturerURL>\r\n"
    "    <modelDescription>%s</modelDescription>\r\n"
    "    <modelName>%s</modelName>\r\n"
    "    <modelNumber>1.0</modelNumber>\r\n"
    "    <modelURL>https://github.com/mvitez/screencast</modelURL>\r\n"
    "    <serialNumber>00000000</serialNumber>\r\n"
    "    <UDN>uuid:%s</UDN>\r\n"
    "    <dlna:X_DLNADOC>DMS-1.50</dlna:X_DLNADOC>\r\n"
    "    <serviceList>\r\n"
    "      <service>\r\n"
    "        <serviceType>urn:schemas-upnp-org:service:ContentDirectory:1</serviceType>\r\n"
    "        <serviceId>urn:upnp-org:serviceId:ContentDirectory</serviceId>\r\n"
    "        <SCPDURL>/ContentDirectory.xml</SCPDURL>\r\n"
    "        <controlURL>/ContentDirectory/control</controlURL>\r\n"
    "        <eventSubURL>/ContentDirectory/event</eventSubURL>\r\n"
    "      </service>\r\n"
    "      <service>\r\n"
    "        <serviceType>urn:schemas-upnp-org:service:ConnectionManager:1</serviceType>\r\n"
    "        <serviceId>urn:upnp-org:serviceId:ConnectionManager</serviceId>\r\n"
    "        <SCPDURL>/ConnectionManager.xml</SCPDURL>\r\n"
    "        <controlURL>/ConnectionManager/control</controlURL>\r\n"
    "        <eventSubURL>/ConnectionManager/event</eventSubURL>\r\n"
    "      </service>\r\n"
    "    </serviceList>\r\n"
    "    <presentationURL>/presentation</presentationURL>\r\n"
    "  </device>\r\n"
    "</root>\r\n";

const char *content_directory_template =
    "<?xml version=\"1.0\"?>\r\n"
    "<scpd xmlns=\"urn:schemas-upnp-org:service-1-0\">\r\n"
    "  <specVersion>\r\n"
    "    <major>1</major>\r\n"
    "    <minor>0</minor>\r\n"
    "  </specVersion>\r\n"
    "  <actionList>\r\n"
    "    <action>\r\n"
    "      <name>Browse</name>\r\n"
    "      <argumentList>\r\n"
    "        <argument>\r\n"
    "          <name>ObjectID</name>\r\n"
    "          <direction>in</direction>\r\n"
    "          <relatedStateVariable>A_ARG_TYPE_ObjectID</relatedStateVariable>\r\n"
    "        </argument>\r\n"
    "        <argument>\r\n"
    "          <name>Result</name>\r\n"
    "          <direction>out</direction>\r\n"
    "          <relatedStateVariable>A_ARG_TYPE_Result</relatedStateVariable>\r\n"
    "        </argument>\r\n"
    "      </argumentList>\r\n"
    "    </action>\r\n"
    "  </actionList>\r\n"
    "  <serviceStateTable>\r\n"
    "    <stateVariable sendEvents=\"no\">\r\n"
    "      <name>A_ARG_TYPE_ObjectID</name>\r\n"
    "      <dataType>string</dataType>\r\n"
    "    </stateVariable>\r\n"
    "    <stateVariable sendEvents=\"no\">\r\n"
    "      <name>A_ARG_TYPE_Result</name>\r\n"
    "      <dataType>string</dataType>\r\n"
    "    </stateVariable>\r\n"
    "    <stateVariable sendEvents=\"no\">\r\n"
    "      <name>SortCapabilities</name>\r\n"
    "      <dataType>string</dataType>\r\n"
    "    </stateVariable>\r\n"
    "  </serviceStateTable>\r\n"
    "</scpd>\r\n";

// DIDL-Lite response template for Browse action
const char *browse_response_template_start =
    "&lt;DIDL-Lite\n"
    "    xmlns:dc=\"http://purl.org/dc/elements/1.1/\"\n"
    "    xmlns:upnp=\"urn:schemas-upnp-org:metadata-1-0/upnp/\"\n"
    "    xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\"\n"
    "    xmlns:dlna=\"urn:schemas-dlna-org:metadata-1-0/\"&gt;\n";
const char *browse_response_template_end = "&lt;/DIDL-Lite&gt;";

const char *browse_response_template_item =
    "  &lt;item id=\"%d\" parentID=\"0\" restricted=\"1\"&gt;\n"
    "    &lt;dc:title&gt;%s&lt;/dc:title&gt;\n"
    "    &lt;upnp:class&gt;object.item.videoItem&lt;/upnp:class&gt;\n"
    "    &lt;res protocolInfo=\"http-get:*:video/MP2T:DLNA.ORG_PN=MPEG_TS_SD_EU_ISO;"
    "DLNA.ORG_OP=01;DLNA.ORG_CI=0;DLNA.ORG_FLAGS=01700000000000000000000000000000\"&gt;"
    "%s&lt;/res&gt;\n"
    "  &lt;/item&gt;\n";

// SOAP response template for Browse action
const char *soap_response_template_start =
    "<?xml version=\"1.0\"?>\n"
    "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"\n"
    "    s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">\n"
    "  <s:Body>\n"
    "    <u:BrowseResponse xmlns:u=\"urn:schemas-upnp-org:service:ContentDirectory:1\">\n"
    "      <Result>";

const char *soap_response_template_end =
    "</Result>\n "
    "      <NumberReturned>%d</NumberReturned>\n"
    "      <TotalMatches>%d</TotalMatches>\n"
    "      <UpdateID>1</UpdateID>\n"
    "    </u:BrowseResponse>\n"
    "  </s:Body>\n"
    "</s:Envelope>";

// Function to handle Browse action
void handle_browse_request(int client_sock, const char *local_endpoint)
{
    char *buffer, *p, *q;
    char **items = get_stream_items();
    char url[300];
    int buflen = 2000;
    int n = 0, rc;
    for (int i = 0; items && items[i]; i++)
        buflen += strlen(items[i]) + strlen(browse_response_template_item) + 100;
    buffer = (char *)malloc(buflen);
    strcpy(buffer, soap_response_template_start);
    strcat(buffer, browse_response_template_start);
    p = buffer + strlen(buffer);
    for (int i = 0; items && items[i]; i++)
    {
        q = strchr(items[i], '\t');
        if (!q)
            continue;
        *q++ = 0;
        if (memcmp(q, "http://", 7) && memcmp(q, "rtsp://", 7))
        {
            sprintf(url, "http://%s/stream/%s", local_endpoint, q);
            q = url;
        }
        sprintf(p, browse_response_template_item, i + 1, items[i], q);
        p += strlen(p);
        free(items[i]);
        n++;
    }
    if (items)
        free(items);
    strcpy(p, browse_response_template_end);
    p += strlen(p);
    sprintf(p, soap_response_template_end, n, n);

    // Send HTTP headers
    char headers[BUFFER_SIZE];
    snprintf(headers, sizeof(headers),
             "HTTP/1.1 200 OK\r\n"
             "Content-Type: text/xml; charset=\"utf-8\"\r\n"
             "Content-Length: %zu\r\n"
             "Connection: close\r\n\r\n",
             strlen(buffer));

    rc = write(client_sock, headers, strlen(headers));
    rc = write(client_sock, buffer, strlen(buffer));
    (void)rc;
    free(buffer);
}

// Helper function to check if a buffer contains a specific SOAP action
int contains_soap_action(const char *buffer, const char *action)
{
    char search_str[256];
    snprintf(search_str, sizeof(search_str),
             "\"urn:schemas-upnp-org:service:ContentDirectory:1#%s\"", action);
    return strstr(buffer, search_str) != NULL;
}

// Function to handle HTTP requests
void handle_http_request(int client_sock, const char *local_endpoint, const char *name, const char *uuid)
{
    char buffer[BUFFER_SIZE];
    char *p;
    ssize_t n = read(client_sock, buffer, BUFFER_SIZE - 1);
    buffer[n] = '\0';
    int rc;

    const char *response;
    const char *content_type = "text/xml";

    // Determine which resource is being requested
    p = strchr(buffer, '\r');
    n = p ? p - buffer : 50;
    printf("Incoming HTTP request: %*.*s\n", (int)n, (int)n, buffer);
    if (strstr(buffer, "GET /description.xml") != NULL)
    {
        snprintf(buffer, sizeof(buffer), device_description_template, name, name, name, uuid);
        response = buffer;
    }
    else if (strstr(buffer, "GET /ContentDirectory.xml") != NULL)
    {
        response = content_directory_template;
    }
    else if (strstr(buffer, "POST /ContentDirectory/control") != NULL)
    {
        if (contains_soap_action(buffer, "Browse"))
        {
            // response = minidlnad;
            handle_browse_request(client_sock, local_endpoint);
        }
        else
        {
            fprintf(stderr, "Unknown SOAP action\n");
            // Unknown SOAP action
            const char *error = "HTTP/1.1 501 Not Implemented\r\n\r\n";
            rc = write(client_sock, error, strlen(error));
        }
        close(client_sock);
        return;
    }
    else if ((p = strstr(buffer, "GET /stream/")) != NULL)
    {
        p += 12;
        char *q = strchr(p, ' ');
        if (q)
            *q = 0;
        serve(client_sock, p);
        close(client_sock);
        return;
    }
    else
    {
        // Send 404 for unknown resources
        const char *not_found = "HTTP/1.1 404 Not Found\r\n\r\n";
        rc = write(client_sock, not_found, strlen(not_found));
        close(client_sock);
        return;
    }

    // Send HTTP response headers
    char headers[BUFFER_SIZE];
    snprintf(headers, sizeof(headers),
             "HTTP/1.1 200 OK\r\n"
             "Content-Type: %s\r\n"
             "Content-Length: %zu\r\n"
             "Connection: close\r\n\r\n",
             content_type, strlen(response));

    rc = write(client_sock, headers, strlen(headers));
    rc = write(client_sock, response, strlen(response));
    (void)rc;
    close(client_sock);
}

struct server_param
{
    int server_sock;
    const char *local_endpoint, *name, *uuid;
};

// Thread function to handle HTTP server
void *http_server_thread(void *arg)
{
    const char *local_endpoint = ((struct server_param *)arg)->local_endpoint;
    int server_sock = ((struct server_param *)arg)->server_sock;
    const char *name = ((struct server_param *)arg)->name;
    const char *uuid = ((struct server_param *)arg)->uuid;

    for (;;)
    {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_sock = accept(server_sock, (struct sockaddr *)&client_addr, &client_len);

        if (client_sock >= 0)
        {
            handle_http_request(client_sock, local_endpoint, name, uuid);
        }
        else
            sleep(1);
    }

    return NULL;
}

// Function to handle M-SEARCH requests
void handle_msearch(int sock, char *buffer, struct sockaddr_in *sender_addr, char *local_endpoint, const char *name, const char *uuid)
{
    if (strstr(buffer, "M-SEARCH") &&
        (strstr(buffer, "ST: ssdp:all") || strstr(buffer, "ST: upnp:rootdevice") ||
         strstr(buffer, "ST: urn:schemas-upnp-org:device:MediaServer")))
    {

        // Get current time for DATE header
        time_t now = time(NULL);
        char date_str[64];
        strftime(date_str, sizeof(date_str), "%a, %d %b %Y %H:%M:%S GMT", gmtime(&now));

        // Format response
        char response[BUFFER_SIZE];
        snprintf(response, sizeof(response), search_response_template, date_str, local_endpoint, name, uuid);

        // Send response back to sender
        printf("send m-search reply to %s\n", inet_ntoa(sender_addr->sin_addr));
        sendto(sock, response, strlen(response), 0, (struct sockaddr *)sender_addr, sizeof(*sender_addr));
    }
}

char *get_default_interface()
{
    FILE *fp;
    char line[256], *iface = NULL;
    char iface_name[64], dest[64], gateway[64];

    fp = fopen("/proc/net/route", "r");
    if (fp == NULL)
    {
        perror("Failed to open /proc/net/route");
        return NULL;
    }

    // Skip the header line
    if (fgets(line, sizeof(line), fp))
    {
        while (fgets(line, sizeof(line), fp))
        {
            sscanf(line, "%s %s %s", iface_name, dest, gateway);

            // Default route has destination 00000000
            if (strcmp(dest, "00000000") == 0)
            {
                iface = strdup(iface_name);
                break;
            }
        }
    }

    fclose(fp);
    return iface;
}

int getlocalipaddr(char *ipaddr)
{
    struct ifaddrs *ifaddr, *ifa;
    int family, s;
    char host[NI_MAXHOST];
    char *default_iface;

    // Get the default interface name
    default_iface = get_default_interface();
    if (default_iface == NULL)
    {
        fprintf(stderr, "Could not determine default interface\n");
        return -1;
    }

    if (getifaddrs(&ifaddr) == -1)
    {
        perror("getifaddrs");
        free(default_iface);
        return -1;
    }

    // Find the IP address for the default interface
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next)
    {
        if (ifa->ifa_addr == NULL)
            continue;

        family = ifa->ifa_addr->sa_family;

        if (family == AF_INET && strcmp(ifa->ifa_name, default_iface) == 0)
        {
            s = getnameinfo(ifa->ifa_addr,
                            sizeof(struct sockaddr_in),
                            host,
                            NI_MAXHOST,
                            NULL,
                            0,
                            NI_NUMERICHOST);
            if (s != 0)
            {
                fprintf(stderr, "getnameinfo() failed: %s\n", gai_strerror(s));
            }
            else
            {
                strcpy(ipaddr, host);
                freeifaddrs(ifaddr);
                free(default_iface);
                return 0;
            }
            break;
        }
    }

    freeifaddrs(ifaddr);
    free(default_iface);
    return -1;
}

void generate_uuid(char *uuid) {
    static const char *const lut = "0123456789abcdef";
    unsigned char buffer[16];
    char *p = uuid;
    int i;

    // Generate random bytes
    srand((unsigned int)time(0));
    for (i = 0; i < 16; i++) {
        buffer[i] = (unsigned char)rand() % 256;
    }

    // Version 4 UUID format: xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx
    // where y is 8, 9, A, or B
    buffer[6] = (buffer[6] & 0x0f) | 0x40;
    buffer[8] = (buffer[8] & 0x3f) | 0x80;

    // Convert binary UUID to a string
    for (i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) *p++ = '-';
        *p++ = lut[buffer[i] >> 4];
        *p++ = lut[buffer[i] & 15];
    }
    *p = '\0';
}

int start_upnp_server(int local_port, const char *name)
{
    int sock;
    struct sockaddr_in bind_addr, dest_addr;
    char message[BUFFER_SIZE];
    char buffer[BUFFER_SIZE];
    char local_endpoint[30];
    char uuid[50];

    if (getlocalipaddr(local_endpoint))
    {
        fprintf(stderr, "Cannot get local IP address");
        return -1;
    }
    generate_uuid(uuid);
    sprintf(local_endpoint + strlen(local_endpoint), ":%d", local_port);
    signal(SIGPIPE, SIG_IGN);
    // Create UDP socket for SSDP
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
    {
        perror("socket");
        return -1;
    }

    // Enable address reuse
    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // Bind to SSDP port
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = INADDR_ANY;
    bind_addr.sin_port = htons(SSDP_PORT);

    if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0)
    {
        perror("bind");
        close(sock);
        return -1;
    }

    // Join multicast group
    struct ip_mreq mreq;
    inet_pton(AF_INET, SSDP_ADDR, &mreq.imr_multiaddr);
    mreq.imr_interface.s_addr = INADDR_ANY;
    setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));

    // Set up multicast destination address
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(SSDP_PORT);
    inet_pton(AF_INET, SSDP_ADDR, &dest_addr.sin_addr);

    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0)
    {
        perror("HTTP server socket");
        close(sock);
        return -1;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(local_port);

    if (bind(server_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        perror("HTTP server bind");
        close(sock);
        close(server_sock);
        return -1;
    }
    listen(server_sock, 5);
    printf("HTTP server listening on port %d\n", local_port);

    // Start HTTP server thread
    pthread_t http_thread;
    struct server_param server_param = (struct server_param){server_sock, local_endpoint, name, uuid};
    if (pthread_create(&http_thread, NULL, http_server_thread, &server_param) != 0)
    {
        perror("pthread_create");
        close(sock);
        close(server_sock);
        return -1;
    }
    pthread_detach(http_thread);

    printf("Starting UPnP service on %s\n", local_endpoint);

    // Main loop for SSDP communication
    for (;;)
    {
        // Wait for M-SEARCH requests (with timeout)
        fd_set readfds;
        struct timeval tv;
        FD_ZERO(&readfds);
        FD_SET(sock, &readfds);
        tv.tv_sec = NOTIFY_INTERVAL;
        tv.tv_usec = 0;

        if (select(sock + 1, &readfds, NULL, NULL, &tv) > 0)
        {
            struct sockaddr_in sender_addr;
            socklen_t sender_len = sizeof(sender_addr);
            ssize_t n = recvfrom(sock, buffer, sizeof(buffer) - 1, 0,
                                 (struct sockaddr *)&sender_addr, &sender_len);

            if (n > 0)
            {
                buffer[n] = '\0';
                handle_msearch(sock, buffer, &sender_addr, local_endpoint, name, uuid);
            }
        }
        else
        {
            // Send periodic advertisement
            snprintf(message, sizeof(message), notify_template, local_endpoint, name, uuid);
            sendto(sock, message, strlen(message), 0,
                   (struct sockaddr *)&dest_addr, sizeof(dest_addr));
        }
    }

    close(sock);
    close(server_sock);
    return 0;
}
