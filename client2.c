#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netinet/in.h>
#include <pthread.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <signal.h>
#include <ctype.h>
#include <stdatomic.h>
#define MAX_PASSWORD_LENGTH 1018  
#define PORT 8080
#define MULTICAST_PORT 8081
#define MULTICAST_GROUP "239.0.0.1"
#define BUFFER_SIZE 1024
#define  TEAM_NAME_MAX_LENGTH 100


//varibles declaretion
int timeS;
int HalfTimer_respose =0  ;// Set to 5 seconds or the desired timeout value
time_t start_time;
int recived_bet;
int udp_multicast_socket;
int tcp_socket;
struct sockaddr_in multicast_addr;
struct ip_mreqn mreq;
int DebugMode=0;
int ready_to_receive_updates = 0; // Flag to indicate readiness
pthread_mutex_t update_mutex = PTHREAD_MUTEX_INITIALIZER;
int stop_udp_listener = 0; // Flag to stop the UDP listener thread
int halftime_received = 0; // Flag to check if halftime message was received
int current_minute = 0;  // Track the current minute of the game
char buffer[BUFFER_SIZE] = {0};
char team1_name[TEAM_NAME_MAX_LENGTH];
char team2_name[TEAM_NAME_MAX_LENGTH];
int last_sequence_number = -1;
char* temp ;
int test_delay_keep_alive_ack=1;




int get_stop_udp_listener();
void set_stop_udp_listener(int value) ;
int get_ready_to_receive_updates();
void set_ready_to_receive_updates(int value);
void disconnected();
void setup_udp_multicast();
int setup_tcp_connection();
void* listen_for_updates(void* arg);//udp messegas
int process_server_messages(int tcp_socket, pthread_t update_thread) ;//  TCP messegas
int authenticate_with_server(int sock);
int place_bet(int sock);
void handle_signal(int signal);
void handle_interruption();
void extract_team_names(const char* message) ;

int KEEP_ALIVE_TIMEOUT;
int GAME_LENGTH;
typedef struct {
    int team;  // 0 for tie, 1 for team1, 2 for team2
    int amount;
   // char my_group[100];
} Bet;

Bet my_bet;

int main() {
    char buffer[BUFFER_SIZE] = {0};
            start_time = time(NULL);

    // Set up signal handler for Ctrl+C and Ctrl+Z
    signal(SIGINT, handle_signal);  // Handle Ctrl+C
    signal(SIGTSTP, handle_signal); // Handle Ctrl+Z

    // Setup UDP multicast for receiving game updates
    setup_udp_multicast();
    if(DebugMode) {printf("UDP multicast setup completed.\n");}

    // Setup TCP connection
    tcp_socket = setup_tcp_connection();
    if (tcp_socket < 0) {
        return -1;
    }
    printf("Connected to server successfully.\n");

    // Start thread to listen for UDP multicast updates
    pthread_t update_thread;
    pthread_create(&update_thread, NULL, listen_for_updates, NULL);


    // Authenticate with server
    if (authenticate_with_server(tcp_socket) < 0) {
        close(tcp_socket);
        if(DebugMode){printf("Socket closed due to authentication failure.\n");}
        set_stop_udp_listener(1); // Signal the UDP thread to stop
        pthread_join(update_thread, NULL);

        return 1;
    }
    set_ready_to_receive_updates(1);
    pthread_t server_messages_thread;

    process_server_messages(tcp_socket, update_thread);
    
    return 0;
    
}

void setup_udp_multicast() {
    // Create a UDP socket
    if ((udp_multicast_socket = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("UDP socket creation failed");
        exit(EXIT_FAILURE);
    }

    // Set reuse options
    int reuse = 1;
    if (setsockopt(udp_multicast_socket, SOL_SOCKET, SO_REUSEADDR, (char *)&reuse, sizeof(reuse)) < 0) {
        perror("Setting SO_REUSEADDR failed");
        close(udp_multicast_socket);
        close(tcp_socket);

        exit(EXIT_FAILURE);
    }

    // Bind the socket to the multicast port
    memset(&multicast_addr, 0, sizeof(multicast_addr));
    multicast_addr.sin_family = AF_INET;
    multicast_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    multicast_addr.sin_port = htons(MULTICAST_PORT);

    if (bind(udp_multicast_socket, (struct sockaddr*)&multicast_addr, sizeof(multicast_addr)) < 0) {
        
        perror("UDP socket bind failed");
        close(udp_multicast_socket);
        exit(EXIT_FAILURE);
    }

    // Join the multicast group
    mreq.imr_multiaddr.s_addr = inet_addr(MULTICAST_GROUP);
    mreq.imr_address.s_addr = htonl(INADDR_ANY);
    mreq.imr_ifindex = 0;

    if (setsockopt(udp_multicast_socket, IPPROTO_IP, IP_ADD_MEMBERSHIP, (char *)&mreq, sizeof(mreq)) < 0) {
        perror("Setting IP_ADD_MEMBERSHIP failed");
        close(udp_multicast_socket);
        exit(EXIT_FAILURE);
    }
}

void* listen_for_updates(void* arg) {
    char buffer[BUFFER_SIZE];
    fd_set readfds;
    struct timeval tv;
    int retval;

    while (!get_stop_udp_listener()) {
        FD_ZERO(&readfds);
        FD_SET(udp_multicast_socket, &readfds);

        // Set timeout for select
        tv.tv_sec = 1;  // Timeout of 1 second
        tv.tv_usec = 0;

        retval = select(udp_multicast_socket + 1, &readfds, NULL, NULL, &tv);

        if (retval == -1) {
            perror("select()");
            break;
        } 
        else if (retval > 0) {
            if (FD_ISSET(udp_multicast_socket, &readfds)) {
                int bytes_received = recv(udp_multicast_socket, buffer, BUFFER_SIZE, 0);
                if (bytes_received < 0) {
                    if (get_stop_udp_listener()) break; // Exit if stop flag is set
                    perror("UDP recv failed");
                    break;
                } else if (bytes_received > 0) {
                    buffer[bytes_received] = '\0';
                    if (get_ready_to_receive_updates()) { // Process updates only if ready
                        int received_sequence_number;
                        char update_message[BUFFER_SIZE];

                        // Extract the sequence number and the actual message
                        sscanf(buffer, "%d:%[^\n]]", &received_sequence_number, update_message);
                     //   printf("received_sequence_number:%d\n",received_sequence_number);
                   //     printf("update_message:%s\n",update_message);

                        
                        // Handle the first packet differently
                        if (last_sequence_number == -1) {
                            // This is the first valid packet we're processing, so just accept it
                            last_sequence_number = received_sequence_number;
                          //  printf("First packet received with sequence number: %d\n", received_sequence_number);
                        } else if (received_sequence_number > last_sequence_number + 1) {
                            // Missed one or more packets
                            printf("Missed packet(s). Last sequence number: %d, received: %d\n", 
                                   last_sequence_number, received_sequence_number);
                            // Optionally, request retransmission here if using TCP for retransmission
                        } else if (received_sequence_number <= last_sequence_number) {
                            // Received a duplicate or out-of-order packet
                            printf("Out-of-order packet received. Ignoring...\n");
                            continue;  // Ignore duplicate or out-of-order packets
                        }

                        // Update last sequence number
                        last_sequence_number = received_sequence_number;
                      //  printf("%d\n",last_sequence_number);
                     //   printf("udp[[[[[[[[[[[[%s]]]]]]]]]]]]\n",update_message);
                        // Handle specific types of messages
                        if (strstr(update_message, "interrupted")) {
                            handle_interruption();  // Handle interruption if found
                        } 
                        else if (strstr(update_message, "HALFTIME")) {
                            printf("[ERROR:] ITS SENT FROM UDP SOCKET AND NOT A TCP\n");
                            halftime_received = 1;  // Mark halftime as received
                        } 
                        else if (strstr(update_message, "GAME UPDATE")) {
                            int time, score1, score2;
                            sscanf(update_message, "GAME UPDATE %d %d %d", &time, &score1, &score2);
                       //     printf("%s",buffer);
                          //  printf("time=%d",time);
                            printf("Minute %d : Team %s: %d v.s Team %s: %d\n",time,team1_name,score1,team2_name,score2);
                            if (time == GAME_LENGTH / 2) {
                                sleep(1);



                                if (!halftime_received) {
                                    //TO DO: IMPLEMENT HOW TO CHECK
                                    printf("Halftime message not received, requesting it from the server...\n");
                                    char request[BUFFER_SIZE] = "REQUEST_HALFTIME_MESSAGE";
                                    send(tcp_socket, request, strlen(request), 0);
                                }
                            }
                            if (time == GAME_LENGTH) {
                                // TO-DO: IMPLEMENT CONGRATS/SORRY MESSAGE DROPPING
                            }                     
                        }
                        else if (strstr(update_message, "COUNTDOWN")) {
                            //printf("%s",update_message);
                            int remaining_time;
                            sscanf(update_message, "COUNTDOWN %d", &remaining_time);
                            printf("The game is about to start in %d minutes\n",remaining_time);
                        //    printf("Broadcasted COUNTDOWN time  %d \n", remaining_time);
                        }
                        else if (strstr(update_message, "Congratulations!") || strstr(update_message, "Sorry")) {
                            printf("[ERROR: ITS SENT FROM UDP SOCKET AND NOT A TCP]\n");
                        } 
                        else {
                            printf("[UDP] %s", update_message);
                        }
                    }
                }
            }
        }
    }

    // If halftime message not received, request it from the server
    if (!halftime_received && ready_to_receive_updates) {
        char request[BUFFER_SIZE] = "REQUEST_HALFTIME_MESSAGE";
        send(tcp_socket, request, strlen(request), 0);
        printf("Requested halftime message from server.\n");
    }

    close(udp_multicast_socket);
   // printf("Multicast socket closed after update thread finished.\n");
    return NULL;
}
    
int setup_tcp_connection() {
    int sock;
    struct sockaddr_in serv_addr;

    // Create TCP socket
    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        printf("\nSocket creation error\n");
        return -1;
    }
    printf("Socket created successfully.\n");

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);
    if (inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr) <= 0) {
        printf("\nInvalid address/ Address not supported\n");
        return -1;
    }
    if(DebugMode){printf("Address converted successfully.\n");}

    // Connect to server via TCP
    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        printf("\nConnection Failed\n");
        return -1;
    }

    return sock;
}

int authenticate_with_server(int sock) {
    char buffer[BUFFER_SIZE];
    int bytes_read;

    // Receive initial message from server
    bytes_read = read(sock, buffer, BUFFER_SIZE);
    if (bytes_read > 0) {
        buffer[bytes_read] = '\0';
        // Extract the team names from the received message
       // extract_team_names(buffer);

        if (strstr(buffer, "interrupted")) {
            handle_interruption();  // Handle interruption if found
        }
        if(strstr(buffer, "Welcome")){

            sscanf(buffer,"Welcome  %99s vs %99s  %d %d %d %d",team1_name, team2_name,&timeS,&GAME_LENGTH,&HalfTimer_respose,&KEEP_ALIVE_TIMEOUT);
            printf("Welcome to our gambling game!\n");
            printf("The game between %s vs %s is about to start in %d minutes.\n",team1_name,team2_name,timeS);
            printf("Please rnter our secret code:");
            struct timeval tv;
            fd_set readfds;
            char password[1019];// Set the timeout value
            int remaining_time = timeS - difftime(time(NULL), start_time);
            tv.tv_sec = remaining_time;
            tv.tv_usec = 0;
            // Set the file descriptor for stdin
            FD_ZERO(&readfds);
            FD_SET(STDIN_FILENO, &readfds);
            fflush(stdout);  // Ensure the prompt is displayed immediately
            int result = select(STDIN_FILENO + 1, &readfds, NULL, NULL, &tv);
            if (result > 0) {
                scanf("%s", password);
                password[strcspn(password, "\n")] = 0;  // Remove trailing newline
                char auth_message[BUFFER_SIZE];
                // Calculate maximum password length based on buffer size and prefix length
                size_t max_password_length = BUFFER_SIZE - strlen("AUTH:") - 1;
                 if (strlen(password) > max_password_length) {
                    password[max_password_length] = '\0'; // Truncate the password if necessary
                }
                // Format the auth_message safely
                snprintf(auth_message, BUFFER_SIZE, "AUTH:%s", password);
                printf("Sending password to server: %s\n", auth_message);  // Debug print
                ssize_t bytes_sent = send(sock, auth_message, strlen(auth_message), 0);
                if (bytes_sent < 0) {perror("send failed");} 
                else {printf("Password message sent successfully: %s\n", auth_message);
                }
            }
            else{
                printf("The game has alredy started :(\n");
                disconnected();
            }
        }
        else if(strstr(buffer,"ALREADYSTARTED")){
                printf("The game has already started. You cannot place a bet now.\n");
                disconnected();
                
            } 
        else{
            printf("[ChECK THIS MESSAGE, ITS NOT DIFEINED IN THE CLIENT!!]: %s",buffer);
        }
       
    } else {
        perror("read");
        return -1;
    }


    // Wait for response on password
    bytes_read = read(sock, buffer, BUFFER_SIZE);
    if (bytes_read > 0) {
        buffer[bytes_read] = '\0';
        if (strstr(buffer, "interrupted")) {
            handle_interruption();  // Handle interruption if found
        }
        else if (strstr(buffer, "Password accepted")) {
            printf("Password accepted\n");
            printf(" Place your bet, Press:\n");
            printf("(0) for a tie\n");
            printf("(1) for %s \n",team1_name);
            printf("(2) for %s\n",team2_name);
            printf("and the amout of money on it\n");
            char buffer[BUFFER_SIZE];
    struct timeval tv;
    fd_set readfds;

    // Set the timeout value for placing a bet
    int remaining_time = timeS - difftime(time(NULL), start_time);

    tv.tv_sec = remaining_time;
    tv.tv_usec = 0;

    FD_ZERO(&readfds);
    FD_SET(STDIN_FILENO, &readfds);

    // Prompt the user for the bet
    fflush(stdout);  // Ensure the prompt is displayed immediately

    int result = select(STDIN_FILENO + 1, &readfds, NULL, NULL, &tv);
    if (result > 0) {
        scanf("%d %d", &my_bet.team, &my_bet.amount); // Store bet in my_bet structure
        // Format the bet message with the "BET" header
        snprintf(buffer, BUFFER_SIZE, "BET:%d %d", my_bet.team, my_bet.amount);
        printf("Sending bet to server: %s\n", buffer);  // Debug print
        send(sock, buffer, strlen(buffer), 0);
        temp= (my_bet.team == 0) ? "tie" : (my_bet.team == 1) ? team1_name :  (my_bet.team == 2) ? team2_name : "unknown";
        // Print the bet details including the team name
        printf("Bet details sent to server: Team %d, Amount %d (the name of the group is %s)\n", 
               my_bet.team, my_bet.amount, temp);
    } 
    else {
                printf("The game has already started.\n");
                disconnected();

    }

    return 0;

        }
    } else {
        perror(" error read");
        return -1;
    }

    return 0;
}

void handle_signal(int signal) {
    if (signal == SIGINT || signal == SIGTSTP) {  // Handle Ctrl+C or Ctrl+Z
        printf("\nYou are paying %d\n", my_bet.amount);

        // Notify the server about the client's termination
        char buffer[BUFFER_SIZE];
        snprintf(buffer, BUFFER_SIZE, "CLIENT_TERMINATED %d", my_bet.amount);
        send(tcp_socket, buffer, strlen(buffer), 0);

        // Close sockets and exit
        set_stop_udp_listener(1);  // Signal the UDP thread to stop
        close(tcp_socket);       // Close the TCP socket
        close(udp_multicast_socket); // Close the UDP socket
        exit(0);                 // Exit the program
    }
}

void handle_interruption() {
    printf("\nThe game has been interrupted by the server. Closing sockets and exiting...\n");
    // Close sockets and exit
    close(tcp_socket);       // Close the TCP socket
    close(udp_multicast_socket); // Close the UDP socket
    exit(0);                 // Exit the program
}

int process_server_messages(int tcp_socket, pthread_t update_thread) {
    char buffer[BUFFER_SIZE];
    char received_group[TEAM_NAME_MAX_LENGTH];

    fd_set readfds;                // Declare the file descriptor set
    struct timeval tv;             // Declare the timeout structure
    int retval;                    // Declare the variable for select's return value
    int response_sent = 0;         // Flag to check if the halftime response was sent


    while (1) {
        int bytes_read = read(tcp_socket, buffer, BUFFER_SIZE);
        if (bytes_read > 0) {
            buffer[bytes_read] = '\0';
            //printf("[[[[[[[%s]]]]]]",buffer);

            if (strstr(buffer, "interrupted")) {
                handle_interruption();  // Handle interruption if found
            }
            
            else if (strstr(buffer, "HALFTIME")) 
            {
                halftime_received = 1;  // Mark halftime as received
                char response[BUFFER_SIZE];
                // Ask the user for a response with a timeout
                printf("Do you want to double your bet? (YES/NO): \n");
                fflush(stdout);
                // Set up select to wait for input
                fd_set readfds;
                struct timeval tv;
                FD_ZERO(&readfds);
                FD_SET(STDIN_FILENO, &readfds);
                tv.tv_sec = HalfTimer_respose;
                tv.tv_usec = 0;
                int retval = select(STDIN_FILENO + 1, &readfds, NULL, NULL, &tv);
                if (retval == -1) {
                    perror("select failed");
                } else if (retval == 0) {
                    // Timeout occurred, set default response to "NO"
                    printf("No response received. Defaulting to 'NO'.\n");
                    strcpy(response, "NO");
                } else {
                    // User input received
                    scanf("%s", response);
                    if (strcmp(response, "YES") != 0 && strcmp(response, "NO") != 0) {
                        printf("Invalid response. Defaulting to 'NO'.\n");
                        strcpy(response, "NO");
                    }
                }
                // Now, send the response with a send timeout or using select
                fd_set writefds;
                FD_ZERO(&writefds);
                FD_SET(tcp_socket, &writefds);
                tv.tv_sec = HalfTimer_respose;
                tv.tv_usec = 0;
                retval = select(tcp_socket + 1, NULL, &writefds, NULL, &tv);
                if (retval == -1) {
                    perror("select failed");
                } else if (retval == 0) {
                    printf("Timeout occurred! The socket is not ready for writing.\n");
                } else {
                    if (send(tcp_socket, response, strlen(response), 0) < 0) {
                        perror("send failed");
                    } else {
                        printf("Response sent successfully.\n");
                        response_sent = 1;
                    }
                }
            }  
            
            else if (strstr(buffer, "KEEP_ALIVE")) {
    if (test_delay_keep_alive_ack) {
        printf("Simulating delay in keep-alive ACK...\n");
        sleep(KEEP_ALIVE_TIMEOUT + 2);  // Delay the ACK by more than the server's timeout period
    }

    const char *ack_message = "KEEP_ALIVE_ACK";
    send(tcp_socket, ack_message, strlen(ack_message), 0);
    printf("Sent keep-alive ACK to server.\n");
}
            
            else if (strstr(buffer, "Congratulations!") || strstr(buffer, "Sorry!")) {
              //  printf("%s",buffer);
                int temp_amount, temp_team;
                // Extract the group name from the received message
                if (sscanf(buffer, "Congratulations! %d %d",&temp_amount, &temp_team) == 2){
                    if(my_bet.team!=temp_team||my_bet.amount!=temp_amount){
                        printf("Recived wrong message. asking to send again (im not printing the mistake)\n");
                        char request[BUFFER_SIZE] = "REQUEST_FINAL_MESSAGE";
                        send(tcp_socket, request, strlen(request), 0);
                    }
                    else{
                        printf("Congratulations! you won the bet because %s won! you recive %d $ :)))\n",temp,my_bet.amount);
                    }
                }
                else if (sscanf(buffer, "Sorry! %d %d",&temp_amount, &temp_team) == 2){
                    if(my_bet.amount!=temp_amount||my_bet.team!=temp_team)
                    {
                        printf("Recived wrong message. asking to send again (im not printing the mistake)\n");
                        char request[BUFFER_SIZE] = "REQUEST_FINAL_MESSAGE";
                        send(tcp_socket, request, strlen(request), 0);
                    }
                    else
                    {
                        printf("Sorry! you lost the bet because %s lost! you pay %d $ :)))\n",temp,my_bet.amount);
                    }
                }
            } 
            
            else if(strstr(buffer,"TOOLATE")){
                printf("Timeout: cant double\n");
            }
            
            else if (strstr(buffer, "DISCONNECT")) {
                 printf("Server disconnected you\n");
                 disconnected();  // Call the disconnection handling function
            }
            
            else if(strstr(buffer,"ALREADYSTARTED")){
                printf("The game has already started. You cannot place a bet now.\n");
                disconnected();
                
            } 
            
            else {
                printf("[TCP]%s", buffer);  // Print the game update or result

            }
        }
        else if (bytes_read == 0) {
            // Connection closed by server
            break;
        } 
        else
        {
            perror("read");
            break;
        }
    }

    set_stop_udp_listener(1); // Signal the UDP thread to stop
    pthread_join(update_thread, NULL); // Wait for the thread to finish
    close(tcp_socket); // Close the TCP socket
    printf("Socket closed after receiving updates.\n");
    return 0;
}

void set_ready_to_receive_updates(int value) {
    pthread_mutex_lock(&update_mutex);
    ready_to_receive_updates = value;
    pthread_mutex_unlock(&update_mutex);
}

int get_ready_to_receive_updates() {
    pthread_mutex_lock(&update_mutex);
    int value = ready_to_receive_updates;
    pthread_mutex_unlock(&update_mutex);
    return value;
}

void set_stop_udp_listener(int value) {
    pthread_mutex_lock(&update_mutex);
    stop_udp_listener = value;
    pthread_mutex_unlock(&update_mutex);
}

int get_stop_udp_listener() {
    pthread_mutex_lock(&update_mutex);
    int value = stop_udp_listener;
    pthread_mutex_unlock(&update_mutex);
    return value;
}

void disconnected() {
    printf("You have been disconnected from the server.\n");

    // Close the TCP socket if it's open
    if (tcp_socket != -1) {
        close(tcp_socket);
        tcp_socket = -1;
        printf("TCP socket closed.\n");
    }

    // Close the UDP multicast socket if it's open
    if (udp_multicast_socket != -1) {
        close(udp_multicast_socket);
        udp_multicast_socket = -1;
        printf("UDP multicast socket closed.\n");
    }

    // Signal the UDP listener thread to stop, if it's running
    set_stop_udp_listener(1);

    // Exit the program
    exit(0);
} 

