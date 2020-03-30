#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/time.h>
#include <pthread.h>

/*
*  Klasėje buvau gavęs pakeitimą padaryti:
*  
*  Du Chat serveriai turi atskirus savo klientus.
* 
*  Bet kurio serverio klientas gali išsiųsti komandą [OPEN S2]  ir ji leis siųsti žinutes, kurias matys tik kito Chat serverio klientai
*  arba [CLOSE S2], kuri uždaro šią komunikaciją ir siunčiamos žinutės vėl matomos tame pačiame serveryje
*/

// kompiliuojama su komanda 'gcc -pthread -o serverThreads serverThreads.c'

#define TRUE 1
#define FALSE 0
#define PORT 32666
#define REMOTE_SRV_PORT 10000 // port used to communicate with the second chat server
#define BUF_SIZE 1025
#define MAX_CLIENTS 30
#define CMDOPEN "[OPEN S2]\n" // command used to redirect messages to remote chat server clients
#define CMDCLOSE "[CLOSE S2]\n" // command used to stop redirecting messages to remote chat server

void *listenToClient(void *threadNum);

void *connectSrv();

int sendToClients(int senderIndex, char *sendThis);

int getSenderIndex(int socket);

void init_arrays();

void removeClient(int index);

int determineMaster(char *argument);

struct ServerInfo
{
    int socket;
};

char *names[MAX_CLIENTS]; // array of user names
int redirectors[MAX_CLIENTS]; // array of which clients have chosen to redirect their messages
int client_socket[MAX_CLIENTS]; // socket descriptor of every connnected client
pthread_mutex_t lock;
pthread_mutex_t lockStdin;
int amIMaster;
int remoteServerSocket = -1;

int main(int argc, char *argv[])
{
    int master_socket,
        addrlen,
        new_socket,
        i = 0,
        curThreadId = 1,
        duplicate = TRUE,
        added = FALSE;

    struct sockaddr_in6 address6;
    char buffer[BUF_SIZE];

    if (pthread_mutex_init(&lock, NULL) != 0)
    {
        puts("mutex init has failed");
        return -1;
    }

    //initialize all array elements to default values
    init_arrays();

    // separating servers and determining the leading one
    switch (fork())
    {
        case -1:
            perror("fork failed");
            return -1;
        case 0:
            amIMaster = FALSE;
            break;
        default:
            amIMaster = TRUE;
            break;
    }

    // start a thread on which the remote server connection will be made
    pthread_t srvThreadId = MAX_CLIENTS + 1;
    pthread_create(&srvThreadId, NULL, connectSrv, NULL);


// # start REGION socket creation and binding 

    if ((master_socket = socket(AF_INET6, SOCK_STREAM, 0)) == 0)
        exit(EXIT_FAILURE);

    address6.sin6_family = AF_INET6;
    address6.sin6_addr = in6addr_any;
    address6.sin6_port = htons(PORT + amIMaster);

    if (bind(master_socket, (struct sockaddr *)&address6, sizeof(address6)) < 0)
    {
        perror("bind failed");
        close(master_socket);
        exit(EXIT_FAILURE);
    }
    printf("Listening on port %d \n", PORT + amIMaster);

    if (listen(master_socket, MAX_CLIENTS) < 0)
    {
        perror("listen");
        exit(EXIT_FAILURE);
    }
    struct ServerInfo *info;
    info = (struct ServerInfo *)malloc(sizeof(struct ServerInfo));
    addrlen = sizeof(address6);

// #end REGION socket creation and binding 

    // accept new connections, add them to the list and launch new threads for them
    while (TRUE)
    {
        if ((new_socket = accept(master_socket, (struct sockaddr *)&address6, (socklen_t *)&addrlen)) < 0)
        {
            perror("accept");
            exit(EXIT_FAILURE);
        }

        added = FALSE;
        pthread_mutex_lock(&lock);
        for (i = 0; i < MAX_CLIENTS; ++i)
        {
            if (client_socket[i] == 0)
            {
                client_socket[i] = new_socket;
                added = TRUE;
                break;
            }
        }
        pthread_mutex_unlock(&lock);

        if(added == FALSE){
            puts("No empty sockets left. Client rejected");
            continue;
        }
        info->socket = new_socket;

        pthread_t tid = curThreadId++;
        pthread_create(&tid, NULL, listenToClient, (void *)info);
    }
    return 0;
}

void *listenToClient(void *serverInfo)
{
    struct ServerInfo *info = (struct ServerInfo *)serverInfo;

    int len,
        duplicate = TRUE,
        socket = info->socket,
        i;

    char *nameConfirmed;
    nameConfirmed = (char *)malloc(BUF_SIZE);
    nameConfirmed = "VARDASOK\n";

    char *messageName;
    messageName = (char *)malloc(BUF_SIZE);
    messageName = "ATSIUSKVARDA\n";

    char *msgStart = "PRANESIMAS";
    char buffer[BUF_SIZE];
    char *sendThis = (char *)malloc(BUF_SIZE);

    char *noRemoteSrvMsg;
    noRemoteSrvMsg = (char *)malloc(BUF_SIZE);
    noRemoteSrvMsg = "remote server has not connected yet\n";


    // ask for a name and determine whether it is not taken yet
    while (duplicate != FALSE)
    {
        memset(buffer, '\0', BUF_SIZE);
        if (send(socket, messageName, strlen(messageName), 0) != strlen(messageName))
            perror("send");

        puts("Request for name sent successfully");
        duplicate = FALSE;

        if ((len = read(socket, buffer, BUF_SIZE)) != -1)
        {
            if(len == 0){
                puts("Host disconnected");
                break;
            }
            buffer[len - 1] = '\0'; // -1 because java gives with \n appended
            pthread_mutex_lock(&lock);
            for (i = 0; i < MAX_CLIENTS; ++i)
            {
                if (strcmp(buffer, (names[i])) == 0)
                {
                    duplicate = TRUE;
                    break;
                }
            }
            pthread_mutex_unlock(&lock);
        }
    }

    puts("name confirmed");
    send(socket, nameConfirmed, strlen(nameConfirmed), 0);
    pthread_mutex_lock(&lock);

    // make name ready for printing 'name + ": " '
    for (i = 0; i < MAX_CLIENTS; ++i)
    {
        if (socket == client_socket[i])
        {
            strcat(buffer, (char *)": ");
            strcpy(names[i], buffer);
        }
    }
    pthread_mutex_unlock(&lock);

    sleep(1);
    int j;
    int senderIndex = -1;
    memset(buffer, '\0', BUF_SIZE);

    while ((len = read(socket, buffer, BUF_SIZE)) != -1)
    {
        if(len == 0){
            puts("Client disconnected");
            break;
        }

        pthread_mutex_lock(&lock);
        senderIndex = getSenderIndex(socket);
        pthread_mutex_unlock(&lock);

        printf("buf: %s\n", buffer);

        // close remote connection command received?
        if (strcmp(CMDCLOSE, buffer) == 0)
        {
            puts("equal to close");
            if (remoteServerSocket != -1)
                redirectors[senderIndex] = 0;
            else
                send(client_socket[senderIndex], noRemoteSrvMsg, strlen(noRemoteSrvMsg), 0);

            memset(buffer, '\0', BUF_SIZE);
            memset(sendThis, '\0', BUF_SIZE);
            continue;
        }
        // open remote connection command received?
        else if (strcmp(CMDOPEN, buffer) == 0)
        {
            puts("equal to open");
            if (remoteServerSocket != -1)
                redirectors[senderIndex] = 1;
            else
                send(client_socket[senderIndex], noRemoteSrvMsg, strlen(noRemoteSrvMsg), 0);

            memset(buffer, '\0', BUF_SIZE);
            memset(sendThis, '\0', BUF_SIZE);
            continue;
        }

        strcpy(sendThis, msgStart);
        strcat(sendThis, names[senderIndex]);
        strcat(sendThis, buffer);

        pthread_mutex_lock(&lockStdin);
        printf("RECEIVED: %s", sendThis);

        // sending the received message to every client on the list
        j = sendToClients(senderIndex, sendThis); 

        printf("SENT TO %d CLIENTS\n", j);
        pthread_mutex_unlock(&lockStdin);

        memset(buffer, '\0', BUF_SIZE);
        memset(sendThis, '\0', BUF_SIZE);
    }

    removeClient(getSenderIndex(socket));
    close(socket);
}

void *connectSrv()
{
    struct sockaddr_in6 address6;
    address6.sin6_family = AF_INET6;
    address6.sin6_addr = in6addr_any;
    address6.sin6_port = htons(REMOTE_SRV_PORT);
    remoteServerSocket = socket(AF_INET6, SOCK_STREAM, 0);
    int masterSocket = socket(AF_INET6, SOCK_STREAM, 0);

    // Wait for connection or connect to remote server right away
    if (amIMaster == TRUE)
    {
        if (bind(masterSocket, (struct sockaddr *)&address6, sizeof(address6)) < 0)
        {
            puts("Failed to bind remoteServerSocket\nending listening...");
            close(masterSocket);
            return NULL;
        }
        if (listen(masterSocket, 3) < 0)
        {
            puts("failed at listening to remote server\nending listening...");
            return NULL;
        }

        int addrlen = sizeof(address6);
        if ((remoteServerSocket = accept(masterSocket, (struct sockaddr *)&address6, (socklen_t *)&addrlen)) < 0)
        {
            puts("failed at accepting remote server\nending listening...");
            return NULL;
        }
    }
    else
    {
        while (connect(remoteServerSocket, (struct sockaddr *)&address6, sizeof(address6)) == -1)
            sleep(1);
    }

    char buffer[BUF_SIZE];
    int len;
    // here start listening for the messages from other server
    while ((len = read(remoteServerSocket, buffer, BUF_SIZE)) != -1)
    {
        // send received text to every client_socket[]
        if(len == 0){
            puts("Client disconnected");
            break;
        }

        puts("received from remote");
        printf("%d\n", len);
        sendToClients(-1, buffer);
        memset(buffer, '\0', BUF_SIZE);
    }

    remoteServerSocket = -1;
}

// returns the number of clients to which it sent the msg or -1 when sent to remote
int sendToClients(int senderIndex, char *sendThis)
{
    int i;
    int j = 0;
    if (senderIndex != -1)
    {
        if (redirectors[senderIndex] != 0)
        {
            send(remoteServerSocket, sendThis, strlen(sendThis), 0);
            return -1;
        }
    }

    for (i = 0; i < MAX_CLIENTS; ++i)
    {
        if (client_socket[i] != 0)
        {
            send(client_socket[i], sendThis, strlen(sendThis), 0);
            j++;
        }
    }
    return j;
}

int getSenderIndex(int socket)
{
    int senderIndex;
    for (senderIndex = 0; senderIndex < MAX_CLIENTS; ++senderIndex)
    {
        if (client_socket[senderIndex] == socket)
        {
            puts("Found socket");
            printf("%d %s\n", senderIndex, names[senderIndex]);
            break;
        }
    }
    return senderIndex;
}

void init_arrays()
{
    for (int i = 0; i < MAX_CLIENTS; ++i)
    {
        client_socket[i] = 0;
        redirectors[i] = 0;
        names[i] = (char *)malloc(BUF_SIZE);
    }
}

// replace client values in the arrays with default (empty ones)
void removeClient(int index)
{
    pthread_mutex_lock(&lock);
    client_socket[index] = 0;
    names[index] = '\0';
    redirectors[index] = 0;
    pthread_mutex_unlock(&lock);
}

/** [DEPRECATED]
 * 
 * Currently not used since both servers are being launched from the same program
 * with automatic master determining
 * 
* determines whether it is the server that needs to bind a socket and listen from remote server
* or is it the second server and it needs to connect to the already launched first server
* use './serverThreads 1' to launch as first one
* './serverThreads 0' to launch as second one
*/

int determineMaster(char *argument){
    if (strcmp(argument, "0") == 0)
    {
        amIMaster = FALSE;
    }
    else if (strcmp(argument, "1") == 0)
    {
        amIMaster = TRUE;
    }
    else
    {
        puts("Wrong parameter for srv initialisation\nMust be 1 or 0");
        return -1;
    }
    return 0;
}