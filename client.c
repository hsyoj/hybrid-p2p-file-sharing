#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <stdbool.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <process.h>
#define Tfunc unsigned __stdcall
#define THandle HANDLE
#define TNcode 0
#define TEcode 1
typedef int socklen_t;
typedef unsigned(__stdcall *tFuncType)(void *);
static inline void sleep(unsigned int x) { Sleep(x * 1000); }
#else
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/time.h>
#define Tfunc void *
#define THandle pthread_t
#define TNcode NULL
#define TEcode ((void *)1)
typedef void *(*tFuncType)(void *);
#endif

#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#endif

#define elif else if
#define BUFFER_SIZE 1024
typedef unsigned long long ull;
typedef struct sockaddr_in sockaddr_in;
typedef struct sockaddr sockaddr;

static const char *ServerIP = "127.0.0.1";
static unsigned ServerPort = 0;
static unsigned TCPSerPort = 0;
volatile bool HBStat = true;
volatile bool TCPSStat = true;
static bool Auth = false;
static char uname[17] = {0};
static char homedir[20] = {0};

Tfunc tHeartbeats(void *args);
Tfunc tTCPServer(void *args);
Tfunc dealRequest(void *psock);
int sendRequest(int port, const char *filename);
void closes(int sock);
char *strndupx(const char *s, size_t n);
char **strSplit(const char *str, const char delimiter, int *count);
void pFree(char ***parr, int n);
int genPort();
int strPort(const char *str);
int chkPort(int port);
bool fileExists(const char *filename);
bool udpSendMsg(int sock, const char *msg);
bool udpReceiveMsg(int sock, char *buffer, size_t buffer_size);
bool sendATH(int sock, char *un, char *pw, int tcpport);
void sendLap(int sock);
void sendSch(int sock, const char *substr);
void sendPub(int sock, const char *filename);
void sendUnp(int sock, const char *filename);
void sendLpf(int sock);
void sendGet(int sock, const char *filename);
void dealDSC();
bool startThread(THandle *thread, tFuncType tf, void *args);
void clear_stdin();
void startCli(int sersock);

Tfunc tHeartbeats(void *args)
{
    int sock;
    sockaddr_in saddr;
    char message[21] = {0};
    sprintf(message, "HBT %.16s", uname);

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
    {
        perror("Socket creation failed");
        return TEcode;
    }

    memset(&saddr, 0, sizeof(saddr));
    saddr.sin_family = AF_INET;
    saddr.sin_port = htons(ServerPort);
    saddr.sin_addr.s_addr = inet_addr(ServerIP);

    while (HBStat)
    {
        if (sendto(sock, message, strlen(message), 0, (struct sockaddr *)&saddr, sizeof(saddr)) < 0)
        {
            fprintf(stderr, "[Error] Failed to send heartbeat\n");
        }
        sleep(2);
    }

    closes(sock);
    return TNcode;
}

Tfunc tTCPServer(void *args)
{
    int ssock, csock;
    sockaddr_in saddr, caddr;
    socklen_t casize = sizeof(caddr);
    ssock = socket(AF_INET, SOCK_STREAM, 0);
    if (ssock < 0)
        return TEcode;
    memset(&saddr, 0, sizeof(saddr));
    saddr.sin_family = AF_INET;
    saddr.sin_addr.s_addr = INADDR_ANY;
    saddr.sin_port = htons(TCPSerPort);

    if (bind(ssock, (sockaddr *)&saddr, sizeof(saddr)) < 0)
    {
        closes(ssock);
        return TEcode;
    }
    if (listen(ssock, 5) < 0)
    {
        closes(ssock);
        return TEcode;
    }

    while (TCPSStat)
    {
        csock = accept(ssock, (sockaddr *)&caddr, &casize);
        if (csock < 0)
            continue;
        THandle subsh;
        int *tsock = (int *)malloc(sizeof(int));
        *tsock = csock;
        if (!startThread(&subsh, dealRequest, tsock))
        {
            free(tsock);
            closes(csock);
            continue;
        }
#ifdef _WIN32
        CloseHandle(subsh);
#else
        pthread_detach(subsh);
#endif
    }

    return TNcode;
}

Tfunc dealRequest(void *psock)
{
    int sock = *(int *)psock;
    free(psock);
    char buffer[BUFFER_SIZE];
    char tfname[141] = {0};
    char filename[161] = {0};
    int n;

    n = (int)recv(sock, buffer, sizeof(buffer) - 1, 0);
    if (n <= 0)
    {
        closes(sock);
        return TEcode;
    }
    buffer[n] = '\0';

    /* Only plain file names inside the shared folder may be requested. */
    if (sscanf(buffer, "GET %140s", tfname) != 1 || strstr(tfname, "..") != NULL ||
        strchr(tfname, '/') != NULL || strchr(tfname, '\\') != NULL)
    {
        const char *error_message = "ERR BRQ\n";
        send(sock, error_message, strlen(error_message), 0);
        closes(sock);
        return TEcode;
    }

    snprintf(filename, sizeof(filename), "%s/%s", homedir, tfname);
    FILE *file = fopen(filename, "rb");
    if (!file)
    {
        const char *error_message = "ERR FNF\n";
        send(sock, error_message, strlen(error_message), 0);
        closes(sock);
        return TEcode;
    }

    struct stat file_stat;
    if (stat(filename, &file_stat) < 0)
    {
        fclose(file);
        closes(sock);
        return TEcode;
    }

    snprintf(buffer, sizeof(buffer), "OK %llu\n", (ull)file_stat.st_size);
    if (send(sock, buffer, strlen(buffer), 0) < 0)
    {
        fclose(file);
        closes(sock);
        return TEcode;
    }

    size_t nread;
    while ((nread = fread(buffer, 1, BUFFER_SIZE, file)) > 0)
    {
        size_t off = 0;
        while (off < nread)
        {
            int s = (int)send(sock, buffer + off, nread - off, 0);
            if (s <= 0)
            {
                fclose(file);
                closes(sock);
                return TEcode;
            }
            off += (size_t)s;
        }
    }

    fclose(file);
    closes(sock);
    return TNcode;
}

int sendRequest(int port, const char *filename)
{
    int sock;
    sockaddr_in saddr;
    char buffer[BUFFER_SIZE];
    int n;
    int result = 0;

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
        return -1;

    memset(&saddr, 0, sizeof(saddr));
    saddr.sin_family = AF_INET;
    saddr.sin_port = htons(port);
    if (inet_pton(AF_INET, "127.0.0.1", &saddr.sin_addr) <= 0)
    {
        closes(sock);
        return -2;
    }

    if (connect(sock, (sockaddr *)&saddr, sizeof(saddr)) < 0)
    {
        closes(sock);
        return -3;
    }

    snprintf(buffer, sizeof(buffer), "GET %s\n", filename);
    if (send(sock, buffer, strlen(buffer), 0) < 0)
    {
        closes(sock);
        return -4;
    }

    /* Read the header line ("OK <size>\n" or "ERR ...\n"). The first recv may
     * also contain the start of the file body, so keep whatever follows the '\n'. */
    size_t hlen = 0;
    char *nl = NULL;
    while (hlen < sizeof(buffer) - 1)
    {
        n = (int)recv(sock, buffer + hlen, sizeof(buffer) - 1 - hlen, 0);
        if (n <= 0)
        {
            closes(sock);
            return -5;
        }
        hlen += (size_t)n;
        buffer[hlen] = '\0';
        nl = (char *)memchr(buffer, '\n', hlen);
        if (nl)
            break;
    }
    if (!nl)
    {
        closes(sock);
        return -5;
    }
    *nl = '\0';
    size_t body_off = (size_t)(nl - buffer) + 1;
    size_t extra = hlen - body_off;

    if (strncmp(buffer, "OK ", 3) == 0)
    {
        ull tfsize;
        if (sscanf(buffer + 3, "%llu", &tfsize) != 1)
        {
            closes(sock);
            return -11;
        }

        char file_path[171] = {0};
        snprintf(file_path, sizeof(file_path), "%s/%s", homedir, filename);

        FILE *file = fopen(file_path, "wb");
        if (!file)
        {
            closes(sock);
            return -12;
        }

        ull remaining = tfsize;
        if (extra > 0)
        {
            size_t w = (ull)extra > remaining ? (size_t)remaining : extra;
            fwrite(buffer + body_off, 1, w, file);
            remaining -= w;
        }
        while (remaining > 0 && (n = (int)recv(sock, buffer, BUFFER_SIZE, 0)) > 0)
        {
            size_t w = (ull)n > remaining ? (size_t)remaining : (size_t)n;
            fwrite(buffer, 1, w, file);
            remaining -= w;
        }
        fclose(file);
        closes(sock);
        if (remaining > 0)
        {
            remove(file_path);
            return -21;
        }
        return 0;
    }
    elif (strncmp(buffer, "ERR BRQ", 7) == 0)
    {
        fprintf(stderr, "[Error] Bad request.\n");
        result = -31;
    }
    elif (strncmp(buffer, "ERR FNF", 7) == 0)
    {
        fprintf(stderr, "[Error] File not found.\n");
        result = -32;
    }
    else
    {
        fprintf(stderr, "[Error] Unknown response from peer.\n");
        result = -33;
    }

    closes(sock);
    return result;
}

void closes(int sock)
{
#ifdef _WIN32
    closesocket(sock);
#else
    close(sock);
#endif
}

char *strndupx(const char *s, size_t n)
{
    size_t len = strnlen(s, n);
    char *copy = malloc(len + 1);

    if (copy)
    {
        memcpy(copy, s, len);
        copy[len] = '\0';
    }
    return copy;
}

char **strSplit(const char *str, const char delimiter, int *count)
{
    int segments = 1;
    for (const char *temp = str; *temp; temp++)
    {
        if (*temp == delimiter)
            segments++;
    }
    char **result = malloc(segments * sizeof(char *));
    if (!result)
        return NULL;

    int index = 0;
    const char *start = str;
    for (const char *temp = str;; temp++)
    {
        if (*temp == delimiter || *temp == '\0')
        {
            size_t length = temp - start;
            result[index] = malloc(length + 1);
            if (!result[index])
            {
                for (int i = 0; i < index; i++)
                    free(result[i]);
                free(result);
                return NULL;
            }
            strncpy(result[index], start, length);
            result[index][length] = '\0';
            index++;
            start = temp + 1;
            if (*temp == '\0')
                break;
        }
    }
    *count = segments;
    return result;
}

void pFree(char ***parr, int n)
{
    if (!parr || !*parr)
        return;
    for (int i = 0; i < n; i++)
        free((*parr)[i]);
    free(*parr);
    *parr = NULL;
}

int genPort()
{
    for (int i = 0; i <= 99; i++)
    {
        int port = (rand() % (65535 - 49152 + 1)) + 49152;
        int sockfd;
        struct sockaddr_in addr;
        sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd < 0)
            continue;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = INADDR_ANY;
        if (bind(sockfd, (struct sockaddr *)&addr, sizeof(addr)) == 0)
        {
            closes(sockfd);
            return port;
        }
        closes(sockfd);
    }
    return (rand() % (65535 - 49152 + 1)) + 49152;
}

int strPort(const char *str)
{
    if (*str == '\0' || strlen(str) > 5)
        return -1;
    for (int i = 0; str[i] != '\0'; i++)
    {
        if (!isdigit((unsigned char)str[i]))
            return -1;
    }
    int port = atoi(str);
    if (port >= 0 && port <= 65535)
        return port;
    else
        return -1;
}

int chkPort(int port)
{
    if (port >= 0 && port <= 65535)
        return port;
    elif (port == -99) return -99;
    else return -1;
}

bool fileExists(const char *filename)
{
    struct stat buffer;
    return (stat(filename, &buffer) == 0);
}

bool udpSendMsg(int sock, const char *msg)
{
    sockaddr_in saddr;

    memset(&saddr, 0, sizeof(saddr));
    saddr.sin_family = AF_INET;
    saddr.sin_port = htons(ServerPort);
    saddr.sin_addr.s_addr = inet_addr(ServerIP);
    int sent_len = (int)sendto(sock, msg, strlen(msg), 0, (sockaddr *)&saddr, sizeof(saddr));
    if (sent_len < 0)
    {
        fprintf(stderr, "[Error] Failed to send message\n");
        return false;
    }
    elif ((size_t)sent_len != strlen(msg))
    {
        fprintf(stderr, "[Error] Partial message sent\n");
        return false;
    }

    return true;
}

bool udpReceiveMsg(int sock, char *buffer, size_t buffer_size)
{
    struct sockaddr_in raddr;
    socklen_t raddr_len = sizeof(raddr);
    int recv_len;

    recv_len = (int)recvfrom(sock, buffer, buffer_size - 1, 0, (struct sockaddr *)&raddr, &raddr_len);
    if (recv_len < 0)
    {
        fprintf(stderr, "[Error] Failed to receive message\n");
        return false;
    }

    buffer[recv_len] = '\0';
    return true;
}

bool sendATH(int sock, char *un, char *pw, int tcpport)
{
    char authmsg[50] = {0};
    char response[BUFFER_SIZE] = {0};

    snprintf(authmsg, sizeof(authmsg), "ATH %s %s %d\n", un, pw, tcpport);
    if (!udpSendMsg(sock, authmsg))
        return false;
    if (!udpReceiveMsg(sock, response, sizeof(response)))
        return false;
    if (strncmp(response, "OK", 2) == 0)
        return true;
    return false;
}

void sendLap(int sock)
{
    char msg[50] = {0};
    char response[BUFFER_SIZE] = {0};

    snprintf(msg, sizeof(msg), "LAP %s\n", uname);
    if (!udpSendMsg(sock, msg))
    {
        printf("[Error] Send lap failed.\n");
        return;
    }
    if (!udpReceiveMsg(sock, response, sizeof(response)))
    {
        printf("[Error] Fail to recieve lap from server.\n");
        return;
    }
    if (strncmp(response, "DSC", 3) == 0)
    {
        dealDSC();
        printf("[Warning] The server actively disconnected.\n");
        return;
    }

    if (strncmp(response, "OK ", 3) == 0)
    {
        const char *result_list = response + 3;
        if (strcmp(result_list, "[]") == 0)
        {
            printf("No active peers\n");
            return;
        }

        size_t len = strlen(result_list);
        if (len < 3 || result_list[0] != '[' || result_list[len - 1] != ']')
        {
            fprintf(stderr, "[Error] Invalid response format.\n");
            return;
        }

        char *content = strndupx(result_list + 1, len - 2);
        int count;
        char **peers = strSplit(content, ',', &count);
        free(content);

        if (count > 0)
        {
            printf("%d active peers:\n", count);
            for (int i = 0; i < count; i++)
                printf("%s\n", peers[i]);
        }
        else
            printf("No active peers.\n");

        pFree(&peers, count);
    }
    else
        fprintf(stderr, "[Error] Invalid response.\n");
}

void sendSch(int sock, const char *substr)
{
    char msg[200] = {0};
    char response[BUFFER_SIZE] = {0};

    snprintf(msg, sizeof(msg), "SCH %s %s\n", uname, substr);
    if (!udpSendMsg(sock, msg))
    {
        printf("[Error] Send sch failed.\n");
        return;
    }
    if (!udpReceiveMsg(sock, response, sizeof(response)))
    {
        printf("[Error] Fail to recieve sch from server.\n");
        return;
    }
    if (strncmp(response, "DSC", 3) == 0)
    {
        dealDSC();
        printf("[Warning] The server actively disconnected.\n");
        return;
    }

    if (strncmp(response, "OK ", 3) == 0)
    {
        const char *result_list = response + 3;
        if (strcmp(result_list, "[]") == 0)
        {
            printf("No files found\n");
            return;
        }

        size_t len = strlen(result_list);
        if (len < 3 || result_list[0] != '[' || result_list[len - 1] != ']')
        {
            fprintf(stderr, "[Error] Invalid response format.\n");
            return;
        }

        char *content = strndupx(result_list + 1, len - 2);
        int count;
        char **files = strSplit(content, '\x1F', &count);
        free(content);

        if (count > 0)
        {
            printf("%d file found:\n", count);
            for (int i = 0; i < count; i++)
                printf("%s\n", files[i]);
        }
        else
            printf("No files found\n");

        pFree(&files, count);
    }
    else
        fprintf(stderr, "[Error] Invalid response.\n");
}

void sendPub(int sock, const char *filename)
{
    char ffname[171];
    sprintf(ffname, "%s/%s", homedir, filename);
    if (!fileExists(ffname))
    {
        fprintf(stderr, "[Error] The file to be published does not exist.\n");
        return;
    }

    char msg[200] = {0};
    char response[BUFFER_SIZE] = {0};

    snprintf(msg, sizeof(msg), "PUB %s %s\n", uname, filename);
    if (!udpSendMsg(sock, msg))
    {
        fprintf(stderr, "[Error] Send pub failed.\n");
        return;
    }
    if (!udpReceiveMsg(sock, response, sizeof(response)))
    {
        fprintf(stderr, "[Error] Fail to recieve pub from server.\n");
        return;
    }
    if (strncmp(response, "DSC", 3) == 0)
    {
        dealDSC();
        fprintf(stderr, "[Warning] The server actively disconnected.\n");
        return;
    }
    if (strncmp(response, "OK", 2) == 0)
        printf("File published successfully\n");
    else
        fprintf(stderr, "File publishing Failed\n");
}

void sendUnp(int sock, const char *filename)
{
    char msg[200] = {0};
    char response[BUFFER_SIZE] = {0};

    snprintf(msg, sizeof(msg), "UNP %s %s\n", uname, filename);
    if (!udpSendMsg(sock, msg))
    {
        fprintf(stderr, "[Error] Send unp failed.\n");
        return;
    }
    if (!udpReceiveMsg(sock, response, sizeof(response)))
    {
        fprintf(stderr, "[Error] Fail to recieve unp from server.\n");
        return;
    }
    if (strncmp(response, "DSC", 3) == 0)
    {
        dealDSC();
        fprintf(stderr, "[Warning] The server actively disconnected.\n");
        return;
    }
    if (strncmp(response, "OK", 2) == 0)
        printf("File unpublished successfully\n");
    else
        fprintf(stderr, "File unpublishing Failed\n");
}

void sendLpf(int sock)
{
    char msg[50] = {0};
    char response[BUFFER_SIZE] = {0};

    snprintf(msg, sizeof(msg), "LPF %s\n", uname);
    if (!udpSendMsg(sock, msg))
    {
        fprintf(stderr, "[Error] Send lpf failed.\n");
        return;
    }
    if (!udpReceiveMsg(sock, response, sizeof(response)))
    {
        fprintf(stderr, "[Error] Fail to recieve lpf from server.\n");
        return;
    }
    if (strncmp(response, "DSC", 3) == 0)
    {
        dealDSC();
        fprintf(stderr, "[Warning] The server actively disconnected.\n");
        return;
    }
    if (strncmp(response, "OK ", 3) == 0)
    {
        const char *result_list = response + 3;
        if (strcmp(result_list, "[]") == 0)
        {
            printf("No files published\n");
            return;
        }

        size_t len = strlen(result_list);
        if (len < 3 || result_list[0] != '[' || result_list[len - 1] != ']')
        {
            fprintf(stderr, "[Error] Invalid response format.\n");
            return;
        }

        char *content = strndupx(result_list + 1, len - 2);
        int count;
        char **files = strSplit(content, '\x1F', &count);
        free(content);

        if (count > 0)
        {
            printf("%d file published:\n", count);
            for (int i = 0; i < count; i++)
                printf("%s\n", files[i]);
        }
        else
            printf("No files published\n");

        pFree(&files, count);
    }
    else
        fprintf(stderr, "[Error] Invalid response.\n");
}

void sendGet(int sock, const char *filename)
{
    char msg[200] = {0};
    char response[BUFFER_SIZE] = {0};

    snprintf(msg, sizeof(msg), "GET %s %s\n", uname, filename);
    if (!udpSendMsg(sock, msg))
    {
        fprintf(stderr, "[Error] Send lpf failed.\n");
        return;
    }
    if (!udpReceiveMsg(sock, response, sizeof(response)))
    {
        fprintf(stderr, "[Error] Fail to recieve lpf from server.\n");
        return;
    }
    if (strncmp(response, "DSC", 3) == 0)
    {
        dealDSC();
        fprintf(stderr, "[Warning] The server actively disconnected.\n");
        return;
    }
    if (strncmp(response, "FNF", 3) == 0)
    {
        fprintf(stderr, "[Warning] File not found.\n");
        return;
    }
    if (strncmp(response, "OK ", 3) == 0)
    {
        int port;
        if (sscanf(response + 3, "%d", &port) != 1)
        {
            fprintf(stderr, "[Error] Invalid response format.\n");
            return;
        }
        if (chkPort(port) == -99)
        {
            printf("This file does not need to be downloaded.\n");
            return;
        }
        elif (chkPort(port) == -1)
        {
            fprintf(stderr, "[Error] Invalid response.\n");
            return;
        }
        if (sendRequest(port, filename) == 0)
            printf("%s downloaded successfully\n", filename);
        else
            fprintf(stderr, "[Error] Failed to download the file.\n");
    }
    else
        fprintf(stderr, "[Error] Invalid response.\n");
}

void dealDSC()
{
    Auth = false;
    memset(uname, 0, sizeof(uname));
    HBStat = false;
    TCPSStat = false;
}

bool startThread(THandle *thread, tFuncType tf, void *args)
{
#ifdef _WIN32
    *thread = (HANDLE)_beginthreadex(NULL, 0, tf, args, 0, NULL);
    if (*thread == NULL)
    {
        fprintf(stderr, "[Fatal] Failed to create thread\n");
        return false;
    }
#else
    if (pthread_create(thread, NULL, tf, args) != 0)
    {
        fprintf(stderr, "[Fatal] Failed to create thread\n");
        return false;
    }
#endif
    return true;
}

void clear_stdin() 
{
    int c;
    while ((c = getchar()) != '\n' && c != EOF)
        ;
}

void startCli(int sersock)
{
    THandle thHB, thTCPServer;
    while (true)
    {
        if (!Auth)
        {
            char pass[17] = {0};
            char user[17] = {0};
            printf("Enter username: ");
            if (scanf("%16s", user) != 1)
                return;
            printf("Enter password: ");
            if (scanf("%16s", pass) != 1)
                return;
            TCPSerPort = genPort();
            if (sendATH(sersock, user, pass, TCPSerPort))
            {
                /* The heartbeat thread reads uname, so set it before the thread starts. */
                strcpy(uname, user);
                sprintf(homedir, "./%.16s", uname);
                HBStat = true;
                TCPSStat = true;
                if (!startThread(&thHB, tHeartbeats, NULL))
                {
                    fprintf(stderr, "[Fatal] Failed to start the heartbeat thread.\n");
                    return;
                }
                if (!startThread(&thTCPServer, tTCPServer, NULL))
                {
                    fprintf(stderr, "[Fatal] Failed to start the TCP server thread.\n");
                    return;
                }
                Auth = true;
                printf("Welcome to PeerShare!\nAvailable commands are: get, lap, lpf, pub, sch, unp, xit\n");
            }
            else
                printf("Authentication failed. Please try again.\n");
            continue;
        }
        char cmd[151] = {0};
        char substr[141] = {0};
        printf("> ");
        if (scanf("%150s", cmd) != 1)
            return;
        if (strncmp(cmd, "lap", 3) == 0)
            sendLap(sersock);
        elif (strncmp(cmd, "sch", 3) == 0)
        {
            if (scanf("%140s", substr) != 1)
                return;
            sendSch(sersock, substr);
        }
        elif (strncmp(cmd, "pub", 3) == 0)
        {
            if (scanf("%140s", substr) != 1)
                return;
            sendPub(sersock, substr);
        }
        elif (strncmp(cmd, "unp", 3) == 0)
        {
            if (scanf("%140s", substr) != 1)
                return;
            sendUnp(sersock, substr);
        }
        elif (strncmp(cmd, "lpf", 3) == 0)
            sendLpf(sersock);
        elif (strncmp(cmd, "get", 3) == 0)
        {
            if (scanf("%140s", substr) != 1)
                return;
            sendGet(sersock, substr);
        }
        elif (strncmp(cmd, "xit", 3) == 0)
        {
            printf("Goodbye!\n");
            return;
        }
        else printf("Unknown command.\n");
        clear_stdin();
    }
}

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        fprintf(stderr, "[Fatal] Please input Server Port.\n");
        return 1;
    }
    int sp = strPort(argv[1]);
    ServerPort = (unsigned)sp;
    if (sp < 0)
    {
        fprintf(stderr, "[Fatal] Invalid Port.\n");
        return 1;
    }
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        return 1;
#endif
    int ssock;
    ssock = socket(AF_INET, SOCK_DGRAM, 0);
    if (ssock < 0)
        return 1;
    /* Do not block forever if the server never answers (UDP has no delivery guarantee). */
#ifdef _WIN32
    DWORD rcvtmo = 5000;
    setsockopt(ssock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&rcvtmo, sizeof(rcvtmo));
#else
    struct timeval rcvtmo = {5, 0};
    setsockopt(ssock, SOL_SOCKET, SO_RCVTIMEO, &rcvtmo, sizeof(rcvtmo));
#endif
    startCli(ssock);
    HBStat = false;
    TCPSStat = false;
    closes(ssock);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}