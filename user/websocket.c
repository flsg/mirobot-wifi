#include "espmissingincludes.h"
#include "c_types.h"
#include "user_interface.h"
#include "espconn.h"
#include "mem.h"
#include "osapi.h"
#include "sha1.h"
#include "base64.h"

#include "espconn.h"
#include "websocket.h"
#include "io.h"
#include "driver/uart.h"

//Max length of request head
#define MAX_HEAD_LEN 1024
//Max amount of connections
#define MAX_CONN 1

//Private data for http connection
struct WsPriv {
	char data[MAX_HEAD_LEN];
	int headPos;
	int postPos;
};

//Connection pool
static WsPriv connPrivData[MAX_CONN];
static WsConnData connData[MAX_CONN];

//Listening connection data
static struct espconn wsConn;
static esp_tcp wsTcp;

typedef void (*wsHandler)(int, char*);
wsHandler handlerCb;

char temp_buff[130];

char webSocketKey[] = "------------------------258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

//Looks up the connData info for a specific esp connection
static WsConnData ICACHE_FLASH_ATTR *wsFindConnData(void *arg) {
	int i;
	for (i=0; i<MAX_CONN; i++) {
		if (connData[i].conn==(struct espconn *)arg) return &connData[i];
	}
	os_printf("FindConnData: Huh? Couldn't find connection for %p\n", arg);
	return NULL; //WtF?
}

//Retires a connection for re-use
static void ICACHE_FLASH_ATTR wsRetireConn(WsConnData *conn) {
	conn->conn=NULL;
}

void ICACHE_FLASH_ATTR wsSendUpgrade(WsConnData *conn) {
	uint8 *hash;
  char b64Result[31];
  sha1nfo s;

  strncpy(webSocketKey, conn->key, 24);
  
  // Work out the sha hash of the full websocket key
  sha1_init(&s);
  sha1_write(&s, webSocketKey, 60);
  hash = sha1_result(&s);
  
  // Base64 encode it
  base64_encode(20, (unsigned char *)hash, 30, b64Result);
  b64Result[30] = 0;

	int l;
  l=os_sprintf(temp_buff, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", b64Result);
  espconn_sent(conn->conn, (uint8 *)temp_buff, l);
}

//Send a http header.
void ICACHE_FLASH_ATTR wsHeader(WsConnData *conn, const char *field, const char *val) {
	char buff[256];
	int l;
	l=os_sprintf(buff, "%s: %s\r\n", field, val);
	espconn_sent(conn->conn, (uint8 *)buff, l);
}

//Finish the headers.
void ICACHE_FLASH_ATTR wsEndHeaders(WsConnData *conn) {
	espconn_sent(conn->conn, (uint8 *)"\r\n", 2);
}

//Callback called when the data on a socket has been successfully
//sent.
static void ICACHE_FLASH_ATTR wsSentCb(void *arg) {
  /*
	int r;
	WsConnData *conn=wsFindConnData(arg);
//	os_printf("Sent callback on conn %p\n", conn);
	if (conn==NULL) return;
	if (conn->cgi==NULL) { //Marked for destruction?
		os_printf("Conn %p is done. Closing.\n", conn->conn);
		espconn_disconnect(conn->conn);
		wsRetireConn(conn);
		return;
	}

	r=conn->cgi(conn); //Execute cgi fn.
	if (r==HTTPD_CGI_DONE) {
		conn->cgi=NULL; //mark for destruction.
	}
	*/
}

//Parse a line of header data and modify the connection data accordingly.
static void ICACHE_FLASH_ATTR wsParseHeader(char *h, WsConnData *conn) {
	int i;
  char *e;
	if (os_strncmp(h, "GET ", 4)==0) {
		//Skip past the space after POST/GET
		i = 0;
		while (h[i] != ' ') i++;
		conn->url = h+i+1;

		//Figure out end of url.
		e = (char*)os_strstr(conn->url, " ");
		if (e == NULL) return; //wtf?
		*e = 0; //terminate url part
	} else if (os_strncmp(h, "Sec-WebSocket-Key: ", 19)==0) {
		//Skip past the space after POST/GET
		i = 0;
		while (h[i] != ' ') i++;
		conn->key = h+i+1;
		//Figure out end of url.
		e = (char*)os_strstr(conn->url, "\r");
		if (e == NULL) return; //wtf?
		*e = 0; //terminate url part
	}
}

bool parseWSFrame(char* out, char* frame){
  bool fin = false;
  uint8_t opcode = 0;
  bool mask_set = false;
  uint8_t length = 0;
  uint8_t mask[4] = {0,0,0,0};
  int i;
  
  // byte 1
  fin = frame[0] >> 7;
  opcode = frame[0] & 0x0F;
  
  if(fin != 1 && opcode != 0x01 && opcode != 0x08){
    //It's not a websocket frame or it's not final
    return false;
  }
  
  //byte 2
  mask_set = frame[1] >> 7;
  length = frame[1] & 0x7F;
  
  if(strlen(frame) >= (length + 6)){
    if(length < 125){
      if(opcode == 0x08){
        // The socket is closing
        return false;
      }
      //extract the mask
      if(mask_set){
        for(int i = 0; i<4; i++){
          mask[i] = frame[i + 2];
        }
      }
      //process the message
      for(i=0; i<length; i++){
        out[i] = (char)(frame[i+6] ^ mask[i % 4]);
      }
      out[i] = 0;
      return true;
    }
  }
  return false;
}

//Callback called when there's data available on a socket.
static void ICACHE_FLASH_ATTR wsRecvCb(void *arg, char *data, unsigned short len) {
	int x;
	char *p, *e;
	char wsBuff[126];
	WsConnData *conn=wsFindConnData(arg);
	if (conn==NULL) return;
	
  if(conn->connType == WEBSOCKET){
    // decode the websocket frame and send data
    if(parseWSFrame(wsBuff, data)){
      handlerCb(0, wsBuff);
    }
  }else if(conn->connType == RAW){
    // just send the data straight through
    handlerCb(0, data);
  }else if(conn->connType == UNKNOWN){
	  // We need to see if the first line of data is a websocket get request
    if(len > 3 && !os_strncmp(data, "GET ", 4)){
      // It looks like it might be a HTTP request
      for (x=0; x<len; x++) {
        if (conn->priv->headPos!=-1) {
          //This byte is a header byte.
          if (conn->priv->headPos!=MAX_HEAD_LEN) conn->priv->data[conn->priv->headPos++]=data[x];
          conn->priv->data[conn->priv->headPos]=0;
          //Scan for \r\n\r\n
          if (data[x]=='\n' && (char *)os_strstr(conn->priv->data, "\r\n\r\n")!=NULL) {
            // The headers have ended so let's parse them
            //Find end of next header line
            p=conn->priv->data;
            while(p<(&conn->priv->data[conn->priv->headPos-4])) {
              e=(char *)os_strstr(p, "\r\n");
              if (e==NULL) break;
              e[0]=0;
              wsParseHeader(p, conn);
              p=e+2;
            }
            //If we don't need to receive post data, we can send the response now.
            if(!strcmp(conn->url, "/websocket") && conn->key != NULL){
              // it's a legitimate websocket request so let's respond accordingly
              conn->connType = WEBSOCKET;
              wsSendUpgrade(conn);
              //if (conn->postLen==0) {
              //  httpdSendResp(conn);
              //  }
            }else{
              // doesn't look legit so let's give a 404 for now
            }
            conn->priv->headPos=-1; //Indicate we're done with the headers.
          }else{
            // it doesn't look like a http request so let's just send the data through
          }
        }
      }
    }else{
      // Looks like a raw socket connection
      conn->connType = RAW;
      uart0_tx_buffer((uint8 *)data, len);
    }
  }
}

static void ICACHE_FLASH_ATTR wsReconCb(void *arg, sint8 err) {
	WsConnData *conn=wsFindConnData(arg);
	os_printf("ReconCb\n");
	if (conn==NULL) return;
	//Yeah... No idea what to do here. ToDo: figure something out.
}

static void ICACHE_FLASH_ATTR wsDisconCb(void *arg) {
#if 0
	//Stupid esp sdk passes through wrong arg here, namely the one of the *listening* socket.
	//If it ever gets fixed, be sure to update the code in this snippet; it's probably out-of-date.
	WsConnData *conn=wsFindConnData(arg);
	os_printf("Disconnected, conn=%p\n", conn);
	if (conn==NULL) return;
	conn->conn=NULL;
#endif
	//Just look at all the sockets and kill the slot if needed.
	int i;
	for (i=0; i<MAX_CONN; i++) {
		if (connData[i].conn!=NULL) {
			if (connData[i].conn->state==ESPCONN_NONE || connData[i].conn->state==ESPCONN_CLOSE) {
				connData[i].conn=NULL;
				wsRetireConn(&connData[i]);
			}
		}
	}
}


static void ICACHE_FLASH_ATTR wsConnectCb(void *arg) {
	struct espconn *conn=arg;
	int i;
	//Find empty conndata in pool
	for (i=0; i<MAX_CONN; i++) if (connData[i].conn==NULL) break;
	os_printf("Con req, conn=%p, pool slot %d\n", conn, i);
	connData[i].priv=&connPrivData[i];
	if (i==MAX_CONN) {
		os_printf("Conn pool overflow!\n");
		espconn_disconnect(conn);
		return;
	}
	connData[i].conn=conn;
	connData[i].priv->headPos=0;
	connData[i].priv->postPos=0;
	connData[i].connType = UNKNOWN;
	connData[i].url = NULL;
	connData[i].key = NULL;

	espconn_regist_recvcb(conn, wsRecvCb);
	espconn_regist_reconcb(conn, wsReconCb);
	espconn_regist_disconcb(conn, wsDisconCb);
	espconn_regist_sentcb(conn, wsSentCb);
}

void ICACHE_FLASH_ATTR wsSend(int conn_no, char *msg){
  if(connData[0].connType == WEBSOCKET){
    os_sprintf(temp_buff, "%c%c%s", 0x81, (strlen(msg) & 0x7F), msg);
    espconn_sent(connData[conn_no].conn, (uint8 *)temp_buff, strlen(temp_buff));
  }else{
    espconn_sent(connData[conn_no].conn, (uint8 *)msg, strlen(msg));
  }
}

void ICACHE_FLASH_ATTR recvTask(void){
  
}

void ICACHE_FLASH_ATTR wsInit(int port, void *handler){
	for (int i=0; i<MAX_CONN; i++) {
		connData[i].conn=NULL;
	}
	wsConn.type=ESPCONN_TCP;
	wsConn.state=ESPCONN_NONE;
	wsTcp.local_port=port;
	wsConn.proto.tcp=&wsTcp;
	handlerCb = handler;

	os_printf("Httpd init, conn=%p\n", &wsConn);
	espconn_regist_connectcb(&wsConn, wsConnectCb);
	espconn_accept(&wsConn);
	espconn_regist_time(&wsConn, 28800, 0);
}