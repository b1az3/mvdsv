/*
This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the included (GNU.txt) GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

    $Id$
*/

//	sv_demo_qtv.c - misc QTV's code

#include "qwsvdef.h"

cvar_t	qtv_streamport		= {"qtv_streamport",		"0"};
cvar_t	qtv_maxstreams		= {"qtv_maxstreams",		"1"};
cvar_t	qtv_password		= {"qtv_password",			""};
cvar_t	qtv_pendingtimeout	= {"qtv_pendingtimeout",	"5"};  // 5  seconds must be enough
cvar_t	qtv_streamtimeout	= {"qtv_streamtimeout",		"45"}; // 45 seconds

static mvddest_t *SV_InitStream (int socket1, netadr_t na, char *userinfo)
{
	static int lastdest = 0;
	int count;
	mvddest_t *dst;
	char name[sizeof(dst->qtvname)];

	// extract name
	strlcpy(name, Info_ValueForKey(userinfo, "name"), sizeof(name));

	count = 0;
	for (dst = demo.dest; dst; dst = dst->nextdest)
	{
		if (dst->desttype == DEST_STREAM)
		{
			if (name[0] && !strcasecmp(name, dst->qtvname))
				return NULL; // duplicate name, well empty names may still duplicates...

			count++;
		}
	}

	if (count >= (int)qtv_maxstreams.value)
		return NULL; //sorry

	dst = (mvddest_t *) Q_malloc (sizeof(mvddest_t));

	dst->desttype = DEST_STREAM;
	dst->socket = socket1;
	dst->maxcachesize = 65536;	//is this too small?
	dst->cache = (char *) Q_malloc(dst->maxcachesize);
	dst->io_time = Sys_DoubleTime();
	dst->id = ++lastdest;
	dst->na = na;

	strlcpy(dst->qtvname, name, sizeof(dst->qtvname));

	if (dst->qtvname[0])
		SV_BroadcastPrintf (PRINT_CHAT, "Smile, you're on QTV(%s)!\n", dst->qtvname);
	else
		SV_BroadcastPrintf (PRINT_CHAT, "Smile, you're on QTV!\n");

	return dst;
}

static void SV_MVD_InitPendingStream (int socket1, netadr_t na)
{
	mvdpendingdest_t *dst;
	unsigned int i;
	dst = (mvdpendingdest_t*) Q_malloc(sizeof(mvdpendingdest_t));
	dst->socket = socket1;
	dst->io_time = Sys_DoubleTime();
	dst->na = na;

	strlcpy(dst->challenge, NET_AdrToString(dst->na), sizeof(dst->challenge));
	for (i = strlen(dst->challenge); i < sizeof(dst->challenge)-1; i++)
		dst->challenge[i] = rand()%(127-33) + 33;	//generate a random challenge

	dst->nextdest = demo.pendingdest;
	demo.pendingdest = dst;
}

static int MVD_StreamStartListening (int port)
{
	int sock;

	struct sockaddr_in	address;
#ifdef SOCKET_CLOSE_TIME
	struct linger lingeropt;
#endif
	//	int fromlen;

	unsigned long nonblocking = true;

	address.sin_family = AF_INET;
	address.sin_addr.s_addr = INADDR_ANY;
	address.sin_port = htons((short)port);

	if ((sock = socket (PF_INET, SOCK_STREAM, IPPROTO_TCP)) == -1)
	{
		Con_Printf ("MVD_StreamStartListening: socket: (%i): %s\n", qerrno, strerror(qerrno));
		return INVALID_SOCKET;
	}

#ifdef SOCKET_CLOSE_TIME
	// hard close: in case of closesocket(), socket will be closen after SOCKET_CLOSE_TIME or earlier
	memset(&lingeropt, 0, sizeof(lingeropt));
	lingeropt.l_onoff  = 1;
	lingeropt.l_linger = SOCKET_CLOSE_TIME;

	if (setsockopt(sock, SOL_SOCKET, SO_LINGER, (void*)&lingeropt, sizeof(lingeropt)) == -1)
	{
		Con_Printf ("MVD_StreamStartListening: setsockopt SO_LINGER: (%i): %s\n", qerrno, strerror (qerrno));
		closesocket(sock);
		return INVALID_SOCKET;
    }
#endif

	if (ioctlsocket(sock, FIONBIO, &nonblocking) == -1)
	{
		Con_Printf ("MVD_StreamStartListening: ioctl FIONBIO: (%i): %s\n", qerrno, strerror(qerrno));
		closesocket(sock);
		return INVALID_SOCKET;
	}

	if(bind(sock, (struct sockaddr *)&address, sizeof(address)) == -1)
	{
		Con_Printf ("MVD_StreamStartListening: bind: (%i): %s\n", qerrno, strerror(qerrno));
		closesocket(sock);
		return INVALID_SOCKET;
	}

	if(listen(sock, 2) == -1)
	{
		Con_Printf ("MVD_StreamStartListening: listen: (%i): %s\n", qerrno, strerror(qerrno));
		closesocket(sock);
		return INVALID_SOCKET;
	}

	if (!TCP_Set_KEEPALIVE(sock))
	{
		Con_Printf ("MVD_StreamStartListening: TCP_Set_KEEPALIVE: failed\n");
		closesocket(sock);
		return INVALID_SOCKET;
	}

	return sock;
}

void SV_MVDCloseStreams(void)
{
	mvddest_t *d;
	mvdpendingdest_t *p;

	for (d = demo.dest; d; d = d->nextdest)
		if (!d->error && d->desttype == DEST_STREAM)
			d->error = true; // mark demo stream dest to close later

	for (p = demo.pendingdest; p; p = p->nextdest)
		if (!p->error)
			p->error = true; // mark pending dest to close later
}


static int		listensocket	= INVALID_SOCKET;
static int		listenport		= 0;
static double	warned_time		= 0;

static void SV_CheckQTVPort(void)
{
	qbool changed;
	int streamport = bound(0, (int)qtv_streamport.value, 64000); // so user can't specifie something stupid

	// if we have non zero stream port, but fail to open listen socket, repeat open listen socket after some time
	changed = ( streamport != listenport || (streamport && listensocket == INVALID_SOCKET && warned_time + 10 < Sys_DoubleTime()) );

	// port not changed
	if (!changed)
		return;

	SV_MVDCloseStreams(); // also close ative connects if any, this will help actually close listen socket, so later we can bind to port again

	warned_time = Sys_DoubleTime(); // so we repeat warning time to time

	if (listensocket != INVALID_SOCKET)
	{
		Con_Printf("Closing TCP port %d for QTV\n", listenport);
		closesocket(listensocket); // so we close socket
		listensocket = INVALID_SOCKET; // and mark as closed
	}

	// port was changed, lets remember
	listenport = streamport;

	if (!listenport)
		return; // they just wanna turn it off

	if ((listensocket = MVD_StreamStartListening(listenport)) == INVALID_SOCKET)
		Con_Printf("WARNING: Cannot open TCP port %d for QTV\n", listenport);
	else
		Con_Printf("Opening TCP port %d for QTV\n", listenport);
}

void SV_MVDStream_Poll (void)
{
	int client;
	netadr_t na;
	struct sockaddr_qstorage addr;
	socklen_t addrlen;
#ifdef SOCKET_CLOSE_TIME
	struct linger lingeropt;
#endif
	int count;
	mvddest_t *dest;
	unsigned long _true = true;

	SV_CheckQTVPort(); // open/close/switch qtv port

	if (listensocket == INVALID_SOCKET) // we can't accept connection from QTV
	{
		SV_MVDCloseStreams(); // also close ative connects if any, this will help actually close listen socket, so later we can bind to port again
		return;
	}

	addrlen = sizeof(addr);
	client = accept (listensocket, (struct sockaddr *)&addr, &addrlen);

	if (client == INVALID_SOCKET)
		return;

#ifdef SOCKET_CLOSE_TIME
	// hard close: in case of closesocket(), socket will be closen after SOCKET_CLOSE_TIME or earlier
	memset(&lingeropt, 0, sizeof(lingeropt));
	lingeropt.l_onoff  = 1;
	lingeropt.l_linger = SOCKET_CLOSE_TIME;

	if (setsockopt(client, SOL_SOCKET, SO_LINGER, (void*)&lingeropt, sizeof(lingeropt)) == -1)
	{
		Con_Printf ("SV_MVDStream_Poll: setsockopt SO_LINGER: (%i): %s\n", qerrno, strerror (qerrno));
		closesocket(client);
		return;
    }
#endif

	if (ioctlsocket (client, FIONBIO, &_true) == SOCKET_ERROR) {
		Con_Printf ("SV_MVDStream_Poll: ioctl FIONBIO: (%i): %s\n", qerrno, strerror (qerrno));
		closesocket(client);
		return;
	}

	if (!TCP_Set_KEEPALIVE(client))
	{
		Con_Printf ("SV_MVDStream_Poll: TCP_Set_KEEPALIVE: failed\n");
		closesocket(client);
		return;
	}

	if ((int)qtv_maxstreams.value > 0)
	{
		count = 0;
		for (dest = demo.dest; dest; dest = dest->nextdest)
		{
			if (dest->desttype == DEST_STREAM)
			{
				count++;
			}
		}

		if (count >= (int)qtv_maxstreams.value)
		{	//sorry
			char *goawaymessage = "QTVSV 1\nERROR: This server enforces a limit on the number of proxies connected at any one time. Please try again later\n\n";

			send(client, goawaymessage, strlen(goawaymessage), 0);
			closesocket(client);
			return;
		}
	}

	SockadrToNetadr(&addr, &na);
	Con_Printf("MVD streaming client connected from %s\n", NET_AdrToString(na));

	SV_MVD_InitPendingStream(client, na);
}

void SV_MVD_RunPendingConnections (void)
{
	unsigned short ushort_result;
	char *e;
	int len;
	mvdpendingdest_t *p;
	mvdpendingdest_t *np;
	char userinfo[1024] = {0};

	if (!demo.pendingdest)
		return;

	for (p = demo.pendingdest; p; p = p->nextdest)
		if (p->io_time + qtv_pendingtimeout.value <= Sys_DoubleTime())
		{
			Con_Printf("Pending dest timeout\n");
			p->error = true;
		}

	while (demo.pendingdest && demo.pendingdest->error)
	{
		np = demo.pendingdest->nextdest;

		if (demo.pendingdest->socket != -1)
			closesocket(demo.pendingdest->socket);
		Q_free(demo.pendingdest);
		demo.pendingdest = np;
	}

	for (p = demo.pendingdest; p && p->nextdest; p = p->nextdest)
	{
		if (p->nextdest->error)
		{
			np = p->nextdest->nextdest;
			if (p->nextdest->socket != -1)
				closesocket(p->nextdest->socket);
			Q_free(p->nextdest);
			p->nextdest = np;
		}
	}

	for (p = demo.pendingdest; p; p = p->nextdest)
	{
		if (p->outsize && !p->error)
		{
			len = send(p->socket, p->outbuffer, p->outsize, 0);

			if (len == 0) //client died
			{
//				p->error = true;
				// man says: The calls return the number of characters sent, or -1 if an error occurred.   
				// so 0 is legal or what?
			}
			else if (len > 0)	//we put some data through
			{
				p->io_time = Sys_DoubleTime(); // update IO activity

				//move up the buffer
				p->outsize -= len;
				memmove(p->outbuffer, p->outbuffer+len, p->outsize );

			}
			else
			{ //error of some kind. would block or something
				if (qerrno != EWOULDBLOCK && qerrno != EAGAIN)
					p->error = true;
			}
		}

		if (!p->error)
		{
			len = recv(p->socket, p->inbuffer + p->insize, sizeof(p->inbuffer) - p->insize - 1, 0);

			if (len > 0)
			{ //fixme: cope with extra \rs
				char *end;

				p->io_time = Sys_DoubleTime(); // update IO activity

				p->insize += len;
				p->inbuffer[p->insize] = 0;

				for (end = p->inbuffer; ; end++)
				{
					if (*end == '\0')
					{
						end = NULL;
						break;	//not enough data
					}

					if (end[0] == '\n')
					{
						if (end[1] == '\n')
						{
							end[1] = '\0';
							break;
						}
					}
				}
				if (end)
				{ //we found the end of the header
					char *start, *lineend;
					int versiontouse = 0;
					int raw = 0;
					char password[256] = "";
					typedef enum {
						QTVAM_NONE,
						QTVAM_PLAIN,
						QTVAM_CCITT,
						QTVAM_MD4,
					} authmethod_t;
					authmethod_t authmethod = QTVAM_NONE;
					start = p->inbuffer;

					lineend = strchr(start, '\n');
					if (!lineend)
					{
//						char *e;
//						e = "This is a QTV server.";
//						send(p->socket, e, strlen(e), 0);

						p->error = true;
						continue;
					}
					*lineend = '\0';
					COM_ParseToken(start, NULL);
					start = lineend+1;
					if (strcmp(com_token, "QTV"))
					{ //it's an error if it's not qtv.
						p->error = true;
						lineend = strchr(start, '\n');
						continue;
					}

					for(;;)
					{
						lineend = strchr(start, '\n');
						if (!lineend)
							break;
						*lineend = '\0';
						start = COM_ParseToken(start, NULL);
						if (*start == ':')
						{
//VERSION: a list of the different qtv protocols supported. Multiple versions can be specified. The first is assumed to be the prefered version.
//RAW: if non-zero, send only a raw mvd with no additional markup anywhere (for telnet use). Doesn't work with challenge-based auth, so will only be accepted when proxy passwords are not required.
//AUTH: specifies an auth method, the exact specs varies based on the method
//		PLAIN: the password is sent as a PASSWORD line
//		MD4: the server responds with an "AUTH: MD4\n" line as well as a "CHALLENGE: somerandomchallengestring\n" line, the client sends a new 'initial' request with CHALLENGE: MD4\nRESPONSE: hexbasedmd4checksumhere\n"
//		CCITT: same as md4, but using the CRC stuff common to all quake engines.
//		if the supported/allowed auth methods don't match, the connection is silently dropped.
//SOURCE: which stream to play from, DEFAULT is special. Without qualifiers, it's assumed to be a tcp address.
//COMPRESSION: Suggests a compression method (multiple are allowed). You'll get a COMPRESSION response, and compression will begin with the binary data.

							start = start+1;
							Con_Printf("qtv, got (%s) (%s)\n", com_token, start);
							if (!strcmp(com_token, "VERSION"))
							{
								start = COM_ParseToken(start, NULL);
								if (atoi(com_token) == 1)
									versiontouse = 1;
							}
							else if (!strcmp(com_token, "RAW"))
							{
								start = COM_ParseToken(start, NULL);
								raw = atoi(com_token);
							}
							else if (!strcmp(com_token, "PASSWORD"))
							{
								start = COM_ParseToken(start, NULL);
								strlcpy(password, com_token, sizeof(password));
							}
							else if (!strcmp(com_token, "AUTH"))
							{
								authmethod_t thisauth;
								start = COM_ParseToken(start, NULL);
								if (!strcmp(com_token, "NONE"))
									thisauth = QTVAM_PLAIN;
								else if (!strcmp(com_token, "PLAIN"))
									thisauth = QTVAM_PLAIN;
								else if (!strcmp(com_token, "CCITT"))
									thisauth = QTVAM_CCITT;
								else if (!strcmp(com_token, "MD4"))
									thisauth = QTVAM_MD4;
								else
								{
									thisauth = QTVAM_NONE;
									Con_DPrintf("qtv: received unrecognised auth method (%s)\n", com_token);
								}

								if (authmethod < thisauth)
									authmethod = thisauth;
							}
							else if (!strcmp(com_token, "SOURCE"))
							{
								//servers don't support source, and ignore it.
								//source is only useful for qtv proxy servers.
							}
							else if (!strcmp(com_token, "COMPRESSION"))
							{
								//compression not supported yet
							}
							else if (!strcmp(com_token, "USERINFO"))
							{
								start = COM_ParseToken(start, NULL);
								strlcpy(userinfo, com_token, sizeof(userinfo));
							}
							else
							{
								//not recognised.
							}
						}
						start = lineend+1;
					}

					len = (end - p->inbuffer)+2;
					p->insize -= len;
					memmove(p->inbuffer, p->inbuffer + len, p->insize);
					p->inbuffer[p->insize] = 0;

					e = NULL;
					if (p->hasauthed)
					{
					}
					else if (!*qtv_password.string)
						p->hasauthed = true; //no password, no need to auth.
					else if (*password)
					{
						switch (authmethod)
						{
						case QTVAM_NONE:
							e = ("QTVSV 1\n"
								 "PERROR: You need to provide a common auth method.\n\n");
							break;

						case QTVAM_PLAIN:
							p->hasauthed = !strcmp(qtv_password.string, password);
							break;

						case QTVAM_CCITT:
							CRC_Init(&ushort_result);
							CRC_AddBlock(&ushort_result, (byte *) p->challenge, strlen(p->challenge));
							CRC_AddBlock(&ushort_result, (byte *) qtv_password.string, strlen(qtv_password.string));
							p->hasauthed = (ushort_result == Q_atoi(password));
							break;

						case QTVAM_MD4:
							{
								char hash[512];
								int md4sum[4];
								
								snprintf (hash, sizeof(hash), "%s%s", p->challenge, qtv_password.string);
								Com_BlockFullChecksum (hash, strlen(hash), (unsigned char*)md4sum);
								snprintf (hash, sizeof(hash), "%X%X%X%X", md4sum[0], md4sum[1], md4sum[2], md4sum[3]);
								p->hasauthed = !strcmp(password, hash);
							}
							break;

						default:
							e = ("QTVSV 1\n"
								 "PERROR: FTEQWSV bug detected.\n\n");
							break;
						}
						if (!p->hasauthed && !e)
						{
							if (raw)
								e = "";
							else
								e = ("QTVSV 1\n"
									 "PERROR: Bad password.\n\n");
						}
					}
					else
					{
						//no password, and not automagically authed
						switch (authmethod)
						{
						case QTVAM_NONE:
							if (raw)
								e = "";
							else
								e = ("QTVSV 1\n"
									 "PERROR: You need to provide a common auth method.\n\n");
							break;

						case QTVAM_PLAIN:
							p->hasauthed = !strcmp(qtv_password.string, password);
							break;

						case QTVAM_CCITT:
							e =	("QTVSV 1\n"
								"AUTH: CCITT\n"
								"CHALLENGE: ");

							send(p->socket, e, strlen(e), 0);
							send(p->socket, p->challenge, strlen(p->challenge), 0);
							e = "\n\n";
							send(p->socket, e, strlen(e), 0);
							continue;

						case QTVAM_MD4:
							e = ("QTVSV 1\n"
								"AUTH: MD4\n"
								"CHALLENGE: ");

							send(p->socket, e, strlen(e), 0);
							send(p->socket, p->challenge, strlen(p->challenge), 0);
							e = "\n\n";
							send(p->socket, e, strlen(e), 0);
							continue;

						default:
							e = ("QTVSV 1\n"
								 "PERROR: FTEQWSV bug detected.\n\n");
							break;
						}
					}

					if (e)
					{
					}
					else if (!versiontouse)
					{
						e = ("QTVSV 1\n"
							 "PERROR: Incompatable version (valid version is v1)\n\n");
					}
					else if (raw)
					{
						if (p->hasauthed == false)
						{
							e =	"";
						}
						else
						{
							mvddest_t *tmpdest;

							if ((tmpdest = SV_InitStream(p->socket, p->na, userinfo)))
							{
								if (!SV_MVD_Record(tmpdest, false))
									DestClose(tmpdest, false); // can't start record for some reason, close dest then

								p->socket = -1;	//so it's not cleared wrongly.
							}
							else
							{
								// RAW mode, can't sent error, right?
//								e = ("QTVSV 1\n"
//									"ERROR: Can't init stream, probably server reach a limit on the number of proxies connected at any one time.\n\n");
							}
						}
						p->error = true;
					}
					else
					{
						if (p->hasauthed == true)
						{
							mvddest_t *tmpdest;

							if ((tmpdest = SV_InitStream(p->socket, p->na, userinfo)))
							{
								e = ("QTVSV 1\n"
								 	"BEGIN\n\n");
								send(p->socket, e, strlen(e), 0);
								e = NULL;

								if (!SV_MVD_Record(tmpdest, false))
									DestClose(tmpdest, false); // can't start record for some reason, close dest then

								p->socket = -1;	//so it's not cleared wrongly.
							}
							else
							{
								e = ("QTVSV 1\n"
									"ERROR: Can't init stream, probably server reach a limit on the number of proxies connected at any one time.\n\n");
							}
						}
						else
						{
							e = ("QTVSV 1\n"
								"PERROR: You need to provide a password.\n\n");
						}
						p->error = true;
					}

					if (e)
					{
						send(p->socket, e, strlen(e), 0);
						p->error = true;
					}
				}
			}
			else if (len == 0)
				p->error = true;
			else
			{	//error of some kind. would block or something
				int err;
				err = qerrno;
				if (err != EWOULDBLOCK && err != EAGAIN)
					p->error = true;
			}
		}
	}
}

//============================================================
//
// QTV user input
//
//============================================================

// { qtv commands

// say [say_game] text
// say_team [say_game] text
// say_game text

void QTVcmd_Say_f(mvddest_t *d)
{
	qbool gameStarted;
	client_t *client;
	int		j;
	char	*p;
	char	text[1024], text2[1024], *cmd;

	if (Cmd_Argc () < 2)
		return;

	if (!strcasecmp(Info_ValueForKey(svs.info, "status"), "Countdown"))
		gameStarted	= false; // if status is "Countdown" then game is not started yet
	else
		gameStarted = GameStarted();

	p = Cmd_Args();

	if (*p == '"' && (j = strlen(p)) > 2)
	{
		p[j-1] = 0;
		p++;
	}

	cmd = Cmd_Argv(0);

	// strip leading say_game but not in case of "cmd say_game say_game"
	if (strcmp(cmd, "say_game") && !strncasecmp(p, "say_game ", sizeof("say_game ") - 1))
	{
		p += sizeof("say_game ") - 1;
	}

	if (!strcmp(cmd, "say_game"))
		cmd = "say"; // this makes qtv_%s_game looks right

	if (!strcmp(cmd, "say_team"))
		gameStarted = true; // send to specs only

	if (gameStarted)
		cmd = "say_team"; // we can accept only this command, since we will send to specs only

	// for clients and demo
	snprintf(text, sizeof(text), "#0:qtv_%s_game:#%d:%s: %s\n", cmd, d->id, d->qtvname, p);
	// for server console and logs
	snprintf(text2, sizeof(text2), "qtv: #0:qtv_%s_game:#%d:%s: %s\n", cmd, d->id, d->qtvname, p);

	for (j = 0, client = svs.clients; j < MAX_CLIENTS; j++, client++)
	{
		if (client->state != cs_spawned)
			continue;

		if (gameStarted && !client->spectator)
			continue; // game started, don't send QTV chat to players, specs still get QTV chat

		SV_ClientPrintf2(client, PRINT_CHAT, "%s", text);
	}

	if (sv.mvdrecording)
	{
		if (MVDWrite_Begin (dem_all, 0, strlen(text)+3))
		{
			MVD_MSG_WriteByte (svc_print);
			MVD_MSG_WriteByte (PRINT_CHAT);
			MVD_MSG_WriteString (text);
		}
	}

	Sys_Printf("%s", text2);
	SV_Write_Log(CONSOLE_LOG, 1, text2);
}

// }

char QTV_cmd[MAX_PROXY_INBUFFER]; // global so it does't allocated on stack, this save some CPU I think

typedef struct
{
	char	*name;
	void	(*func) (mvddest_t *d);
}
qtv_ucmd_t;

static qtv_ucmd_t ucmds[] =
{
	{"say", 			QTVcmd_Say_f},
	{"say_team",		QTVcmd_Say_f},
	{"say_game",		QTVcmd_Say_f},

	{NULL, NULL}
};

// { qtv utils

typedef struct {
	unsigned int readpos;
	unsigned int cursize;
	unsigned int maxsize;
	char *data;
	unsigned int startpos;
//	qbool overflowed;
//	qbool allowoverflow;
} netmsg_t;

static void InitNetMsg(netmsg_t *b, char *buffer, int bufferlength)
{
	memset(b, 0, sizeof(netmsg_t));

	b->data    = buffer;
	b->maxsize = bufferlength;
}

//probably not the place for these any more..
static unsigned char ReadByte(netmsg_t *b)
{
	if (b->readpos >= b->cursize)
	{
		b->readpos = b->cursize+1;
		return 0;
	}
	return b->data[b->readpos++];
}

static unsigned short ReadShort(netmsg_t *b)
{
	int b1, b2;
	b1 = ReadByte(b);
	b2 = ReadByte(b);

	return b1 | (b2<<8);
}

void ReadString(netmsg_t *b, char *string, int maxlen)
{
	maxlen--;	//for null terminator
	while(maxlen)
	{
		*string = ReadByte(b);
		if (!*string)
			return;
		string++;
		maxlen--;
	}
	*string++ = '\0';	//add the null
}

// }

void QTV_ExecuteCmd(mvddest_t *d, char *cmd)
{
    char *arg0;
    qbool found = false;
	qtv_ucmd_t *u;

//	Sys_Printf("qtv cmd: %s\n", cmd);

	Cmd_TokenizeString (cmd);

	arg0 = Cmd_Argv(0);

//	Sys_RedirectStart(???);

	for (u = ucmds; u->name; u++)
	{
		if (!strcmp(arg0, u->name))
		{
			if (u->func)
				u->func(d);
			found = true;
			break;
		}
	}

	if (!found)
	{
		if (developer.value)
			Sys_Printf("Bad QTV command: %s\n", arg0);
	}

//	Sys_RedirectStop();
}

void QTV_ReadInput( mvddest_t *d )
{
	int len, parse_end, clc;
	netmsg_t buf;

	if (d->error)
		return;

	len = sizeof(d->inbuffer) - d->inbuffersize - 1; // -1 since it null terminated

	if (len)
	{
		len = recv(d->socket, d->inbuffer + d->inbuffersize, len, 0);

		if (len == 0)
		{
			Sys_Printf("QTV_ReadInput: read error from QTV client, dropping\n");
			d->error = true;
			return;
		}
		else if (len < 0)
		{
			len = 0;
		}

		d->inbuffersize += len;
		d->inbuffer[d->inbuffersize] = 0; // null terminated
	}

	if (d->inbuffersize < 2)
		return; // we need at least size

	InitNetMsg(&buf, d->inbuffer, d->inbuffersize);
	buf.cursize	= d->inbuffersize; // we laredy have some data in buffer

	parse_end	= 0;

	while(buf.readpos < buf.cursize)
	{
//		Sys_Printf("%d %d\n", buf.readpos, buf.cursize);

		if (buf.readpos > buf.cursize)
		{
			d->error = true;
			Sys_Printf("QTV_ReadInput: Read past end of parse buffer\n");
			return;
		}

		buf.startpos = buf.readpos;

		if (buf.cursize - buf.startpos < 2)
			break; // we need at least size

		len = ReadShort(&buf);

		if (len > (int)sizeof(d->inbuffer) - 1 || len < 3)
		{
			d->error = true;
			Sys_Printf("QTV_ReadInput: can't handle such long/short message: %i\n", len);
			return;
		}

		if (len > buf.cursize - buf.startpos)
			break; // not enough data yet

		parse_end = buf.startpos + len; // so later we know which part of buffer we alredy served

		switch (clc = ReadByte(&buf))
		{
		#define qtv_clc_stringcmd    1

		case qtv_clc_stringcmd:
			QTV_cmd[0] = 0;
			ReadString(&buf, QTV_cmd, sizeof(QTV_cmd));
			QTV_ExecuteCmd(d, QTV_cmd);
			break;

		default:
			d->error = true;
			Sys_Printf("QTV_ReadInput: can't handle clc %i\n", clc);
			return;
		}
	}

	if (parse_end)
	{
		d->inbuffersize -= parse_end;
		memmove(d->inbuffer, d->inbuffer + parse_end, d->inbuffersize);
	}
}

void QTV_ReadDests( void )
{
	mvddest_t *d;

	for (d = demo.dest; d; d = d->nextdest)
	{
		if (d->desttype != DEST_STREAM)
			continue;

		if (d->error)
			continue;

		QTV_ReadInput(d);
	}
}

//============================================================ 




/*
void DemoWriteQTVTimePad (int msecs)	//broadcast to all proxies
{
	mvddest_t *d;
	unsigned char buffer[6];
	while (msecs > 0)
	{
		//duration
		if (msecs > 255)
			buffer[0] = 255;
		else
			buffer[0] = msecs;
		msecs -= buffer[0];
		//message type
		buffer[1] = dem_read;
		//length
		buffer[2] = 0;
		buffer[3] = 0;
		buffer[4] = 0;
		buffer[5] = 0;

		for (d = demo.dest; d; d = d->nextdest)
		{
			if (d->desttype == DEST_STREAM)
			{
				DemoWriteDest(buffer, sizeof(buffer), d);
			}
		}
	}
}
*/

void Qtv_List_f(void)
{
	mvddest_t *d;
	int cnt;

	for (cnt = 0, d = demo.dest; d; d = d->nextdest)
	{
		if (d->desttype != DEST_STREAM)
			continue; // not qtv

		if (!cnt) // print banner
			Con_Printf ("QTV list:\n"
						"%4.4s %s\n", "#Id", "Addr");

		cnt++;

		Con_Printf ("%4d %s\n", d->id, NET_AdrToString(d->na));
	}

	if (!cnt)
		Con_Printf ("QTV list: empty\n");
}

void Qtv_Close_f(void)
{
	mvddest_t *d;
	int id, cnt;
	qbool all;

	if (Cmd_Argc() < 2 || !*Cmd_Argv(1))  // not less than one param, first param non empty
	{
		Con_Printf ("Usage: %s <#id | all>\n", Cmd_Argv(0));
		return;
	}

	for (d = demo.dest; d; d = d->nextdest)
		if (d->desttype == DEST_STREAM)
			break; // at least one qtv present

	if (!d) {
		Con_Printf ("QTV list alredy empty\n");
		return;
	}

	id  = atoi(Cmd_Argv(1));
	all = !strcasecmp(Cmd_Argv(1), "all");
	cnt = 0;

	for (d = demo.dest; d; d = d->nextdest)
	{
		if (d->desttype != DEST_STREAM)
			continue; // not qtv

		if (all || d->id == id) {
			Con_Printf ("QTV id:%d aka %s will be dropped asap\n", d->id, NET_AdrToString(d->na));
			d->error = true;
			cnt++;
		}
	}

	if (!cnt)
		Con_Printf ("QTV id:%d not found\n", id);
}

void Qtv_Status_f(void)
{
	int cnt;
	mvddest_t *d;
	mvdpendingdest_t *p;

	Con_Printf ("QTV status\n");
	Con_Printf ("Listen socket  : %s\n", listensocket == INVALID_SOCKET ? "invalid" : "listen");
	Con_Printf ("Port           : %d\n", listenport);

	for (cnt = 0, d = demo.dest; d; d = d->nextdest)
		if (d->desttype == DEST_STREAM)
			cnt++;

	Con_Printf ("Streams        : %d\n", cnt);

	for (cnt = 0, p = demo.pendingdest; p; p = p->nextdest)
		cnt++;

	Con_Printf ("Pending streams: %d\n", cnt);
}

//====================================

void QTV_Init(void)
{
	Cvar_Register (&qtv_streamport);
	Cvar_Register (&qtv_maxstreams);
	Cvar_Register (&qtv_password);
	Cvar_Register (&qtv_pendingtimeout);
	Cvar_Register (&qtv_streamtimeout);

	Cmd_AddCommand ("qtv_list", Qtv_List_f);
	Cmd_AddCommand ("qtv_close", Qtv_Close_f);
	Cmd_AddCommand ("qtv_status", Qtv_Status_f);
}
