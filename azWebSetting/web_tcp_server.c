/* Copyright (c) SC Lee. All rights reserved.
	Licensed under the GNU GPLv3 License.

	part of code copies from Microsoft Sample Private Network Services
	@link https://github.com/Azure/azure-sphere-samples/tree/master/Samples/PrivateNetworkServices
	 Copyright (c) Microsoft Corporation. All rights reserved.
		  Licensed under the MIT License.
	*/

/* Copyright (c) Microsoft Corporation. All rights reserved.
   Licensed under the MIT License. */

#define _GNU_SOURCE // required for asprintf
#include <stdbool.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <stddef.h>
#include <stdarg.h> 
#include <time.h>

#include <sys/socket.h>
#include "applibs_versions.h"
#include <applibs/log.h>
#include <applibs/networking.h>
#include <applibs/storage.h>
#include <applibs/wificonfig.h>
#include "web_tcp_server.h"








static bool isNetworkStackReady = false;
webServer_ServerState* serverState = NULL;

#define MAX_WEBLOG 2048

char weblogBuffer[MAX_WEBLOG];

int weblogBuffer_n = 0;
uint8_t isWebDebug = 0;

#define AFTER_NONE 0
#define AFTER_FORGET 1
#define AFTER_CHANGEWIFI 2
int afterprocess = AFTER_NONE;

char* new_wifi_ssid = NULL;
char* new_wifi_psk = NULL;

extern int epollFd;
int timerFd = -1;


// Support functions.
static void HandleListenEvent(EventData *eventData);
static void LaunchRead(webServer_ServerState *serverState);
static void HandleClientReadEvent(EventData *eventData);
static void LaunchWrite(webServer_ServerState *serverState);
static void HandleClientWriteEvent(EventData *eventData);
static int OpenIpV4Socket(in_addr_t ipAddr, uint16_t port, int sockType);
static void ReportError(const char *desc);
static void StopServer(webServer_ServerState *serverState, webServer_StopReason reason);
static webServer_ServerState *EventDataToServerState(EventData *eventData, size_t offset);


/// <summary>
///     Called when the TCP server stops processing messages from clients.
/// </summary>
void ServerStoppedHandler(webServer_StopReason reason)
{
	const char* reasonText;
	switch (reason) {
	case EchoServer_StopReason_ClientClosed:
		reasonText = "client closed the connection.";

		break;

	case EchoServer_StopReason_Error:
		//	terminationRequired = true;
		reasonText = "an error occurred. See previous log output for more information.";
		break;

	default:
		//	terminationRequired = true;
		reasonText = "unknown reason.";
		break;
	}

	//Restart server
	isNetworkStackReady = false;

	LogWebDebug("INFO: TCP server stopped: %s\n", reasonText);

}



webServer_ServerState *webServer_Start(int epollFd, in_addr_t ipAddr, uint16_t port,
                                         int backlogSize,
                                         void (*shutdownCallback)(webServer_StopReason))
{
    webServer_ServerState *serverState = malloc(sizeof(*serverState));
    if (!serverState) {
        abort();
    }

    // Set EchoServer_ServerState state to unused values so it can be safely cleaned up if only a
    // subset of the resources are successfully allocated.
    serverState->epollFd = epollFd;
    serverState->listenFd = -1;
    serverState->clientFd = -1;
    serverState->listenEvent.eventHandler = HandleListenEvent;
    serverState->clientReadEvent.eventHandler = HandleClientReadEvent;
    serverState->epollInEnabled = false;
    serverState->clientWriteEvent.eventHandler = HandleClientWriteEvent;
    serverState->epollOutEnabled = false;
    serverState->txPayload = NULL;
    serverState->shutdownCallback = shutdownCallback;

    int sockType = SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK;
    serverState->listenFd = OpenIpV4Socket(ipAddr, port, sockType);
    if (serverState->listenFd < 0) {
        ReportError("open socket");
        goto fail;
    }

    // Be notified asynchronously when a client connects.
    RegisterEventHandlerToEpoll(epollFd, serverState->listenFd, &serverState->listenEvent, EPOLLIN);

    int result = listen(serverState->listenFd, backlogSize);
    if (result != 0) {
        ReportError("listen");
        goto fail;
    }

    LogWebDebug("INFO: TCP server: Listening for client connection (fd %d).\n",
              serverState->listenFd);

    return serverState;

fail:
    webServer_ShutDown(serverState);
    return NULL;
}

void webServer_ShutDown(webServer_ServerState *serverState)
{
    if (!serverState) {
        return;
    }

    CloseFdAndPrintError(serverState->clientFd, "clientFd");
    CloseFdAndPrintError(serverState->listenFd, "listenFd");

    free(serverState->txPayload);
	

   
}

void webServer_Restart(webServer_ServerState* serverState){
	int theEpollFd = serverState->epollFd;
	webServer_ShutDown(serverState);
	
	serverState = webServer_Start(theEpollFd, localServerIpAddress.s_addr, LocalTcpServerPort,
		serverBacklogSize, ServerStoppedHandler);
}


static void HandleListenEvent(EventData *eventData)
{
    webServer_ServerState *serverState =
        EventDataToServerState(eventData, offsetof(webServer_ServerState, listenEvent));
    int localFd = -1;

    do {
        // Create a new accepted socket to connect to the client.
        // The newly-accepted sockets should be opened in non-blocking mode, and use
        // EPOLLIN and EPOLLOUT to transfer data.
        struct sockaddr in_addr;
        socklen_t sockLen = sizeof(in_addr);
        localFd = accept4(serverState->listenFd, &in_addr, &sockLen, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (localFd < 0) {
            ReportError("accept");
            break;
        }

        LogWebDebug("INFO: TCP server: Accepted client connection (fd %d).\n", localFd);

        // If already have a client, then close the newly-accepted socket.
        if (serverState->clientFd >= 0) {
            LogWebDebug(
                "INFO: TCP server: Closing incoming client connection: only one client supported "
                "at a time.\n");
            break;
        }

        // Socket opened successfully, so transfer ownership to EchoServer_ServerState object.
        serverState->clientFd = localFd;
        localFd = -1;

        LaunchRead(serverState);
    } while (0);

    close(localFd);
}

static void LaunchRead(webServer_ServerState *serverState)
{
    serverState->inLineSize = 0;
    RegisterEventHandlerToEpoll(serverState->epollFd, serverState->clientFd,
                                &serverState->clientReadEvent, EPOLLIN);
}



static void HandleClientReadEvent(EventData *eventData)
{
    webServer_ServerState *serverState =
        EventDataToServerState(eventData, offsetof(webServer_ServerState, clientReadEvent));

    if (serverState->epollInEnabled) {
        UnregisterEventHandlerFromEpoll(serverState->epollFd, serverState->clientFd);
        serverState->epollInEnabled = false;
    }

    // Continue until no immediately available input or until an error occurs.
    size_t maxChars = sizeof(serverState->input) - 1;
	uint8_t last;

    while (true) {
        // Read a single byte from the client and add it to the buffered line.
        uint8_t b;
		
        ssize_t bytesReadOneSysCall = recv(serverState->clientFd, &b, 1, /* flags */ 0);
		
        // If successfully read a single byte then process it.
        if (bytesReadOneSysCall == 1) {
            // If received newline then print received line to debug log.
			if (b == '\r') {
				serverState->input[serverState->inLineSize] = '\0';
				serverState->inLineSize = 0;
				
			}else if (b== '\n' && last=='\r')
			{
				
                
				//Check the hearder recevied include GET and HTTP string as a http reqest 
				char pos = strstr(serverState->input, "GET");
				if ( pos != NULL && strstr(serverState->input, "HTTP") != NULL) {
					serverState->isHttp = 1;
					int begin = 4;
					int end = 5;
				
					if (serverState->input[begin] == '/') {
						while (serverState->input[end] != ' ')
							end++;
						strncpy(serverState->post, &serverState->input[begin], end - begin);
						serverState->post[end - begin] = '\0';

					
					}
						
				
					 

				}


                LogWebDebug("INFO: TCP server: Received \"%s\"\n", serverState->input);
				//serverState->inLineSize = 0;
                //LaunchWrite(serverState);
                
            }

            // If new character is not printable then discard.
            else if (!isprint(b)) {
                // Special case '\n' to avoid printing a message for every line of input.
                if (b != '\n') {
                    LogWebDebug("INFO: TCP server: Discarding unprintable character 0x%02x\n", b);
                }
            }

            // If new character would leave no space for NUL terminator then reset buffer.
            else if (serverState->inLineSize == maxChars) {
                LogWebDebug("INFO: TCP server: Input data overflow. Discarding %zu characters.\n",
                          maxChars);
                serverState->input[0] = b;
                serverState->inLineSize = 1;
            }

            // Else append character to buffer.
            else {
                serverState->input[serverState->inLineSize] = b;
                ++serverState->inLineSize;
            }

			last = b;
        }

        // If client has shut down restart the webServer.
        else if (bytesReadOneSysCall == 0) {
            LogWebDebug("INFO: TCP server: Client has closed connection.\n");
			webServer_Restart(serverState);
			
        //StopServer(serverState, EchoServer_StopReason_ClientClosed);
            break;
        }

        // If receive buffer is empty then wait for EPOLLIN event.
        else if (bytesReadOneSysCall == -1 && errno == EAGAIN) {
            RegisterEventHandlerToEpoll(serverState->epollFd, serverState->clientFd,
                                        &serverState->clientReadEvent, EPOLLIN);
            serverState->epollInEnabled = true;

			//Launch send after received hearder 
			if (serverState->isHttp==1)
				LaunchWrite(serverState);
			
            break;
        }

        // Another error occured so abort the program.
        else {
            ReportError("recv");
		
			webServer_Restart(serverState);
			


         //  StopServer(serverState, EchoServer_StopReason_Error);
            break;
        }
    }
}

/// <summary>
///    special placeholder for replace loaded page position's string  
/// </summary>
char* str_replace(char* body,int* bodylen,... ) {
	
	char* placeholder = "<!!!---%s";
	int phlen = strlen(placeholder);
	
	int num = 0;
	char* insert = strstr(body, placeholder);
	while (insert != NULL) {
		++num;
		insert = strstr(insert+phlen, placeholder);
	}

	

	
	va_list valist;
	va_start(valist, num);

	
	for (int i = 0;i < num;i++) {
		const char* replace = va_arg(valist,const char*);

		int rlen = strlen(replace);
		
		int shift = rlen - phlen;
		body = realloc(body, *bodylen + shift);

		insert = strstr(body, placeholder);
		char* pos = body + *bodylen;
		if (shift > 0) {
			while (pos-- != insert) {
				*(pos + shift) = *pos;

			}

		}
		else if (shift < 0) {
			pos = insert + rlen - 1;
			char* end = body + *bodylen;
			while (pos - shift != end) {
				*pos = *(++pos - shift);
			}

		}


		pos = insert + rlen - 1;

		while (rlen > 0) {
			*pos-- = replace[--rlen];
		}

		*bodylen += shift;
	}

	va_end(valist);

	return body;
	//*(body + (*bodylen - 1)) = '\0';
	
}


static void afterProcessTimerEventHandler(EventData* eventData)
{
	if (afterprocess == AFTER_FORGET) {
		WifiConfig_ForgetAllNetworks();
	}
	else if (afterprocess == AFTER_CHANGEWIFI) {
		
		
		if (strlen(new_wifi_ssid) == NULL)
			return;
		WifiConfig_ForgetAllNetworks();
		int newid = WifiConfig_AddNetwork();
		WifiConfig_SetSSID(newid,new_wifi_ssid,strlen(new_wifi_ssid));
		if (strlen(new_wifi_psk) != NULL) {
			WifiConfig_SetSecurityType(newid, WifiConfig_Security_Wpa2_Psk);
			WifiConfig_SetPSK(newid, new_wifi_psk, strlen(new_wifi_psk));
		}
		else {
			WifiConfig_SetSecurityType(newid, WifiConfig_Security_Open);
		}

		WifiConfig_SetNetworkEnabled(newid, true);

		WifiConfig_PersistConfig();

	}
	afterprocess = AFTER_NONE;
	CloseFdAndPrintError(timerFd, "afterprocess delay timer error");
	timerFd = -1;
}

static EventData afterPrcoessTimerEventData = { .eventHandler = &afterProcessTimerEventHandler };

/// <summary>
///     Called when the website finished send for after process 
/// </summary>

void web_afterprocess() {
	

	//Wait for 1 second ensure all data transfered before run
	struct timespec delay = { 1, 0 };
	if (timerFd <= 0) {
		timerFd = CreateTimerFdAndAddToEpoll(epollFd, &delay, &afterPrcoessTimerEventData, EPOLLIN);
		if (timerFd < 0) {
			return -1;
		}
	}

	
}
/// <summary>
///    process of "GET" required by client
/// 
/// </summary>
/// <param name="filename">
///     request filename with path 
/// </param>
/// <param name="filetype">
///     file type of request 
/// </param>
/// <param name="body">
///    the body of return http 
/// </param>
/// <param name="bodylen">
///    the len of body
/// </param>
/// <param name="get_name">
///    the array of GET name
/// </param>
/// <param name="get_name">
///    the array of GET value
/// </param>
/// <param name="get_name">
///    total number of get parameter
/// </param>
/// <returns>new body pointer</returns>
char* web_interact(const char* filename,const char* filetype,char* body,int* bodylen, const char** get_name,const char** get_value, int numofget) {
	
	if (!strcmp(filename,"index")) {

		

		WifiConfig_ConnectedNetwork* connected = malloc(sizeof(WifiConfig_ConnectedNetwork));

		char* runjs = malloc(1);
		runjs[0] = '\0';
		char* currentwifi;
		bool withwifi = false;
		
		int result = WifiConfig_GetCurrentNetwork(connected);

		char* ssid = malloc(connected->ssidLength + 1);
		strncpy(ssid, connected->ssid, connected->ssidLength);
		ssid[connected->ssidLength] = '\0';

		if (result < 0) {
			if (errno == ENOTCONN)
				currentwifi = "No WIFI Connected";
			else
				currentwifi = "WIFI not available";
		}
		else {
		

			
			asprintf(&currentwifi, "Current WIFI: %s",ssid );
			withwifi = true;
		}
		
		free(connected);

		if (numofget > 0) {

			int mode = -1;


			for (int i = 0;i < numofget;i++) {
				if (!strcmp(get_name[i], "switchwifi")) {
					if (!strcmp(get_value[i], "OFF")) {
						afterprocess = AFTER_FORGET;

						char* run = "wifioffscreen();";
						int runjslen = strlen(runjs);
						int runlen = strlen(run);
						runjs = realloc(runjs, runjslen+runlen+1);
						for (int i = 0;i < runlen;i++)
							runjs[i + runjslen] = run[i];
						runjs[runjslen + runlen] = '\0';
						
						body = str_replace(body, bodylen, currentwifi, "", withwifi ? "OFF" : "ON", withwifi ? "REMOVE" : "ON", runjs);
						return body;
					}

					break;
				}

				if (!strcmp(get_name[i], "ssid")) {
					new_wifi_ssid = get_value[i];
					afterprocess = AFTER_CHANGEWIFI;
					
				}

				if (!strcmp(get_name[i], "password")) {
					new_wifi_psk = get_value[i];
					afterprocess = AFTER_CHANGEWIFI;
					char* run = "changewifi();";
					int runjslen = strlen(runjs);
					int runlen = strlen(run);
					runjs = realloc(runjs, runjslen + runlen + 1);
					for (int i = 0;i < runlen;i++)
						runjs[i + runjslen] = run[i];
					runjs[runjslen + runlen] = '\0';
					body = str_replace(body, bodylen, currentwifi, "", withwifi ? "OFF" : "ON", withwifi ? "REMOVE" : "ON", runjs);
					return body;
				}

				
			}



			
		}

		

		int scannedwifi = WifiConfig_TriggerScanAndGetScannedNetworkCount();

		WifiConfig_ScannedNetwork* scannetworks= malloc(sizeof(WifiConfig_ScannedNetwork)*scannedwifi);

	

		int numofwifi = WifiConfig_GetScannedNetworks(scannetworks, scannedwifi);

		char* options = malloc(1);
		options[0] = '\0';
		
		for (int i = 0;i < numofwifi;i++) {

			if (scannetworks[i].ssidLength <= 0)
				continue;
			char* ssid = malloc(scannetworks[i].ssidLength + 1);
			
			strncpy(ssid, scannetworks[i].ssid, scannetworks[i].ssidLength);
			ssid[scannetworks[i].ssidLength] = '\0';
			if (scannetworks[i].security == WifiConfig_Security_Unknown)
				continue;
			int id = scannetworks[i].security == WifiConfig_Security_Wpa2_Psk ? 0 : 1;
			char* option;

		
			
	
				

			asprintf(&option, "<option id=\"%d\" value=\"%s\">%-30s %dmhz (%d)</option>\n", id, ssid, ssid,scannetworks[i].frequencyMHz,scannetworks[i].signalRssi);
		
			int optionslen = strlen(options);
			int optionlen = strlen(option);
			options = realloc(options, optionslen + optionlen +1);

			for (int i = 0;i <= optionlen;i++) {
				options[optionslen + i] = option[i];
			}

		}
	

		body = str_replace(body, bodylen,currentwifi, options, withwifi?"OFF":"ON", withwifi?"REMOVE":"ON",runjs);

		free(options);
		free(scannetworks);
		
		return body;


	}
}

static void LaunchWrite(webServer_ServerState *serverState)
{
  

	int begin = 0;
	int end = 0;
	while (serverState->post[++end] != '\0' && serverState->post[end] != '?');
		
	char* path = malloc(end);

	strncpy(path, &serverState->post[begin], end);
	path[end - begin] = '\0';
	int update = 3;
	size_t bodylen = 0;

	//Set value for GET query 
	char** get_name = NULL;
	char** get_value = NULL;
	int numofget = 0;

	if (serverState->post[end++] == '?') {
		begin = end;
		while (serverState->post[end++] != '\0') {
			if (serverState->post[end] == '=') {
				char name[end - begin+1];
				
				strncpy(name, &serverState->post[begin], end-begin);
				name[end - begin] = '\0';
				begin = ++end;
				while (serverState->post[end] != '\0' && serverState->post[end] != '&')
					end++;
				char value[end - begin+1];
				value[end - begin] = '\0';
				strncpy(value, &serverState->post[begin], end-begin);
				
				/// HTTP GET query 
			
				++numofget;
				get_name = realloc(get_name, sizeof(char**) * numofget);
				get_value = realloc(get_value, sizeof(char**) * numofget);

				char* namePtr;
				asprintf(&namePtr, name);
				char* valuePtr;
				asprintf(&valuePtr, value);

				get_name[numofget - 1] = namePtr;
				get_value[numofget - 1] = valuePtr;

				begin = ++end;

				
			}
		}
	}

	

	
	char* timestr;
	time_t t = time(NULL);
	struct tm tm = *localtime(&t);
	
	int hour = tm.tm_hour == 0 ? 12 : tm.tm_hour > 12 ? tm.tm_hour-12 : tm.tm_hour;

	
	asprintf(&timestr, "Time: %d-%d-%d   %s %02d:%02d:%02d",
		tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,tm.tm_hour>=12?"PM":"AM", hour,tm.tm_min, tm.tm_sec);

	
	char* mime = "text/html";


	char* body;
	char* html;
	int result;
	int code;

	
	if (!strcmp(path, "/")) {
		char* first = "index.htm";
		path = "/index.htm";
	}

	
	char* filetype;
	asprintf(&filetype, path);
	




	*filetype = strtok(filetype, ".");
	
	

	while (true) {
		char* next = strtok(NULL, ".");
		if (next != NULL) {
		
			filetype = next;
		}
		else
			break;

	}
	
	int namelen = strlen(path) - strlen(filetype)-2;
	char* filename = malloc(namelen);
	for (int i = 0;i < namelen;i++) {
		filename[i] = path[i + 1];
	}
	filename[namelen] = '\0';

	
	
	//First page, simple show weather data
	





		
	int mimetype = MIME_NONE;

	if (!strcmp(filetype, "htm") || !strcmp(filetype, "html")) {
		mimetype = MIME_TEXT;
		
	}
	else if (!strcmp(filetype, "txt")) {
		mimetype = MIME_TEXT;
		mime = "text/plain";
	}
	else if (!strcmp(filetype, "json")) {
		mimetype = MIME_TEXT;
		mime = "application/json";
	}
	else if (!strcmp(filetype, "js")) {
		mimetype = MIME_TEXT;
		mime = "text/javascript";
	}
	else if (!strcmp(filetype, "ico")) {
		mimetype = MIME_DATA;
		mime = "image/x-icon";
	}
	else if (!strcmp(filetype, "jpg") || !strcmp(filetype, "jpeg")) {
		mimetype = MIME_DATA;
		mime = "image/jpeg";
	}
	else if (!strcmp(filetype, "png")) {
		mimetype = MIME_DATA;
		mime = "image/png";
	}
	else if (!strcmp(filetype, "gif")) {
		mimetype = MIME_DATA;
		mime = "image/gif";
	}
	else if (!strcmp(filetype, "bmp")) {
		mimetype = MIME_DATA;
		mime = "image/bmp";
	}
	else if (!strcmp(filetype, "webp")) {
		mimetype = MIME_DATA;
		mime = "image/webp";
	}
	else if (!strcmp(filetype, "pdf")) {
		mimetype = MIME_DATA;
		mime = "application/pdf";
	}
	else if (!strcmp(filetype, "bz")) {
		mimetype = MIME_DATA;
		mime = "application/x-bzip";
	}
	else if (!strcmp(filetype, "bz2")) {
		mimetype = MIME_DATA;
		mime = "application/x-bzip2";
	}
	else if (!strcmp(filetype, "rar")) {
		mimetype = MIME_DATA;
		mime = "application/x-rar-compressed";
	}
	else if (!strcmp(filetype, "zip")) {
		mimetype = MIME_DATA;
		mime = "application/zip";
	}
	else if (!strcmp(filetype, "7z")) {
		mimetype = MIME_DATA;
		mime = "application/x-7z-compressed";
	}
		
		

	
	
	bool nofile = true;
		
	if (mimetype!=MIME_NONE) {
		char* filepath = ((char*)path)+1;
			
		int fileFD = Storage_OpenFileInImagePackage(filepath);
		if (fileFD < 0) {
			Log_Debug("ERROR: Storage Error: errno=%d (%s)\n", errno,
					strerror(errno));
			
		}else {
				nofile = false;

				char buf[FILEREADBUFFERSIZE];
				
				char* data = NULL;
				size_t size = 0;
				size_t total = 0;
				do{
					size = read(fileFD, buf, FILEREADBUFFERSIZE);
					if (size <= 0)
						break;


					data = (char*) realloc(data,(total+size)*sizeof(char));

					for (int i = 0;i < size;i++)
						data[i + total] = buf[i];

					total += size;
						

				}while (true);


				free(buf);

				if (mimetype==MIME_TEXT){
					data = (char*)realloc(data, ((total) + 1)*sizeof(char));
					data[total] = '\0';
					total += 1;
					
				}
					
				body=data;

				
				
				bodylen = total;
			}
		}


		if (nofile) {
			code = 404;
			result = asprintf(&body, "<!DOCTYPE html PUBLIC \"-//W3C//DTD HTML 4.01 Transitional//EN\" \"http://www.w3.org/TR/html4/loose.dtd\">\n\
			<html>\n\
			<head>\n\
			 <meta http-equiv=\"Content-type\" content=\"text/html;charset=UTF-8\">\n\
			<title>Weather </title>\n\
			 </head>\n\
			  <body >\n\
				<h4>404 Not found</h4></body></html><br>azsphere webInterface", localServerIpAddress.s_addr, LocalTcpServerPort);

			bodylen = strlen(body);
			mimetype == MIME_TEXT;
			mime = "text/html";

			if (result == -1) {
				ReportError("asprintf");
				StopServer(serverState, EchoServer_StopReason_Error);
				return;
			}
		}
		else {
			code = 200;
		}
	


		if (mimetype == MIME_TEXT) 
			body = web_interact(filename, filetype, body, &bodylen, get_name, get_value, numofget);

	//header
	char* status;
	if (code == 200)
		asprintf(&status, "%d %s", code, "Ok");
	else
		asprintf(&status, "%d %s", code, "Not found");
	char* header;	result = asprintf(&header, "HTTP/1.1 %s \015\012\
Server: AzSphere\015\012\
Cache-Control: private, max-age=0\015\012\
Content-Length:%d\015\012\
Content-Type: %s\015\012\
Connection:close\015\012\
\015\012",status,bodylen,mime);

	int headerlen = strlen(header);

	if (mimetype == MIME_TEXT) {

		
		asprintf(&html, "%s%s", header, body);
	}
	else if (mimetype == MIME_DATA) {
	
		html = realloc(header,  sizeof(char)*(headerlen  + bodylen));
		for (int i = 0;i < bodylen;i++)
			html[headerlen + i] = body[i];

		
	}


	//free(body);
	//free(status);

	serverState->txPayloadSize = bodylen+headerlen;
	serverState->txPayload = (uint8_t*)html;
	serverState->txBytesSent = 0;
	HandleClientWriteEvent(&serverState->clientWriteEvent);

	
	
	//free(serverState->txPayload);

	

	//
	webServer_Restart(serverState);
	web_afterprocess();
	
}

static void HandleClientWriteEvent(EventData *eventData)
{
    webServer_ServerState *serverState =
        EventDataToServerState(eventData, offsetof(webServer_ServerState, clientWriteEvent));

    if (serverState->epollOutEnabled) {
        UnregisterEventHandlerFromEpoll(serverState->epollFd, serverState->clientFd);
        serverState->epollOutEnabled = false;
    }

    // Continue until have written entire response, error occurs, or OS TX buffer is full.
    while (serverState->txBytesSent < serverState->txPayloadSize) {
        size_t remainingBytes = serverState->txPayloadSize - serverState->txBytesSent;
        const uint8_t *data = &serverState->txPayload[serverState->txBytesSent];
        ssize_t bytesSentOneSysCall =
            send(serverState->clientFd, data, remainingBytes, /* flags */ 0);

        // If successfully sent data then stay in loop and try to send more data.
        if (bytesSentOneSysCall > 0) {
            serverState->txBytesSent += (size_t)bytesSentOneSysCall;
        }

        // If OS TX buffer is full then wait for next EPOLLOUT.
        else if (bytesSentOneSysCall < 0 && errno == EAGAIN) {
            RegisterEventHandlerToEpoll(serverState->epollFd, serverState->clientFd,
                                        &serverState->clientWriteEvent, EPOLLOUT);
            serverState->epollOutEnabled = true;
            return;
        }

        // Another error occurred so terminate the program.
        else {
            ReportError("send");
           // StopServer(serverState, EchoServer_StopReason_Error);
			webServer_Restart(serverState);
			
            return;
        }
    }
	

    // If reached here then successfully sent entire payload so clean up and read next line from
    // client.
    free(serverState->txPayload);
    serverState->txPayload = NULL;
	//int fd = serverState->epollFd;
	
	

	//Restart for next process 
	webServer_Restart(serverState);
	
   // LaunchRead(serverState);
}

static int OpenIpV4Socket(in_addr_t ipAddr, uint16_t port, int sockType)
{
    int localFd = -1;
    int retFd = -1;

    do {
        // Create a TCP / IPv4 socket. This will form the listen socket.
        localFd = socket(AF_INET, sockType, /* protocol */ 0);
        if (localFd < 0) {
            ReportError("socket");
            break;
        }

        // Enable rebinding soon after a socket has been closed.
        int enableReuseAddr = 1;
        int r = setsockopt(localFd, SOL_SOCKET, SO_REUSEADDR, &enableReuseAddr,
                           sizeof(enableReuseAddr));
        if (r != 0) {
            ReportError("setsockopt/SO_REUSEADDR");
            break;
        }

        // Bind to a well-known IP address.
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = ipAddr;
        addr.sin_port = htons(port);

        r = bind(localFd, (const struct sockaddr *)&addr, sizeof(addr));
        if (r != 0) {
            ReportError("bind");
            break;
        }

        // Port opened successfully.
        retFd = localFd;
        localFd = -1;
    } while (0);

    close(localFd);

    return retFd;
}

static void ReportError(const char *desc)
{
    LogWebDebug("ERROR: TCP server: \"%s\", errno=%d (%s)\n", desc, errno, strerror(errno));
}

static void StopServer(webServer_ServerState *serverState, webServer_StopReason reason)
{
    // Stop listening for incoming connections.
    if (serverState->listenFd != -1) {
        UnregisterEventHandlerFromEpoll(serverState->epollFd, serverState->listenFd);
    }

    serverState->shutdownCallback(reason);
}

static webServer_ServerState *EventDataToServerState(EventData *eventData, size_t offset)
{
    uint8_t *eventData8 = (uint8_t *)eventData;
    uint8_t *serverState8 = eventData8 - offset;
    return (webServer_ServerState *)serverState8;
}


int LogWebDebug(const char* fmt, ...) {

	va_list argptr;
	va_start(argptr, fmt);

	Log_DebugVarArgs(fmt, argptr);

	if (!isWebDebug)
		return 0;


	char buffer[256];


	int total = vsprintf(buffer, fmt, argptr);

	if (total == 0)
		return 0;

	if (buffer[total - 1] != '\n') {
		buffer[total++] = '\n';
		buffer[total] = '\0';
	}
	
	for (int i = 0;i < total;i++) {
		if (buffer[i] == '<') {
			// replace to "(" if not enough buffer
			if (total + 3 > 256) {
				buffer[i] = '(';
				continue;
			}
			// otherwise replace html escape
			for (int j = total;j > i + 3;j--)
				buffer[j] = buffer[j - 3];
			buffer[i] = '&';
			buffer[i + 1] = 'l';
			buffer[i + 2] = 't';
			buffer[i + 3] = ';';
			total += 3;
		}
		else if (buffer[i] == '>') {
			// replace to ")" if not enough buffer
			if (total + 3 > 256) {
				buffer[i] = ')';
				continue;
			}
			// otherwise replace html escape
			for (int j = total;j > i + 3;j--)
				buffer[j] = buffer[j - 3];
			buffer[i] = '&';
			buffer[i + 1] = 'g';
			buffer[i + 2] = 't';
			buffer[i + 3] = ';';
			total += 3;

		}
		else if (buffer[i] == '&') {
			// replace to "A" if not enough buffer
			if (total + 4 > 256) {
				buffer[i] = 'A';
				continue;
			}
			// otherwise replace html escape
			for (int j = total;j > i + 4;j--)
				buffer[j] = buffer[j - 4];
			buffer[i] = '&';
			buffer[i + 1] = 'a';
			buffer[i + 2] = 'm';
			buffer[i + 3] = 'p';
			buffer[i + 4] = ';';
			total += 4;
		}

	}


	weblogBuffer_n += total;

	while (weblogBuffer_n > MAX_WEBLOG) {
		// find first next line char 
		char* firstnextline = strchr(weblogBuffer, '\n');
		if (firstnextline == NULL) {
			weblogBuffer_n = 0;
				break;
		}

		//shift content to begin

		int i = 0;
		do {
			weblogBuffer[i++] = *++firstnextline;
		} while (*(firstnextline) != '\0');
		weblogBuffer_n = i+total-1;


	}

    int j = 0;
	int i = weblogBuffer_n - total;
	if (i > 0)
		i--;

	for (;i < weblogBuffer_n;i++)
		weblogBuffer[i] = buffer[j++];







	
	

}
