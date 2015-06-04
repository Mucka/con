/*********************
 * Console utility
 *********************
 *
 */

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <memory.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include "tty.h"

#define PERR(args...)   do { fprintf(stderr, args); finish(1); } while(0)

Tty             *tty = 0;
int             tty1 = -1;
char            *tty1_name = 0;
const char      *tty2_name = "/dev/tty";

void usage(const char *s)
{
    const char *msg =
        "Usage:\n"
        "\t%s [SWITCHES] {tty_device | path_to_local_socket | [host]:port}\n"
        "\n"
        "The tool can be used in a few different modes:\n"
        "\n"
        "1. Communicaton program to serial line (like minicom):\n"
        "       %s [-t] [-b BAUDRATE] TERM_DEVICE\n"
        "   Example:\n"
        "       %s /dev/ttyUSB0\n"
        "\n"
        "2. Communicaton program to TCP socket in a client mode:\n"
        "       %s -c ADDR:PORT\n"
        "   Example:\n"
        "       %s -c www.something.org:80\n"
        "       %s -c 192.168.2.100:8080\n"
        "\n"
        "3. Communicaton program to TCP socket in a server mode):\n"
        "       %s -s :PORT\n"
        "   Example:\n"
        "       %s -s :8080\n"
        "\n"
        "4. Communicaton program to UNIX socket in a client mode:\n"
        "       %s -c SOCKET_PATH\n"
        "   Example:\n"
        "       %s -c /tmp/my_named_socket\n"
        "\n"
        "5. Communicaton program to UNIX socket in a server mode:\n"
        "       %s -s SOCKET_PATH\n"
        "   Example:\n"
        "       %s -s /tmp/my_named_socket\n"
        "\n"
        "SWITCHES may be:\n"
        "\t-h[elp]             - Print help message.\n"
        "\t-e[cho]             - Echo keyboard input locally.\n"
        "\t-q[uit] KEY         - Quit connection key. May be in integer as 0x01 or 001\n"
        "\t                      or in a \"control-a\", \"cntrl/a\" or \"ctrl/a\" form\n"
        "\t                      Default is \"cntrl/a\".\n"
        "\n"
        "Switches specific for tty_device:\n"
        "\t-t[erm]             - Work as serial communicaton program. The is a default\n"
        "\t                      mode. Note that \"-b\" switch assumes \"-t\"\n"
        "\t-b[aud] <baud_rate> - Set the baud rate for target connection.\n"
        "\n"
        "Switches specific for socket connection:\n"
        "\t-s[erver]           - Accept connection to socket as server.\n"
        "\t-c[lient]           - Connection to socket as client.\n"
        ;
    fprintf(stderr, msg, s, s, s, s,    s, s, s, s,    s, s, s, s);
    exit (1);
}

void finish(int stat = 0)
{
    fprintf(stderr, "\r\n");
    if (tty)
    {
        delete tty;
        tty = 0;
    }
    if (tty1 >= 0)
    {
        close(tty1);
        tty1 = -1;
    }
    if (tty1_name)
    {
        delete [] tty1_name;
        tty1_name = 0;
    }
    exit (stat);
}
extern "C" void finish_int(int)
{
    finish(1);
}

int main(int ac, char *av[])
{
    const int            MAXBUF = 1024;
    static unsigned char buf[MAXBUF];
    unsigned char        exitChr = '\001';
    int                  TargetBaud = 0, nparams=0;
    int                  tty_flag=0, socket_flag=0, cli_flag=0, srv_flag=0, echo_flag=0;
    char                 *TargetCon = 0;

    /* Command line parsing. */
    if (ac < 2)
        usage(av[0]);
    for (int i=1; i<ac; i++)
    {
        switch (*av[i])
        {
        case '-':
            ++av[i];
            if (!strcmp(av[i], "e")  ||  !strcmp(av[i], "echo"))
            {
                echo_flag = 1;
            }
            else if (!strcmp(av[i], "b")  ||  !strcmp(av[i], "baud"))
            {
                if (++i >= ac)
                    PERR("After switch \"%s\" baud rate is expected.\n",av[--i]);
                char *end;
                TargetBaud = (int)strtol(av[i], &end, 0);
                if (*end)
                    PERR("Invalid baud rate: \"%s\" -- ?\n", end);
                tty_flag = 1;
            }
            else if (!strcmp(av[i], "t")  ||  !strcmp(av[i], "term"))
            {
                tty_flag = 1;
            }
            else if (!strcmp(av[i], "q")  ||  !strcmp(av[i], "quit"))
            {
                if (++i >= ac)
                    PERR("After switch \"%s\" quit character is expected.\n",av[--i]);
                char *end;
                exitChr = (unsigned char)strtol(av[i], &end, 0);
                if (*end)
                {
                    char *p = strchr(av[i], '/');
                    if (!p)
                        p = strchr(av[i], '-');
                    if (!p)
                        PERR("No delimiter ('-' or '/') found in \"KEY\" specification.\n");

                    *p++ = 0;
                    if (!strcasecmp(av[i], "control")
                        || !strcasecmp(av[i], "cntrl")
                        || !strcasecmp(av[i], "ctrl"))
                    {
                    }
                    else
                    {
                        fprintf(stderr, "No modificator found in \"KEY\" specification.\n");
                        fprintf(stderr, "Can be \"control\", \"cntrl\" or \"ctrl\"\n");
                        finish(1);
                    }

                    if (strlen(p) != 1)
                    {
                        fprintf(stderr, "Should be one character after modificator in \"KEY\" specification.\n");
                        fprintf(stderr, "Can be \"control\", \"cntrl\" or \"ctrl\"\n");
                        finish(1);
                    }

                    if (*p >= 0x40 && *p <= 0x60)
                        exitChr = *p - 0x40;
                    else if (*p > 0x60)
                        exitChr = *p - 0x60;
                    else
                    {
                        fprintf(stderr, "Invalid character after modificator in \"KEY\" specification.\n");
                        fprintf(stderr, "Can be \"control\", \"cntrl\" or \"ctrl\"\n");
                        finish(1);
                    }
                }
            }
            else if (!strcmp(av[i], "s")  ||  !strcmp(av[i], "server"))
            {
                srv_flag = 1;
                socket_flag = 1;
            }
            else if (!strcmp(av[i], "c")  ||  !strcmp(av[i], "client"))
            {
                cli_flag = 1;
                socket_flag = 1;
            }
            else if (!strcmp(av[i], "h")  ||  !strcmp(av[i], "help"))
            {
                usage(av[0]);
            }
            else
                PERR("Invalid switch \"%s\".\n", av[i]);
            break;
        case '?':
            usage(av[0]);
            break;
        default:
            TargetCon = av[i];
            nparams++;
        }
    }

    if (nparams != 1)
        PERR("Invalid number of parameters.\n");
    if ((socket_flag && tty_flag)  ||  (srv_flag && cli_flag))
        PERR("Mutually exclusive flags are specified.\n");

    // tty or socket ?
    if (!socket_flag && !tty_flag)
    {
        if (strchr(TargetCon, ':'))
            socket_flag = 1;
        else if (strchr(TargetCon, '/'))
            tty_flag = 1;
        else
            socket_flag = 1;
    }

    // server or client ?
    if (socket_flag  &&  (!srv_flag && !cli_flag))
    {
        if (*TargetCon == ':')
            srv_flag = 1;
        else if (!strchr(TargetCon, ':'))
            srv_flag = 1;
        else
            cli_flag = 1;
    }

    signal(SIGINT,  finish_int);
    signal(SIGQUIT, finish_int);
    signal(SIGTERM, finish_int);
    signal(SIGPIPE, finish_int);
    tty = new Tty();

    // Open first connection
    if (socket_flag)
    {
        if (srv_flag)
        {
            const int name_l = 100;
            const int addr_l = 20;
            char      name[name_l], addr[addr_l];
            name[name_l-1] = 0;
            addr[addr_l-1] = 0;

            char *p = strchr(TargetCon, ':');
            if (!p)
            {
                /*
                 * UNIX socket server
                 */

                struct sockaddr_un  cli_unix_addr;
                struct sockaddr_un  serv_addr;
                int                 servlen;
                int                 one=1;

                // Open a TCP socket (an Internet stream socket).
                if ((tty1 = socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
                    PERR("socket (AF_UNIX): %s", strerror(errno));

                // Reuse address
                if (setsockopt(tty1, SOL_SOCKET, SO_REUSEADDR, (char *) &one, sizeof(one)) < 0)
                    PERR("setsockopt (SO_REUSEADDR): %s", strerror(errno));

                // Bind our local address so that the client can send to us.
                memset((char *) &serv_addr, 0, sizeof(serv_addr));
                serv_addr.sun_family = AF_UNIX;
                strncpy(serv_addr.sun_path, TargetCon, sizeof(serv_addr.sun_path));
                servlen = strlen(serv_addr.sun_path) + sizeof(serv_addr.sun_family);
                if (bind(tty1, (struct sockaddr *)&serv_addr, servlen) < 0)
                    PERR("bind: %s", strerror(errno));

                // Get ready to accept connection
                if (listen(tty1, 1) < 0)
                    PERR("listen: %s", strerror(errno));

                int clilen = sizeof(cli_unix_addr);
                int new_sock = accept(tty1, (struct sockaddr *)&cli_unix_addr, (socklen_t *)&clilen);
                if (new_sock < 0)
                    PERR("accept: %s", strerror(errno));
                close(tty1);
                tty1 = new_sock;

                strncpy(addr, cli_unix_addr.sun_path, addr_l-1);

                fprintf(stderr,"Connection accepted from %s\n", addr);
                tty1_name = new char[strlen(TargetCon) + 16];
                snprintf(tty1_name, 32, "server %s", TargetCon);
            }
            else
            {
                /*
                 * TCP socket server
                 */
                char      *end;

                TargetCon = p + 1;
                int port = (int)strtol(TargetCon, &end, 0);
                if (*end)
                    PERR("Invalid port value: \"%s\" -- ?\n", end);

                struct sockaddr_in  cli_inet_addr;
                struct sockaddr_in  serv_addr;
                struct hostent      *hent;
                int                 one=1;

                // Open a TCP socket (an Internet stream socket).
                if ((tty1 = socket(AF_INET, SOCK_STREAM, 0)) < 0)
                    PERR("socket (AF_INET): %s", strerror(errno));

                // Reuse address
                if (setsockopt(tty1, SOL_SOCKET, SO_REUSEADDR, (char *) &one, sizeof(one)) < 0)
                    PERR("setsockopt (SO_REUSEADDR): %s", strerror(errno));

                // Bind our local address so that the client can send to us.
                memset((char *) &serv_addr, 0, sizeof(serv_addr));
                serv_addr.sin_family      = AF_INET;
                serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
                serv_addr.sin_port        = htons(port);
                if (bind(tty1, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
                    PERR("bind: %s", strerror(errno));

                // Get ready to accept connection
                if (listen(tty1, 1) < 0)
                    PERR("listen: %s", strerror(errno));

                int clilen = sizeof(cli_inet_addr);
                int new_sock = accept(tty1, (struct sockaddr *)&cli_inet_addr, (socklen_t *)&clilen);
                if (new_sock < 0)
                    PERR("accept: %s", strerror(errno));
                close(tty1);
                tty1 = new_sock;

                // Determine the client host name
                hent = gethostbyaddr((char *)&cli_inet_addr.sin_addr,
                                     sizeof(cli_inet_addr.sin_addr), AF_INET);
                if (hent)
                    strncpy(name, hent->h_name, name_l-1);
                else
                    strncpy(name, "????", name_l-1);

                // Determine the client host address
                strncpy(addr, inet_ntoa(cli_inet_addr.sin_addr), addr_l-1);

                fprintf(stderr,"Connection accepted from %s (%s)\n", name, addr);
                tty1_name = new char[32];
                snprintf(tty1_name, 32, "server :%d", port);
            }
        }
        else if (cli_flag)
        {
            char *p = strchr(TargetCon, ':');
            if (!p)
            {
                /*
                 * UNIX local socket client
                 */

                struct sockaddr_un  serv_addr;
                int                 servlen;

                // Fill the "serv_addr" structure
                memset((char *) &serv_addr, 0, sizeof(serv_addr));
                serv_addr.sun_family      = AF_UNIX;
                strncpy(serv_addr.sun_path, TargetCon, sizeof(serv_addr.sun_path));
                servlen = strlen(serv_addr.sun_path) + sizeof(serv_addr.sun_family);

                // Open a UNIX socket
                if ( (tty1 = socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
                    PERR("socket (AF_UNIX): %s", strerror(errno));
                tty1_name = strdup(TargetCon);

                // Connect to the server.
                if (connect(tty1, (struct sockaddr *) &serv_addr, servlen) < 0)
                    PERR("connect: %s", strerror(errno));

            }
            else
            {
                /*
                 * TCP socket client
                 */

                *p = '\0';
                char *end;
                int port = (int)strtol(p+1, &end, 0);
                if (*end)
                    PERR("Invalid port value: \"%s\" -- ?\n", end);
                struct sockaddr_in  serv_addr;
                struct hostent      *hent;

                // Try to determinate server IP address
                if ((hent = gethostbyname(TargetCon)) == NULL)
                    PERR("gethostbyname(%s): %s\n", TargetCon, hstrerror(h_errno));

                // Fill the "serv_addr" structure
                memset((char *) &serv_addr, 0, sizeof(serv_addr));
                serv_addr.sin_family      = AF_INET;
                memcpy(&serv_addr.sin_addr, (*(hent->h_addr_list)), sizeof(struct in_addr));
                serv_addr.sin_port        = htons(port);

                // Open a TCP socket (an Internet stream socket).
                if ( (tty1 = socket(AF_INET, SOCK_STREAM, 0)) < 0)
                    PERR("socket (AF_INET): %s", strerror(errno));

                // Connect to the server.
                if (connect(tty1, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0)
                    PERR("connect: %s", strerror(errno));

                tty1_name = new char[strlen(TargetCon) + 32];
                snprintf(tty1_name, strlen(TargetCon) + 32, "%s:%d", TargetCon, port);
            }
        }
        else
            PERR("Internal error #1\n");
    }
    else if (tty_flag)
    {
        tty1_name = strdup(TargetCon);
        try
        {
            tty1 = tty->open(TargetCon, TargetBaud);
        }
        catch (char *err)
        {
            PERR("Can't open %s: %s\n", TargetCon, err);
        }
        catch (const char *err)
        {
            PERR("Can't open %s: %s\n", TargetCon, err);
        }
        if (tty1 == -1)
        {
            if (errno == ENOTTY)
            {
                // Regular file
                tty1 = open(TargetCon, O_RDWR | O_SYNC);
                if (tty1 == -1)
                    PERR("Can't open file %s: %s\n", TargetCon, strerror(errno));
            }
            else
                PERR("Can't open tty %s: %s\n", TargetCon, strerror(errno));
        }
    }
    else
        PERR("Internal error #2\n");

    // Open second connection, always to /dev/tty
    int tty2 = tty->open(tty2_name);
    if (tty2 == -1)
        PERR("Can't open /dev/tty: %s\n", strerror(errno));

    // Main loop
    fd_set rds;
    int num = (tty1 > tty2 ? tty1 : tty2) + 1;
    for (;;)
    {
        FD_ZERO(&rds);
        FD_SET(tty1, &rds);
        FD_SET(tty2, &rds);
        if (select(num, &rds, 0, 0, 0) < 0)
            PERR("select failure: %s\n", strerror(errno));
        if (FD_ISSET(tty1, &rds))
        {
            int buf_cnt = read(tty1, buf, MAXBUF);
            if (buf_cnt < 0)
                PERR("\r\n\"%s\" read error: %s\n", tty1_name, strerror(errno));
            if (buf_cnt == 0)
                PERR("\r\n\"%s\" EOF\n", tty1_name);
            if (write(tty2, buf, buf_cnt) != buf_cnt)
                PERR("\r\n\"%s\" write error: %s\n", tty2_name, strerror(errno));
        }
        if (FD_ISSET(tty2, &rds))
        {
            int buf_cnt = read(tty2, buf, MAXBUF);
            if (buf_cnt < 0)
                PERR("\r\n\"%s\" read error: %s\n", tty2_name, strerror(errno));
            if (buf_cnt == 0)
                PERR("\r\n\"%s\" EOF\n", tty2_name);
            if (buf_cnt == 1  &&  *buf == exitChr)
                break;
            if (echo_flag  &&  write(tty2, buf, buf_cnt) != buf_cnt)
                PERR("\r\n\"%s\" write error: %s\n", tty2_name, strerror(errno));
            if (write(tty1, buf, buf_cnt) != buf_cnt)
                PERR("\r\n\"%s\" write error: %s\n", tty1_name, strerror(errno));
       }
    }
    finish();
}