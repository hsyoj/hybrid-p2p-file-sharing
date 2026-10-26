/*
 * PeerShare index server.
 *
 * Handles authentication, peer presence (UDP heartbeats) and the file index.
 * File contents never pass through the server: peers download from each
 * other directly over TCP.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <signal.h>
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
static CRITICAL_SECTION gLock;
#define LOCK_INIT() InitializeCriticalSection(&gLock)
#define LOCK() EnterCriticalSection(&gLock)
#define UNLOCK() LeaveCriticalSection(&gLock)
#else
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <sys/select.h>
#define Tfunc void *
#define THandle pthread_t
#define TNcode NULL
#define TEcode ((void *)1)
typedef void *(*tFuncType)(void *);
static pthread_mutex_t gLock = PTHREAD_MUTEX_INITIALIZER;
#define LOCK_INIT() ((void)0)
#define LOCK() pthread_mutex_lock(&gLock)
#define UNLOCK() pthread_mutex_unlock(&gLock)
static inline void Sleep(unsigned int x) { usleep(x * 1000); }
#endif

#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#endif

#define elif else if
#define BUFFER_SIZE 1024
/* A peer is considered offline if no heartbeat arrives within this many ms.
 * Clients send one every 2 s, so this leaves a margin for scheduling jitter. */
#define HEARTBEAT_INTERVAL 3000
#define nullptr NULL
typedef struct sockaddr_in sockaddr_in;
typedef struct sockaddr sockaddr;
typedef struct timeval timeval;

#ifdef _WIN32
int gettimeofday(timeval *tv, void *tz);
#endif

/* ------------------Hash Table------------------ */
#define TABLE_SIZE 111
#define FNV_OFFSET 2166136261u
#define FNV_PRIME 16777619u

typedef struct KeyValue
{
    char *key;
    char *value;
    timeval *time;
    struct KeyValue *next;
} KeyValue;

typedef struct HashTable
{
    KeyValue **table;
} HashTable;

HashTable *htCreate();
unsigned int hash(const char *key);
void htInsert(HashTable *ht, const char *key, const char *value, timeval *ltime);
char *htGetValue(HashTable *ht, const char *key);
timeval *htGetTime(HashTable *ht, const char *key);
bool htKeyExists(HashTable *ht, const char *key);
void htRemove(HashTable *ht, const char *key);
void htUpdate(HashTable *ht, const char *key, const char *newValue, timeval *newTime);
void htFree(HashTable *ht);

HashTable *htCreate()
{
    HashTable *ht = malloc(sizeof(HashTable));
    ht->table = malloc(sizeof(KeyValue *) * TABLE_SIZE);
    for (int i = 0; i < TABLE_SIZE; i++)
        ht->table[i] = nullptr;
    return ht;
}

unsigned int hash(const char *key)
{
    unsigned int h = FNV_OFFSET;
    while (*key)
    {
        h ^= (unsigned char)(*key++);
        h *= FNV_PRIME;
    }
    return h % TABLE_SIZE;
}

/* Insert a new entry, or update the existing one. value and ltime may be NULL. */
void htInsert(HashTable *ht, const char *key, const char *value, timeval *ltime)
{
    unsigned int index = hash(key);
    for (KeyValue *cur = ht->table[index]; cur != nullptr; cur = cur->next)
    {
        if (strcmp(cur->key, key) == 0)
        {
            htUpdate(ht, key, value, ltime);
            return;
        }
    }

    KeyValue *newPair = malloc(sizeof(KeyValue));
    newPair->key = strdup(key);
    newPair->value = value ? strdup(value) : nullptr;
    if (ltime)
    {
        newPair->time = malloc(sizeof(timeval));
        *newPair->time = *ltime;
    }
    else
        newPair->time = nullptr;

    newPair->next = ht->table[index];
    ht->table[index] = newPair;
}

char *htGetValue(HashTable *ht, const char *key)
{
    if (key == nullptr)
        return nullptr;

    unsigned int index = hash(key);
    KeyValue *pair = ht->table[index];
    while (pair != nullptr)
    {
        if (strcmp(pair->key, key) == 0)
            return pair->value;
        pair = pair->next;
    }
    return nullptr;
}

timeval *htGetTime(HashTable *ht, const char *key)
{
    if (key == nullptr)
        return nullptr;

    unsigned int index = hash(key);
    KeyValue *pair = ht->table[index];
    while (pair != nullptr)
    {
        if (strcmp(pair->key, key) == 0)
            return pair->time;
        pair = pair->next;
    }
    return nullptr;
}

bool htKeyExists(HashTable *ht, const char *key)
{
    return htGetValue(ht, key) != nullptr;
}

void htRemove(HashTable *ht, const char *key)
{
    unsigned int index = hash(key);
    KeyValue *pair = ht->table[index];
    KeyValue *prev = nullptr;
    while (pair != nullptr)
    {
        if (strcmp(pair->key, key) == 0)
        {
            if (prev == nullptr)
                ht->table[index] = pair->next;
            else
                prev->next = pair->next;
            free(pair->key);
            free(pair->value);
            free(pair->time);
            free(pair);
            return;
        }
        prev = pair;
        pair = pair->next;
    }
}

void htUpdate(HashTable *ht, const char *key, const char *newValue, timeval *newTime)
{
    unsigned int index = hash(key);
    KeyValue *curr = ht->table[index];

    while (curr != nullptr)
    {
        if (strcmp(curr->key, key) == 0)
        {
            if (newValue && (!curr->value || strcmp(curr->value, newValue) != 0))
            {
                free(curr->value);
                curr->value = strdup(newValue);
            }
            if (newTime)
            {
                if (curr->time == nullptr)
                    curr->time = malloc(sizeof(timeval));
                *curr->time = *newTime;
            }
            return;
        }
        curr = curr->next;
    }
}

void htFree(HashTable *ht)
{
    for (int i = 0; i < TABLE_SIZE; i++)
    {
        KeyValue *curr = ht->table[i];
        while (curr != nullptr)
        {
            KeyValue *temp = curr;
            curr = curr->next;
            free(temp->key);
            free(temp->value);
            free(temp->time);
            free(temp);
        }
    }
    free(ht->table);
    free(ht);
}
/* ----------------Hash Table END---------------- */

#define SEND 1
#define RECV 2

HashTable *auth = nullptr, *filel = nullptr, *peerl = nullptr, *alive = nullptr;
static const char *homedir = "./server";
volatile bool HBCStat = true;
static volatile sig_atomic_t gStop = 0;
static const char *ServerIP = "127.0.0.1";
int ssock;

void trim(char *str);
bool readDB();
char *formatTime(const timeval *tv, char *buffer, size_t buffer_size);
double timeDiff(const timeval *start, const timeval *end);
bool udpSendMsg(int sock, const char *msg, sockaddr_in *client_addr);
void printLog(int port, int mode, const char *username, const char *cmd);
void procHBT(sockaddr_in *client_addr, const char *username);
void procLAP(int sock, sockaddr_in *client_addr, const char *username);
void procLPF(int sock, sockaddr_in *client_addr, const char *username);
void procPUB(int sock, sockaddr_in *client_addr, const char *username, const char *filename);
void procUNP(int sock, sockaddr_in *client_addr, const char *username, const char *filename);
void procSCH(int sock, sockaddr_in *client_addr, const char *username, const char *substr);
void procGET(int sock, sockaddr_in *client_addr, const char *username, const char *filename);
void procATH(int sock, sockaddr_in *client_addr, const char *username, const char *passwd, const char *port);
void closes(int sock);
bool startThread(THandle *thread, tFuncType tf, void *args);
void joinThread(THandle thread);
int strPort(const char *str);
Tfunc tHBChecker(void *args);
void hdlCommand(int sock, char *buffer, sockaddr_in *client_addr);

#ifdef _WIN32
int gettimeofday(timeval *tv, void *tz)
{
    (void)tz;
    if (tv == NULL)
        return -1;
    FILETIME ft;
    ULARGE_INTEGER u;
    GetSystemTimeAsFileTime(&ft);
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    /* 100 ns ticks since 1601 -> seconds/microseconds since 1970 */
    unsigned long long t = u.QuadPart - 116444736000000000ULL;
    tv->tv_sec = (long)(t / 10000000ULL);
    tv->tv_usec = (long)((t % 10000000ULL) / 10ULL);
    return 0;
}
#endif

void trim(char *str)
{
    if (!str)
        return;

    int len = (int)strlen(str);
    while (len > 0 && isspace((unsigned char)str[len - 1]))
    {
        str[len - 1] = '\0';
        len--;
    }
}

bool readDB()
{
    char filepath[256];
    snprintf(filepath, sizeof(filepath), "%s/credentials.txt", homedir);

    FILE *file = fopen(filepath, "r");
    if (!file)
        return false;

    char line[256];
    char username[17] = {0};
    char password[17] = {0};
    while (fgets(line, sizeof(line), file))
    {
        trim(line);
        if (sscanf(line, "%16s %16s", username, password) == 2)
            htInsert(auth, username, password, nullptr);
    }

    fclose(file);
    return true;
}

char *formatTime(const timeval *tv, char *buffer, size_t buffer_size)
{
    struct tm *tm_info;
    time_t time_value = (time_t)tv->tv_sec;
    tm_info = localtime(&time_value);
    strftime(buffer, buffer_size, "%H:%M:%S", tm_info);
    int ms = (int)(tv->tv_usec / 1000);
    snprintf(buffer + strlen(buffer), buffer_size - strlen(buffer), ".%03d", ms);
    return buffer;
}

/* Milliseconds elapsed from start to end. */
double timeDiff(const timeval *start, const timeval *end)
{
    double seconds = (double)(end->tv_sec - start->tv_sec) * 1000.0;
    double microseconds = (double)(end->tv_usec - start->tv_usec) / 1000.0;
    return seconds + microseconds;
}

bool udpSendMsg(int sock, const char *msg, sockaddr_in *client_addr)
{
    int sent_len = (int)sendto(sock, msg, strlen(msg), 0, (sockaddr *)client_addr, sizeof(*client_addr));
    if (sent_len < 0)
    {
        perror("[Error] Failed to send message");
        return false;
    }
    elif ((size_t)sent_len != strlen(msg))
    {
        fprintf(stderr, "[Error] Partial message sent\n");
        return false;
    }
    return true;
}

void printLog(int port, int mode, const char *username, const char *cmd)
{
    timeval tv;
    char tbuff[20] = {0};
    gettimeofday(&tv, nullptr);
    formatTime(&tv, tbuff, sizeof(tbuff));
    if (mode == SEND)
        printf("%s: %d: Send %s to %s\n", tbuff, port, cmd, username);
    elif (mode == RECV)
        printf("%s: %d: Received %s from %s\n", tbuff, port, cmd, username);
    fflush(stdout);
}

/* Append item (preceded by sep unless it is the first) to a "OK [...]" reply,
 * always leaving room for the closing ']'. Returns false if it does not fit. */
static bool appendItem(char *dst, size_t cap, const char *item, char sep, bool first)
{
    size_t len = strlen(dst);
    size_t need = strlen(item) + (first ? 0 : 1) + 2; /* item, sep, ']', NUL */
    if (len + need > cap)
        return false;
    if (!first)
    {
        dst[len++] = sep;
        dst[len] = '\0';
    }
    strcat(dst, item);
    return true;
}

static bool replyDSCIfOffline(int sock, sockaddr_in *client_addr, const char *username)
{
    if (htKeyExists(alive, username))
        return false;
    udpSendMsg(sock, "DSC", client_addr);
    printLog(ntohs(client_addr->sin_port), SEND, username, "DSC");
    return true;
}

void procHBT(sockaddr_in *client_addr, const char *username)
{
    /* Heartbeats only refresh sessions that were established with ATH. */
    if (!htKeyExists(alive, username))
        return;

    timeval tv;
    gettimeofday(&tv, nullptr);
    htUpdate(alive, username, nullptr, &tv);
    printLog(ntohs(client_addr->sin_port), RECV, username, "HBT");
}

void procLAP(int sock, sockaddr_in *client_addr, const char *username)
{
    if (replyDSCIfOffline(sock, client_addr, username))
        return;

    char result[BUFFER_SIZE] = "OK [";
    bool first = true;

    for (size_t i = 0; i < TABLE_SIZE; i++)
    {
        KeyValue *current = peerl->table[i];
        while (current)
        {
            if (strcmp(current->key, username) != 0 && htKeyExists(alive, current->key) && current->value != nullptr)
            {
                if (appendItem(result, sizeof(result), current->key, ',', first))
                    first = false;
            }
            current = current->next;
        }
    }
    strcat(result, "]");

    if (udpSendMsg(sock, result, client_addr))
        printLog(ntohs(client_addr->sin_port), SEND, username, "OK");
    else
        fprintf(stderr, "ERROR when sending LAP response to %s\n", username);
}

void procLPF(int sock, sockaddr_in *client_addr, const char *username)
{
    if (replyDSCIfOffline(sock, client_addr, username))
        return;

    char result[BUFFER_SIZE] = "OK [";
    bool first = true;

    for (size_t i = 0; i < TABLE_SIZE; i++)
    {
        KeyValue *current = filel->table[i];
        while (current)
        {
            /* Only list the requesting user's own published files. */
            if (current->value && strcmp(current->value, username) == 0)
            {
                if (appendItem(result, sizeof(result), current->key, '\x1F', first))
                    first = false;
            }
            current = current->next;
        }
    }
    strcat(result, "]");

    if (udpSendMsg(sock, result, client_addr))
        printLog(ntohs(client_addr->sin_port), SEND, username, "OK");
    else
        fprintf(stderr, "ERROR when sending LPF response to %s\n", username);
}

void procPUB(int sock, sockaddr_in *client_addr, const char *username, const char *filename)
{
    if (replyDSCIfOffline(sock, client_addr, username))
        return;

    /* A file name can be claimed by one live peer at a time. */
    char *owner = htGetValue(filel, filename);
    if (owner && strcmp(owner, username) != 0 && htKeyExists(alive, owner))
    {
        udpSendMsg(sock, "ERR", client_addr);
        printLog(ntohs(client_addr->sin_port), SEND, username, "ERR");
        return;
    }

    htInsert(filel, filename, username, nullptr);

    if (udpSendMsg(sock, "OK", client_addr))
        printLog(ntohs(client_addr->sin_port), SEND, username, "OK");
    else
        fprintf(stderr, "ERROR when sending PUB response to %s\n", username);
}

void procUNP(int sock, sockaddr_in *client_addr, const char *username, const char *filename)
{
    if (replyDSCIfOffline(sock, client_addr, username))
        return;

    char *owner = htGetValue(filel, filename);
    if (owner && strcmp(owner, username) == 0)
        htRemove(filel, filename);

    if (udpSendMsg(sock, "OK", client_addr))
        printLog(ntohs(client_addr->sin_port), SEND, username, "OK");
    else
        fprintf(stderr, "ERROR when sending UNP response to %s\n", username);
}

/* Substring search: every published file of an online peer whose name contains substr. */
void procSCH(int sock, sockaddr_in *client_addr, const char *username, const char *substr)
{
    if (replyDSCIfOffline(sock, client_addr, username))
        return;

    char result[BUFFER_SIZE] = "OK [";
    bool first = true;

    for (size_t i = 0; i < TABLE_SIZE; i++)
    {
        KeyValue *current = filel->table[i];
        while (current)
        {
            if (current->value && htKeyExists(alive, current->value) && strstr(current->key, substr) != nullptr)
            {
                if (appendItem(result, sizeof(result), current->key, '\x1F', first))
                    first = false;
            }
            current = current->next;
        }
    }
    strcat(result, "]");

    if (udpSendMsg(sock, result, client_addr))
        printLog(ntohs(client_addr->sin_port), SEND, username, "OK");
    else
        fprintf(stderr, "ERROR when sending SCH response to %s\n", username);
}

void procGET(int sock, sockaddr_in *client_addr, const char *username, const char *filename)
{
    if (replyDSCIfOffline(sock, client_addr, username))
        return;

    char *owner = htGetValue(filel, filename);
    char *port = htGetValue(peerl, owner);
    char buff[64] = {0};
    if (owner == nullptr || port == nullptr || !htKeyExists(alive, owner))
    {
        udpSendMsg(sock, "FNF", client_addr);
        printLog(ntohs(client_addr->sin_port), SEND, username, "FNF");
        return;
    }

    if (strcmp(owner, username) == 0)
        snprintf(buff, sizeof(buff), "OK -99");
    else
        snprintf(buff, sizeof(buff), "OK %s", port);

    if (udpSendMsg(sock, buff, client_addr))
        printLog(ntohs(client_addr->sin_port), SEND, username, "OK");
    else
        fprintf(stderr, "ERROR when sending GET response to %s\n", username);
}

void procATH(int sock, sockaddr_in *client_addr, const char *username, const char *passwd, const char *port)
{
    char *stored = htGetValue(auth, username);
    if (!htKeyExists(alive, username) && stored && strcmp(stored, passwd) == 0)
    {
        timeval now;
        gettimeofday(&now, nullptr);
        htInsert(peerl, username, port, nullptr);
        htInsert(alive, username, "True", &now);
        if (udpSendMsg(sock, "OK", client_addr))
            printLog(ntohs(client_addr->sin_port), SEND, username, "OK");
        else
            fprintf(stderr, "ERROR when sending ATH response to %s\n", username);
    }
    else
    {
        if (udpSendMsg(sock, "ERR", client_addr))
            printLog(ntohs(client_addr->sin_port), SEND, username, "ERR");
        else
            fprintf(stderr, "ERROR when sending ATH response to %s\n", username);
    }
}

void closes(int sock)
{
#ifdef _WIN32
    closesocket(sock);
#else
    close(sock);
#endif
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

void joinThread(THandle thread)
{
#ifdef _WIN32
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
#else
    pthread_join(thread, NULL);
#endif
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

/* Expires peers whose last heartbeat is older than HEARTBEAT_INTERVAL. */
Tfunc tHBChecker(void *args)
{
    (void)args;
    while (HBCStat)
    {
        timeval now;
        gettimeofday(&now, nullptr);

        LOCK();
        for (int i = 0; i < TABLE_SIZE; i++)
        {
            KeyValue *curr = alive->table[i];
            while (curr != nullptr)
            {
                KeyValue *next = curr->next;
                if (curr->time && timeDiff(curr->time, &now) > HEARTBEAT_INTERVAL)
                {
                    char *name = strdup(curr->key);
                    htRemove(alive, name);
                    htRemove(peerl, name);
                    printf("Peer %s timed out\n", name);
                    fflush(stdout);
                    free(name);
                }
                curr = next;
            }
        }
        UNLOCK();
        Sleep(45);
    }
    return TNcode;
}

void hdlCommand(int sock, char *buffer, sockaddr_in *client_addr)
{
    char command[4] = {0};
    char username[17] = {0};
    char arg2[141] = {0};
    char port[6] = {0};

    if (sscanf(buffer, "%3s", command) != 1)
        return;

    if (strcmp(command, "LAP") == 0)
    {
        if (sscanf(buffer, "LAP %16s", username) == 1)
            procLAP(sock, client_addr, username);
    }
    elif (strcmp(command, "LPF") == 0)
    {
        if (sscanf(buffer, "LPF %16s", username) == 1)
            procLPF(sock, client_addr, username);
    }
    elif (strcmp(command, "PUB") == 0)
    {
        if (sscanf(buffer, "PUB %16s %140s", username, arg2) == 2)
            procPUB(sock, client_addr, username, arg2);
    }
    elif (strcmp(command, "UNP") == 0)
    {
        if (sscanf(buffer, "UNP %16s %140s", username, arg2) == 2)
            procUNP(sock, client_addr, username, arg2);
    }
    elif (strcmp(command, "SCH") == 0)
    {
        if (sscanf(buffer, "SCH %16s %140s", username, arg2) == 2)
            procSCH(sock, client_addr, username, arg2);
    }
    elif (strcmp(command, "GET") == 0)
    {
        if (sscanf(buffer, "GET %16s %140s", username, arg2) == 2)
            procGET(sock, client_addr, username, arg2);
    }
    elif (strcmp(command, "ATH") == 0)
    {
        if (sscanf(buffer, "ATH %16s %140s %5s", username, arg2, port) == 3)
            procATH(sock, client_addr, username, arg2, port);
    }
    elif (strcmp(command, "HBT") == 0)
    {
        if (sscanf(buffer, "HBT %16s", username) == 1)
            procHBT(client_addr, username);
    }
    else
        fprintf(stderr, "[Error] Unknown command: %s\n", buffer);
}

static void onSigint(int sig)
{
    (void)sig;
    gStop = 1;
}

void finalClean()
{
    htFree(auth);
    htFree(peerl);
    htFree(filel);
    htFree(alive);
    closes(ssock);
#ifdef _WIN32
    WSACleanup();
#endif
}

int main(int argc, char **argv)
{
    int ServerPort = -1;
    if (argc != 2)
    {
        fprintf(stderr, "[Fatal] Please input Server Port.\n");
        return -1;
    }
    ServerPort = strPort(argv[1]);
    if (ServerPort < 0)
    {
        fprintf(stderr, "[Fatal] Invalid Port.\n");
        return -1;
    }

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    LOCK_INIT();

    ssock = socket(AF_INET, SOCK_DGRAM, 0);
    if (ssock < 0)
    {
        perror("Socket creation failed");
        return 1;
    }

    auth = htCreate();
    peerl = htCreate();
    filel = htCreate();
    alive = htCreate();
    if (!readDB())
        fprintf(stderr, "[Warning] Could not read %s/credentials.txt; nobody will be able to log in.\n", homedir);

    sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr(ServerIP);
    server_addr.sin_port = htons(ServerPort);

    if (bind(ssock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        perror("Bind failed");
        finalClean();
        return 1;
    }

    THandle thHBC;
    if (!startThread(&thHBC, tHBChecker, nullptr))
    {
        fprintf(stderr, "[Fatal] Failed to start the heartbeat checker thread.\n");
        finalClean();
        return 1;
    }

    signal(SIGINT, onSigint);

    sockaddr_in client_addr;
    while (!gStop)
    {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(ssock, &readfds);
        timeval tmo = {1, 0}; /* wake up regularly so Ctrl+C is noticed on every platform */

        int activity = select(ssock + 1, &readfds, NULL, NULL, &tmo);
        if (activity < 0)
        {
            if (errno == EINTR)
                continue;
            perror("select error");
            break;
        }

        if (activity > 0 && FD_ISSET(ssock, &readfds))
        {
            char buffer[BUFFER_SIZE];
            socklen_t addr_len = sizeof(client_addr);
            int bytes = (int)recvfrom(ssock, buffer, BUFFER_SIZE - 1, 0, (struct sockaddr *)&client_addr, &addr_len);
            if (bytes > 0)
            {
                buffer[bytes] = '\0';
                LOCK();
                hdlCommand(ssock, buffer, &client_addr);
                UNLOCK();
            }
        }
    }

    HBCStat = false;
    joinThread(thHBC);
    finalClean();
    printf("Server stopped.\n");
    return 0;
}
